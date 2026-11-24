// xmem.dll: the GDK's tracked allocator. Mapped onto the CRT's aligned heap and VirtualQueryEx.
//
// The measured ABI (docs from the clean-room spec) shows four imports:
//   XMemAlloc(size, attributes)
//   XMemFree(ptr, attributes)
//   XMemVirtualQuery(process, address, out64)
//   XMemGetWorkingSetStatistics(kind, out)   (kind 0x20000001 at all seven FSR call sites)
//
// The public call-site measurements in Leonida PR #3601 establish a log2 alignment in bits 24-28 and page-size
// code 1 in bits 14-15 for 64 KiB pages. The retail executable's 0xf0814000 word selects 64 KiB alignment. The
// memory-type field and other page codes remain unmeasured here. A zero size keeps the existing one-byte policy.
#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shims.h"
#include "startup_trace.h"
#include "xmem_physical.h"
#include "xmem_heap_profile.h"
#include "xmem_control_pages.h"

namespace {

constexpr size_t kAlign = 64;
constexpr size_t kNoSize = static_cast<size_t>(-1);  // XMemFree has no size field
constexpr SIZE_T kTitleCommitTraceMinimum = 4 * 1024 * 1024;

void trace_large_title_allocation(const char* api, const void* requested, SIZE_T bytes, DWORD flags,
                                  DWORD protect, const void* result, DWORD error, const void* caller) {
    if (bytes < kTitleCommitTraceMinimum) return;
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("LEONIDA_TRACE_TITLE_COMMIT", value, sizeof(value)) == 1 && value[0] == '1';
    }();
    if (!enabled) return;
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 4096) return;
    MEMORY_BASIC_INFORMATION info{};
    if (result) VirtualQuery(result, &info, sizeof(info));
    std::fprintf(stderr, "[rt] xmem: %s request=%p result=%p allocation=%p bytes=%zu flags=0x%lx "
                         "protect=0x%lx host_state=0x%lx host_type=0x%lx error=%lu caller=%p\n",
                 api, requested, result, info.AllocationBase, static_cast<size_t>(bytes), flags, protect,
                 info.State, info.Type, result ? ERROR_SUCCESS : error, caller);
}

// gta6rt --trace FILE: the first allocation/free wrapper or XMemVirtualAlloc call for each distinct raw attribute word goes to the
// startup trace as an "xmem_attributes" event (api, the word, the size for an allocation, the caller as module+RVA). The
// remaining bits of the attribute word are not known yet; this records them without logging every allocation,
// which would fill the file. Off: one relaxed load per call.
void note_attributes(const char* api, uint64_t attributes, size_t size, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::pair<std::string, uint64_t>> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert({api, attributes}).second) return;
    }
    std::string fields = std::string("\"api\":\"") + api +
                         "\",\"attributes\":" + rt::startup_trace::quote(rt::startup_trace::hex(attributes));
    if (size != kNoSize) fields += ",\"size\":" + std::to_string(size);
    fields += ",\"caller\":" + rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller)));
    rt::startup_trace::event("xmem_attributes", fields);
}

size_t heap_alignment(uint64_t attributes) {
    const unsigned exponent = static_cast<unsigned>((attributes >> 24) & 0x1f);
    // Larger values can request GiB of heap padding per block. No measured caller asks for those. Refuse them
    // explicitly rather than returning a successful pointer that does not meet the requested alignment.
    if (exponent > 21) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    size_t alignment = size_t{1} << exponent;
    if (alignment < kAlign) alignment = kAlign;
    if (((attributes >> 14) & 3) == 1 && alignment < 0x10000) alignment = 0x10000;
    return alignment;
}

void* allocate_block(size_t size, uint64_t attributes) {
    // The page code controls alignment only; this remains the CRT heap, with no console physical/GPU pool.
    const size_t alignment = heap_alignment(attributes);
    return alignment ? _aligned_malloc(size ? size : 1, alignment) : nullptr;
}

void* __stdcall XMemAlloc(size_t size, uint64_t attributes) {
    note_attributes("XMemAlloc", attributes, size, _ReturnAddress());
    void* block = allocate_block(size, attributes);
    rt::xmem::heap_profile::sample_allocation("XMemAlloc", block, size, attributes, _ReturnAddress());
    return block;
}

void __stdcall XMemFree(void* p, uint64_t attributes) {
    note_attributes("XMemFree", attributes, kNoSize, _ReturnAddress());
    _aligned_free(p);
}

// The default routines have the same two-argument ABI (public SlimEra era.h and Leonida PR #6919's measured
// tail-call wrappers). Share the heap helper directly, so defaults cannot recurse through future allocation hooks
// and each public entry point records its own caller and API name.
void* __stdcall XMemAllocDefault(size_t size, uint64_t attributes) {
    note_attributes("XMemAllocDefault", attributes, size, _ReturnAddress());
    void* block = allocate_block(size, attributes);
    rt::xmem::heap_profile::sample_allocation("XMemAllocDefault", block, size, attributes, _ReturnAddress());
    return block;
}

void __stdcall XMemFreeDefault(void* p, uint64_t attributes) {
    note_attributes("XMemFreeDefault", attributes, kNoSize, _ReturnAddress());
    _aligned_free(p);
}

// PR #6919 measures a no-argument BOOL query on an allocation-failure path. A desktop process has no separate
// development-kit tools-memory pool. FALSE preserves that fact instead of advertising an unavailable fallback.
BOOL WINAPI XMemIsToolsMemAvailable() { return FALSE; }

// Measured call sites pass a zeroed 64-byte structure. Only the MEMORY_BASIC_INFORMATION-compatible prefix is
// known: callers read at least the base and size fields. Fill that prefix from VirtualQueryEx and zero the rest.
constexpr size_t kVirtualQueryOutSize = 64;
static_assert(sizeof(MEMORY_BASIC_INFORMATION) <= kVirtualQueryOutSize,
              "the 64-byte XMemVirtualQuery output must hold MEMORY_BASIC_INFORMATION");

// Returns the number of bytes written (the VirtualQuery convention) or 0 on failure. The exact return type and the
// meaning of every output field are not fully known; callers in the package only test for success.
SIZE_T __stdcall XMemVirtualQuery(HANDLE process, const void* address, void* out) {
    if (!out) return 0;
    std::memset(out, 0, kVirtualQueryOutSize);
    if (!address) return 0;

    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemVirtualQuery: fields after MEMORY_BASIC_INFORMATION are unknown; "
                     "the 64-byte output is zero-filled there\n");
    }

    // A null handle means the current process; GetCurrentProcess() is the (HANDLE)-1 pseudo handle.
    HANDLE target = process ? process : GetCurrentProcess();
    MEMORY_BASIC_INFORMATION info{};
    SIZE_T written = VirtualQueryEx(target, address, &info, sizeof(info));
    if (!written) return 0;

    std::memcpy(out, &info, written < kVirtualQueryOutSize ? written : kVirtualQueryOutSize);
    return written;
}

// BOOL XMemGetWorkingSetStatistics(UINT32 kind, XMEM_WORKING_SET_STATISTICS* stats). Measured at all seven call sites
// in amd_fidelityfx_upscaler_xbox.dll (1.0.2.1; calls at RVAs 0x26b3, 0x2ea1, 0x39a9, 0x7452, 0x74d2, 0x7568,
// 0x75b1): ECX is the constant 0x20000001 (its meaning is not known) and RDX points at a stack buffer (0x80 bytes
// where the frame shows it). The caller reads a 64-bit size at +8 and treats a zero return as "no data". Taking the
// pointer from the first argument wrote to address 0x20000001.
// The measured FSR sites establish argument shape, kind and the 64-bit working-set read at +8; +0's title-total memory
// limit interpretation is inferred from other Series implementations (xcl, xbox-emulator) and title engine telemetry
// sizing memory pools (RT-220). Note that if GlobalMemoryStatusEx reports 0 physical memory, XMem falls back to the
// 12 GiB title limit, whereas XSystemGetAppMemoryUsageLimit remains 0.
BOOL __stdcall XMemGetWorkingSetStatistics(uint32_t /*kind*/, void* stats) {
    if (!stats) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] xmem: XMemGetWorkingSetStatistics(nullptr) -> FALSE\n");
        }
        return FALSE;
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return FALSE;

    MEMORYSTATUSEX ms{sizeof(ms)};
    uint64_t total_memory = 12ULL * 1024 * 1024 * 1024;
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys != 0) {
        total_memory = ms.ullTotalPhys;
    }

    uint8_t* bytes = static_cast<uint8_t*>(stats);
    std::memset(bytes, 0, 16);
    std::memcpy(bytes + 0, &total_memory, sizeof(total_memory));
    uint64_t working_set = pmc.WorkingSetSize;
    std::memcpy(bytes + 8, &working_set, sizeof(working_set));
    return TRUE;
}

// The placeholder APIs (Windows 10 1803 and later) come from kernelbase.dll at run time, so neither gta6rt nor the
// tests link onecore.lib. A host without them fails the calls that need them with ERROR_NOT_SUPPORTED.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*,
                                        ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct PlaceholderApi {
    VirtualAlloc2Fn virtual_alloc2 = nullptr;
    MapViewOfFile3Fn map_view3 = nullptr;
    UnmapViewOfFile2Fn unmap_view2 = nullptr;
    bool available() const { return virtual_alloc2 && map_view3 && unmap_view2; }
};

const PlaceholderApi& placeholder_api() {
    static const PlaceholderApi api = [] {
        PlaceholderApi resolved;
        if (const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll")) {
            resolved.virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelbase, "VirtualAlloc2"));
            resolved.map_view3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelbase, "MapViewOfFile3"));
            resolved.unmap_view2 =
                reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kernelbase, "UnmapViewOfFile2"));
        }
        return resolved;
    }();
    return api;
}

// XMem allocation-type page bits select the guest page granularity and must not be forwarded as Win32 flags.
constexpr DWORD kXmemLargePages = 0x20000000;  // 2 MiB pages
constexpr DWORD kXmem64kPages = 0x20400000;    // 64 KiB pages
constexpr DWORD kXmemPageModeMask = kXmem64kPages;
constexpr SIZE_T kXmemPageSize64k = 0x10000;
constexpr SIZE_T kXmemPageSize2m = 0x200000;
constexpr DWORD kXmemHostAllocationTypeMask =
    MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH | 0x01000000;
// The physical-page store's reservation, set by xmem_physical.cpp when it is linked (xmem_physical.h).
std::atomic<rt::xmem::PhysicalReserveFn> g_physical_reserve{nullptr};
std::atomic<rt::xmem::PlaceholderReserveFn> g_placeholder_reserve{nullptr};

bool round_up_size(SIZE_T value, SIZE_T alignment, SIZE_T& rounded) {
    const SIZE_T max = (std::numeric_limits<SIZE_T>::max)();
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || value > max - (alignment - 1)) return false;
    rounded = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

// The console's PAGE_GRAPHICS_* protection bits (winnt.h: PAGE_GRAPHICS_NOACCESS 0x800 .. PAGE_GRAPHICS_NOCACHE
// 0x40000). The host refuses them with ERROR_INVALID_PARAMETER, and the D3D12 layer reads guest memory through the
// CPU mapping, so only the CPU protection is applied. The retail executable passes 0x801 and 0x22004.
constexpr DWORD kPageGraphicsMask = 0x0007F800;

DWORD host_protect(DWORD p) {
    const DWORD cpu = p & ~kPageGraphicsMask;
    return cpu ? cpu : PAGE_NOACCESS;
}

void* reserve_aligned_2m(SIZE_T size, DWORD host_type, DWORD protect) {
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    static const VirtualAlloc2Fn alloc2 = [] {
        const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        return kernelbase ? reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelbase, "VirtualAlloc2")) : nullptr;
    }();
    if (!alloc2) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = kXmemPageSize2m;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    // These are ordinary host pages. Guest MEM_LARGE_PAGES was stripped before this call, so no lock-memory
    // privilege is required; one host reservation also keeps VirtualFree(base, 0, MEM_RELEASE) valid.
    return alloc2(GetCurrentProcess(), nullptr, size, host_type | MEM_RESERVE, protect, &parameter, 1);
}

void* xmem_virtual_alloc(void* address, SIZE_T size, DWORD allocation_type, ULONGLONG attributes, DWORD protect) {
    // Ordinary, 64 KiB and 2 MiB guest pages use normal host pages with guest-sized alignment and rounding.
    // This does not reproduce console GPU coherence or its physical allocation budgets.
    //
    // This API's access-attribute word is recorded, not decoded; it is not the heap allocator's alignment word.
    // The host has no corresponding console allocator ids or memory types.
    // The retail executable passes 0x21, 0x3, 0x183, 0x283 and 0x181 and stops at an int3 when a reservation
    // returns NULL, so refusing a nonzero word is not an option.
    if (size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // The measured boot sequence creates an explicit placeholder, then selects
    // reserve-only 64 KiB physical windows inside it. Those windows stay reserved
    // until physical pages replace them; they are not private committed memory.
    constexpr DWORD placeholder_flags = MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER;
    if (allocation_type & placeholder_flags) {
        const DWORD mode = allocation_type & kXmemPageModeMask;
        const DWORD flags = allocation_type & ~kXmemPageModeMask;
        const bool create = mode == 0 && flags == (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER);
        const bool select = mode == kXmem64kPages && flags == (MEM_RESERVE | MEM_REPLACE_PLACEHOLDER);
        if (!create && !select) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return nullptr;
        }
        if (const auto reserve = rt::xmem::placeholder_reserve())
            return reserve(address, size, allocation_type, host_protect(protect));
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    // A new reserve-only range in the 64 KiB page mode (MEM_LARGE_PAGES | MEM_PHYSICAL) is where the title maps the
    // pages of XMemAllocatePhysicalPages, so it comes from the physical-page store when that is linked
    // (xmem_physical.cpp). Nothing is backed yet, so the attribute word does not matter here; the console's
    // PAGE_GRAPHICS_* bits are dropped and the CPU protection is what each mapped page gets.
    if (address == nullptr && (allocation_type & ~MEM_TOP_DOWN) == (MEM_RESERVE | kXmem64kPages)) {
        if (const rt::xmem::PhysicalReserveFn reserve = rt::xmem::physical_reserve()) {
            return reserve(size, host_protect(protect));
        }
    }

    const DWORD page_mode = allocation_type & kXmemPageModeMask;
    if (page_mode != 0 && page_mode != kXmem64kPages && page_mode != kXmemLargePages) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const DWORD unsupported = allocation_type & ~(kXmemHostAllocationTypeMask | kXmemPageModeMask);
    if (unsupported != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    DWORD host_type = allocation_type & kXmemHostAllocationTypeMask;
    if (host_type == 0) host_type = MEM_RESERVE | MEM_COMMIT;

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const SIZE_T guest_page = page_mode == kXmemLargePages ? kXmemPageSize2m : kXmemPageSize64k;
    const SIZE_T page_granularity = page_mode ? guest_page : system_info.dwPageSize;
    const SIZE_T base_granularity =
        page_mode
            ? guest_page
            : ((host_type & MEM_RESERVE) ? system_info.dwAllocationGranularity : system_info.dwPageSize);
    if (page_granularity == 0 || (page_granularity & (page_granularity - 1)) != 0 || base_granularity == 0 ||
        (base_granularity & (base_granularity - 1)) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    if (address != nullptr) {
        const uintptr_t requested = reinterpret_cast<uintptr_t>(address);
        const uintptr_t max = (std::numeric_limits<uintptr_t>::max)();
        if (size > max - requested) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const uintptr_t requested_end = requested + size;
        const uintptr_t base = requested & ~(static_cast<uintptr_t>(base_granularity) - 1);
        if (requested_end > max - (static_cast<uintptr_t>(page_granularity) - 1)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const uintptr_t end = (requested_end + static_cast<uintptr_t>(page_granularity) - 1) &
                              ~(static_cast<uintptr_t>(page_granularity) - 1);
        address = reinterpret_cast<void*>(base);
        size = static_cast<SIZE_T>(end - base);
    } else if (page_mode && !round_up_size(size, page_granularity, size)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    if (page_mode == kXmemLargePages && address == nullptr && (host_type & (MEM_RESERVE | MEM_COMMIT))) {
        return reserve_aligned_2m(size, host_type, host_protect(protect));
    }
    return VirtualAlloc(address, size, host_type, host_protect(protect));
}

void* WINAPI XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocation_type, ULONGLONG attributes, DWORD protect) {
    const void* caller = _ReturnAddress();
    note_attributes("XMemVirtualAlloc", attributes, size, caller);
    // The observed control allocation and shared CPU/GPU arena are imported through cached host memory. Windows
    // documents WRITECOMBINE as a device-specific cache modifier, not an access permission; Vulkan recommends
    // HOST_CACHED memory for device-to-host reads/read-modify-writes (spec, Memory Allocation / Memory Types).
    // Use ordinary write-back pages for these exact new-allocation profiles. A coherent Vulkan import and actual GPU
    // synchronization are still required; this neither admits WC imports nor changes any existing pages.
    // https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants
    // https://docs.vulkan.org/spec/latest/chapters/memory.html
    char cache_mode[13]{};
    const bool conservative_cache = GetEnvironmentVariableA("LEONIDA_XBOX_CACHE_SYNC", cache_mode, sizeof(cache_mode)) == 12 &&
        std::strcmp(cache_mode, "conservative") == 0;
    const bool coherent_control = conservative_cache && address == nullptr && size == kXmemPageSize64k &&
        allocation_type == (kXmem64kPages | MEM_COMMIT) && attributes == 0x283 && protect == 0x42404;
    const bool coherent_arena = conservative_cache && address == nullptr && size == SIZE_T{2305818624} &&
        allocation_type == (kXmemLargePages | MEM_RESERVE | MEM_COMMIT) && attributes == 0x283 && protect == 0x2404;
    const bool cached_host_allocation = coherent_control || coherent_arena;
    const DWORD effective_protect = cached_host_allocation ? protect & ~PAGE_WRITECOMBINE : protect;
    void* result = xmem_virtual_alloc(address, size, allocation_type, attributes, effective_protect);
    const DWORD error = GetLastError();
    trace_large_title_allocation("XMemVirtualAlloc", address, size, allocation_type, protect, result, error, caller);
    if (coherent_control && result)
        rt::shims::control_pages::note_allocation(reinterpret_cast<std::uintptr_t>(result), size);
    // Shared CPU/GPU write-back arenas used by CopyMemoryToMemoryX. Record successful new allocations
    // independently of host address. Control-page acquires cannot claim these ranges.
    if (result && !address && allocation_type == (kXmemLargePages | MEM_RESERVE | MEM_COMMIT) &&
        attributes == 0x183 && protect == 0x22004 && host_protect(effective_protect) == PAGE_READWRITE)
        rt::shims::control_pages::note_allocation(reinterpret_cast<std::uintptr_t>(result), size, true);
    if (cached_host_allocation && result) {
        static std::atomic<unsigned> controls{0};
        if (controls.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr, "[rt] xmem: EXPERIMENTAL %s allocation %p uses write-back CPU cache; "
                                 "size=%zu requested_protect=0x%lx host_protect=0x%lx "
                                 "(LEONIDA_XBOX_CACHE_SYNC=conservative)\n",
                coherent_control ? "coherent control" : "shared CPU/GPU arena", result,
                static_cast<std::size_t>(size), static_cast<unsigned long>(protect),
                static_cast<unsigned long>(host_protect(effective_protect)));
    }
    if (rt::startup_trace::enabled()) {
        static std::atomic<unsigned> calls{0};
        if (calls.fetch_add(1, std::memory_order_relaxed) < 64) {
            rt::startup_trace::event(
                "xmem_virtual_alloc_result",
                "\"address\":" + rt::startup_trace::quote(rt::startup_trace::hex(reinterpret_cast<uint64_t>(address))) +
                ",\"size\":" + std::to_string(size) + ",\"allocation_type\":" + std::to_string(allocation_type) +
                ",\"attributes\":" + std::to_string(attributes) + ",\"protect\":" + std::to_string(protect) +
                ",\"effective_host_protect\":" + std::to_string(host_protect(effective_protect)) +
                ",\"result\":" + rt::startup_trace::quote(rt::startup_trace::hex(reinterpret_cast<uint64_t>(result))) +
                ",\"error\":" + std::to_string(result ? ERROR_SUCCESS : error) + ",\"caller\":" +
                rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller))));
        }
    }
    SetLastError(error);
    return result;
}

UINT WINAPI XMemGetWriteWatch(DWORD flags, void* base_address, SIZE_T region_size, void** addresses, ULONG_PTR* count,
                              ULONG* granularity) {
    return GetWriteWatch(flags, base_address, region_size, addresses, count, granularity);
}

UINT WINAPI XMemResetWriteWatch(void* base_address, SIZE_T region_size) {
    return ResetWriteWatch(base_address, region_size);
}

// BOOL XMemTransferMemory(XMEM_TRANSFER_TYPE type, SIZE_T size). Its reference page and the XMEM_TRANSFER_TYPE enum
// page (gaming/gdk/docs/reference/system/xmem/{functions/xmemtransfermemory,enums/xmem_transfer_type}) are NDA
// topics on Microsoft Learn, so the shape comes from public code built against the GDK's xmem.h: Microsoft's
// XBOX-MonoGame-NetRumble sample (console/NetRumble.Bootstrap/Main.cpp) and FNA-XNA/NativeAOT-Xbox (Bootstrap/Main.cpp)
// both call `if (!XMemTransferMemory(XMemTransferTitleToSystem, additionalKernelPoolMemory))` with a `const size_t`,
// commented "Transfer additional memory to kernel pool" and "Must be multiples of 2MB sizes". So: two arguments, the
// enum (32 bits) in ECX and the size in RDX, and a zero return is failure. The public XLaunchNewGame page adds that
// "Memory transferred to the system using XMemTransferMemory (NDA topic) is persisted", and the game-save debugging
// page suggests it for E_OUTOFMEMORY "to transfer some memory to the system partition".
//
// On the console it moves budget between the title's partition and the system's. A PC has one pool and no budget
// split, so there is nothing to move: every direction and size returns TRUE and nothing is allocated or reserved. Not
// known (the pages are NDA): the numeric XMEM_TRANSFER_TYPE values, and whether the console refuses or rounds a size
// that is not a multiple of 2 MiB. Refusing would stop a title at startup (both samples return -1), while succeeding
// changes nothing here. TRUE is 1 in EAX, so it also reads as true if the real return type is a one-byte bool.
//
// gta6rt --trace FILE: the first call for each distinct (type, size) pair goes to the startup trace as an
// "xmem_transfer" event with the caller as module+RVA, so a run with the executable records the values it passes.
BOOL WINAPI XMemTransferMemory(uint32_t type, SIZE_T size) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemTransferMemory(type %u, %zu bytes): a PC has no title/system memory partition; "
                     "returning TRUE without moving anything (logged once)\n",
                     type, static_cast<size_t>(size));
    }
    if (rt::startup_trace::enabled()) {
        static std::mutex mutex;
        static std::set<std::pair<uint32_t, uint64_t>> seen;
        bool first = false;
        {
            const std::lock_guard lock(mutex);
            first = seen.insert({type, static_cast<uint64_t>(size)}).second;
        }
        if (first) {
            rt::startup_trace::event(
                "xmem_transfer",
                "\"api\":\"XMemTransferMemory\",\"type\":" + std::to_string(type) +
                    ",\"size\":" + std::to_string(static_cast<uint64_t>(size)) + ",\"caller\":" +
                    rt::startup_trace::quote(
                        rt::startup_trace::where(reinterpret_cast<uint64_t>(_ReturnAddress()))));
        }
    }
    return TRUE;
}

// kernel32!VirtualAlloc as a title calls it. The console's allocation type has two bits of its own, MEM_GRAPHICS
// (0x10000000, memory the GPU reads) and MEM_TITLE (0x40000000, the title's own heap). On a PC the host rejects both
// with ERROR_INVALID_PARAMETER (measured on Windows 11 26200: VirtualAlloc(NULL, 64 KiB, MEM_COMMIT | MEM_RESERVE |
// 0x10000000, PAGE_READWRITE) returns NULL; so does 0x40000000), so an engine allocating its heaps this way gets
// NULL at boot. Both are hints about where the memory lives; the host has no such split, so they are dropped and
// the rest is passed through. The PAGE_GRAPHICS_* protection bits are dropped (host_protect). Not done: whether
// graphics memory has to come from an arena the D3D12 layer can see (#313).
constexpr DWORD kMemGraphics = 0x10000000;
constexpr DWORD kMemTitle = 0x40000000;

LPVOID WINAPI TitleVirtualAlloc(LPVOID address, SIZE_T size, DWORD allocation_type, DWORD protect) {
    void* const result = VirtualAlloc(address, size, allocation_type & ~(kMemGraphics | kMemTitle), host_protect(protect));
    if (size >= kTitleCommitTraceMinimum) {
        const DWORD error = GetLastError();
        trace_large_title_allocation("VirtualAlloc", address, size, allocation_type, protect, result, error, _ReturnAddress());
        SetLastError(error);
    }
    return result;
}

}  // namespace

namespace rt::xmem {
void set_physical_reserve(PhysicalReserveFn fn) { g_physical_reserve.store(fn, std::memory_order_release); }
PhysicalReserveFn physical_reserve() { return g_physical_reserve.load(std::memory_order_acquire); }
void set_placeholder_reserve(PlaceholderReserveFn fn) { g_placeholder_reserve.store(fn, std::memory_order_release); }
PlaceholderReserveFn placeholder_reserve() { return g_placeholder_reserve.load(std::memory_order_acquire); }
}  // namespace rt::xmem

#define TITLE_VIRTUAL_ALLOC(dll) {dll, "VirtualAlloc", reinterpret_cast<void*>(&TitleVirtualAlloc)}

// The classic libraries and the memory api sets a title's imports name.
SHIM_TABLE(title_memory,
    TITLE_VIRTUAL_ALLOC("kernel32.dll"),
    TITLE_VIRTUAL_ALLOC("kernelbase.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-0.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-1.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-2.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-3.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-4.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-5.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-6.dll"),
);

SHIM_TABLE(xmem,
    {"xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&XMemAlloc)},
    {"xmem.dll", "XMemFree", reinterpret_cast<void*>(&XMemFree)},
    {"xmem.dll", "XMemVirtualQuery", reinterpret_cast<void*>(&XMemVirtualQuery)},
    {"xmem.dll", "XMemGetWorkingSetStatistics", reinterpret_cast<void*>(&XMemGetWorkingSetStatistics)},
    {"xmem.dll", "XMemVirtualAlloc", reinterpret_cast<void*>(&XMemVirtualAlloc)},
    {"xmem.dll", "XMemGetWriteWatch", reinterpret_cast<void*>(&XMemGetWriteWatch)},
    {"xmem.dll", "XMemResetWriteWatch", reinterpret_cast<void*>(&XMemResetWriteWatch)},
    {"xmem.dll", "XMemTransferMemory", reinterpret_cast<void*>(&XMemTransferMemory)},
    {"xmem.dll", "XMemAllocDefault", reinterpret_cast<void*>(&XMemAllocDefault)},
    {"xmem.dll", "XMemFreeDefault", reinterpret_cast<void*>(&XMemFreeDefault)},
    {"xmem.dll", "XMemIsToolsMemAvailable", reinterpret_cast<void*>(&XMemIsToolsMemAvailable)},
);
