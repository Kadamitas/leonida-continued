// Xbox-ABI proxy around a PC ID3D12Device. See d3d12_xs_device.h for the measured slots and the limits.
//
// How forwarding works: the proxy object's first word is a vtable built once from one thunk per slot. A thunk
// receives the proxy in RCX, replaces it with the PC device pointer from offset 8, and tail-calls the PC method at
// the same index through a 15-argument integer prototype. Every PC D3D12 device method takes integer/pointer
// arguments only, so the x64 registers and stack words line up exactly; methods that return a struct keep their
// hidden return pointer in RDX, which is passed through as the first argument unchanged. The measured exceptions
// are IUnknown (0-2), slot 14 (which wraps the heap), 25 and 26 (the title passes the return buffer in RCX and the
// device in RDX), 48 (a queue creation, where the PC has OpenExistingHeapFromAddress), 62 and 76, plus the heap proxy's 9/10
// conversion from the PC hidden-pointer form to the Xbox RAX form. Slots 8/75 (command queues), 36 (fences) and 32 (a shared fence) wrap
// what they create in queue and fence proxies built the same way, and slots 31/34/35/45/46/50 hand the real objects
// behind heap, queue and fence proxies to the real device (see the command queue and fence section).
//
// A thunk declares 14 argument words, but the title passes only as many as the method takes, so the caller's frame
// may end right after the four register homes. The compiler owns a function's incoming stack-argument slots and may
// write them, a tail call included. Every function a thunk hands its words to therefore takes them in the same
// positions (slot 8: desc, riid, out as a1-a3; slot 50: a1-a5; and so on). A tail call then writes back only the
// values it read from those slots. A helper that moved a word to another stack position would overwrite the title's
// frame. xs_proxy_mock_test's mock hit this in an optimized build, and its thunks now pass their words by pointer.
//
// The public HLE Freminet/ScarlettX reports five more Xbox device slots (a third-party observation, not a Microsoft
// ABI); the generic thunk routes them before it forwards anything: 48 CreateCommandQueueX, 73 CreateCommandListX,
// 78 SetFrameIntervalX, 79 ScheduleFrameEventX and 80 WaitFrameEventX. The slots can be moved with LEONIDA_XS_*_SLOT
// for a trace that disagrees. Frame-event calls and PresentX share one per-device clock and pacer, including token
// validation when the first frame wait happens before the window presenter exists.
#include "d3d12_xs_device.h"
#include "d3d12_xs_cache_sync.h"
#include "d3d12_title_resource_profile.h"
#include "xbox_linear_texture.h"
#include "xmem_pages.h"
#include "d3d12_com.h"
#include "d3d12_trace.h"
#include "shims.h"
#include "../src/shader_library.h"  // leonida::fspx_payload_from_parts for slot 67

#ifdef LEONIDA_VULKAN_DEVICE
// PresentX wiring: the queue's console slots land on the Xbox presentation core, which needs the Vulkan layer's
// WindowPresenter. Other builds of this file (mock and COM tests) keep the refused-slot behaviour.
#include "d3d12_com.h"
#include "xbox_present.h"
#endif

#include <windows.h>
#include <intrin.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef LEONIDA_VULKAN_DEVICE
#include <future>
#include <thread>
#endif

#include "d3d12_xs_command_list_iid.h"
#include "../src/frame_pacer.h"

namespace rt::d3d12_xs {
namespace {

// The retail title calls device slots well past ID3D12Device14 (slot 133 at least); every slot past the PC device's
// vtable goes through the generic thunk and is refused with E_NOTIMPL, so the table must outlast the highest one.
constexpr unsigned kMaxSlots = 192;
static_assert(kMaxSlots > 133, "the proxy table must cover the slots the retail title calls (up to 133)");

// Set by the Vulkan device behind the proxy; slot 14 uses it for a console-extended CreateDescriptorHeap.
std::atomic<ConsoleDescriptorHeapHandler> g_console_descriptor_heap{nullptr};
static_assert(kMaxSlots > 83, "the proxy table must cover ID3D12Device14 (83 slots)");
static_assert(sizeof(void*) == 8, "the Xbox-ABI proxy is x64-only");
static_assert(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) == 8,
              "the Xbox handle ABI is an 8-byte value in RAX; the heap thunk converts the PC hidden-pointer form");
static_assert(sizeof(D3D12_RESOURCE_DESC) == 56, "the measured Xbox description prefix is 56 bytes");

struct DeviceProxy {
    void** vtable;          // offset 0: the caller's ID3D12Device* points here
    ID3D12Device* real;     // offset 8: the PC device every thunk forwards to
    void** real_vtable;
    unsigned slot_count;    // number of valid entries in real_vtable
    std::atomic<unsigned long> refs;
    // The PresentX state (window, presenter, XboxPresentation) the first queue PresentX builds on this device,
    // under LEONIDA_VULKAN_DEVICE; always null elsewhere. Opaque: PresentState is defined with the queue proxy.
    void* present_state = nullptr;
    std::mutex present_state_mutex;
    // The upload-memory arena console list slot 148 (AllocateUploadMemory) carves from; always null until the
    // first call. Opaque: UploadArena is defined with the list proxy. Atomic: lists record on several threads
    // at once, so the first AllocateUploadMemory installs the arena by compare-exchange.
    std::atomic<void*> upload_state{nullptr};
    // The console runtime layout this title was built against (docs/d3d12_xs_console_abi.md Â§4): the queue proxy
    // writes it on detection and every object shares it â€” the queue/list slot tails shift +2 under the newer one.
    std::atomic<unsigned char> console_layout{0};  // 0 unknown, 1 older, 2 newer
    // Set by create_device_proxy: false hands real (PC-shaped) lists back from CreateCommandList for callers
    // compiled against the PC headers.
    bool console_lists = true;
    // The device's IDXGIDevice1 (see "IDXGIDevice1 on the device" below): a title's IDXGIDevice1* points here.
    void** dxgi_vtable;
    DxgiAdapterProvider adapter_provider;
    std::atomic<int> gpu_thread_priority;
    std::atomic<unsigned> max_frame_latency;
    // Device frame waits and every queue PresentX share one clock, token sequence and timing state. PresentState
    // is destroyed before this device, so its XboxPresentation can safely borrow this pacer.
    leonida::SteadyFrameClock frame_clock;
    leonida::FramePacer frame_pacer{frame_clock};
    struct PagePool {
        std::uintptr_t address;
        UINT pages;
    };
    std::mutex page_pool_mutex;
    std::map<std::uintptr_t, PagePool> page_pools;
    DeviceProxy* live_next = nullptr;  // allocation-free identity registry; protected by g_live_device_lock
    // Optional presentation wiring attached via set_presentation_handler (guarded by presentation_lock).
    SRWLOCK presentation_lock = SRWLOCK_INIT;
    PresentationHandler presentation{};
};

// Runs f(handler) with the presentation lock held shared, so a detach cannot destroy the presentation mid-call. f
// returns false, without calling the presentation, when it has no function for the slot; the result says whether
// f handled the call.
// ponytail: a presenter callback must not call set_presentation_handler (exclusive inside shared deadlocks), and a
// detach waits for a WaitFrameEventX in flight up to its timeout.
template <typename F>
bool call_presentation(DeviceProxy* self, F&& f) {
    if (!self) return false;
    AcquireSRWLockShared(&self->presentation_lock);
    const bool handled = f(static_cast<const PresentationHandler&>(self->presentation));
    ReleaseSRWLockShared(&self->presentation_lock);
    return handled;
}

SRWLOCK g_live_device_lock = SRWLOCK_INIT;
DeviceProxy* g_live_devices = nullptr;
std::atomic<std::uintptr_t> g_next_page_pool{1};

void register_live_device(DeviceProxy* device) {
    AcquireSRWLockExclusive(&g_live_device_lock);
    device->live_next = g_live_devices;
    g_live_devices = device;
    ReleaseSRWLockExclusive(&g_live_device_lock);
}

void unregister_live_device(DeviceProxy* device) {
    AcquireSRWLockExclusive(&g_live_device_lock);
    for (auto** at = &g_live_devices; *at; at = &(*at)->live_next) {
        if (*at != device) continue;
        *at = device->live_next;
        device->live_next = nullptr;
        break;
    }
    ReleaseSRWLockExclusive(&g_live_device_lock);
}

// Compare candidate addresses, never candidate contents. The shared lock keeps a listed allocation alive until
// it has been pinned. A last Release can set refs to zero before unlinking; CAS must never resurrect that object.
DeviceProxy* pin_live_device(const void* candidate) {
    DeviceProxy* pinned = nullptr;
    AcquireSRWLockShared(&g_live_device_lock);
    for (auto* device = g_live_devices; device; device = device->live_next) {
        if (device != candidate) continue;
        auto refs = device->refs.load(std::memory_order_relaxed);
        while (refs && refs != (std::numeric_limits<unsigned long>::max)()) {
            if (device->refs.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed)) {
                pinned = device;
                break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_live_device_lock);
    return pinned;
}

// Freed in proxy_release; defined where the states are complete.
void destroy_present_state(void* present_state);
void destroy_upload_state(void* upload_state);

constexpr unsigned kHeapSlots = 11;  // IUnknown + ID3D12Object + DeviceChild + Pageable + GetDesc + 2 handles

struct HeapProxy {
    void** vtable;          // offset 0: the caller's ID3D12DescriptorHeap* points here
    ID3D12DescriptorHeap* real;
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    std::atomic<unsigned long> refs;
    // Non-zero when the title's XMem allocation hook supplied the heap's backing address: slot 9 returns
    // custom_cpu_start and device descriptor calls translate [custom_cpu_start, +custom_byte_size) to real_cpu_start.
    SIZE_T custom_cpu_start;
    SIZE_T custom_byte_size;
    SIZE_T real_cpu_start;
    DescriptorHeapBackingFree custom_free;
};

struct DescriptorRange {
    SIZE_T custom_start;
    SIZE_T custom_end;  // exclusive
    SIZE_T real_start;
    const DeviceProxy* device;
    const HeapProxy* heap;
};

// Immortal: late COM Release calls during process teardown may still unregister a heap or check the table.
std::mutex& descriptor_range_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

std::vector<DescriptorRange>& descriptor_ranges() {
    static auto* v = new std::vector<DescriptorRange>;
    return *v;
}

std::atomic<std::size_t> g_custom_range_count{0};

std::mutex& heap_alloc_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

DescriptorHeapBackingAlloc g_heap_alloc = nullptr;
DescriptorHeapBackingFree g_heap_free = nullptr;
bool g_heap_alloc_explicit = false;

bool register_descriptor_range(const DeviceProxy* device, const HeapProxy* heap, SIZE_T custom_start,
                               SIZE_T byte_size, SIZE_T real_start) {
    if (!custom_start || !real_start || !byte_size ||
        custom_start > (std::numeric_limits<SIZE_T>::max)() - byte_size ||
        real_start > (std::numeric_limits<SIZE_T>::max)() - byte_size)
        return false;
    try {
        std::lock_guard lock(descriptor_range_mutex());
        descriptor_ranges().push_back({custom_start, custom_start + byte_size, real_start, device, heap});
        g_custom_range_count.store(descriptor_ranges().size(), std::memory_order_release);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void unregister_descriptor_range(const HeapProxy* heap) {
    if (g_custom_range_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard lock(descriptor_range_mutex());
    auto& ranges = descriptor_ranges();
    for (auto it = ranges.begin(); it != ranges.end();) {
        if (it->heap == heap) {
            it = ranges.erase(it);
        } else {
            ++it;
        }
    }
    g_custom_range_count.store(ranges.size(), std::memory_order_release);
}

D3D12_CPU_DESCRIPTOR_HANDLE translate_cpu_handle(const DeviceProxy* device, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (handle.ptr == 0 || g_custom_range_count.load(std::memory_order_acquire) == 0) return handle;
    std::lock_guard lock(descriptor_range_mutex());
    const auto& ranges = descriptor_ranges();
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        if (device && it->device != device) continue;
        if (handle.ptr >= it->custom_start && handle.ptr < it->custom_end) {
            return D3D12_CPU_DESCRIPTOR_HANDLE{it->real_start + (handle.ptr - it->custom_start)};
        }
    }
    return handle;
}

std::atomic<std::uint64_t> g_hits[kMaxSlots];
std::atomic<bool> g_first[kMaxSlots];
std::atomic<bool> g_beyond_warned[kMaxSlots];
std::atomic<std::uint64_t> g_heap_hits[kHeapSlots];
std::atomic<bool> g_heap_first[kHeapSlots];
// The hit counters are for tests. They count from the first call of a *_slot_hits reader or of
// reset_device_proxy_stats, so the calls of a title, which reads no counter, write no shared counter.
std::atomic<bool> g_count_hits{false};

// For the readers and the reset. The flag is written one time only.
void start_hit_counts() {
    if (!g_count_hits.load(std::memory_order_relaxed)) g_count_hits.store(true, std::memory_order_relaxed);
}

void count_hit(std::atomic<std::uint64_t>& hits) {
    if (g_count_hits.load(std::memory_order_relaxed)) hits.fetch_add(1, std::memory_order_relaxed);
}

// True for the first call of a slot only. The flag is read first: a compare-exchange takes the cache line for
// writing also when it fails, and the flags of all slots share one or two lines.
bool first_call(std::atomic<bool>& seen) {
    if (seen.load(std::memory_order_relaxed)) return false;
    bool expected = false;
    return seen.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

bool trace_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("XS_TRACE", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

const char* slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 8: return "CreateCommandQueue -> queue proxy";
        case 9: return "CreateCommandAllocator (PC slot 9)";
        case 10: return "CreateGraphicsPipelineState (PC slot 10)";
        case 11: return "CreateComputePipelineState (PC slot 11)";
        case 14: return "CreateDescriptorHeap";
        case 16: return "CreateRootSignature";
        case 17: return "CreateConstantBufferView (CPU handle translated)";
        case 18: return "CreateShaderResourceView (CPU handle translated)";
        case 19: return "CreateUnorderedAccessView (CPU handle translated)";
        case 20: return "CreateRenderTargetView (CPU handle translated)";
        case 21: return "CreateDepthStencilView (CPU handle translated)";
        case 22: return "CreateSampler (CPU handle translated)";
        case 23: return "CopyDescriptors (CPU handles translated)";
        case 24: return "CopyDescriptorsSimple (CPU handles translated)";
        case 25: return "GetResourceAllocationInfo (C or PC form)";
        case 26: return "GetCustomHeapProperties";
        case 27: return "CreateCommittedResource (PC slot 27)";
        case 31: return "CreateSharedHandle (proxies unwrapped)";
        case 32: return "OpenSharedHandle (a fence comes back wrapped)";
        case 34: return "MakeResident (proxies unwrapped)";
        case 35: return "Evict (proxies unwrapped)";
        case 36: return "CreateFence -> fence proxy";
        case 41: return "CreateCommandSignature (console group-count dispatch adapter)";
        case 43: return "GetAdapterLuid";
        case 45: return "SetEventOnMultipleFenceCompletion (fence proxies unwrapped)";
        case 46: return "SetResidencyPriority (proxies unwrapped)";
        case 48: return "Xbox CreateCommandQueue (32-byte desc) -> queue proxy";
        case 50: return "EnqueueMakeResident (proxies unwrapped)";
        case 57: return "Xbox DeserializeGraphicsPipelineStateX (opaque packet)";
        case 61: return "Xbox CreateSamplerX";
        case 62: return "Xbox CreateComputePipelineState";
        case 67: return "Xbox DeserializeComputePipelineStateX";
        case 73: return "Xbox CreateCommandListX -> PC CreateCommandList";
        case 74: return "Xbox RegisterPagePool";
        case 75: return "Xbox UnregisterPagePool / PC CreateCommandQueue1";
        case 76: return "Xbox resource creation";
        case 78: return "Xbox SetFrameIntervalX";
        case 79: return "Xbox ScheduleFrameEventX";
        case 80: return "Xbox WaitFrameEventX";
        case 86: return "Xbox CreatePipelineState -> PC Device2";
        case 101: return "Xbox CreateStateObject -> PC Device5";
        case 108: return "Xbox GetResourceAllocationInfo2 -> PC Device8 (known layouts/flags only)";
        case 70: return "Xbox CreateShaderResourceView at address";
        case 127: return "Xbox CreateCommittedResource with barrier layout";
        case 133: return "Xbox CreatePlacedResourceX (title-owned backing, initial barrier layout)";
        default: return "forwarded";
    }
}

void note_slot(unsigned slot) {
    if (slot >= kMaxSlots) return;
    count_hit(g_hits[slot]);
    const bool first = first_call(g_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) first call\n", slot, slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) call\n", slot, slot_name(slot));
    }
}

// Xbox device slots (public HLE Freminet/ScarlettX, an observation, not Microsoft documentation). The defaults are
// the HLE's; an environment variable that names another slot in 3..127 replaces one (a whole-table move for a trace
// that disagrees). A value outside that range is logged once and ignored. The table is read once, on the first
// generic slot call, so a test that wants other slots sets the variables before it creates a device.
struct XboxDeviceSlotRoutes {
    unsigned frame_interval = 78;  // SetFrameIntervalX(output, interval_us, latency, flags)
    unsigned schedule_event = 79;  // ScheduleFrameEventX(event, offset_us, reserved, flags)
    unsigned wait_event = 80;      // WaitFrameEventX(event, timeout_ms, reserved, flags, &token)
    unsigned frame_statistics = 81;  // GetFrameStatisticsX(token, type_mask, count, out)
    unsigned queue_x = 48;         // CreateCommandQueueX(desc, riid, out)
    unsigned list_x = 73;          // CreateCommandListX(desc, allocator, initial_state, riid, out)
};

// Reads a slot number in lo..hi from `name`: an unset variable gives `fallback`; a bad value is logged (`what` names
// the kind of slot) and ignored.
unsigned env_slot(const char* name, unsigned fallback, unsigned lo, unsigned hi, const char* what) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0) return fallback;
    if (length >= sizeof(value)) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s is too long for a %s; the default is kept\n", name, what);
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long slot = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || slot < lo || slot > hi) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s=%s is not a %s in %u..%u; the default is kept\n", name, value, what,
                     lo, hi);
        return fallback;
    }
    return static_cast<unsigned>(slot);
}

const XboxDeviceSlotRoutes& xbox_device_slot_routes() {
    static const XboxDeviceSlotRoutes routes = [] {
        XboxDeviceSlotRoutes r;
        const unsigned frame_interval = env_slot("LEONIDA_XS_FRAME_INTERVAL_SLOT", r.frame_interval, 3, 127,
                                                 "device slot");
        const unsigned schedule_event = env_slot("LEONIDA_XS_SCHEDULE_EVENT_SLOT", r.schedule_event, 3, 127,
                                                 "device slot");
        const unsigned wait_event = env_slot("LEONIDA_XS_WAIT_EVENT_SLOT", r.wait_event, 3, 127, "device slot");
        const unsigned frame_statistics = env_slot("LEONIDA_XS_FRAME_STATISTICS_SLOT", r.frame_statistics, 3, 127,
                                                   "device slot");
        const unsigned queue_x = env_slot("LEONIDA_XS_QUEUE_X_SLOT", r.queue_x, 3, 127, "device slot");
        const unsigned list_x = env_slot("LEONIDA_XS_LIST_X_SLOT", r.list_x, 3, 127, "device slot");
        const bool moved = frame_interval != r.frame_interval || schedule_event != r.schedule_event ||
                           wait_event != r.wait_event || frame_statistics != r.frame_statistics ||
                           queue_x != r.queue_x || list_x != r.list_x;
        r.frame_interval = frame_interval;
        r.schedule_event = schedule_event;
        r.wait_event = wait_event;
        r.frame_statistics = frame_statistics;
        r.queue_x = queue_x;
        r.list_x = list_x;
        if (moved)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: Xbox device slots: SetFrameIntervalX=%u ScheduleFrameEventX=%u "
                         "WaitFrameEventX=%u GetFrameStatisticsX=%u CreateCommandQueueX=%u CreateCommandListX=%u\n",
                         r.frame_interval, r.schedule_event, r.wait_event, r.frame_statistics, r.queue_x, r.list_x);
        return r;
    }();
    return routes;
}

// The public HLE call shapes. SetFrameIntervalX's output and flags are unused while there is no output or pacing;
// ScheduleFrameEventX's reserved event pointer is a3 in the console call and a4 is its flags. The logged words are the
// caller's, so a trace names the slot even when an override moved it.
void log_xbox_route_once(unsigned slot, const char* name, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                         std::uint64_t a4, std::uint64_t a5) {
    static std::atomic<bool> logged[kMaxSlots];
    if (slot >= kMaxSlots) return;
    bool expected = false;
    if (!logged[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: device slot %u routed to %s (args 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                 name, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 static_cast<unsigned long long>(a5));
}

const char* heap_slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 7: return "GetDevice";
        case 8: return "GetDesc";
        case 9: return "GetCPUDescriptorHandleForHeapStart";
        case 10: return "GetGPUDescriptorHandleForHeapStart";
        default: return "forwarded";
    }
}

void note_heap_slot(unsigned slot) {
    if (slot >= kHeapSlots) return;
    count_hit(g_heap_hits[slot]);
    const bool first = first_call(g_heap_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) first call\n", slot, heap_slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) call\n", slot, heap_slot_name(slot));
    }
}

std::uint64_t hr_to_u64(HRESULT hr) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(hr));
}

unsigned long proxy_add_ref(DeviceProxy* self) {
    note_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long release_proxy_reference(DeviceProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        unregister_live_device(self);  // no callbacks or COM release run under the registry lock
        destroy_present_state(self->present_state);
        destroy_upload_state(self->upload_state.load(std::memory_order_relaxed));
        ID3D12Device* real = self->real;
        delete self;
        if (real) real->Release();
    }
    return refs;
}

unsigned long proxy_release(DeviceProxy* self) {
    note_slot(2);
    return release_proxy_reference(self);
}

bool is_device_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device14))) return true;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device13))) return true;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device12))) return true;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device11))) return true;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device10))) return true;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device9))) return true;
#endif
    if (IsEqualIID(iid, __uuidof(ID3D12Device8))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device7))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device6))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device5))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device4))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device3))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device2))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device1))) return true;
    return false;
}

HRESULT proxy_query_interface(DeviceProxy* self, const IID* riid, void** out) {
    note_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object))) {
        *out = self;
        proxy_add_ref(self);
        return S_OK;
    }
    if (IsEqualIID(*riid, __uuidof(IDXGIObject)) || IsEqualIID(*riid, __uuidof(IDXGIDevice)) ||
        IsEqualIID(*riid, __uuidof(IDXGIDevice1))) {
        *out = &self->dxgi_vtable;
        proxy_add_ref(self);
        return S_OK;
    }
    if (!is_device_iid(*riid)) return E_NOINTERFACE;

    // Only claim an interface the PC device really implements; the proxy vtable covers its slots.
    void* supported = nullptr;
    HRESULT hr = self->real->QueryInterface(*riid, &supported);
    if (FAILED(hr)) return hr;
    if (!supported) return E_NOINTERFACE;
    static_cast<IUnknown*>(supported)->Release();
    *out = self;
    proxy_add_ref(self);
    return S_OK;
}

// ---- IDXGIDevice1 on the device ---------------------------------------------------------------------------------
//
// A GDKX title finds its output through the device: QueryInterface for IDXGIDevice1, GetAdapter, EnumOutputs(0),
// and hands that output to SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5, from the public samples). A PC
// ID3D12Device has no IDXGIDevice, so the proxy answers it itself: a second vtable pointer inside the proxy object,
// with the proxy's reference count and QueryInterface, so the two pointers are one COM object. Only IDXGIDevice and
// IDXGIDevice1 are answered (what the samples ask for). The 14 slots are the public dxgi.h order; that the console
// keeps it is the same assumption as for the queue and the fence.
//
// GetAdapter and GetParent hand out the adapter of the device behind the proxy. The provider says how: the default
// asks the host's DXGI for the adapter with the device's LUID (the host backend); set_device_adapter_provider puts
// the Vulkan layer's own adapter there, so that backend never loads the host's dxgi.dll for it.
constexpr unsigned kDxgiDeviceSlots = 14;  // IUnknown 3, IDXGIObject 4, IDXGIDevice 5, IDXGIDevice1 2

DeviceProxy* dxgi_owner(void* self) {
    return reinterpret_cast<DeviceProxy*>(static_cast<char*>(self) - offsetof(DeviceProxy, dxgi_vtable));
}

HRESULT host_adapter(ID3D12Device* real, REFIID riid, void** out) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static const CreateFactoryFn create_factory = [] {
        const HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dxgi ? reinterpret_cast<CreateFactoryFn>(
                          reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                    : nullptr;
    }();
    if (!create_factory) return DXGI_ERROR_NOT_FOUND;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    hr = factory->EnumAdapterByLuid(real->GetAdapterLuid(), riid, out);
    factory->Release();
    return hr;
}

HRESULT dxgi_adapter(DeviceProxy* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const DxgiAdapterProvider provider = self->adapter_provider ? self->adapter_provider : &host_adapter;
    const HRESULT hr = provider(self->real, riid, out);
    if (FAILED(hr)) *out = nullptr;
    return hr;
}

void note_dxgi(const char* method) {
    d3d12_trace::record(method);
}

HRESULT WINAPI dxgi_query_interface(void* self, const IID* riid, void** out) {
    return proxy_query_interface(dxgi_owner(self), riid, out);
}
ULONG WINAPI dxgi_add_ref(void* self) { return proxy_add_ref(dxgi_owner(self)); }
ULONG WINAPI dxgi_release(void* self) { return proxy_release(dxgi_owner(self)); }

// IDXGIObject's private data is the device's: ID3D12Object has the same three methods with the same arguments.
HRESULT WINAPI dxgi_set_private_data(void* self, const GUID* name, UINT size, const void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_set_private_data_interface(void* self, const GUID* name, const IUnknown* object) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateDataInterface(*name, object);
}
HRESULT WINAPI dxgi_get_private_data(void* self, const GUID* name, UINT* size, void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->GetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_get_parent(void* self, const IID* riid, void** out) {
    note_dxgi("XboxIDXGIDevice::GetParent");
    if (!riid) return E_POINTER;
    return dxgi_adapter(dxgi_owner(self), *riid, out);
}
HRESULT WINAPI dxgi_get_adapter(void* self, IDXGIAdapter** out) {
    note_dxgi("XboxIDXGIDevice::GetAdapter");
    return dxgi_adapter(dxgi_owner(self), __uuidof(IDXGIAdapter), reinterpret_cast<void**>(out));
}
// CreateSurface and QueryResourceResidency are D3D10/11-era methods no D3D12 title has a use for; what the console
// answers is not measured, so they are refused and say so once.
HRESULT WINAPI dxgi_create_surface(void*, const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
                                   IDXGISurface** out) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: IDXGIDevice::CreateSurface is not implemented; returning E_NOTIMPL\n");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI dxgi_query_resource_residency(void*, IUnknown* const*, DXGI_RESIDENCY*, UINT) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: IDXGIDevice::QueryResourceResidency is not implemented; returning E_NOTIMPL\n");
    return E_NOTIMPL;
}
// The priority and the frame latency are kept and read back; nothing acts on them (the Xbox presenter's latency is
// SetFrameIntervalX's). The ranges are the documented ones: -7..7, and 1..16 with 0 for the default of 3.
HRESULT WINAPI dxgi_set_gpu_thread_priority(void* self, INT priority) {
    if (priority < -7 || priority > 7) return E_INVALIDARG;
    dxgi_owner(self)->gpu_thread_priority.store(priority, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_gpu_thread_priority(void* self, INT* priority) {
    if (!priority) return E_POINTER;
    *priority = dxgi_owner(self)->gpu_thread_priority.load(std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_set_maximum_frame_latency(void* self, UINT latency) {
    if (latency > 16) return DXGI_ERROR_INVALID_CALL;
    dxgi_owner(self)->max_frame_latency.store(latency ? latency : 3, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_maximum_frame_latency(void* self, UINT* latency) {
    if (!latency) return E_POINTER;
    *latency = dxgi_owner(self)->max_frame_latency.load(std::memory_order_relaxed);
    return S_OK;
}

void** get_dxgi_vtable() {
    static void* table[kDxgiDeviceSlots] = {
        reinterpret_cast<void*>(&dxgi_query_interface),          // 0
        reinterpret_cast<void*>(&dxgi_add_ref),                  // 1
        reinterpret_cast<void*>(&dxgi_release),                  // 2
        reinterpret_cast<void*>(&dxgi_set_private_data),         // 3  IDXGIObject
        reinterpret_cast<void*>(&dxgi_set_private_data_interface),
        reinterpret_cast<void*>(&dxgi_get_private_data),
        reinterpret_cast<void*>(&dxgi_get_parent),
        reinterpret_cast<void*>(&dxgi_get_adapter),              // 7  IDXGIDevice
        reinterpret_cast<void*>(&dxgi_create_surface),
        reinterpret_cast<void*>(&dxgi_query_resource_residency),
        reinterpret_cast<void*>(&dxgi_set_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_get_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_set_maximum_frame_latency),  // 12 IDXGIDevice1
        reinterpret_cast<void*>(&dxgi_get_maximum_frame_latency),
    };
    return table;
}

unsigned long heap_add_ref(HeapProxy* self) {
    note_heap_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long heap_release(HeapProxy* self) {
    note_heap_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        if (self->custom_cpu_start != 0) unregister_descriptor_range(self);
        void* const backing = reinterpret_cast<void*>(self->custom_cpu_start);
        const DescriptorHeapBackingFree free_fn = self->custom_free;
        ID3D12DescriptorHeap* real = self->real;
        DeviceProxy* device = self->device;
        delete self;
        if (real) real->Release();
        if (device) proxy_release(device);
        if (backing && free_fn) free_fn(backing, kXmemDescriptorHeapAttributes);
    }
    return refs;
}

bool is_heap_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(IUnknown));
}

HRESULT heap_query_interface(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_heap_iid(*riid)) return E_NOINTERFACE;
    *out = self;
    heap_add_ref(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice on the heap must hand back the Xbox-ABI device, not the raw PC device.
HRESULT heap_get_device(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(7);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

// The PC methods write through the hidden return pointer (RDX) on the measured runtime; the Xbox ABI wants the
// 8-byte value in RAX. Call with both conventions and keep whichever produced a value, so a runtime that really
// returns in RAX also works. The extra register is harmless when the callee ignores it.
D3D12_CPU_DESCRIPTOR_HANDLE raw_heap_get_cpu_handle(ID3D12DescriptorHeap* real, void** real_vtable) {
#ifdef LEONIDA_VULKAN_DEVICE
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    D3D12_GPU_VIRTUAL_ADDRESS base = 0;
    if (rt::d3d12::descriptor_heap_info(real, desc, base))
        return real->GetCPUDescriptorHandleForHeapStart();
#endif
    constexpr SIZE_T kUnset = ~static_cast<SIZE_T>(0);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    const D3D12_CPU_DESCRIPTOR_HANDLE returned = reinterpret_cast<Fn>(real_vtable[9])(real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

D3D12_CPU_DESCRIPTOR_HANDLE heap_get_cpu_handle(HeapProxy* self) {
    note_heap_slot(9);
    if (self->custom_cpu_start != 0) return D3D12_CPU_DESCRIPTOR_HANDLE{self->custom_cpu_start};
    return raw_heap_get_cpu_handle(self->real, self->real_vtable);
}

D3D12_GPU_DESCRIPTOR_HANDLE heap_get_gpu_handle(HeapProxy* self) {
    note_heap_slot(10);
#ifdef LEONIDA_VULKAN_DEVICE
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    D3D12_GPU_VIRTUAL_ADDRESS base = 0;
    if (rt::d3d12::descriptor_heap_info(self->real, desc, base))
        return self->real->GetGPUDescriptorHandleForHeapStart();
#endif
    constexpr UINT64 kUnset = ~static_cast<UINT64>(0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    const D3D12_GPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[10])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

template <unsigned Slot>
std::uint64_t WINAPI heap_slot(HeapProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(heap_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return heap_add_ref(self);
    } else if constexpr (Slot == 2) {
        return heap_release(self);
    } else if constexpr (Slot == 7) {
        return hr_to_u64(heap_get_device(self, reinterpret_cast<const IID*>(a1),
                                         reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 9) {
        return static_cast<std::uint64_t>(heap_get_cpu_handle(self).ptr);
    } else if constexpr (Slot == 10) {
        return static_cast<std::uint64_t>(heap_get_gpu_handle(self).ptr);
    } else {
        note_heap_slot(Slot);
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_heap_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&heap_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_heap_vtable() {
    static void** vtable = [] {
        static void* storage[kHeapSlots];
        fill_heap_vtable(storage, std::make_index_sequence<kHeapSlots>{});
        return storage;
    }();
    return vtable;
}

HRESULT create_heap_proxy(ID3D12DescriptorHeap* real, DeviceProxy* device, REFIID riid, void** out,
                          SIZE_T custom_cpu_start = 0, SIZE_T custom_byte_size = 0, SIZE_T real_cpu_start = 0,
                          DescriptorHeapBackingFree custom_free = nullptr) {
    if (!real || !device) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    HeapProxy* proxy = new (std::nothrow) HeapProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_heap_vtable();
    proxy->real = real;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->custom_cpu_start = custom_cpu_start;
    proxy->custom_byte_size = custom_byte_size;
    proxy->real_cpu_start = real_cpu_start;
    proxy->custom_free = custom_free;
    proxy_add_ref(device);  // the heap keeps its Xbox-ABI device alive for GetDevice

    if (custom_cpu_start != 0 &&
        !register_descriptor_range(device, proxy, custom_cpu_start, custom_byte_size, real_cpu_start)) {
        delete proxy;
        proxy_release(device);
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = heap_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its references to `real` and `device`.
        if (custom_cpu_start != 0) unregister_descriptor_range(proxy);
        delete proxy;
        proxy_release(device);
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: heap proxy %p wraps PC heap %p (device proxy %p)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), static_cast<void*>(device));
    heap_release(proxy);
    return S_OK;
}

// ---- Command queue and fence proxies --------------------------------------------------------------------------
//
// Device slots 8 (CreateCommandQueue), 75 (CreateCommandQueue1), 36 (CreateFence) and 32 (OpenSharedHandle, for a
// fence) wrap what they create, so a title never holds a PC or Vulkan-layer queue or fence. The proxies forward
// the public chain in PC order, exactly like the device proxy; on a fence a slot past the PC vtable is logged and
// refused with E_NOTIMPL. PresentX, SuspendX and ResumeX are queue methods past the public chain
// (docs/d3d12_command_objects.md Â§5.3). A sibling layer measured two queue layouts on retail titles
// (docs/d3d12_xs_console_abi.md Â§4, unverified here): the older has Signal 14, Wait 15,
// GetTimestampFrequency 16, GetClockCalibration 17, GetDesc 18, SignalX 23, PresentX 30; a newer game
// OS inserts two methods before Signal, shifting to Signal 16, Wait 17, frequency 18, SignalX 25,
// PresentX 32 â€” a fence passed to slot 16 or 17 marks the newer layout, and the detection is shared
// across queues through the device. PresentX is wired to the Vulkan layer's Xbox presentation,
// SignalX forwards as Signal, and the unmodelled console-only slots answer S_OK (the PresentX
// section below); the first run of the executable confirms which layout.
//
// Slot numbers are those of the Windows SDK 10.0.26100 d3d12.h: ID3D12CommandQueue has 19, ID3D12Fence 11 and
// ID3D12Fence1 12. That the console keeps these public slots, and keeps device slots 8/32/36/75, is an assumption
// the shipped binaries cannot test (Â§3.5: the package has no queue or fence client). Every queue and fence method
// takes integer and pointer arguments only; GetDesc returns its struct through the hidden pointer in RDX, which is
// forwarded unchanged (no Xbox variant is measured, Â§6).
//
// The refused range is the proxy's own table: 128 queue slots and 64 fence slots, every one a thunk. The table sits
// at the end of the first page of a 64 KiB reservation whose other pages are never committed, so a slot index past
// the table makes the vtable read itself fault, at an address that names the slot (a vectored handler logs it and
// lets the fault continue), instead of jumping through whatever memory follows. A title whose vtable read lands
// further away than that reservation is not covered.
//
// Objects cross back into the real runtime wherever a device or queue method takes them, and the real object is
// passed there: queue Signal/Wait (14/15) take a fence; device CreateSharedHandle (31) a device child,
// MakeResident/Evict (34/35), SetResidencyPriority (46) and EnqueueMakeResident (50) pageables, and
// SetEventOnMultipleFenceCompletion (45) and EnqueueMakeResident fences. The heap proxies of slot 14 are pageables
// and device children too and are unwrapped in the same places. Command lists from device slot 12 come back
// console-shaped (the list proxy below): queue ExecuteCommandLists (10) unwraps the list array, the list's own
// Reset and ExecuteBundle unwrap allocator and bundle proxies, and its SetDescriptorHeaps unwraps heap proxies â€”
// all through proxy_real_object, so any object kind added later unwraps in every place at once.
//
// QueryInterface answers only the interfaces the proxy models (the base chain and its own interface, Fence1 when
// the real fence has it), like the device and heap proxies, so the COM identity holds and no real object leaks to
// the title. Leonida's own code reaches the real object through queue_proxy_real_queue / fence_proxy_real_fence /
// allocator_proxy_real_allocator.
//
// Command allocators (device slot 9) are the third kind: their public chain is 9 slots (Reset at 8). Slot 9 wraps
// only an allocator that answers kLeonidaCommandAllocatorTag, because a list takes the title's allocator in
// CreateCommandList and Reset and only the Vulkan layer's lists resolve a proxy there (CommandAllocator::from);
// a PC runtime allocator is handed out unchanged until a command-list proxy unwraps it at Reset.

enum class ChildKind { queue, fence, allocator };

constexpr unsigned kQueueSlots = 128;        // the table: 19 public slots plus a refused tail
constexpr unsigned kFenceSlots = 64;         // the table: 12 public slots plus a refused tail
constexpr unsigned kQueuePublicSlots = 19;   // ID3D12CommandQueue
constexpr unsigned kFencePublicSlots = 11;   // ID3D12Fence
constexpr unsigned kFence1PublicSlots = 12;  // ID3D12Fence1
constexpr unsigned kAllocatorSlots = 64;     // the table: 9 public slots plus a refused tail
constexpr unsigned kAllocatorPublicSlots = 9;  // ID3D12CommandAllocator: the base chain and Reset (slot 8)
constexpr SIZE_T kTableRegion = 64 * 1024;   // one allocation granule: the table page plus 15 never-committed pages
static_assert(kQueueSlots > kQueuePublicSlots && kFenceSlots > kFence1PublicSlots &&
                  kAllocatorSlots > kAllocatorPublicSlots,
              "the tables need a tail");
static_assert(kQueueSlots * sizeof(void*) <= 4096 && kFenceSlots * sizeof(void*) <= 4096 &&
                  kAllocatorSlots * sizeof(void*) <= 4096,
              "a table must fit in the first page of its reservation");

constexpr unsigned kQueueSignal = 14, kQueueWait = 15;  // (ID3D12Fence*, UINT64)

struct ChildProxy {
    void** vtable;          // offset 0: the caller's ID3D12CommandQueue* / ID3D12Fence* points here
    IUnknown* real;         // offset 8: the PC or Vulkan-layer object every thunk forwards to
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    unsigned slot_count;    // public slots the real object implements; the rest of the table is refused
    std::atomic<unsigned long> refs;
    // Queues only: the console runtime layout this title was built against (docs/d3d12_xs_console_abi.md Â§4),
    // detected by a fence reaching slot 16/17 or SignalX slot 25. Unused on fences and allocators. A queue is
    // used from many threads, so the cache of the device's shared detection is atomic like the shared flag
    // itself.
    static constexpr unsigned char kQueueLayoutUnknown = 0;
    static constexpr unsigned char kQueueLayoutOlder = 1;   // PC order; PresentX 30, SignalX 23
    static constexpr unsigned char kQueueLayoutNewer = 2;   // +2 shift; PresentX 32, SignalX 25
    std::atomic<unsigned char> queue_layout{kQueueLayoutUnknown};
    ChildKind kind = ChildKind::allocator;
    ChildProxy* live_next = nullptr;  // protected by g_live_child_lock; no allocation for identity tracking
};

SRWLOCK g_live_child_lock = SRWLOCK_INIT;
constexpr std::size_t kLiveChildBuckets = 256;
ChildProxy* g_live_children[kLiveChildBuckets]{};

std::size_t live_child_bucket(const void* object) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    return ((address >> 4) ^ (address >> 12)) & (kLiveChildBuckets - 1);
}

void register_live_child(ChildProxy* child) {
    AcquireSRWLockExclusive(&g_live_child_lock);
    auto& head = g_live_children[live_child_bucket(child)];
    child->live_next = head;
    head = child;
    ReleaseSRWLockExclusive(&g_live_child_lock);
}

void unregister_live_child(ChildProxy* child) {
    AcquireSRWLockExclusive(&g_live_child_lock);
    for (auto** at = &g_live_children[live_child_bucket(child)]; *at; at = &(*at)->live_next) {
        if (*at != child) continue;
        *at = child->live_next;
        child->live_next = nullptr;
        break;
    }
    ReleaseSRWLockExclusive(&g_live_child_lock);
}

// Match addresses and kind, never candidate bytes: a frequency/calibration UINT64 can contain any vtable value.
// The shared lock keeps a matching allocation alive while CAS pins it; refs0 must never be resurrected.
template <ChildKind K>
ChildProxy* pin_live_child(const void* candidate) {
    ChildProxy* pinned = nullptr;
    AcquireSRWLockShared(&g_live_child_lock);
    for (auto* child = g_live_children[live_child_bucket(candidate)]; child; child = child->live_next) {
        if (child != candidate || child->kind != K) continue;
        auto refs = child->refs.load(std::memory_order_relaxed);
        while (refs && refs != (std::numeric_limits<unsigned long>::max)()) {
            if (child->refs.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed)) {
                pinned = child;
                break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_live_child_lock);
    return pinned;
}

template <ChildKind K>
bool is_live_child(const void* candidate) {
    bool found = false;
    AcquireSRWLockShared(&g_live_child_lock);
    for (const auto* child = g_live_children[live_child_bucket(candidate)]; child; child = child->live_next) {
        if (child != candidate || child->kind != K) continue;
        found = child->refs.load(std::memory_order_relaxed) != 0;
        break;
    }
    ReleaseSRWLockShared(&g_live_child_lock);
    return found;
}

template <ChildKind K>
struct ChildStats {
    static constexpr unsigned kSlots = K == ChildKind::queue   ? kQueueSlots
                                       : K == ChildKind::fence ? kFenceSlots
                                                               : kAllocatorSlots;
    static inline std::atomic<std::uint64_t> hits[kSlots];
    static inline std::atomic<bool> first[kSlots];
    static inline std::atomic<bool> beyond_warned[kSlots];
    static inline std::atomic<std::uint64_t> overruns{0};
    // Where the table and its reservation are: written once, before the table is handed out.
    static inline std::atomic<std::uintptr_t> table_begin{0}, table_end{0}, region_end{0};
};

template <ChildKind K>
constexpr const char* child_kind_name() {
    return K == ChildKind::queue ? "queue" : K == ChildKind::fence ? "fence" : "allocator";
}

template <ChildKind K>
const char* child_public_name(unsigned slot) {
    static constexpr const char* object[] = {"QueryInterface", "AddRef", "Release", "GetPrivateData",
                                             "SetPrivateData", "SetPrivateDataInterface", "SetName", "GetDevice"};
    static constexpr const char* queue[] = {"UpdateTileMappings", "CopyTileMappings", "ExecuteCommandLists",
                                            "SetMarker", "BeginEvent", "EndEvent", "Signal", "Wait",
                                            "GetTimestampFrequency", "GetClockCalibration", "GetDesc"};
    static constexpr const char* fence[] = {"GetCompletedValue", "SetEventOnCompletion", "Signal",
                                            "GetCreationFlags"};
    if (slot < 8) return object[slot];
    if constexpr (K == ChildKind::queue) {
        if (slot < kQueuePublicSlots) return queue[slot - 8];
    } else if constexpr (K == ChildKind::fence) {
        if (slot < kFence1PublicSlots) return fence[slot - 8];
    } else {
        if (slot == 8) return "Reset";
    }
    return nullptr;
}

// Fixed names for the trace channel (d3d12_trace::record takes fixed strings): one per table slot, built once, in
// the trace grammar of runtime/README.md and check_d3d12_trace.py (an identifier with `::`, no spaces):
// XboxID3D12CommandQueue::Signal, and XboxID3D12CommandQueue::Slot40 for a slot past the public chain.
template <ChildKind K>
const char* child_trace_name(unsigned slot) {
    static const auto names = [] {
        static char storage[ChildStats<K>::kSlots][64];
        const char* iface = K == ChildKind::queue   ? "XboxID3D12CommandQueue"
                            : K == ChildKind::fence ? "XboxID3D12Fence"
                                                    : "XboxID3D12CommandAllocator";
        for (unsigned s = 0; s < ChildStats<K>::kSlots; ++s) {
            const char* method = child_public_name<K>(s);
            if (method)
                std::snprintf(storage[s], sizeof(storage[s]), "%s::%s", iface, method);
            else
                std::snprintf(storage[s], sizeof(storage[s]), "%s::Slot%u", iface, s);
        }
        return &storage;
    }();
    return (*names)[slot < ChildStats<K>::kSlots ? slot : 0];
}

template <ChildKind K>
void note_child_slot(unsigned slot) {
    using S = ChildStats<K>;
    if (slot >= S::kSlots) return;
    count_hit(S::hits[slot]);
    const bool first = first_call(S::first[slot]);
    const char* method = child_public_name<K>(slot);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) first call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    }
}

// The counters reset_device_proxy_stats clears for one kind: per-slot hits, first-call and beyond-table flags, and
// the faulted reads past the table.
template <ChildKind K>
void reset_child_stats() {
    using S = ChildStats<K>;
    for (unsigned slot = 0; slot < S::kSlots; ++slot) {
        S::hits[slot].store(0, std::memory_order_relaxed);
        S::first[slot].store(false, std::memory_order_relaxed);
        S::beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    S::overruns.store(0, std::memory_order_relaxed);
}

// Frees a proxy whose last reference is gone, with the real object and the device reference it held.
void destroy_child(ChildProxy* self) {
    unregister_live_child(self);  // no foreign Release callback runs under the registry lock
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

unsigned long release_child_reference(ChildProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_child(self);
    return refs;
}

template <ChildKind K>
unsigned long child_add_ref(ChildProxy* self) {
    note_child_slot<K>(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <ChildKind K>
unsigned long child_release(ChildProxy* self) {
    note_child_slot<K>(2);
    return release_child_reference(self);
}

// The interfaces the proxy answers: the base chain and the object's own interface.
template <ChildKind K>
bool is_child_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
        IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)))
        return true;
    if constexpr (K == ChildKind::queue) {
        return IsEqualIID(iid, __uuidof(ID3D12CommandQueue));
    } else if constexpr (K == ChildKind::fence) {
        return IsEqualIID(iid, __uuidof(ID3D12Fence)) || IsEqualIID(iid, __uuidof(ID3D12Fence1));
    } else {
        return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator));
    }
}

bool supports_iid(IUnknown* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// An interface the real object implements but the proxy does not model is refused where the PC runtime would answer:
// logged once per interface (the first 32 of them), so a title that stops after such a refusal shows what it asked
// for. Also reached from wrap_child, for the interface a title creates a queue or fence as.
void log_refused_iid(const char* kind, const IID& iid) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static IID seen[32];
    static unsigned count = 0;
    AcquireSRWLockExclusive(&lock);
    bool known = false;
    for (unsigned i = 0; i < count && !known; ++i) known = IsEqualIID(seen[i], iid) != 0;
    const bool log = !known && count < std::size(seen);
    if (log) seen[count++] = iid;
    ReleaseSRWLockExclusive(&lock);
    if (!log) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: %s proxy refuses {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}, which the real "
                 "object implements; E_NOINTERFACE\n",
                 kind, iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                 iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

template <ChildKind K>
HRESULT child_query_interface(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // Only what the proxy models, and of that only what the real object implements (ID3D12Fence1 is optional).
    if (!is_child_iid<K>(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid(child_kind_name<K>(), *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    child_add_ref<K>(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice hands back the Xbox-ABI device, not the real one. Without an output it is the real
// object's own answer (what the PC runtime returns for a null pointer).
template <ChildKind K>
HRESULT child_get_device(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(7);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure (bc59ef46)
    if (!riid) return E_POINTER;
    if (!out) {
        using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
        return reinterpret_cast<Fn>(self->real_vtable[7])(self->real, riid, nullptr);
    }
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

void** get_heap_vtable();
void** get_list_vtable();

template <ChildKind K>
void** get_child_vtable();

// The command-list proxy's helpers (defined with the list proxy below): proxy_real_object checks lists through
// list_real_or_null, since the ListProxy type is complete only there.
IUnknown* list_real_or_null(const void* object);

template <ChildKind K>
bool is_child_proxy(const void* object) {
    return is_live_child<K>(object);
}

// The real object behind a heap, queue, fence or list proxy, or null when `object` is none of them.
IUnknown* proxy_real_object(const void* object) {
    if (!object) return nullptr;
    void* const vtable = *reinterpret_cast<void* const*>(object);
    if (vtable == reinterpret_cast<void*>(get_heap_vtable())) return static_cast<const HeapProxy*>(object)->real;
    // Native objects take no registry lock or walk. A child-vtable match only selects the one identity lookup;
    // matching bytes alone still do not establish that the candidate is a live child proxy.
    if ((vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::queue>()) &&
         is_child_proxy<ChildKind::queue>(object)) ||
        (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::fence>()) &&
         is_child_proxy<ChildKind::fence>(object)) ||
        (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::allocator>()) &&
         is_child_proxy<ChildKind::allocator>(object)))
        return static_cast<const ChildProxy*>(object)->real;
    if (IUnknown* real = list_real_or_null(object)) return real;
    return nullptr;
}

// `object` with a proxy replaced by its real object; anything else (a real object, null) unchanged.
inline std::uint64_t unwrap_object(std::uint64_t object) {
    IUnknown* real = proxy_real_object(reinterpret_cast<const void*>(object));
    return real ? reinterpret_cast<std::uint64_t>(real) : object;
}

// A copy of an array of interface pointers with every proxy replaced by its real object. Nothing here throws, since
// an exception must not leave a thunk for the title's code: up to 16 entries are kept inline (no allocation, which
// also suits a per-frame ExecuteCommandLists), a longer array is allocated with new (std::nothrow).
class UnwrappedArray {
public:
    // Copies `count` entries (a UINT in every PC prototype) of `objects`. False on allocation failure.
    bool fill(std::uint64_t objects, std::uint64_t count) {
        const auto n = static_cast<std::uint32_t>(count);
        if (!objects || !n) return true;
        std::uint64_t* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) std::uint64_t[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
        for (std::uint32_t i = 0; i < n; ++i) copy[i] = unwrap_object(in[i]);
        data_ = copy;
        return true;
    }
    // The copy to pass on, or 0 when there was no array (the caller's argument then goes on unchanged).
    std::uint64_t data() const { return reinterpret_cast<std::uint64_t>(data_); }

private:
    std::uint64_t inline_[16];
    std::unique_ptr<std::uint64_t[]> heap_;
    std::uint64_t* data_ = nullptr;
};

// The vtable read for a slot past a table faults in the reservation behind it; name the slot. Only reads the
// exception record and writes with WriteFile, so it is safe whatever the faulting thread holds.
template <ChildKind K>
void report_table_overrun(std::uintptr_t address) {
    using S = ChildStats<K>;
    const std::uintptr_t begin = S::table_begin.load(std::memory_order_acquire);
    const std::uintptr_t end = S::table_end.load(std::memory_order_acquire);
    const std::uintptr_t limit = S::region_end.load(std::memory_order_acquire);
    if (!end || address < end || address >= limit) return;
    S::overruns.fetch_add(1, std::memory_order_relaxed);
    char line[160];
    unsigned long long slot = (address - begin) / sizeof(void*);
    char digits[24];
    int d = 0;
    do {
        digits[d++] = static_cast<char>('0' + slot % 10);
        slot /= 10;
    } while (slot && d < 23);
    int n = 0;
    const auto put = [&](const char* text) {
        while (*text && n < static_cast<int>(sizeof(line)) - 1) line[n++] = *text++;
    };
    put("[rt] d3d12_xs: ");
    put(child_kind_name<K>());
    put(" slot ");
    while (d > 0 && n < static_cast<int>(sizeof(line)) - 1) line[n++] = digits[--d];
    put(" is past the proxy table; the vtable read faults\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

LONG CALLBACK table_overrun_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        report_table_overrun<ChildKind::queue>(address);
        report_table_overrun<ChildKind::fence>(address);
        report_table_overrun<ChildKind::allocator>(address);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // diagnostics only: the fault goes on to the title's handlers or the crash
}

// PresentX is at queue slot 30 in an older runtime layout and 32 in a newer one (public corroboration, XCL; see
// docs/d3d12_vulkan.md). 30 is the default and LEONIDA_XS_PRESENT_X_SLOT sets the other. SuspendX and ResumeX have no
// default: 0 means not routed, so they answer only when their variable is set.
struct XboxQueueSlotRoutes {
    unsigned present_x = 30;  // PresentX(plane_count, planes, params)
    unsigned suspend_x = 0;   // SuspendX(flags)
    unsigned resume_x = 0;    // ResumeX()
};

const XboxQueueSlotRoutes& xbox_queue_slot_routes() {
    static const XboxQueueSlotRoutes routes = [] {
        XboxQueueSlotRoutes r;
        r.present_x = env_slot("LEONIDA_XS_PRESENT_X_SLOT", r.present_x, kQueuePublicSlots, kQueueSlots - 1,
                               "queue slot");
        r.suspend_x = env_slot("LEONIDA_XS_SUSPEND_X_SLOT", r.suspend_x, kQueuePublicSlots, kQueueSlots - 1,
                               "queue slot");
        r.resume_x = env_slot("LEONIDA_XS_RESUME_X_SLOT", r.resume_x, kQueuePublicSlots, kQueueSlots - 1,
                              "queue slot");
        // Two routes on one slot cannot both answer: PresentX keeps it, then SuspendX; the later route is unrouted.
        if (r.suspend_x && r.suspend_x == r.present_x) {
            std::fprintf(stderr, "[rt] d3d12_xs: SuspendX slot %u is PresentX's slot; SuspendX is not routed\n",
                         r.suspend_x);
            r.suspend_x = 0;
        }
        if (r.resume_x && (r.resume_x == r.present_x || r.resume_x == r.suspend_x)) {
            std::fprintf(stderr, "[rt] d3d12_xs: ResumeX slot %u is already routed; ResumeX is not routed\n",
                         r.resume_x);
            r.resume_x = 0;
        }
        return r;
    }();
    return routes;
}

// ---- PresentX on the queue (LEONIDA_VULKAN_DEVICE) ---------------------------------------------------------
//
// The measured console queue slots (docs/d3d12_xs_console_abi.md Â§4): the older runtime keeps the PC order â€”
// Signal 14, Wait 15, frequency 16, calibration 17, GetDesc 18 â€” and presents at slot 30, with SignalX at 23. A
// newer game OS inserts two methods before Signal: Signal 16, Wait 17, frequency 18, calibration 19, GetDesc 20,
// SignalX 25, PresentX 32. The layout is detected once, by a fence object reaching slot 16 or 17 (the older
// layout's frequency and calibration getters take no object argument) or by a fence reaching SignalX slot 25 â€”
// a title that signals only through SignalX would otherwise stay undetected and lose every signal.
//
// Every other console-only queue method answers S_OK (sibling layer: title frame pacing flows through them, and a
// refusal wedges the present loop), still recorded by note_child_slot. Slot 148's AllocateUploadMemory is a list
// method, not a queue method, so it is not part of this table.

#ifdef LEONIDA_VULKAN_DEVICE

void release_present_window(HWND window) {
    if (!window) return;
    // A queue can present on a different thread from the one that made the fallback. HWND destruction must run
    // on its owner; these messages contain no PresentState pointer and remain safe after a rebind or teardown.
    if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) DestroyWindow(window);
    else {
        ShowWindowAsync(window, SW_HIDE);
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK present_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

struct TitleWindowSearch { HWND window = nullptr; unsigned matches = 0; };

// Use the same explicit monitor policy as window_placement.cpp. A fallback has no guest CreateWindow import for
// that adapter to intercept, so it must select the exact device itself, never a nearest/primary substitute.
struct PresentMonitorSearch {
    wchar_t device[64]{};
    MONITORINFOEXW info{};
    bool found = false;
};

BOOL CALLBACK find_present_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM value) {
    auto& search = *reinterpret_cast<PresentMonitorSearch*>(value);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && !_wcsicmp(info.szDevice, search.device)) {
        search.info = info;
        search.found = true;
        return FALSE;
    }
    return TRUE;
}

bool present_monitor(MONITORINFOEXW& info) {
    PresentMonitorSearch search;
    const DWORD count = GetEnvironmentVariableW(L"LEONIDA_WINDOW_MONITOR", search.device, static_cast<DWORD>(std::size(search.device)));
    if (!count || count >= std::size(search.device)) return false;
    EnumDisplayMonitors(nullptr, nullptr, find_present_monitor, reinterpret_cast<LPARAM>(&search));
    if (search.found) info = search.info;
    return search.found;
}

struct PresentPhysicalCoordinates {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~PresentPhysicalCoordinates() { if (previous) SetThreadDpiAwarenessContext(previous); }
};

// The guest and its ordinary windows stay on the launcher's isolated desktop. This thread owns
// only the optional presentation HWND on the original desktop and pumps it throughout Vulkan WSI's lifetime.
class DesktopPreview {
public:
    using Presenter = std::unique_ptr<rt::d3d12::WindowPresenter>;

    ~DesktopPreview() { close(); }

    bool requested() const { return requested_; }
    HWND window() const { return window_; }

    Presenter open_if_requested(ID3D12Device* device) {
        SetLastError(ERROR_SUCCESS);
        const DWORD count = GetEnvironmentVariableW(L"LEONIDA_PREVIEW_DESKTOP", name_, DWORD(std::size(name_)));
        requested_ = count || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (!requested_) return {};
        if (!count || count >= std::size(name_) || std::wcschr(name_, L'\\')) {
            std::fprintf(stderr, "[rt] isolated preview: invalid desktop name\n");
            return {};
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        GetStartupInfoW(&startup);
        if ((startup.dwFlags & STARTF_USESHOWWINDOW) && startup.wShowWindow != SW_SHOWNOACTIVATE) {
            std::fprintf(stderr, "[rt] isolated preview: inherited startup show mode overrides no-activation\n");
            return {};
        }
        try {
            stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stop_) return {};
            std::packaged_task<Presenter()> create([this, device] { return create_window(device); });
            auto ready = create.get_future();
            thread_ = std::thread([this, create = std::move(create)]() mutable {
                create();  // publishes the HWND and presenter, or the creation failure, to the caller
                if (window_) {
                    pump();
                    DestroyWindow(window_);
                }
            });
            auto presenter = ready.get();
            if (!presenter) close();
            return presenter;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[rt] isolated preview: %s\n", error.what());
            close();
            return {};
        }
    }

    void close() {
        if (thread_.joinable()) {
            SetEvent(stop_);
            thread_.join();
        }
        if (desktop_) CloseDesktop(std::exchange(desktop_, nullptr));  // its last attached thread has exited
        if (stop_) CloseHandle(std::exchange(stop_, nullptr));
    }

private:
    static bool desktop_named(HDESK desktop, const wchar_t* expected, bool& matches) {
        wchar_t actual[256]{};
        DWORD bytes = 0;
        if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, actual, sizeof(actual), &bytes)) return false;
        matches = _wcsicmp(actual, expected) == 0;
        return true;
    }

    bool on_target() const {
        MONITORINFOEXW monitor{};
        return present_monitor(monitor) && !(monitor.dwFlags & MONITORINFOF_PRIMARY) &&
            EqualRect(&area_, &monitor.rcMonitor);
    }

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<DesktopPreview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (self && (message == WM_DISPLAYCHANGE || message == WM_CLOSE)) {
            // A changed topology can reassign a display name. Keep the surface alive for the queue
            // but hide it until a new launch verifies the output again.
            self->hidden_ = true;
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE);
            return 0;
        }
        if (self && message == WM_WINDOWPOSCHANGING) {
            auto* pos = reinterpret_cast<WINDOWPOS*>(lparam);
            pos->flags |= SWP_NOACTIVATE;
            if ((pos->flags & SWP_SHOWWINDOW) || !(pos->flags & SWP_NOMOVE) || !(pos->flags & SWP_NOSIZE)) {
                if (self->hidden_ || !self->on_target()) {
                    self->hidden_ = true;
                    pos->flags = (pos->flags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE;
                } else {
                    pos->x = self->area_.left;
                    pos->y = self->area_.top;
                    pos->cx = self->area_.right - self->area_.left;
                    pos->cy = self->area_.bottom - self->area_.top;
                }
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    Presenter create_window(ID3D12Device* device) {
        const auto fail = [](const char* step) -> Presenter {
            std::fprintf(stderr, "[rt] isolated preview: %s\n", step);
            return {};
        };
        // SetThreadDesktop must precede this thread's first window or hook. Opening the input
        // desktop checks its name only; it never switches the user's desktop.
        bool same = false;
        if (!desktop_named(GetThreadDesktop(GetCurrentThreadId()), name_, same) || same)
            return fail("guest desktop is unavailable or already the preview desktop");
        HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        const bool interactive = desktop_named(input, name_, same) && same;
        if (input) CloseDesktop(input);
        if (!interactive) return fail("configured preview desktop is not the current input desktop");
        desktop_ = OpenDesktopW(name_, 0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
        if (!desktop_ || !SetThreadDesktop(desktop_)) return fail("could not attach preview thread to the desktop");

        PresentPhysicalCoordinates coordinates;
        MONITORINFOEXW monitor{};
        if (!coordinates.previous || !present_monitor(monitor) || (monitor.dwFlags & MONITORINFOF_PRIMARY))
            return fail("selected non-primary monitor or physical coordinates unavailable");
        area_ = monitor.rcMonitor;
        if (IsRectEmpty(&area_)) return fail("selected monitor has empty bounds");
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LeonidaIsolatedPresentPreview";
        if (!RegisterClassW(&wc)) {
            WNDCLASSW existing{};
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||
                !GetClassInfoW(wc.hInstance, wc.lpszClassName, &existing) || existing.lpfnWndProc != proc)
                return fail("could not register the preview window class");
        }
        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Leonida GTA VI preview",
            WS_POPUP | WS_DISABLED, area_.left, area_.top, area_.right - area_.left, area_.bottom - area_.top,
            nullptr, nullptr, wc.hInstance, this);
        RECT outer{}, client{};
        if (!window_ || hidden_ || !on_target() || !GetWindowRect(window_, &outer) || !EqualRect(&area_, &outer) ||
            !GetClientRect(window_, &client) || client.right != area_.right - area_.left ||
            client.bottom != area_.bottom - area_.top) return fail("window bounds differ from the selected monitor");
        auto presenter = rt::d3d12::create_window_presenter(device, window_);
        if (!presenter || hidden_ || !on_target()) return fail("Vulkan surface or selected monitor unavailable");
        // Show a verified surface before the first acquire: a hidden swapchain can be occluded.
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        if (hidden_ || !IsWindowVisible(window_)) return fail("verified preview window did not become visible");
        std::fprintf(stderr, "[rt] isolated preview: desktop=%ls monitor=%ls client=%ldx%ld hwnd=%p\n",
            name_, monitor.szDevice, client.right, client.bottom, static_cast<void*>(window_));
        return presenter;
    }

    void pump() const {
        for (;;) {
            const DWORD status = MsgWaitForMultipleObjectsEx(1, &stop_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (status != WAIT_OBJECT_0 + 1) return;
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                // Lifetime belongs to the Vulkan presenter. Ignore unrelated WM_QUIT posts.
                if (msg.message == WM_QUIT) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    wchar_t name_[256]{};
    RECT area_{};
    bool requested_ = false, hidden_ = false;
    HDESK desktop_ = nullptr;
    HWND window_ = nullptr;
    HANDLE stop_ = nullptr;
    std::thread thread_;
};

struct PresentState {
    ~PresentState() {
        xbox.reset();  // retire the native surface before either owned HWND is destroyed
        if (owns_window) release_present_window(window);
    }
    HWND window = nullptr;
    DesktopPreview preview;
    std::unique_ptr<rt::d3d12::XboxPresentation> xbox;
    bool owns_window = false;  // legacy fallback; the isolated preview owns its own HWND
    std::atomic<bool> show_requested{false};
    bool fallback_placed = false;
    bool tried = false;  // initial creation is attempted once; a late title HWND can replace an owned fallback
};

void destroy_present_state(void* present_state) {
    delete static_cast<PresentState*>(present_state);
}

BOOL CALLBACK find_present_title_window(HWND window, LPARAM value) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) ||
        GetAncestor(window, GA_ROOT) != window) return TRUE;
    wchar_t title[16]{};
    // As in the placement observer, read the native caption without sending WM_GETTEXT into a guest procedure.
    // The title UI thread may still be initializing rather than pumping messages when the first frame arrives.
    using Caption = int(WINAPI*)(HWND, LPWSTR, int);
    static const auto caption = reinterpret_cast<Caption>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "InternalGetWindowText"));
    if (!caption || caption(window, title, static_cast<int>(std::size(title))) != 8 ||
        std::wcscmp(title, L"Americas") != 0) return TRUE;
    auto& found = *reinterpret_cast<TitleWindowSearch*>(value);
    found.window = window;
    ++found.matches;
    return TRUE;
}

// Lazily builds the window, presenter and XboxPresentation a console PresentX needs, once per device. Returns the
// state even when `xbox` stays null (no presentation support). A late title HWND may replace an owned fallback.
PresentState* present_state_for(DeviceProxy* device) {
    std::lock_guard creation_lock(device->present_state_mutex);
    if (auto* state = static_cast<PresentState*>(device->present_state)) return state;
    auto* state = new (std::nothrow) PresentState;
    if (!state) return nullptr;
    device->present_state = state;
    state->tried = true;
    auto preview = state->preview.open_if_requested(device->real);
    if (state->preview.requested()) {
        if (preview) {
            state->window = state->preview.window();
            try {
                state->xbox = std::make_unique<rt::d3d12::XboxPresentation>(std::move(preview), nullptr, &device->frame_pacer);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[rt] isolated preview: presentation startup failed: %s\n", error.what());
                preview.reset();
                state->preview.close();
                state->window = nullptr;
            }
        } else {
            std::fprintf(stderr, "[rt] isolated preview: requested desktop, monitor, window or Vulkan surface unavailable\n");
        }
        return state;  // never borrow, show or rebind to the isolated guest window in preview mode
    }
    TitleWindowSearch title;
    EnumWindows(find_present_title_window, reinterpret_cast<LPARAM>(&title));
    if (title.matches > 1) {
        std::fprintf(stderr, "[rt] d3d12_xs: PresentX refused ambiguous own-process Americas windows (%u)\n", title.matches);
        return state;
    }
    if (title.matches == 1) state->window = title.window;
    if (!state->window) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = present_window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"LeonidaPresentX";
        if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            std::fprintf(stderr, "[rt] d3d12_xs: PresentX window class failed (error %lu)\n", GetLastError());
            return state;
        }
        PresentPhysicalCoordinates coordinates;
        MONITORINFOEXW monitor{};
        state->fallback_placed = present_monitor(monitor);
        const RECT area = monitor.rcMonitor;
        wchar_t background[2]{};
        const bool no_activate = GetEnvironmentVariableW(L"LEONIDA_WINDOW_BACKGROUND", background, 2) == 1 &&
                                 background[0] == L'1';
        // A borderless fallback's client rectangle is exactly the selected physical display, including at 4K
        // with non-100% DPI. It starts hidden and is shown only after a real native image present below.
        state->window = CreateWindowExW(no_activate ? WS_EX_NOACTIVATE : 0, window_class.lpszClassName, L"Leonida",
                                      state->fallback_placed ? WS_POPUP : WS_OVERLAPPEDWINDOW,
                                      state->fallback_placed ? area.left : CW_USEDEFAULT,
                                      state->fallback_placed ? area.top : CW_USEDEFAULT,
                                      state->fallback_placed ? area.right - area.left : CW_USEDEFAULT,
                                      state->fallback_placed ? area.bottom - area.top : CW_USEDEFAULT,
                                      nullptr, nullptr, window_class.hInstance, nullptr);
        if (!state->window) {
            std::fprintf(stderr, "[rt] d3d12_xs: PresentX window failed (error %lu)\n", GetLastError());
            return state;
        }
        state->owns_window = true;
        RECT outer{}, client{};
        state->fallback_placed = state->fallback_placed && GetWindowRect(state->window, &outer) &&
            GetClientRect(state->window, &client) && outer.left == area.left && outer.top == area.top &&
            outer.right == area.right && outer.bottom == area.bottom &&
            client.right - client.left == area.right - area.left &&
            client.bottom - client.top == area.bottom - area.top && client.right > client.left &&
            client.bottom > client.top;
        std::fprintf(stderr, "[rt] d3d12_xs: PresentX owns hidden fallback window; selected-monitor placement=%u "
            "client=%ldx%ld\n", unsigned(state->fallback_placed), client.right - client.left,
            client.bottom - client.top);
    }
    auto presenter = rt::d3d12::create_window_presenter(device->real, state->window);
    if (!presenter) {
        std::fprintf(stderr, "[rt] d3d12_xs: no window presenter on this device; PresentX is unavailable\n");
        return state;
    }
    state->xbox = std::make_unique<rt::d3d12::XboxPresentation>(std::move(presenter), nullptr, &device->frame_pacer);
    std::fprintf(stderr, "[rt] d3d12_xs: PresentX ready (window %p, %s, shared frame pacer)\n",
        static_cast<void*>(state->window), state->owns_window ? "owned hidden fallback" : "borrowed Americas");
    return state;
}

// The native title UI can appear after the first PresentX. Prefer it to a second visible fallback. Creation of
// the replacement is transactional; destruction of the old presenter joins its worker and waits for the Vulkan
// queue under its submit mutex before releasing its swapchain and surface. The device frame pacer survives.
void rebind_present_title_window(DeviceProxy* device, PresentState* state) {
    if (!state->owns_window) return;
    TitleWindowSearch title;
    EnumWindows(find_present_title_window, reinterpret_cast<LPARAM>(&title));
    if (title.matches != 1) return;
    auto presenter = rt::d3d12::create_window_presenter(device->real, title.window);
    if (!presenter) return;
    auto xbox = std::make_unique<rt::d3d12::XboxPresentation>(std::move(presenter), nullptr, &device->frame_pacer);
    const HWND fallback = state->window;
    state->xbox = std::move(xbox);
    state->window = title.window;
    state->owns_window = false;
    state->show_requested = false;
    release_present_window(fallback);
    RECT client{};
    GetClientRect(state->window, &client);
    std::fprintf(stderr, "[rt] d3d12_xs: PresentX rebound fallback %p to borrowed Americas %p client=%ldx%ld\n",
        static_cast<void*>(fallback), static_cast<void*>(state->window), client.right - client.left,
        client.bottom - client.top);
}

std::uint64_t queue_present_x(ChildProxy* self, unsigned slot, std::uint64_t caller, std::uint64_t plane_count,
                              std::uint64_t planes, std::uint64_t params) {
    PresentState* state = self->device ? present_state_for(self->device) : nullptr;
    if (!state) {
        rt::shims::unimplemented_proxy_effect("queue", slot, caller, reinterpret_cast<std::uint64_t>(self), plane_count, planes, params);
        return hr_to_u64(E_NOTIMPL);
    }
    // Serialize the rebind, the native present and visibility accounting across all queues on this device.
    std::lock_guard present_lock(self->device->present_state_mutex);
    rebind_present_title_window(self->device, state);
    if (!state->xbox) {
        rt::shims::unimplemented_proxy_effect("queue", slot, caller, reinterpret_cast<std::uint64_t>(self), plane_count, planes, params);
        return hr_to_u64(E_NOTIMPL);
    }
    const HRESULT result = state->xbox->PresentX(
        static_cast<UINT>(plane_count),
        reinterpret_cast<const rt::d3d12::XboxPresentPlaneParameters*>(planes),
        reinterpret_cast<const rt::d3d12::XboxPresentParameters*>(params));
    // S_OK also covers minimized, deferred and lost-surface no-ops. Only the native WSI present counter proves
    // that an image was accepted, including a deferred present completed between calls. Never expose an unplaced
    // fallback on another monitor.
    if (!state->preview.requested() && result == S_OK && state->xbox->presenter().presented() != 0 &&
        (!state->owns_window || state->fallback_placed) && !state->show_requested.exchange(true)) {
        const BOOL requested = ShowWindowAsync(state->window, SW_SHOWNOACTIVATE);
        if (!requested) state->show_requested = false;
        std::fprintf(stderr, "[rt] d3d12_xs: PresentX %s visibility requested without activation (accepted=%u)\n",
            state->owns_window ? "fallback" : "title", unsigned(requested != FALSE));
    }
    return hr_to_u64(result);
}

#else  // !LEONIDA_VULKAN_DEVICE: this translation unit stands alone (mock and COM tests)

void destroy_present_state(void*) {}

std::uint64_t queue_present_x(ChildProxy* self, unsigned slot, std::uint64_t caller, std::uint64_t a1,
                              std::uint64_t a2, std::uint64_t a3) {
    rt::shims::unimplemented_proxy_effect("queue", slot, caller, reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
    return hr_to_u64(E_NOTIMPL);
}

#endif

// Process policy, set before launch. This deliberately reduces the unmeasured console token/flag pacing to a
// real immediate PC submission. It is an experiment, not an implementation of the console scheduling contract.
bool immediate_queue_submit_enabled() {
    static const bool enabled = [] {
        char value[10]{};
        return GetEnvironmentVariableA("LEONIDA_XBOX_QUEUE_SUBMIT", value, sizeof(value)) == 9 &&
               std::strcmp(value, "immediate") == 0;
    }();
    return enabled;
}

// A bounded startup fast path, not a general GPU memory wait: the measured CPU-owned control cell starts at
// generation 1. Replay must independently observe the already-satisfied predicate and import prior host writes.
// Unsatisfied waits and later generations still refuse; the console flag's complete contract remains unverified.
bool satisfied_host_generation_enabled() {
    static const bool enabled = [] {
        char value[64]{};
        const DWORD length = GetEnvironmentVariableA("LEONIDA_XBOX_MEMORY_WAIT", value, sizeof(value));
        return length && length < sizeof(value) && std::strcmp(value, "satisfied_host_generation") == 0;
    }();
    return enabled;
}

bool try_immediate_queue_submit(ChildProxy* self, std::uint64_t count, std::uint64_t lists,
                                std::uint64_t token, std::uint64_t raw_flags, std::uint64_t caller);

#ifdef LEONIDA_VULKAN_DEVICE
HRESULT queue_copy_page_mappings(ChildProxy* self, UINT count, const void* batches_ptr,
                                 std::uint64_t pool, UINT pool_pages, UINT flags) {
    struct PageRange { UINT kind, first, count; };
    struct PageBatch { std::uint64_t destination; UINT count; UINT padding; const PageRange* ranges; };
    static_assert(sizeof(PageRange) == 12 && sizeof(PageBatch) == 24 && offsetof(PageBatch, ranges) == 16);
    constexpr auto page_bytes = rt::xmem::kPhysicalPageBytes;
    if (flags || (count && !batches_ptr)) return E_INVALIDARG;
    if (!count) return S_OK;
    const auto snapshot = [](const void* source, auto& records) {
        const auto bytes = records.size() * sizeof(records[0]);
        SIZE_T copied = 0;
        return bytes == 0 || (ReadProcessMemory(GetCurrentProcess(), source, records.data(), bytes, &copied) &&
                              copied == bytes);
    };
    try {
        std::vector<PageBatch> batches(count);
        if (!snapshot(batches_ptr, batches)) return E_POINTER;
        std::vector<rt::xmem::PhysicalPageAliasRange> aliases;
        std::vector<PageRange> ranges;
        for (const auto& batch : batches) {
            auto destination = batch.destination;
            if (!destination || destination % page_bytes) return E_INVALIDARG;
            ranges.resize(batch.count);
            if (!snapshot(batch.ranges, ranges)) return E_POINTER;
            for (const auto& range : ranges) {
                if (range.kind > 3 || range.count > (UINT64_MAX - destination) / page_bytes) return E_INVALIDARG;
                if (!range.count) continue;
                std::uint64_t source = 0;
                if (range.kind < 2) {
                    const UINT source_pages = range.kind == 0 ? range.count : 1;
                    if (!pool || pool % page_bytes || range.first >= pool_pages ||
                        source_pages > pool_pages - range.first || pool_pages > (UINT64_MAX - pool) / page_bytes)
                        return E_INVALIDARG;
                    source = pool + std::uint64_t(range.first) * page_bytes;
                }
                if (range.kind != 3)
                    aliases.push_back({destination, source, range.count, range.kind == 1});
                destination += std::uint64_t(range.count) * page_bytes;
            }
        }
        if (aliases.empty()) return S_OK;
        const HRESULT result = rt::d3d12::copy_console_page_mappings(self->real, aliases);
        static std::atomic<unsigned> traces{0};
        if (traces.fetch_add(1, std::memory_order_relaxed) < 12) {
            const auto& first = aliases.front();
            std::fprintf(stderr, "[rt] d3d12_xs: page mappings batches=%u aliases=%zu pool_pages=%u "
                                 "first_destination=0x%llx first_source=0x%llx first_count=%zu repeat=%u hr=0x%08x\n",
                count, aliases.size(), pool_pages, static_cast<unsigned long long>(first.destination),
                static_cast<unsigned long long>(first.source), first.page_count, unsigned(first.repeat_source),
                static_cast<unsigned>(result));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (const std::length_error&) {
        return E_INVALIDARG;
    }
}
#endif

template <ChildKind K, unsigned Slot>
std::uint64_t WINAPI child_slot(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_query_interface<K>(self, reinterpret_cast<const IID*>(a1),
                                                  reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return child_add_ref<K>(self);
    } else if constexpr (Slot == 2) {
        return child_release<K>(self);
    } else if constexpr (Slot == 7) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_get_device<K>(self, reinterpret_cast<const IID*>(a1),
                                             reinterpret_cast<void**>(a2)));
    } else {
        note_child_slot<K>(Slot);
        // Retain these through the final backend call, including the generic old-layout Signal/Wait path.
        // Internal references do not affect the guest-visible AddRef/Release hit counters.
        using Pin = std::unique_ptr<ChildProxy, decltype(&release_child_reference)>;
        Pin queue_pin(nullptr, &release_child_reference), fence_pin(nullptr, &release_child_reference);
#ifdef LEONIDA_VULKAN_DEVICE
        if constexpr (K == ChildKind::fence && Slot == 12) {
            fence_pin.reset(pin_live_child<ChildKind::fence>(self));
            if (fence_pin && self->device && self->device->console_lists) {
                // The return value is an address, as in VIctory's fence slot 12. The title reads the
                // completed value directly. The native timeline, not a submitted signal, advances it.
                if (const auto* completion = rt::d3d12::fence_completion_address(self->real)) {
                    d3d12_trace::record("XboxID3D12Fence::GetCompletionAddress");
                    return reinterpret_cast<std::uintptr_t>(completion);
                }
                rt::shims::unimplemented_proxy_effect("fence", Slot,
                    reinterpret_cast<std::uint64_t>(_ReturnAddress()), reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
                return 0;
            }
        }
#endif
        if constexpr (K == ChildKind::queue) {
            queue_pin.reset(pin_live_child<ChildKind::queue>(self));
            if (!queue_pin) return hr_to_u64(E_INVALIDARG);
            const XboxQueueSlotRoutes& qroutes = xbox_queue_slot_routes();
            HRESULT hr = S_OK;
            if (Slot >= self->slot_count && call_presentation(self->device, [&](const PresentationHandler& p) {
                    if (Slot == qroutes.present_x && p.present_x) {
                        hr = p.present_x(p.context, static_cast<UINT>(a1), reinterpret_cast<const void*>(a2),
                                         reinterpret_cast<const void*>(a3));
                        return true;
                    }
                    if (Slot == qroutes.suspend_x && p.suspend_x) {
                        hr = p.suspend_x(p.context, static_cast<UINT>(a1));
                        return true;
                    }
                    if (Slot == qroutes.resume_x && p.resume_x) {
                        hr = p.resume_x(p.context);
                        return true;
                    }
                    return false;
                }))
                return hr_to_u64(hr);

            if constexpr (Slot == 14 || Slot == 15 || Slot == 16 || Slot == 17 || Slot == 23 || Slot == 25)
                fence_pin.reset(pin_live_child<ChildKind::fence>(reinterpret_cast<const void*>(a1)));
            // Layout detection (the comment at the PresentX section): a fence reaching slot 16 or 17 is the newer
            // layout's Signal/Wait; anything else there is the older layout's frequency/calibration getter. A
            // fence at slot 14 or 15 is the older Signal/Wait; a non-fence argument there is one of the newer
            // layout's two inserted console methods.
            // A queue made after another one detected the layout inherits it from the device; undetected queues
            // assume the older layout, whose unshifted slots are also the PC chain. A fence reaching SignalX
            // slot 25 while the layout is still unknown is itself the newer-layout evidence: a title that
            // signals only through SignalX never reaches Signal/Wait at 16/17, and an unanswered SignalX
            // returns S_OK having signalled nothing, leaving a waiter on that fence value hanging.
            const bool newer = [&] {
                const unsigned char own = self->queue_layout.load(std::memory_order_relaxed);
                if (own == ChildProxy::kQueueLayoutNewer) return true;
                const unsigned char shared =
                    self->device->console_layout.load(std::memory_order_relaxed);
                return own == ChildProxy::kQueueLayoutUnknown && shared == ChildProxy::kQueueLayoutNewer;
            }();
            if (Slot == 16 || Slot == 17) {
                if (fence_pin) {
                    self->queue_layout.store(ChildProxy::kQueueLayoutNewer, std::memory_order_relaxed);
                    self->device->console_layout.store(ChildProxy::kQueueLayoutNewer,
                                                       std::memory_order_relaxed);
                    using Fn = HRESULT(WINAPI*)(IUnknown*, IUnknown*, UINT64);
                    return hr_to_u64(reinterpret_cast<Fn>(self->real_vtable[kQueueSignal + (Slot - 16)])(
                        self->real, fence_pin->real, a2));
                }
                if (newer) {
                    rt::shims::unimplemented_proxy_effect("queue", Slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
                        reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
                    return hr_to_u64(S_OK);  // legacy: non-fence Signal/Wait argument on a proven-newer queue
                }
                // not evidence of the older layout â€” do not downgrade the shared detection on it
                if (self->queue_layout.load(std::memory_order_relaxed) ==
                    ChildProxy::kQueueLayoutUnknown) {
                    self->queue_layout.store(ChildProxy::kQueueLayoutOlder, std::memory_order_relaxed);
                    self->device->console_layout.store(ChildProxy::kQueueLayoutOlder,
                                                       std::memory_order_relaxed);
                }
            } else if ((Slot == 14 || Slot == 15) && !fence_pin &&
                       newer) {
                rt::shims::unimplemented_proxy_effect("queue", Slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
                    reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
                return hr_to_u64(S_OK);  // the newer layout's two inserted console methods, unmeasured
            }
            if (newer && Slot >= 18 && Slot <= 20) {
                // Newer layout: frequency 18, calibration 19, GetDesc 20 are the PC slots 16-18 shifted.
                d3d12_trace::record(Slot == 18 ? "XboxID3D12CommandQueue::GetTimestampFrequency" :
                    Slot == 19 ? "XboxID3D12CommandQueue::GetClockCalibration" : "XboxID3D12CommandQueue::GetDesc");
                using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t);
                auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot - 2]);
                return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            }
            // Slot 18 while the layout is still unknown is ambiguous: the older GetDesc (a struct-return
            // write of a whole desc) or the newer frequency getter (an 8-byte out). Answering it as GetDesc
            // writes far past the 8 bytes a frequency caller passes, so the call goes to the frequency
            // getter â€” the only interpretation that cannot overrun its argument, and a title often queries
            // the frequency before its first fence call. A proven-older queue keeps the generic GetDesc
            // path below; slots 19 and 20 stay no-ops, since their newer interpretations write through
            // caller pointers an unmodelled older method might not supply.
            if (Slot == 18 && !newer && self->queue_layout == ChildProxy::kQueueLayoutUnknown &&
                self->device->console_layout.load(std::memory_order_relaxed) !=
                    ChildProxy::kQueueLayoutOlder) {
                d3d12_trace::record("XboxID3D12CommandQueue::GetTimestampFrequency (layout probe)");
                using Fn = HRESULT(WINAPI*)(IUnknown*, UINT64*);
                return hr_to_u64(reinterpret_cast<Fn>(self->real_vtable[16])(
                    self->real, reinterpret_cast<UINT64*>(a1)));
            }
            if (Slot == (newer ? 25u : 23u) ||
                (Slot == 25u && self->queue_layout.load(std::memory_order_relaxed) ==
                                    ChildProxy::kQueueLayoutUnknown &&
                 self->device->console_layout.load(std::memory_order_relaxed) ==
                     ChildProxy::kQueueLayoutUnknown)) {
                // SignalX: console Signal (older 23, newer 25); the extra X word is dropped. The slot-25
                // alternative is the fence itself carrying the layout evidence, per the comment above; a
                // proven-older queue still leaves slot 25 to the console-only no-op, since its fence-taking
                // methods sit at 14/15/23 there.
                if (fence_pin) {
                    if (Slot == 25u && !newer) {
                        self->queue_layout.store(ChildProxy::kQueueLayoutNewer,
                                                 std::memory_order_relaxed);
                        self->device->console_layout.store(ChildProxy::kQueueLayoutNewer,
                                                           std::memory_order_relaxed);
                    }
                    static std::atomic<bool> logged{false};
                    if (!logged.exchange(true, std::memory_order_relaxed))
                        std::fprintf(stderr, "[rt] d3d12_xs: SignalX (queue slot %u) forwarded as Signal\n", Slot);
                    using Fn = HRESULT(WINAPI*)(IUnknown*, IUnknown*, UINT64);
                    return hr_to_u64(reinterpret_cast<Fn>(self->real_vtable[kQueueSignal])(
                        self->real, fence_pin->real, a2));
                }
                rt::shims::unimplemented_proxy_effect("queue", Slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
                    reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
                return hr_to_u64(S_OK);
            }
#ifdef LEONIDA_VULKAN_DEVICE
            if constexpr (Slot == 31) {
                if (newer && self->device->console_lists) {
                    const HRESULT result = queue_copy_page_mappings(self, static_cast<UINT>(a1),
                        reinterpret_cast<const void*>(a2), a3, static_cast<UINT>(a4), static_cast<UINT>(a5));
                    return hr_to_u64(d3d12_trace::result("XboxQueue31CopyPageMappingsBatchX", result));
                }
            }
#endif
            if (Slot == (newer ? 32u : 30u))
                return queue_present_x(self, Slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()), a1, a2, a3);
            if constexpr (Slot == 35) {
                static std::atomic<unsigned> submits{0};
                if (submits.fetch_add(1, std::memory_order_relaxed) < 16)
                    std::fprintf(stderr, "[rt] d3d12_xs: queue35 count=%llu lists=0x%llx token=%llu "
                                         "raw_flags=0x%llx flags32=0x%x issued_next=%llu newer=%u\n",
                        a1, a2, a3, a4, static_cast<unsigned>(static_cast<std::uint32_t>(a4)),
                        self->device->frame_pacer.last_issued_token() + 1, unsigned(newer));
                if (self->device->console_lists && newer &&
                    try_immediate_queue_submit(self, a1, a2, a3, a4,
                                               reinterpret_cast<std::uint64_t>(_ReturnAddress())))
                    return 0;  // the measured callers ignore RAX; this does not establish its console return type
            }
            if (Slot >= self->slot_count || (newer && (Slot == 14 || Slot == 15))) {
                d3d12_trace::record(child_trace_name<K>(Slot), "console-only");
                rt::shims::unimplemented_proxy_effect("queue", Slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
                    reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
                return hr_to_u64(self->device->console_lists ? S_OK : E_NOTIMPL);
            }
        }
        if (Slot >= self->slot_count) {
            bool expected = false;
            if (ChildStats<K>::beyond_warned[Slot].compare_exchange_strong(expected, true,
                                                                            std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: %s slot %u is beyond the PC %s vtable (%u slots); returning E_NOTIMPL "
                             "(args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                             child_kind_name<K>(), Slot, child_kind_name<K>(), self->slot_count,
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            }
            d3d12_trace::record(child_trace_name<K>(Slot), "unsupported");
            return hr_to_u64(E_NOTIMPL);
        }
        d3d12_trace::record(child_trace_name<K>(Slot));  // "called": the real object's result is not inspected
        if constexpr (K == ChildKind::queue && (Slot == kQueueSignal || Slot == kQueueWait))
            a1 = fence_pin ? reinterpret_cast<std::uint64_t>(fence_pin->real) : unwrap_object(a1);

        // Queue slot 10: ExecuteCommandLists(count, lists). The array is copied with each list proxy replaced by
        // its real list; a bundle on the list side unwraps the same way at the console's ExecuteBundle.
        UnwrappedArray unwrapped;
        if constexpr (K == ChildKind::queue && Slot == 10) {
            if (!unwrapped.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
            if (unwrapped.data()) a2 = unwrapped.data();
        }

        // The PC vtable entry with the real object as `this`; integer/pointer arguments, the hidden struct-return
        // pointer and stack arguments keep their positions, as in the device proxy.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <ChildKind K, std::size_t... I>
void fill_child_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&child_slot<K, static_cast<unsigned>(I)>)), ...);
}

// True when the fault logger is installed (once per process; a failure is logged once).
bool install_table_overrun_handler() {
    static const bool installed = [] {
        if (AddVectoredExceptionHandler(0, &table_overrun_handler)) return true;
        std::fprintf(stderr, "[rt] d3d12_xs: AddVectoredExceptionHandler failed (error %lu); a slot past a queue or "
                     "fence proxy table faults without being named\n", GetLastError());
        return false;
    }();
    return installed;
}

template <ChildKind K>
void** get_child_vtable() {
    static void** vtable = [] {
        using S = ChildStats<K>;
        constexpr std::size_t bytes = S::kSlots * sizeof(void*);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const SIZE_T page = system.dwPageSize;
        if (void* base = page >= bytes ? VirtualAlloc(nullptr, kTableRegion, MEM_RESERVE, PAGE_NOACCESS) : nullptr) {
            if (VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE)) {
                auto* table = reinterpret_cast<void**>(static_cast<char*>(base) + page - bytes);
                fill_child_vtable<K>(table, std::make_index_sequence<S::kSlots>{});
                DWORD old = 0;
                if (!VirtualProtect(base, page, PAGE_READONLY, &old))  // the table works, but stays writable
                    std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table stays writable (VirtualProtect error %lu)\n",
                                 child_kind_name<K>(), GetLastError());
                S::table_begin.store(reinterpret_cast<std::uintptr_t>(table), std::memory_order_release);
                S::region_end.store(reinterpret_cast<std::uintptr_t>(base) + kTableRegion, std::memory_order_release);
                // table_end is what "guarded" means (command_object_tables_guarded, the fault logger): set only when
                // a fault past the table will be named. Without the logger the region still faults, unnamed.
                if (install_table_overrun_handler())
                    S::table_end.store(reinterpret_cast<std::uintptr_t>(base) + page, std::memory_order_release);
                return table;
            }
            VirtualFree(base, 0, MEM_RELEASE);
        }
        // No reservation: the table still works, but a slot past it is not caught.
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table without a guard region\n", child_kind_name<K>());
        static void* storage[S::kSlots];
        fill_child_vtable<K>(storage, std::make_index_sequence<S::kSlots>{});
        return storage;
    }();
    return vtable;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
template <ChildKind K>
HRESULT wrap_child(IUnknown* created, DeviceProxy* device, const IID* riid, void** out) {
    if (!created) return E_FAIL;
    ChildProxy* proxy = new (std::nothrow) ChildProxy;
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_child_vtable<K>();
    proxy->real = created;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(created);
    if constexpr (K == ChildKind::queue) {
        proxy->slot_count = kQueuePublicSlots;
    } else if constexpr (K == ChildKind::fence) {
        proxy->slot_count = supports_iid(created, __uuidof(ID3D12Fence1)) ? kFence1PublicSlots : kFencePublicSlots;
    } else {
        proxy->slot_count = kAllocatorPublicSlots;
    }
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->kind = K;
    proxy_add_ref(device);  // the object keeps its Xbox-ABI device alive for GetDevice
    register_live_child(proxy);  // before QueryInterface publishes the output pointer

    const HRESULT hr = child_query_interface<K>(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_child(proxy);  // no reference escaped
        return hr;
    }
    // Titles create fences at run time: the first proxy of each kind is logged, every one only with XS_TRACE.
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy %p wraps %p (device proxy %p, %u public slots)\n",
                     child_kind_name<K>(), static_cast<void*>(proxy), static_cast<void*>(created),
                     static_cast<void*>(device), proxy->slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// ---- Command list proxy ---------------------------------------------------------------------------------
//
// Device slot 12 (CreateCommandList) hands the title a console-shaped command list: the console vtable order, a
// 160-slot table, and the object fields the console SDK's inline writers touch (docs/d3d12_xs_console_abi.md
// Â§5). The write cursor (+0x18) is pinned at both limits (+0x28, +0x30), so a title always finds the record
// buffer full and takes the vtable path for every command â€” the proxy never has to interpret inline records.
// The flag bytes (+0x140..+0x143), the root-parameter header table (+0x344) and the pipeline record (+0x544)
// stay zeroed: they are only read while writing a record, which never happens at the limit.
//
// The console slot order keeps the PC order through slot 26 (measured: 9, 10, 12-17, 19, 20, 25, 26), places
// four console-only methods at 27-30, then the PC chain resumes shifted +4 â€” confirmed at console 32->PC 28
// (SetDescriptorHeaps), 33->29, 41->37, 43->39, 45->41, 48->44, 51->47 and 53->49, and the same shift fills the
// block through console 59 -> PC 55. The console has no PIX markers: console 60 is EndEvent (PC 58) and 61 is
// ExecuteIndirect (PC 59, package-measured). Console 146 is the enhanced Barrier taking one D3D12_BARRIER_GROUP
// (package-confirmed; PC equivalent is GCL7 slot 80). Console-only slots â€” the 27-30 extras, AllocateUploadMemory
// (148), the disputed tail past 61 and the newer-layout +2 tail â€” are recorded with their first argument words
// and return 0 without a PC call: a wrong forward is worse than a clean refusal.

constexpr unsigned kListSlots = 160;
// ---- Upload memory (console list slot 148, AllocateUploadMemory) ----
//
// On the console the CPU and GPU share one address space: the pointer AllocateUploadMemory(size, alignment)
// returns is written by the CPU and handed to the GPU unchanged â€” the package measures it going straight into a
// root SRV (docs/d3d12_xs_console_abi.md Â§5.3, d3d12_command_objects.md Â§3.3a). A PC has two address spaces, so
// each arena chunk is a persistently-mapped UPLOAD-heap buffer: the title gets the CPU pointer and writes the
// very memory the GPU samples, and the root-descriptor setters translate a chunk address to the buffer's device
// address (the same backing memory) before forwarding. Translation covers the six root VA setters (console
// 41-46); VAs inside view structs (IASetVertexBuffers/IASetIndexBuffer) and ExecuteIndirect arguments are not
// rewritten yet and stay title addresses.

struct UploadChunk {
    UploadChunk* next;
    ID3D12Resource* resource;      // the mapped buffer; released with the chunk
    std::uint8_t* cpu_base;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base;
    std::size_t size;
    std::size_t used;
};

struct UploadArena {
    UploadChunk* chunks = nullptr;
    std::mutex lock;
};

constexpr std::size_t kUploadChunkMin = 8 << 20;  // one chunk serves a title's whole frame of uploads

UploadChunk* upload_chunk_new(DeviceProxy* device, std::size_t size) {
    auto* chunk = new (std::nothrow) UploadChunk{};
    if (!chunk) return nullptr;
    chunk->size = size;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    const HRESULT hr = device->real->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    void* cpu = nullptr;
    if (SUCCEEDED(hr) && resource && SUCCEEDED(resource->Map(0, nullptr, &cpu)) && cpu &&
        resource->GetGPUVirtualAddress() != 0) {
        chunk->resource = resource;
        chunk->cpu_base = static_cast<std::uint8_t*>(cpu);
        chunk->gpu_base = resource->GetGPUVirtualAddress();
        return chunk;
    }
    if (resource) resource->Release();
    // A CPU-only block would fabricate a GPU address and lose the title's writes.
    // AllocateUploadMemory must fail when the actual upload resource cannot be mapped.
    delete chunk;
    return nullptr;
}

std::uint64_t upload_alloc(DeviceProxy* device, std::uint64_t size, std::uint64_t alignment) {
    if (!device || !size) return 0;
    alignment = alignment < 16 ? 16 : alignment;  // every measured call asked for 16
    // Refuse what cannot be served before any arithmetic runs: only power-of-two alignments are measured,
    // chunks map at 64K granularity at best, and a size too big to add the alignment to would wrap the
    // checks below â€” one such call returned a pointer and left the cursor so the next request re-handed
    // the same range (daemosofchaos's report on #2490).
    if ((alignment & (alignment - 1)) != 0 || alignment > (64 << 10) ||
        size > ~static_cast<std::size_t>(0) - alignment) {
        std::fprintf(stderr, "[rt] d3d12_xs: AllocateUploadMemory(%llu, %llu) refused\n",
                     static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment));
        return 0;
    }
    auto* arena = static_cast<UploadArena*>(device->upload_state.load(std::memory_order_acquire));
    if (!arena) {
        auto* fresh = new (std::nothrow) UploadArena;
        if (!fresh) return 0;
        void* expected = nullptr;  // a concurrent first call loses its arena but keeps the winner's chunks
        if (device->upload_state.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
            arena = fresh;
        else {
            delete fresh;
            arena = static_cast<UploadArena*>(expected);
        }
    }
    std::lock_guard<std::mutex> lock(arena->lock);
    const std::size_t need = static_cast<std::size_t>(size);
    for (UploadChunk* chunk = arena->chunks; chunk; chunk = chunk->next) {
        const std::size_t aligned = (chunk->used + (alignment - 1)) & ~(static_cast<std::size_t>(alignment) - 1);
        if (aligned >= chunk->used && aligned <= chunk->size && need <= chunk->size - aligned) {
            chunk->used = aligned + need;
            return reinterpret_cast<std::uint64_t>(chunk->cpu_base + aligned);
        }
    }
    const std::size_t chunk_size = need + alignment > kUploadChunkMin ? need + alignment : kUploadChunkMin;
    UploadChunk* chunk = upload_chunk_new(device, chunk_size);
    if (!chunk) {
        std::fprintf(stderr, "[rt] d3d12_xs: AllocateUploadMemory(%llu, %llu) failed\n",
                     static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment));
        return 0;
    }
    chunk->next = arena->chunks;
    arena->chunks = chunk;
    const std::size_t aligned = (alignment - 1) & ~(static_cast<std::size_t>(alignment) - 1);  // 0-based
    chunk->used = aligned + need;
    return reinterpret_cast<std::uint64_t>(chunk->cpu_base + aligned);
}

// An address inside an upload chunk becomes the chunk's device address plus the offset; anything else â€” a
// real resource VA the title already holds â€” passes through.
std::uint64_t upload_translate(DeviceProxy* device, std::uint64_t va) {
    auto* arena = static_cast<UploadArena*>(
        device ? device->upload_state.load(std::memory_order_acquire) : nullptr);
    if (!arena) return va;
    std::lock_guard<std::mutex> lock(arena->lock);
    for (UploadChunk* chunk = arena->chunks; chunk; chunk = chunk->next) {
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(chunk->cpu_base);
        if (va >= base && va < base + chunk->size) return chunk->gpu_base + (va - base);
    }
    return va;
}

void destroy_upload_state(void* upload_state) {
    auto* arena = static_cast<UploadArena*>(upload_state);
    if (!arena) return;
    for (UploadChunk* chunk = arena->chunks; chunk;) {
        UploadChunk* next = chunk->next;
        if (chunk->resource) {
            chunk->resource->Unmap(0, nullptr);
            chunk->resource->Release();
        } else {
            VirtualFree(chunk->cpu_base, 0, MEM_RELEASE);
        }
        delete chunk;
        chunk = next;
    }
    delete arena;
}

constexpr unsigned kListNotForwarded = ~0u;
constexpr unsigned kConsoleSetDescriptorHeaps = 32;
constexpr unsigned kConsoleExecuteBundle = 31;
constexpr unsigned kConsoleClearDepthStencilView = 51;
constexpr unsigned kConsoleBarrier = 146;
constexpr unsigned kConsoleAllocateUpload = 148;

constexpr unsigned console_to_pc_list_slot(unsigned slot) {
    if (slot <= 26) return slot;               // PC order, measured
    if (slot <= 30) return kListNotForwarded;  // console-only extras
    if (slot <= 59) return slot - 4;           // PC chain, shifted +4
    if (slot == 60) return 58;                 // EndEvent (no console SetMarker/BeginEvent)
    if (slot == 61) return 59;                 // ExecuteIndirect (Package)
    if (slot == kConsoleBarrier) return 80;    // D3D12_BARRIER_GROUP (Package; PC GCL7)
    return kListNotForwarded;
}

struct ListProxy {
    void** vtable;                      // +0x00
    ID3D12GraphicsCommandList* real;    // +0x08
    DeviceProxy* device;                // +0x10: keeps the Xbox-ABI device alive; GetDevice returns it
    // +0x18..: the console object body a title's inline writers read. +0x18 holds the write cursor, +0x28 and
    // +0x30 the record limits; the rest is room for the measured flags, header table and pipeline record.
    alignas(8) std::uint8_t console_fields[0x600];
    std::atomic<unsigned long> refs;    // past the console body
    unsigned pc_slot_count;             // slots the real list implements; mapped slots past it are refused
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vb_scratch;  // slot-48 views with their VAs translated
    std::vector<D3D12_STREAM_OUTPUT_BUFFER_VIEW> so_scratch;  // slot-49 views, same treatment
    ListProxy* submit_live_next = nullptr;  // opt-in queue experiment's identity registry; not console object data

    void pin_cursor_at_limit() {
        const std::uint64_t at_limit = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(this) + 0x18 + 0x500);
        for (const size_t offset : {0x18, 0x28, 0x30})
            std::memcpy(reinterpret_cast<std::uint8_t*>(this) + offset, &at_limit, sizeof(at_limit));
    }
};
static_assert(offsetof(ListProxy, console_fields) == 0x18, "the console fields start at +0x18");

// Only the opt-in submit experiment needs these pins. Match allocation addresses before reading guest-supplied
// objects; a copied list vtable is not enough to establish identity. Never resurrect a reference count of zero.
SRWLOCK g_submit_list_lock = SRWLOCK_INIT;
ListProxy* g_submit_lists[kLiveChildBuckets]{};

void register_submit_list(ListProxy* list) {
    AcquireSRWLockExclusive(&g_submit_list_lock);
    auto& head = g_submit_lists[live_child_bucket(list)];
    list->submit_live_next = head;
    head = list;
    ReleaseSRWLockExclusive(&g_submit_list_lock);
}

void unregister_submit_list(ListProxy* list) {
    AcquireSRWLockExclusive(&g_submit_list_lock);
    for (auto** at = &g_submit_lists[live_child_bucket(list)]; *at; at = &(*at)->submit_live_next) {
        if (*at != list) continue;
        *at = list->submit_live_next;
        list->submit_live_next = nullptr;
        break;
    }
    ReleaseSRWLockExclusive(&g_submit_list_lock);
}

ListProxy* pin_submit_list(const void* candidate) {
    ListProxy* pinned = nullptr;
    AcquireSRWLockShared(&g_submit_list_lock);
    for (auto* list = g_submit_lists[live_child_bucket(candidate)]; list; list = list->submit_live_next) {
        if (list != candidate) continue;
        auto refs = list->refs.load(std::memory_order_relaxed);
        while (refs && refs != (std::numeric_limits<unsigned long>::max)()) {
            if (list->refs.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed)) {
                pinned = list;
                break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_submit_list_lock);
    return pinned;
}

struct ListStats {
    static inline std::atomic<std::uint64_t> hits[kListSlots];
    static inline std::atomic<bool> first[kListSlots];
};

// The list tail (D3D-445). From CopyTextureRegionX on, the newer console layout sits two slots later than the older
// one (docs/d3d12_xs_console_abi.md 5.3). The table is keyed by the older-layout slot. Only CopyTextureRegionX has a
// measured name; the other measured pairs are named by their older slot, so that both layouts print the same text
// for the same method.
struct ListTailName {
    unsigned older_slot;
    const char* name;
};
constexpr ListTailName kListTailNames[] = {
    {83, "CopyTextureRegionX"},         {84, "tail method, older slot 84"}, {85, "tail method, older slot 85"},
    {88, "tail method, older slot 88"}, {89, "tail method, older slot 89"}, {90, "tail method, older slot 90"},
    {93, "tail method, older slot 93"}, {94, "tail method, older slot 94"},
};
constexpr unsigned kListTailShift = 2;
static_assert(kConsoleLayoutOlder == ChildProxy::kQueueLayoutOlder &&
                  kConsoleLayoutNewer == ChildProxy::kQueueLayoutNewer,
              "the list tail names use the queue proxy's layout values");

// Null while the layout is unknown, and for a slot that is not a measured tail slot of that layout: a slot number
// never gets the other layout's name.
const char* list_tail_name(unsigned slot, unsigned char console_layout) {
    if (console_layout != kConsoleLayoutOlder && console_layout != kConsoleLayoutNewer) return nullptr;
    const unsigned shift = console_layout == kConsoleLayoutNewer ? kListTailShift : 0;
    if (slot < shift) return nullptr;
    for (const ListTailName& entry : kListTailNames)
        if (entry.older_slot == slot - shift) return entry.name;
    return nullptr;
}

void note_list_slot(unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                    unsigned char console_layout = 0) {
    if (slot >= kListSlots) return;
    ListStats::hits[slot].fetch_add(1, std::memory_order_relaxed);
    bool expected = false;
    if (ListStats::first[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        const char* name = list_tail_name(slot, console_layout);
        std::fprintf(stderr, "[rt] d3d12_xs: list slot %u%s%s%s first call (args 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                     name ? " (" : "", name ? name : "", name ? ")" : "", static_cast<unsigned long long>(a1),
                     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                     static_cast<unsigned long long>(a4));
    }
}

// The interfaces the console list answers: the base chain, ID3D12CommandList and every graphics-command-list
// interface the real list implements â€” on the console the list is a single object, so any of them returns the
// proxy.
bool is_list_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList8));
}

unsigned long list_add_ref(ListProxy* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

void destroy_list(ListProxy* self) {
    if (immediate_queue_submit_enabled()) unregister_submit_list(self);
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

unsigned long list_release(ListProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_list(self);
    return refs;
}

HRESULT list_query_interface(ListProxy* self, const IID* riid, void** out) {
    note_list_slot(0, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0, 0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // A console title asks for the console command-list IID (device slot 73 forwards it): it maps to a PC IID the
    // real list answers, and the caller gets this console-shaped object back.
    const IID& pc_iid = IsEqualIID(*riid, kCommandListIidPairs[0].console)
                            ? __uuidof(ID3D12GraphicsCommandList) : command_list_iid_for_pc(*riid);
    if (!is_list_iid(pc_iid)) {
        if (supports_iid(self->real, pc_iid)) log_refused_iid("list", *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, pc_iid)) return E_NOINTERFACE;
    *out = self;
    list_add_ref(self);
    return S_OK;
}

// Highest list interface the real object implements; its cumulative slot count bounds mapped forwards
// (ID3D12GraphicsCommandList8 ends at slot 81, OMSetFrontAndBackStencilRef).
unsigned detect_list_slot_count(IUnknown* real) {
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList8))) return 82;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList7))) return 81;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList6))) return 78;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList5))) return 75;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList4))) return 69;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList3))) return 67;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList2))) return 66;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList1))) return 65;
    return 60;
}

template <unsigned Slot>
std::uint64_t WINAPI list_slot(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(list_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return list_add_ref(self);
    } else if constexpr (Slot == 2) {
        return list_release(self);
    } else if constexpr (Slot == 7) {  // GetDevice: the Xbox-ABI device, as on the other proxies
        note_list_slot(7, a1, a2, 0, 0);
        if (a2) *reinterpret_cast<void**>(a2) = nullptr;
        if (!a1) return hr_to_u64(E_POINTER);
        if (!a2) {  // no output: the real object's own answer
            using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
            return hr_to_u64(reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[7])(
                self->real, reinterpret_cast<const IID*>(a1), nullptr));
        }
        return hr_to_u64(proxy_query_interface(self->device, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else {
        note_list_slot(Slot, a1, a2, a3, a4, self->device->console_layout.load(std::memory_order_relaxed));
        if constexpr (Slot == 60 || Slot == 62) {
#ifdef LEONIDA_VULKAN_DEVICE
            // CopyMemoryToMemoryX: old slot 60, shifted by two in newer lists (VIctory list::newer_slot).
            // Only the low DWORD of flags is an argument; bytes are the original console representation.
            const auto layout = self->device->console_layout.load(std::memory_order_relaxed);
            if (self->device->console_lists && (Slot == 62 ? layout == kConsoleLayoutNewer : layout == kConsoleLayoutOlder) &&
                !static_cast<std::uint32_t>(a4)) {
                const HRESULT hr = rt::d3d12::record_console_address_copy(self->device->real, self->real,
                    upload_translate(self->device, a1), upload_translate(self->device, a2), a3);
                d3d12_trace::result("XboxListCopyMemoryToMemoryX", hr);
                if (SUCCEEDED(hr)) return 0;
            }
#endif
        }
        if constexpr (Slot == kConsoleResetX) {
            // Newer ResetX is (allocator, pipeline, flags). The observed zero-flags/null-pipeline shape is also
            // described by VIctory's reset_x at d2a4bae1f49533ef5cb512fd1b1ba1513f2e1989 (GPL-3.0-or-later).
            // No allocator callback or console command-memory behavior is implied by this narrow adapter.
            const auto caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
            const auto refuse = [&] {
                rt::shims::unimplemented_proxy_effect("list", Slot, caller,
                    reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
                return hr_to_u64(E_NOTIMPL);
            };
            if (!self->device || !self->device->console_lists || a2 || a3 ||
                self->device->console_layout.load(std::memory_order_relaxed) == kConsoleLayoutOlder)
                return refuse();
            using AllocatorPin = std::unique_ptr<ChildProxy, decltype(&release_child_reference)>;
            AllocatorPin allocator(pin_live_child<ChildKind::allocator>(reinterpret_cast<const void*>(a1)),
                                    &release_child_reference);
            if (!allocator || allocator->device != self->device) return refuse();
            // Backend Reset may release state from the previous recording. Retain both proxy owners through
            // that call and the successful cursor reset; a matching vtable alone is never allocator identity.
            list_add_ref(self);
            const std::unique_ptr<ListProxy, decltype(&list_release)> list_pin(self, &list_release);
            const HRESULT hr = self->real->Reset(reinterpret_cast<ID3D12CommandAllocator*>(allocator->real), nullptr);
            if (SUCCEEDED(hr)) {
                self->pin_cursor_at_limit();
                // A checked ResetX at 139 identifies the same newer tail layout as the queue's shifted fence calls.
                self->device->console_layout.store(kConsoleLayoutNewer, std::memory_order_relaxed);
            }
            return hr_to_u64(d3d12_trace::result("XboxList139ResetX", hr));
        }
        if constexpr (Slot == 71) {
            // Newer slot 71 is old slot 69: a bottom-of-pipe 32-bit write (address,value,flush,flags).
            // The measured call uses zero modifiers. Flags occupy only a DWORD in the fifth argument slot;
            // its upper stack padding is not an argument. Older slot 71 has a different contract.
            const auto caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
            if (self->device && self->device->console_lists &&
                self->device->console_layout.load(std::memory_order_relaxed) == kConsoleLayoutNewer &&
                static_cast<std::uint32_t>(a3) == 0 && static_cast<std::uint32_t>(a4) == 0 &&
                a1 && a1 % 4 == 0 && a1 <= UINT64_MAX - 4) {
#ifdef LEONIDA_VULKAN_DEVICE
                list_add_ref(self);
                const std::unique_ptr<ListProxy, decltype(&list_release)> list_pin(self, &list_release);
                const HRESULT hr = rt::d3d12::record_console_marker(self->device->real, self->real, a1,
                                                                    static_cast<std::uint32_t>(a2));
                d3d12_trace::result("XboxList71BottomOfPipeWrite32", hr);
                if (SUCCEEDED(hr)) return 0;
#endif
            }
            rt::shims::unimplemented_proxy_effect("list", Slot, caller, reinterpret_cast<std::uint64_t>(self),
                                                   a1, a2, a3);
            return 0;
        }
        if constexpr (Slot == 72) {
            // Microsoft's public PerformanceTimersXbox sample uses Write64BitValueBottomOfPipeX with value 0,
            // FLUSH_NONE and GPU_TIMESTAMP. The measured newer-slot call supplies that flag as DWORD 1.
            // Other modifiers and literal/system-clock writes need separate semantics.
            // https://github.com/microsoft/Xbox-GDK-Samples/blob/b1e77b6ac10f95b07296025fda171ca2ddc42dbe/Kits/ATGTK/PerformanceTimersXbox.cpp
            const auto caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
            if (self->device && self->device->console_lists &&
                self->device->console_layout.load(std::memory_order_relaxed) == kConsoleLayoutNewer &&
                a2 == 0 && static_cast<std::uint32_t>(a3) == 0 && static_cast<std::uint32_t>(a4) == 1 &&
                a1 && a1 % 8 == 0 && a1 <= UINT64_MAX - 8) {
#ifdef LEONIDA_VULKAN_DEVICE
                list_add_ref(self);
                const std::unique_ptr<ListProxy, decltype(&list_release)> list_pin(self, &list_release);
                const HRESULT hr = rt::d3d12::record_console_timestamp(self->device->real, self->real, a1);
                d3d12_trace::result("XboxList72GpuTimestamp", hr);
                if (SUCCEEDED(hr)) return 0;
#endif
            }
            rt::shims::unimplemented_proxy_effect("list", Slot, caller, reinterpret_cast<std::uint64_t>(self),
                                                   a1, a2, a3);
            return 0;
        }
        if constexpr (Slot == 74) {
            const auto report_wait = [&](const char* status) {
                std::uint64_t observed = 0;
                SIZE_T copied = 0;
                const bool readable = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a1),
                    &observed, sizeof(observed), &copied) && copied == sizeof(observed);
                std::fprintf(stderr, "[rt] d3d12_xs: list74 memory wait address=0x%llx comparison=%u "
                                     "reference=%llu flags32=0x%x readable=%u observed=%llu %s\n",
                    a1, static_cast<unsigned>(static_cast<std::uint32_t>(a2)), a3,
                    static_cast<unsigned>(static_cast<std::uint32_t>(a4)), unsigned(readable), observed, status);
            };
#ifdef LEONIDA_VULKAN_DEVICE
            if (satisfied_host_generation_enabled() && self->device && self->device->console_lists &&
                self->device->console_layout.load(std::memory_order_relaxed) == kConsoleLayoutNewer &&
                a1 && a1 % 8 == 0 && a1 <= UINT64_MAX - 8 && static_cast<std::uint32_t>(a2) == 7 &&
                a3 == 1 && static_cast<std::uint32_t>(a4) == 1) {
                list_add_ref(self);
                const std::unique_ptr<ListProxy, decltype(&list_release)> list_pin(self, &list_release);
                const HRESULT hr = rt::d3d12::record_console_memory_acquire(self->device->real, self->real, a1, a3);
                d3d12_trace::result("XboxList74SatisfiedStartupHostAcquire", hr);
                if (SUCCEEDED(hr)) {
                    static std::atomic<unsigned> waits{0};
                    if (waits.fetch_add(1, std::memory_order_relaxed) < 16) report_wait("recorded");
                    return 0;
                }
            }
#endif
            report_wait("refused");
        }
#ifdef LEONIDA_VULKAN_DEVICE
        if constexpr (Slot == 75) {
            // Zero selects ordinary pixel-shader depth ordering. Translated fragment shaders use Vulkan's
            // default depth-test order. The nonzero forced order requires a different fragment pipeline.
            if (self->device && self->device->console_lists &&
                self->device->console_layout.load(std::memory_order_relaxed) == kConsoleLayoutNewer &&
                static_cast<std::uint32_t>(a1) == 0 && rt::d3d12::can_record_graphics_state(self->real)) {
                d3d12_trace::record("XboxList75DefaultPixelShaderDepthOrder", "called");
                return 0;
            }
        }
        if constexpr (Slot == 126) {
            if (self->device && self->device->console_lists &&
                self->device->console_layout.load(std::memory_order_relaxed) == kConsoleLayoutNewer) {
                // RSSetShadingRate takes a base rate and two optional combiners. 1x1 with both pass-through
                // is exactly the fixed fragment size the Vulkan pipelines already use.
                std::uint32_t combiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT]{};
                SIZE_T copied = 0;
                const bool readable = !a2 || (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a2),
                    combiners, sizeof(combiners), &copied) && copied == sizeof(combiners));
                const auto rate = static_cast<std::uint32_t>(a1);
                if (readable && rate == D3D12_SHADING_RATE_1X1 &&
                    combiners[0] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH &&
                    combiners[1] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH &&
                    rt::d3d12::can_record_graphics_state(self->real)) {
                    d3d12_trace::record("XboxList126ShadingRate1x1", "called");
                    return 0;
                }
                std::fprintf(stderr, "[rt] d3d12_xs: shading rate refused rate=%u readable=%u combiners=%u/%u\n",
                    rate, unsigned(readable), combiners[0], combiners[1]);
            }
        }
        if constexpr (Slot == 142) {
            // The title skips reapplying the all-ones value when a new pipeline is bound. Retain the bound
            // pipeline's native state for that sentinel. A non-default override still requires its own model.
            if (self->device && self->device->console_lists &&
                self->device->console_layout.load(std::memory_order_relaxed) == kConsoleLayoutNewer &&
                a1 == UINT64_MAX && rt::d3d12::can_record_graphics_state(self->real)) {
                d3d12_trace::record("XboxList142PipelineDefault", "called");
                return 0;
            }
        }
#endif
        constexpr unsigned pc = console_to_pc_list_slot(Slot);
        if constexpr (pc == kListNotForwarded) {
            if constexpr (Slot == kConsoleAllocateUpload) {
                return upload_alloc(self->device, a1, a2);  // (size, alignment); the comment at UploadChunk
            }
            if constexpr (Slot == 131 || Slot == 132) {
                if (try_record_conservative_cache_sync(self->real, Slot, a1, a2, a3, a4)) {
                    d3d12_trace::record(Slot == 131 ? "XboxList131CacheSync" : "XboxList132CacheSync",
                                         "called");
                    if (d3d12_trace::enabled() || trace_enabled())
                        std::fprintf(stderr,
                            "[rt] d3d12_xs: experimental list %u caller %p args 0x%llx 0x%llx 0x%llx 0x%llx\n",
                            Slot, _ReturnAddress(), static_cast<unsigned long long>(a1),
                            static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                            static_cast<unsigned long long>(a4));
                    return 0;
                }
            }
            rt::shims::unimplemented_proxy_effect("list", Slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
                reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
            return 0;  // unmodelled console slot: recorded above, never a PC call
        } else {
            if (pc >= self->pc_slot_count) {
                static std::atomic<bool> beyond_warned[kListSlots];
                bool expected = false;
                if (beyond_warned[Slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: list slot %u maps to PC slot %u, beyond the real list's %u "
                                 "slots; returning E_NOTIMPL\n",
                                 Slot, pc, self->pc_slot_count);
                return hr_to_u64(E_NOTIMPL);
            }
            using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t);
            auto** const real_vtable = *reinterpret_cast<void***>(self->real);
            if constexpr (Slot == 10) {
                const auto result = reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, unwrap_object(a1), a2,
                    a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                if (SUCCEEDED(static_cast<HRESULT>(result))) self->pin_cursor_at_limit();
                return result;
            } else if constexpr (Slot == kConsoleExecuteBundle) {
                a1 = unwrap_object(a1);  // the bundle is a list proxy too
            } else if constexpr (Slot >= 41 && Slot <= 46) {
                // The six root descriptor setters (index, GPU VA): an upload-arena CPU pointer becomes the
                // chunk's device address (d3d12_xs_console_abi.md Â§5.3 measures slots 41/43/45 for the compute
                // side; the graphics setters interleave on even slots).
                a2 = upload_translate(self->device, a2);
            } else if constexpr (Slot == 47) {
                // IASetIndexBuffer(pView): the view's BufferLocation is a GPU VA; translate in a copy so the
                // title's struct is not written.
                if (a1) {
                    auto view = *reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a1);
                    view.BufferLocation = upload_translate(self->device, view.BufferLocation);
                    return reinterpret_cast<ForwardFn>(real_vtable[pc])(
                        self->real, reinterpret_cast<std::uint64_t>(&view), a2, a3, a4, a5, a6, a7, a8, a9,
                        a10, a11, a12, a13, a14);
                }
            } else if constexpr (Slot == 48) {
                // IASetVertexBuffers(start, count, views): each view's BufferLocation is a GPU VA. The views are
                // translated in the proxy's scratch array for the same reason as the index view.
                if (a3 && a2) {
                    const auto* in = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(a3);
                    self->vb_scratch.assign(in, in + a2);
                    for (auto& view : self->vb_scratch)
                        view.BufferLocation = upload_translate(self->device, view.BufferLocation);
                    return reinterpret_cast<ForwardFn>(real_vtable[pc])(
                        self->real, a1, a2, reinterpret_cast<std::uint64_t>(self->vb_scratch.data()), a4, a5,
                        a6, a7, a8, a9, a10, a11, a12, a13, a14);
                }
            } else if constexpr (Slot == 49) {
                // SOSetTargets(start, count, views): BufferLocation and BufferFilledSizeLocation are both VAs.
                if (a3 && a2) {
                    const auto* in = reinterpret_cast<const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>(a3);
                    self->so_scratch.assign(in, in + a2);
                    for (auto& view : self->so_scratch) {
                        view.BufferLocation = upload_translate(self->device, view.BufferLocation);
                        view.BufferFilledSizeLocation =
                            upload_translate(self->device, view.BufferFilledSizeLocation);
                    }
                    return reinterpret_cast<ForwardFn>(real_vtable[pc])(
                        self->real, a1, a2, reinterpret_cast<std::uint64_t>(self->so_scratch.data()), a4, a5,
                        a6, a7, a8, a9, a10, a11, a12, a13, a14);
                }
            } else if constexpr (Slot == kConsoleSetDescriptorHeaps) {
                UnwrappedArray heaps;
                if (!heaps.fill(a2, a1)) return 0;  // void on the PC side; the call simply does not happen
                const std::uint64_t real_heaps = heaps.data() ? heaps.data() : a2;
                return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, a1, real_heaps, a3, a4, a5, a6,
                                                                  a7, a8, a9, a10, a11, a12, a13, a14);
            } else if constexpr (pc == 46) {
                // OMSetRenderTargets reads CPU handles from arrays. Translate copies,
                // including the optional depth handle, without changing the title's arrays.
                D3D12_CPU_DESCRIPTOR_HANDLE targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
                D3D12_CPU_DESCRIPTOR_HANDLE depth{};
                const UINT count = static_cast<UINT>(a1);
                const UINT supplied = a3 ? 1u : count;
                if (a2 && count && supplied <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) {
                    const auto* input = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a2);
                    for (UINT i = 0; i < supplied; ++i) targets[i] = translate_cpu_handle(self->device, input[i]);
                    a2 = reinterpret_cast<std::uint64_t>(targets);
                }
                if (a4) {
                    depth = translate_cpu_handle(self->device, *reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a4));
                    a4 = reinterpret_cast<std::uint64_t>(&depth);
                }
                return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, a1, a2, a3, a4, a5, a6, a7, a8,
                                                                  a9, a10, a11, a12, a13, a14);
            } else if constexpr (pc == 48) {
                // ClearRenderTargetView takes the CPU handle by value. Depth clear has a typed thunk below.
                a1 = translate_cpu_handle(self->device, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a1)}).ptr;
            } else if constexpr (pc == 49 || pc == 50) {
                // ClearUnorderedAccessViewUint / Float: the GPU handle stays as supplied.
                a2 = translate_cpu_handle(self->device, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)}).ptr;
            }
            return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9,
                                                              a10, a11, a12, a13, a14);
        }
    }
}

// Adapted from PR #7880, commit 0aba6c361: Depth is the fourth argument including `this`, so the x64 ABI
// carries it in XMM3 rather than R9. The generic integer thunk cannot forward it across note_list_slot's calls.
void WINAPI list_clear_depth_stencil_view(ListProxy* self, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                          D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT num_rects,
                                          const D3D12_RECT* rects) {
    std::uint32_t depth_bits = 0;
    std::memcpy(&depth_bits, &depth, sizeof(depth_bits));
    note_list_slot(kConsoleClearDepthStencilView, dsv.ptr, flags, depth_bits, stencil,
                   self->device->console_layout.load(std::memory_order_relaxed));
    constexpr unsigned pc = console_to_pc_list_slot(kConsoleClearDepthStencilView);
    static_assert(pc == 47);
    if (pc >= self->pc_slot_count) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: depth-clear PC slot %u is beyond the real list's %u slots\n",
                         pc, self->pc_slot_count);
        return;
    }
    self->real->ClearDepthStencilView(translate_cpu_handle(self->device, dsv), flags, depth, stencil,
                                       num_rects, rects);
}

// The newer list takes these floats in XMM1/XMM2, outside the generic integer forwarding ABI.
void WINAPI list_depth_bounds(ListProxy* self, FLOAT lower, FLOAT upper) {
    constexpr unsigned slot = 70;
    static_assert(console_to_pc_list_slot(slot) == kListNotForwarded);
    const auto layout = self->device->console_layout.load(std::memory_order_relaxed);
    std::uint32_t low_bits = 0, high_bits = 0;
    std::memcpy(&low_bits, &lower, sizeof(low_bits));
    std::memcpy(&high_bits, &upper, sizeof(high_bits));
    note_list_slot(slot, low_bits, high_bits, 0, 0, layout);
#ifdef LEONIDA_VULKAN_DEVICE
    if (layout == kConsoleLayoutNewer && self->device->console_lists && lower == 0.0f && upper == 1.0f &&
        rt::d3d12::can_record_graphics_state(self->real)) {
        // The full interval clips nothing, including on the Vulkan device which reports no depth-bounds support.
        d3d12_trace::record("XboxList70FullDepthBounds", "called");
        return;
    }
#endif
    rt::shims::unimplemented_proxy_effect("list", slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
        reinterpret_cast<std::uint64_t>(self), low_bits, high_bits, 0);
}

template <std::size_t... I>
void fill_list_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&list_slot<static_cast<unsigned>(I)>) ), ...);
    vtable[kConsoleClearDepthStencilView] = reinterpret_cast<void*>(&list_clear_depth_stencil_view);
    vtable[70] = reinterpret_cast<void*>(&list_depth_bounds);
}

void** get_list_vtable() {
    static void** vtable = [] {
        static void* storage[kListSlots];
        fill_list_vtable(storage, std::make_index_sequence<kListSlots>{});
        return storage;
    }();
    return vtable;
}

bool is_list_proxy_impl(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

IUnknown* list_real_or_null(const void* object) {
    return is_list_proxy_impl(object) ? static_cast<IUnknown*>(static_cast<const ListProxy*>(object)->real)
                                    : nullptr;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
HRESULT wrap_list(IUnknown* created, DeviceProxy* device, const IID* riid, void** out) {
    if (!created) return E_FAIL;
    ListProxy* proxy = new (std::nothrow) ListProxy();  // (): the console body starts zeroed
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_list_vtable();
    proxy->real = static_cast<ID3D12GraphicsCommandList*>(created);
    proxy->device = device;
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->pc_slot_count = detect_list_slot_count(created);
    proxy->pin_cursor_at_limit();
    proxy_add_ref(device);  // the list keeps its Xbox-ABI device alive for GetDevice
    if (immediate_queue_submit_enabled()) register_submit_list(proxy);

    const HRESULT hr = list_query_interface(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_list(proxy);  // no reference escaped
        return hr;
    }
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: list proxy %p wraps %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(created), proxy->pc_slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// Keep SEH in a leaf without C++ objects needing unwinding. No guest source array is revisited after this copy.
bool snapshot_submit_lists(std::uint64_t source, ID3D12CommandList** destination, UINT count) noexcept {
#ifdef _MSC_VER
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(source), count * sizeof(*destination));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    (void)source;
    (void)destination;
    (void)count;
    return false;  // no fault-contained reader on this compiler: retain the strict unsupported path
#endif
}

bool try_immediate_queue_submit(ChildProxy* self, std::uint64_t count, std::uint64_t lists,
                                std::uint64_t token, std::uint64_t raw_flags, std::uint64_t caller) {
    const auto flags = static_cast<std::uint32_t>(raw_flags);  // the measured caller stores a DWORD stack argument
    if (!immediate_queue_submit_enabled() || !self || !self->device || !self->device->console_lists ||
        !count || count > 256 || !lists || (flags != 0 && flags != 3))
        return false;
    // Validate against the very pacer that issued the device's WaitFrameEventX tokens and that PresentX uses.
    // This does not establish the experimental adapter's remaining console scheduling/flag semantics.
    if (!self->device->frame_pacer.issued_token(token)) return false;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(self->real->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))) || !queue) return false;
    const auto type = queue->GetDesc().Type;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;

    std::array<ID3D12CommandList*, 256> incoming{};
    if (!snapshot_submit_lists(lists, incoming.data(), static_cast<UINT>(count))) return false;
    struct PinnedLists {
        std::array<ListProxy*, 256> proxies{};
        std::array<Microsoft::WRL::ComPtr<ID3D12CommandList>, 256> real;
        ~PinnedLists() {
            for (auto* proxy : proxies) if (proxy) list_release(proxy);
        }
    } pinned;
    std::array<ID3D12CommandList*, 256> unwrapped{};
    for (UINT i = 0; i < count; ++i) {
        auto* list = pin_submit_list(incoming[i]);
        if (!list) return false;
        pinned.proxies[i] = list;
        if (list->device != self->device ||
            FAILED(list->real->QueryInterface(IID_PPV_ARGS(pinned.real[i].GetAddressOf()))) || !pinned.real[i] ||
            pinned.real[i]->GetType() != type)
            return false;
        unwrapped[i] = pinned.real[i].Get();
    }

    // This is a real PC queue call, after the complete array has been snapshotted, validated and retained.
    // The backend's normal closed-list/recording validation and later Signal ordering remain in force.
    queue->ExecuteCommandLists(static_cast<UINT>(count), unwrapped.data());
    d3d12_trace::record("XboxQueue35ImmediateSubmit", "called");
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: EXPERIMENTAL queue35 immediate PC submission; token/flag pacing "
                             "UNVERIFIED (LEONIDA_XBOX_QUEUE_SUBMIT=immediate)\n");
    if (d3d12_trace::enabled() || trace_enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: experimental queue35 caller %p count %llu token 0x%llx "
                             "flags 0x%x raw_flags 0x%llx; real PC ExecuteCommandLists called\n",
                     reinterpret_cast<void*>(caller), static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(token), flags, static_cast<unsigned long long>(raw_flags));
    return true;  // call issued, not proof of GPU completion or of the console scheduling/return contract
}

// Device slot 12: CreateCommandList(node_mask, type, allocator, initial_pso, riid, out). The title hands its
// allocator proxy, which the real device unwraps to the real allocator; the created list goes back console-shaped.
HRESULT proxy_create_command_list(DeviceProxy* self, std::uint64_t node_mask, std::uint64_t type,
                                  std::uint64_t allocator, std::uint64_t pso, const IID* riid, void** out) {
    note_slot(12);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    if (!out) return self->real->CreateCommandList(static_cast<UINT>(node_mask),
                                                 static_cast<D3D12_COMMAND_LIST_TYPE>(type),
                                                 reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator)),
                                                 reinterpret_cast<ID3D12PipelineState*>(pso), *riid, nullptr);
    if (!self->console_lists) {
        // PC caller (create_device_proxy's console_lists=false): the real list answers every PC slot, and the
        // console-only pieces (slot 148, the shifted tail) are simply not there for this caller to reach.
        return self->real->CreateCommandList(static_cast<UINT>(node_mask),
                                             static_cast<D3D12_COMMAND_LIST_TYPE>(type),
                                             reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator)),
                                             reinterpret_cast<ID3D12PipelineState*>(pso), *riid, out);
    }
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandList(
        static_cast<UINT>(node_mask), static_cast<D3D12_COMMAND_LIST_TYPE>(type),
        reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator)),
        reinterpret_cast<ID3D12PipelineState*>(pso), __uuidof(ID3D12GraphicsCommandList), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    return wrap_list(static_cast<IUnknown*>(created), self, riid, out);
}

// Calls the real device's slot `Slot` with these five words in the positions the thunk received them (see "A thunk
// declares 14 argument words" at the top: no word may move to another stack position on the way).
template <unsigned Slot>
std::uint64_t forward_device(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                             std::uint64_t a4, std::uint64_t a5) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t);
    return reinterpret_cast<Fn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5);
}

// A device slot past the PC device's vtable (the real device predates the interface): E_NOTIMPL, logged once per
// slot. The generic thunk and the slots the proxy handles itself (31/34/35/45/46/50, 75) log the same line.
HRESULT refuse_beyond_device(unsigned slot, unsigned slot_count) {
    bool expected = false;
    if (slot < kMaxSlots && g_beyond_warned[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) is beyond the PC device vtable (%u slots); returning "
                     "E_NOTIMPL\n",
                     slot, slot_name(slot), slot_count);
    return E_NOTIMPL;
}

// Device slot 8: CreateCommandQueue(desc, riid, out). Without an output it is the device's own capability answer
// (S_FALSE for a valid description on the PC), as #558 does for slot 14.
HRESULT proxy_create_command_queue(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid,
                                   void** out) {
    note_slot(8);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    if (!riid) return E_POINTER;
    // The caller's null output goes to the real device on purpose: its answer (S_FALSE, E_UNEXPECTED, ...) is the
    // one to give, although the SDK annotates the parameter as required.
#pragma warning(suppress : 6387)
    if (!out) return self->real->CreateCommandQueue(desc, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Xbox device slot 78: the observed low-DWORD flags are zero. Timing is registered before a presenter exists.
HRESULT proxy_set_frame_interval_x(DeviceProxy* self, std::uint64_t output, std::uint64_t interval_us,
                                   std::uint64_t latency, std::uint64_t flags) {
    HRESULT hr = S_OK;
    if (call_presentation(self, [&](const PresentationHandler& p) {
            if (!p.set_frame_interval) return false;
            hr = p.set_frame_interval(p.context, reinterpret_cast<IUnknown*>(output), static_cast<UINT>(interval_us),
                                      static_cast<UINT>(latency), static_cast<UINT>(flags));
            return true;
        }))
        return hr;
    (void)output;
    const HRESULT result = interval_us > UINT32_MAX || latency > UINT32_MAX || static_cast<std::uint32_t>(flags)
        ? E_INVALIDARG : HRESULT(self->frame_pacer.set_frame_interval(static_cast<std::uint32_t>(interval_us),
                                                                     static_cast<std::uint32_t>(latency), 0));
    static std::atomic<unsigned> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr, "[rt] d3d12_xs: shared frame interval device=%p period=%llu latency=%llu flags=0x%x hr=0x%08x\n",
            static_cast<void*>(self), interval_us, latency, static_cast<unsigned>(flags), static_cast<unsigned>(result));
    return result;
}

// Xbox device slot 79: observed origin and kind-2 schedules use null ancillary data and zero low-DWORD flags.
HRESULT proxy_schedule_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t offset_us,
                                     std::uint64_t flags) {
    HRESULT hr = S_OK;
    if (call_presentation(self, [&](const PresentationHandler& p) {
            if (!p.schedule_frame_event) return false;
            hr = p.schedule_frame_event(p.context, static_cast<UINT>(event), static_cast<UINT>(offset_us),
                                        static_cast<UINT>(flags));
            return true;
        }))
        return hr;
    const HRESULT result = event > UINT32_MAX || offset_us > UINT32_MAX || static_cast<std::uint32_t>(flags)
        ? E_INVALIDARG : HRESULT(self->frame_pacer.schedule_frame_event(static_cast<std::uint32_t>(event),
                                                                       static_cast<std::uint32_t>(offset_us), 0));
    static std::atomic<unsigned> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr, "[rt] d3d12_xs: shared frame schedule device=%p event=%llu offset=%llu flags=0x%x hr=0x%08x\n",
            static_cast<void*>(self), event, offset_us, static_cast<unsigned>(flags), static_cast<unsigned>(result));
    return result;
}

bool write_frame_token(std::uint64_t* destination, std::uint64_t value) {
    __try {
        *destination = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Xbox device slot 80: observed timeout=0/flags=2 uses the same pacer as PresentX. Flag-2 timeout processing is
// peer-derived (TomBox f89e4106 frame.rs), not claimed as an official console definition. Its real result is S_FALSE.
HRESULT proxy_wait_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t timeout_ms,
                                 std::uint64_t flags, std::uint64_t token_word) {
    auto* token = reinterpret_cast<std::uint64_t*>(token_word);
    HRESULT hr = S_OK;
    if (call_presentation(self, [&](const PresentationHandler& p) {
            if (!p.wait_frame_event) return false;
            hr = p.wait_frame_event(p.context, static_cast<UINT>(event), static_cast<DWORD>(timeout_ms),
                                    static_cast<UINT>(flags), token);
            return true;
        }))
        return hr;
    if (!token && event == leonida::frame_event_origin) return E_POINTER;
    if (event > UINT32_MAX || timeout_ms > UINT32_MAX) return E_INVALIDARG;
    std::uint64_t value = 0;
    // pToken is treated as output: the peer's read of its previous value does not establish an Xbox INOUT contract.
    const HRESULT result = HRESULT(self->frame_pacer.wait_frame_event(static_cast<std::uint32_t>(event),
        static_cast<std::uint32_t>(timeout_ms), static_cast<std::uint32_t>(flags), &value));
    // Only origin waits produce a frame token. Kind-2 waits pace that frame and must preserve its existing token.
    if (SUCCEEDED(result) && event == leonida::frame_event_origin && !write_frame_token(token, value)) return E_POINTER;
    static std::atomic<unsigned> waits{0};
    if (waits.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr, "[rt] d3d12_xs: shared frame wait event=%u timeout=%u flags=0x%x hr=0x%08x token=%llu\n",
            static_cast<unsigned>(event), static_cast<unsigned>(timeout_ms), static_cast<unsigned>(flags),
            static_cast<unsigned>(result), static_cast<unsigned long long>(
                event == leonida::frame_event_origin ? value : self->frame_pacer.last_issued_token()));
    return result;
}

// Xbox device slot 73 (public HLE): CreateCommandListX(desc, allocator, initial_state, riid, out). The console call is
// not the PC six-argument form: the type is the first word of D3D12XBOX_COMMAND_LIST_DESC and there is no node mask
// (the HLE passes 0). The second desc word is logged, not interpreted: its public layout is not established.
HRESULT proxy_create_command_list_x(DeviceProxy* self, std::uint64_t desc_word, std::uint64_t allocator_word,
                                    std::uint64_t initial_state_word, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc_word || !allocator_word) return E_INVALIDARG;
    if (!riid) return E_POINTER;
    if (!out) return E_POINTER;

    std::uint32_t words[2] = {};
    std::memcpy(words, reinterpret_cast<const void*>(desc_word), sizeof(words));
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY)
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // as the public HLE does

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 73 CreateCommandListX desc words 0x%08x 0x%08x -> node mask 0, type %d\n",
                     words[0], words[1], static_cast<int>(type));

    // Leonida wraps its Vulkan allocators; a PC allocator comes back unwrapped and is passed as it is. Pipeline states
    // are not wrapped by this proxy, so initial_state is already the real object (a proxy there is passed through too).
    auto* allocator = allocator_proxy_real_allocator(reinterpret_cast<void*>(allocator_word));
    if (!allocator) allocator = reinterpret_cast<ID3D12CommandAllocator*>(allocator_word);
    auto* initial_state = reinterpret_cast<ID3D12PipelineState*>(initial_state_word);
    void* created = nullptr;
    const IID& create_iid = IsEqualIID(*riid, kCommandListIidPairs[0].console)
                                ? __uuidof(ID3D12GraphicsCommandList) : *riid;
    const HRESULT hr = self->real->CreateCommandList(0, type, allocator, initial_state,
                                                     create_iid, &created);
    if (FAILED(hr)) return hr;
    return wrap_list(static_cast<IUnknown*>(created), self, riid, out);
}

// Device slot 48, measured on the retail title: (desc, riid, out) with riid = IID_ID3D12CommandQueue. On the PC the
// slot is OpenExistingHeapFromAddress, so forwarding it by number fails. desc is a PC D3D12_COMMAND_QUEUE_DESC
// (16 bytes) followed by four more 32-bit Xbox words of unknown meaning, logged once. Only the PC prefix is used and
// only the flag the PC knows is kept; the rest is the slot 8 path.
HRESULT proxy_create_command_queue_xbox(DeviceProxy* self, const std::uint32_t* desc, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc) return E_INVALIDARG;
    static_assert(sizeof(D3D12_COMMAND_QUEUE_DESC) == 16, "the PC prefix of the Xbox description is 16 bytes");
    D3D12_COMMAND_QUEUE_DESC pc{};
    std::memcpy(&pc, desc, sizeof(pc));
    pc.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 48 queue: flags=0x%x extra words=0x%x 0x%x 0x%x 0x%x\n",
                     static_cast<unsigned>(desc[2]), desc[4], desc[5], desc[6], desc[7]);
    return proxy_create_command_queue(self, &pc, riid, out);
}

// Xbox device slot 70: (texture_words, view_desc, destination), no return value. The retail executable's SRV writer
// makes a view from raw descriptions and a GPU address with no resource object (docs/retail_boot_chain.md, stop 14,
// measured by call shape only). The PC slot 70 is ID3D12Device8::CreatePlacedResource1, which would take the
// destination handle for a resource description and write through it. The layout of the raw words is not measured,
// so nothing reads them: the destination gets the PC's null shader-resource view, which a shader reads as zeros, and
// the call returns. A view that reaches the GPU address needs that layout.
void proxy_create_srv_from_raw_x(DeviceProxy* self, std::uint64_t texture_words, std::uint64_t view_desc,
                                 std::uint64_t destination) {
    note_slot(70);
    log_xbox_route_once(70, "CreateShaderResourceView from raw words (null view)", texture_words, view_desc,
                        destination, 0, 0);
    if (!destination) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC null_view{};
    null_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_view.Texture2D.MipLevels = 1;
    self->real->CreateShaderResourceView(nullptr, &null_view, D3D12_CPU_DESCRIPTOR_HANDLE{destination});
}

// Device slot 75: ID3D12Device9::CreateCommandQueue1(desc, creator_id, riid, out), called by slot index.
HRESULT proxy_create_command_queue1(DeviceProxy* self, std::uint64_t desc, std::uint64_t creator_id,
                                    const IID* riid, void** out) {
    note_slot(75);
    if (out) *out = nullptr;  // as slot 8
    if (self->slot_count <= 75) return refuse_beyond_device(75, self->slot_count);  // predates ID3D12Device9
    if (!riid) return E_POINTER;
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, std::uint64_t, std::uint64_t, const IID*, void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[75]);
    if (!out) return create(self->real, desc, creator_id, riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = create(self->real, desc, creator_id, &__uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Device slot 36: CreateFence(initial_value, flags, riid, out).
HRESULT proxy_create_fence(DeviceProxy* self, std::uint64_t initial_value, std::uint64_t flags, const IID* riid,
                           void** out) {
    note_slot(36);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto fence_flags = static_cast<D3D12_FENCE_FLAGS>(static_cast<std::uint32_t>(flags));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateFence(initial_value, fence_flags, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateFence(initial_value, fence_flags, __uuidof(ID3D12Fence), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(created), self, riid, out);
}

// True when `allocator` is one whose consumers resolve allocator proxies: Leonida's own Vulkan-layer allocator,
// whose lists (CreateCommandList, Reset) find the real allocator through allocator_proxy_real_allocator. A PC
// runtime allocator does not answer the tag, and its lists would take a proxy for their own allocator.
bool resolves_allocator_proxies(IUnknown* allocator) {
    void* found = nullptr;
    if (FAILED(allocator->QueryInterface(kLeonidaCommandAllocatorTag, &found)) || !found) return false;
    static_cast<IUnknown*>(found)->Release();  // the implementation pointer starts with its IUnknown chain
    return true;
}

// Device slot 9: CreateCommandAllocator(type, riid, out). An allocator proxy where the list side resolves it (see
// resolves_allocator_proxies); any other allocator goes back unchanged, logged once, so the host runtime's lists
// never see a proxy. Null output and null IID as slot 8.
HRESULT proxy_create_command_allocator(DeviceProxy* self, std::uint64_t type, const IID* riid, void** out) {
    note_slot(9);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto list_type = static_cast<D3D12_COMMAND_LIST_TYPE>(static_cast<std::uint32_t>(type));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateCommandAllocator(list_type, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandAllocator(list_type, __uuidof(ID3D12CommandAllocator), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    auto* allocator = static_cast<IUnknown*>(created);
    if (!resolves_allocator_proxies(allocator)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: allocators of this device are not wrapped: its command lists "
                                 "cannot resolve an allocator proxy\n");
        const HRESULT asked = allocator->QueryInterface(*riid, out);
        allocator->Release();
        return asked;
    }
    return wrap_child<ChildKind::allocator>(allocator, self, riid, out);
}

// Device slot 32: OpenSharedHandle(handle, riid, out). The object is opened as asked; when it is a fence (whatever
// interface was asked for, IUnknown included) the title gets a fence proxy, anything else (heaps, resources) is
// returned unchanged.
HRESULT proxy_open_shared_handle(DeviceProxy* self, std::uint64_t handle, const IID* riid, void** out) {
    note_slot(32);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto shared = reinterpret_cast<HANDLE>(handle);
    if (!out) return self->real->OpenSharedHandle(shared, *riid, nullptr);
    void* opened = nullptr;
    HRESULT hr = self->real->OpenSharedHandle(shared, *riid, &opened);
    if (FAILED(hr) || !opened) return FAILED(hr) ? hr : E_FAIL;
    void* fence = nullptr;
    if (FAILED(static_cast<IUnknown*>(opened)->QueryInterface(__uuidof(ID3D12Fence), &fence)) || !fence) {
        *out = opened;  // not a fence
        return hr;
    }
    static_cast<IUnknown*>(opened)->Release();
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(fence), self, riid, out);
}

// Device slots that take heap, queue or fence objects: the real runtime gets the real objects.
//   31 CreateSharedHandle(object, attributes, access, name, handle)
//   34 MakeResident(count, objects)      35 Evict(count, objects)
//   45 SetEventOnMultipleFenceCompletion(fences, values, count, flags, event)            (ID3D12Device1)
//   46 SetResidencyPriority(count, objects, priorities)                                  (ID3D12Device1)
//   50 EnqueueMakeResident(flags, count, objects, fence, value)                          (ID3D12Device3)
template <unsigned Slot>
HRESULT proxy_unwrapping_call(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4, std::uint64_t a5) {
    note_slot(Slot);
    if (Slot >= self->slot_count) return refuse_beyond_device(Slot, self->slot_count);  // predates the interface
    UnwrappedArray objects;
    std::uint64_t r1 = a1, r2 = a2, r3 = a3, r4 = a4;
    if constexpr (Slot == 31) {
        r1 = unwrap_object(a1);
    } else if constexpr (Slot == 34 || Slot == 35 || Slot == 46) {
        if (!objects.fill(a2, a1)) return E_OUTOFMEMORY;
        if (objects.data()) r2 = objects.data();
    } else if constexpr (Slot == 45) {
        if (!objects.fill(a1, a3)) return E_OUTOFMEMORY;
        if (objects.data()) r1 = objects.data();
    } else if constexpr (Slot == 50) {
        if (!objects.fill(a3, a2)) return E_OUTOFMEMORY;
        if (objects.data()) r3 = objects.data();
        r4 = unwrap_object(a4);
    }
    // r1-r4 differ from a1-a4 only in a position the method takes, so the title's frame has that word.
    return static_cast<HRESULT>(static_cast<std::uint32_t>(forward_device<Slot>(self, r1, r2, r3, r4, a5)));
}

static bool accessible_range(const void* ptr, std::size_t bytes, bool write = false) {
    auto at = reinterpret_cast<std::uintptr_t>(ptr);
    if (!at || !bytes || bytes > UINTPTR_MAX - at) return false;
    const auto end = at + bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_GUARD)) return false;
        const DWORD protect = mbi.Protect & 0xffu;
        if (!protect || protect == PAGE_NOACCESS || protect == PAGE_EXECUTE ||
            (write && !(protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))))
            return false;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize > UINTPTR_MAX - base || base + mbi.RegionSize <= at) return false;
        at = base + mbi.RegionSize;
    }
    return true;
}

HRESULT proxy_create_descriptor_heap(DeviceProxy* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const IID* riid,
                                     void** out) {
    note_slot(14);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as the PC device does
    if (!riid) return E_POINTER;
    // No output: a capability check, answered by the device (S_FALSE, as on the PC).
    if (!out) return self->real->CreateDescriptorHeap(desc, *riid, nullptr);

    void* created = nullptr;
    HRESULT hr = E_INVALIDARG;
    bool console_extended = false;
    if (desc && UINT(desc->Type) <= D3D12_DESCRIPTOR_HEAP_TYPE_DSV && desc->NumDescriptors &&
        (desc->Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(desc);
        const void* tail = bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC);
        if (accessible_range(tail, sizeof(std::uint64_t) * 2)) {
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            std::memcpy(&address, tail, sizeof(address));
            std::memcpy(&size, bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC) + sizeof(address), sizeof(size));
            // Both ABI forms use slot 14. Like VIctory, distinguish placement by its exact byte count.
            // The ordinary form may have unrelated nonzero words immediately after its inline description.
            const auto stride = self->real->GetDescriptorHandleIncrementSize(desc->Type);
            const std::uint64_t required = std::uint64_t(desc->NumDescriptors) * stride;
            if (stride && address && address % stride == 0 && size == required && size <= SIZE_MAX &&
                accessible_range(reinterpret_cast<const void*>(address), static_cast<std::size_t>(size), true)) {
                if (const auto handler = g_console_descriptor_heap.load(std::memory_order_acquire)) {
                    console_extended = true;
                    hr = handler(self->real, desc, address, size, __uuidof(ID3D12DescriptorHeap), &created);
                }
            }
            static std::atomic<unsigned> placement_traces{0};
            if (address && placement_traces.fetch_add(1, std::memory_order_relaxed) < 12)
                std::fprintf(stderr, "[rt] d3d12_xs: descriptor heap type=%u count=%u tail_va=0x%llx tail_bytes=%llu "
                                     "required_bytes=%llu placed=%u\n",
                    unsigned(desc->Type), desc->NumDescriptors, static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(required),
                    unsigned(console_extended));
        }
    }
    if (!console_extended)
        hr = self->real->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), &created);
    if (FAILED(hr) || !created) return FAILED(hr) ? hr : E_FAIL;

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(created);
    SIZE_T custom_cpu_start = 0;
    SIZE_T custom_byte_size = 0;
    SIZE_T real_cpu_start = 0;
    DescriptorHeapBackingAlloc alloc_fn = nullptr;
    DescriptorHeapBackingFree free_fn = nullptr;
    {
        std::lock_guard lock(heap_alloc_mutex());
        alloc_fn = g_heap_alloc;
        free_fn = g_heap_free;
    }
    if (!console_extended && alloc_fn && desc && desc->NumDescriptors > 0) {
        const UINT inc = self->real->GetDescriptorHandleIncrementSize(desc->Type);
        if (inc > 0 && desc->NumDescriptors <= (std::numeric_limits<SIZE_T>::max)() / inc) {
            const SIZE_T byte_size = static_cast<SIZE_T>(desc->NumDescriptors) * static_cast<SIZE_T>(inc);
            void* backing = alloc_fn(byte_size, kXmemDescriptorHeapAttributes);
            if (backing) {
                const auto addr = reinterpret_cast<SIZE_T>(backing);
                if (addr <= (std::numeric_limits<SIZE_T>::max)() - byte_size) {
                    bool adopted = false;
#ifdef LEONIDA_VULKAN_DEVICE
                    if (self->console_lists && (desc->Flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE ||
                                                accessible_range(backing, byte_size, true)))
                        adopted = rt::d3d12::adopt_console_descriptor_cpu_backing(heap, backing, byte_size, free_fn);
#endif
                    if (!adopted) {
                        custom_cpu_start = addr;
                        custom_byte_size = byte_size;
                        real_cpu_start = raw_heap_get_cpu_handle(heap, *reinterpret_cast<void***>(heap)).ptr;
                    }
                } else if (free_fn) {
                    free_fn(backing, kXmemDescriptorHeapAttributes);
                }
            }
        }
    }

    hr = create_heap_proxy(heap, self, *riid, out, custom_cpu_start, custom_byte_size, real_cpu_start,
                           custom_cpu_start ? free_fn : nullptr);
    if (FAILED(hr)) {
        heap->Release();
        if (custom_cpu_start && free_fn)
            free_fn(reinterpret_cast<void*>(custom_cpu_start), kXmemDescriptorHeapAttributes);
    }
    return hr;
}

// Device slots 17-24 write or copy descriptors using CPU handles. When a title supplies its own descriptor-heap base
// via __XMemAllocationHooks__, those handles fall in [custom_start, custom_end) and must be translated back to the
// underlying PC/Vulkan heap before forwarding.
void proxy_create_cbv(DeviceProxy* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(17);
    const auto translated = translate_cpu_handle(self, dest);
    if (self->console_lists && dest.ptr && translated.ptr < 0x10000u) {
        std::fprintf(stderr, "[rt] d3d12_xs: invalid CBV destination handle=0x%llx translated=0x%llx\n",
            static_cast<unsigned long long>(dest.ptr), static_cast<unsigned long long>(translated.ptr));
        rt::shims::unimplemented_proxy_effect("device", 17, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
            reinterpret_cast<std::uint64_t>(self), reinterpret_cast<std::uint64_t>(desc), dest.ptr, translated.ptr);
        return;
    }
    self->real->CreateConstantBufferView(desc, translated);
}

void proxy_create_srv(DeviceProxy* self, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(18);
    self->real->CreateShaderResourceView(resource, desc, translate_cpu_handle(self, dest));
}

void proxy_create_uav(DeviceProxy* self, ID3D12Resource* resource, ID3D12Resource* counter,
                      const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(19);
    self->real->CreateUnorderedAccessView(resource, counter, desc, translate_cpu_handle(self, dest));
}

void proxy_create_rtv(DeviceProxy* self, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(20);
    self->real->CreateRenderTargetView(resource, desc, translate_cpu_handle(self, dest));
}

void proxy_create_dsv(DeviceProxy* self, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(21);
    self->real->CreateDepthStencilView(resource, desc, translate_cpu_handle(self, dest));
}

void proxy_create_sampler(DeviceProxy* self, const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(22);
    self->real->CreateSampler(desc, translate_cpu_handle(self, dest));
}

void proxy_copy_descriptors(DeviceProxy* self, UINT num_dest, const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts,
                            const UINT* dest_sizes, UINT num_src, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                            const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    note_slot(23);
    if (g_custom_range_count.load(std::memory_order_acquire) == 0) {
        self->real->CopyDescriptors(num_dest, dest_starts, dest_sizes, num_src, src_starts, src_sizes, type);
        return;
    }
    constexpr UINT kInline = 32;
    D3D12_CPU_DESCRIPTOR_HANDLE dest_inline[kInline];
    D3D12_CPU_DESCRIPTOR_HANDLE src_inline[kInline];
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dest_heap;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> src_heap;
    const D3D12_CPU_DESCRIPTOR_HANDLE* dest_out = dest_starts;
    const D3D12_CPU_DESCRIPTOR_HANDLE* src_out = src_starts;
    try {
        if (dest_starts && num_dest > 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE* dst = dest_inline;
            if (num_dest > kInline) {
                dest_heap.resize(num_dest);
                dst = dest_heap.data();
            }
            for (UINT i = 0; i < num_dest; ++i) dst[i] = translate_cpu_handle(self, dest_starts[i]);
            dest_out = dst;
        }
        if (src_starts && num_src > 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE* src = src_inline;
            if (num_src > kInline) {
                src_heap.resize(num_src);
                src = src_heap.data();
            }
            for (UINT i = 0; i < num_src; ++i) src[i] = translate_cpu_handle(self, src_starts[i]);
            src_out = src;
        }
    } catch (const std::bad_alloc&) {
        return;
    }
    self->real->CopyDescriptors(num_dest, dest_out, dest_sizes, num_src, src_out, src_sizes, type);
}

void proxy_copy_descriptors_simple(DeviceProxy* self, UINT num_descriptors, D3D12_CPU_DESCRIPTOR_HANDLE dest_start,
                                   D3D12_CPU_DESCRIPTOR_HANDLE src_start, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    note_slot(24);
    self->real->CopyDescriptorsSimple(num_descriptors, translate_cpu_handle(self, dest_start),
                                      translate_cpu_handle(self, src_start), type);
}

// The Xbox slot 62 takes two more words between the description and the IID: (desc, w2, w3, riid, out). Measured
// on the package's XboxBvhBuilder.dll (its constructor passes w2 = w3 = 0 and IID_ID3D12PipelineState in the fourth
// argument), as the prior art had recorded. Their meaning is unknown; non-zero values are logged once.
HRESULT proxy_create_compute_pipeline_state(DeviceProxy* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                            std::uint64_t w2, std::uint64_t w3, const IID* riid, void** out) {
    note_slot(62);
    if (w2 || w3) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: slot 62 extra words 0x%llx 0x%llx (ignored)\n",
                         static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3));
    }
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    return self->real->CreateComputePipelineState(desc, *riid, out);
}

// The Xbox slot 67 creates a compute pipeline from native GPU code: (desc, riid, out), named
// DeserializeComputePipelineStateX in amd_fidelityfx_upscaler_xbox.dll's messages (call at RVA 0x56d9). The desc
// passes the root signature at +0x00, followed by pointer/size pairs for the shader's FSPX entries (docs/fspx.md):
// kind 0 (dispatch state) at +0x08/+0x10, kind 1 at +0x18/+0x20, kind 6 (the RDNA2 program) at +0x28/+0x30, and
// kind 7 (records) at +0x38/+0x40. Translates the console description to a standard D3D12_COMPUTE_PIPELINE_STATE_DESC
// with CS bytecode pointing at kind 6 and routes it to CreateComputePipelineState on the PC device (slot 11), which
// on the Vulkan device resolves through the translator. The PC slot 67 is CreateProtectedResourceSession1.
HRESULT proxy_create_compute_pipeline_from_native_code(DeviceProxy* self, const void* desc,
                                                       const IID* riid, void** out) {
    note_slot(67);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;

    // FSPX description layout (docs/fspx.md Â§How the DLL reads it):
    struct XboxNativeComputePipelineStateDesc {
        ID3D12RootSignature* root_signature;  // +0x00
        const void* kind0;                    // +0x08: dispatch state
        std::uint64_t kind0_size;             // +0x10: 28 bytes
        const void* kind1;                    // +0x18: unknown
        std::uint64_t kind1_size;             // +0x20: 20 bytes
        const void* kind6;                    // +0x28: RDNA2 program code
        std::uint64_t kind6_size;             // +0x30: program size in bytes
        const void* kind7;                    // +0x38: resource/hash records
        std::uint64_t kind7_size;             // +0x40: records size in bytes
    };
    const auto* xbox_desc = static_cast<const XboxNativeComputePipelineStateDesc*>(desc);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pc_desc{};
    pc_desc.pRootSignature = xbox_desc->root_signature;
    pc_desc.CS.pShaderBytecode = xbox_desc->kind6;
    pc_desc.CS.BytecodeLength = static_cast<SIZE_T>(xbox_desc->kind6_size);
    // With kind 0 (the DLL always passes it: its reader refuses a payload without one), the four pairs go on as one
    // FSPX payload, so the device matches the program by its kind-6 and kind-0 keys (docs/fspx.md, "Build and run-time
    // lookup"): two programs with the same code and different dispatch state stay apart. Every pair is checked before
    // it is read (leonida::fspx_payload_from_parts); an inconsistent one refuses the call. Without kind 0 the kind-6
    // bytes go on alone, as before.
    std::vector<std::uint8_t> payload;
    if (xbox_desc->kind0 || xbox_desc->kind0_size) {
        const leonida::FspxPart parts[] = {{0, xbox_desc->kind0, xbox_desc->kind0_size},
                                           {1, xbox_desc->kind1, xbox_desc->kind1_size},
                                           {6, xbox_desc->kind6, xbox_desc->kind6_size},
                                           {7, xbox_desc->kind7, xbox_desc->kind7_size}};
        payload = leonida::fspx_payload_from_parts(parts);
        if (payload.empty()) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 67 refused: a pointer/size pair is inconsistent or too large "
                             "(kind0 %llu B, kind1 %llu B, kind6 %llu B, kind7 %llu B)\n",
                             static_cast<unsigned long long>(xbox_desc->kind0_size),
                             static_cast<unsigned long long>(xbox_desc->kind1_size),
                             static_cast<unsigned long long>(xbox_desc->kind6_size),
                             static_cast<unsigned long long>(xbox_desc->kind7_size));
            return E_INVALIDARG;
        }
        pc_desc.CS.pShaderBytecode = payload.data();
        pc_desc.CS.BytecodeLength = payload.size();
    }

    pc_desc.NodeMask = 0;
    pc_desc.CachedPSO = {nullptr, 0};
    pc_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Every failed call is logged with its number: the DLL creates its pipelines one after the other, and only the
    // first call of a slot is announced, so a miss on a later program would otherwise read as the first call's.
    static std::atomic<std::uint64_t> calls{0};
    const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const HRESULT hr = self->real->CreateComputePipelineState(&pc_desc, *riid, out);
    if (FAILED(hr))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 67 call %llu failed with 0x%08lx (kind 0 %llu B, kind 1 %llu B, kind 6 %llu B, "
                     "kind 7 %llu B)\n",
                     static_cast<unsigned long long>(call), static_cast<unsigned long>(hr),
                     static_cast<unsigned long long>(xbox_desc->kind0_size),
                     static_cast<unsigned long long>(xbox_desc->kind1_size),
                     static_cast<unsigned long long>(xbox_desc->kind6_size),
                     static_cast<unsigned long long>(xbox_desc->kind7_size));
    return hr;
}

HRESULT proxy_create_resource(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_properties,
                              D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* xbox_desc,
                              D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE* clear_value,
                              const IID* riid, void** out) {
    note_slot(76);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!xbox_desc) return E_POINTER;
    if (!riid) return E_POINTER;

    // The measured description is 56 bytes plus Xbox extension words; D3D12_RESOURCE_DESC is exactly 56 bytes.
    // The Xbox layout word is 0x101 (tiled) or 0x1 (row-major buffer), and is not a PC layout value. Committed
    // resources take ROW_MAJOR for buffers and UNKNOWN for everything else, so the word is ignored and replaced.
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 76 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout));
    }
    return self->real->CreateCommittedResource(heap_properties, heap_flags, &pc_desc, initial_state, clear_value,
                                               *riid, out);
}

// PR #7201's displaced public methods. QueryInterface can return a different receiver from the base device;
// probing a slot count and then indexing the original base pointer would violate that COM contract.
HRESULT proxy_create_resource_layout(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap,
                                      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1* desc,
                                      D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear,
                                      ID3D12ProtectedResourceSession* session, UINT32 castable_count,
                                      const DXGI_FORMAT* castable, const IID* riid, void** out) {
    note_slot(127);
    if (out) *out = nullptr;
    if (!heap || !desc || !riid) return E_POINTER;
    D3D12_RESOURCE_DESC1 pc_desc = *desc;
    static std::atomic<unsigned> logged{0};
    if (d3d12_trace::enabled() && logged.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr, "[rt] d3d12_xs: slot127 dimension %u format %u flags 0x%x texture_layout 0x%x "
                             "initial_layout 0x%x heap_type %u heap_flags 0x%x castable %u session %p\n",
                     static_cast<unsigned>(pc_desc.Dimension), static_cast<unsigned>(pc_desc.Format),
                     static_cast<unsigned>(pc_desc.Flags), static_cast<unsigned>(pc_desc.Layout),
                     static_cast<unsigned>(layout), static_cast<unsigned>(heap->Type),
                     static_cast<unsigned>(heap_flags), castable_count, session);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                          : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The console high flags have no established PC meaning. Admit only the observed unprotected RGBA8 color
    // target under an explicit experiment; keep its RT/UAV capabilities and let Vulkan own its image layout.
    // This does not claim support for console compression metadata or for aliasing the image's raw storage.
    if (static_cast<UINT>(pc_desc.Flags) == 0x300005 && pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        pc_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM && heap->Type == D3D12_HEAP_TYPE_DEFAULT &&
        static_cast<UINT>(heap_flags) == 8 && !session && !castable_count) {
        char mode[32]{};
        if (GetEnvironmentVariableA("LEONIDA_XBOX_RESOURCE_LAYOUT", mode, sizeof(mode)) == 14 &&
            std::strcmp(mode, "observed_color") == 0) {
            pc_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr, "[rt] d3d12_xs: EXPERIMENTAL slot127 observed RGBA8 target flags; "
                                     "console metadata semantics unverified\n");
        }
    }
    Microsoft::WRL::ComPtr<ID3D12Device10> device;
    const HRESULT hr = self->real->QueryInterface(IID_PPV_ARGS(device.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (!device) return E_NOINTERFACE;
    return device->CreateCommittedResource3(heap, heap_flags, &pc_desc, layout, clear, session,
                                             castable_count, castable, *riid, out);
}

template <typename T>
constexpr std::size_t pipeline_stream_record_size() {
    constexpr auto payload = (sizeof(std::uint32_t) + alignof(T) - 1) / alignof(T) * alignof(T);
    return (payload + sizeof(T) + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
}

// Xbox's common stream records match the PC payloads, but AS/MS are numbered 23/24 instead of 24/25.
// Corroborated by Marvisak/VIctory d2a4bae1, crates/victory-gpu/src/pipeline.rs::subobject. Console
// extensions from 25 onward have no established size here; never search their payload for apparent headers.
std::size_t console_pipeline_stream_record_size(std::uint32_t kind) {
    switch (kind) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
        return pipeline_stream_record_size<ID3D12RootSignature*>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case 23:  // Xbox amplification shader
    case 24:  // Xbox mesh shader
        return pipeline_stream_record_size<D3D12_SHADER_BYTECODE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
        return pipeline_stream_record_size<D3D12_STREAM_OUTPUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
        return pipeline_stream_record_size<D3D12_BLEND_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
        return pipeline_stream_record_size<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
        return pipeline_stream_record_size<D3D12_RASTERIZER_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        return pipeline_stream_record_size<D3D12_DEPTH_STENCIL_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
        return pipeline_stream_record_size<D3D12_INPUT_LAYOUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        return pipeline_stream_record_size<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        return pipeline_stream_record_size<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        return pipeline_stream_record_size<D3D12_RT_FORMAT_ARRAY>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        return pipeline_stream_record_size<DXGI_FORMAT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        return pipeline_stream_record_size<DXGI_SAMPLE_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        return pipeline_stream_record_size<D3D12_CACHED_PIPELINE_STATE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        return pipeline_stream_record_size<D3D12_PIPELINE_STATE_FLAGS>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
        return pipeline_stream_record_size<D3D12_DEPTH_STENCIL_DESC1>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
        return pipeline_stream_record_size<D3D12_VIEW_INSTANCING_DESC>();
    default: return 0;
    }
}

bool snapshot_pipeline_stream(void* destination, const void* source, std::size_t bytes) {
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT normalize_console_pipeline_stream(const D3D12_PIPELINE_STATE_STREAM_DESC* source,
                                          std::vector<std::uint64_t>& storage,
                                          D3D12_PIPELINE_STATE_STREAM_DESC& normalized) {
    if (!source || !snapshot_pipeline_stream(&normalized, source, sizeof(normalized))) return E_INVALIDARG;
    constexpr std::size_t max_stream_bytes = 1024 * 1024;
    const auto size = normalized.SizeInBytes;
    if (!normalized.pPipelineStateSubobjectStream || !size || size > max_stream_bytes || size % sizeof(void*))
        return E_INVALIDARG;
    storage.resize(size / sizeof(storage[0]));
    if (!snapshot_pipeline_stream(storage.data(), normalized.pPipelineStateSubobjectStream, size)) return E_INVALIDARG;
    auto* bytes = reinterpret_cast<std::uint8_t*>(storage.data());
    for (std::size_t at = 0; at < size;) {
        if (size - at < sizeof(std::uint32_t)) return E_INVALIDARG;
        std::uint32_t kind = 0;
        std::memcpy(&kind, bytes + at, sizeof(kind));
        const auto record_size = console_pipeline_stream_record_size(kind);
        if (!record_size) {
            static std::atomic<unsigned> refusals{0};
            if (refusals.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr, "[rt] d3d12_xs: console pipeline stream has unknown subobject %u at byte %zu\n",
                             kind, at);
            return E_NOTIMPL;
        }
        if (record_size > size - at) return E_INVALIDARG;
        if (kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) {
            std::uint32_t topology = 0;
            std::memcpy(&topology, bytes + at + sizeof(kind), sizeof(topology));
            // Xbox type 19 is RECTLIST. Replay retains its IA topology 17 and expands verified affine vertices.
            if (topology == 19) {
                topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                std::memcpy(bytes + at + sizeof(kind), &topology, sizeof(topology));
            }
        }
        if (kind == 23 || kind == 24) {
            const auto pc_kind = static_cast<std::uint32_t>(kind == 23 ? D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS
                                                                     : D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS);
            std::memcpy(bytes + at, &pc_kind, sizeof(pc_kind));
        }
        at += record_size;
    }
    normalized.pPipelineStateSubobjectStream = storage.data();
    return S_OK;
}

HRESULT proxy_create_pipeline_state(DeviceProxy* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                     const IID* riid, void** out) {
    note_slot(86);
    if (out) *out = nullptr;
    if (!riid) return E_POINTER;
    Microsoft::WRL::ComPtr<ID3D12Device2> device;
    const HRESULT hr = self->real->QueryInterface(IID_PPV_ARGS(device.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (!device) return E_NOINTERFACE;
    if (!self->console_lists) return device->CreatePipelineState(desc, *riid, out);
    try {
        std::vector<std::uint64_t> storage;
        D3D12_PIPELINE_STATE_STREAM_DESC normalized{};
        const HRESULT converted = normalize_console_pipeline_stream(desc, storage, normalized);
        if (FAILED(converted)) return converted;
        return device->CreatePipelineState(&normalized, *riid, out);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

HRESULT proxy_create_state_object(DeviceProxy* self, const D3D12_STATE_OBJECT_DESC* desc,
                                   const IID* riid, void** out) {
    note_slot(101);
    if (out) *out = nullptr;
    if (!riid) return E_POINTER;
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    const HRESULT hr = self->real->QueryInterface(IID_PPV_ARGS(device.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (!device) return E_NOINTERFACE;
    return device->CreateStateObject(desc, *riid, out);
}

// Normalize measured descriptions for real native allocation queries and independently checked placement.
// This does not expose optimal storage as CPU bytes or claim the console metadata has a PC representation.
bool normalize_observed_native_texture(ID3D12Device& device, D3D12_RESOURCE_DESC& d) {
    char mode[32]{};
    D3D12_RESOURCE_DESC native{};
    if (GetEnvironmentVariableA("LEONIDA_XBOX_RESOURCE_LAYOUT", mode, sizeof(mode)) != 14 ||
        std::strcmp(mode, "observed_color") != 0 || !rt::d3d12::translate_title_texture_desc(d, native)) return false;
    using rt::d3d12::TitleTextureUse;
    const auto use = rt::d3d12::title_texture_use(native);
    if (use != TitleTextureUse::depth) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT format{native.Format};
        if (FAILED(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)))) return false;
        UINT required = D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
            (native.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                : native.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
                                                                        : D3D12_FORMAT_SUPPORT1_TEXTURE2D);
        if (native.MipLevels > 1) required |= D3D12_FORMAT_SUPPORT1_MIP;
        if (use == TitleTextureUse::color || use == TitleTextureUse::color_storage)
            required |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if (use == TitleTextureUse::storage || use == TitleTextureUse::color_storage) {
            const DXGI_FORMAT storage_format = native.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                ? DXGI_FORMAT_R8G8B8A8_UNORM : native.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                ? DXGI_FORMAT_B8G8R8A8_UNORM : native.Format == DXGI_FORMAT_B5G5R5A1_UNORM
                ? DXGI_FORMAT_R16_UINT : native.Format;
            if (storage_format != native.Format) {
                // The existing native paths use compatible UNORM storage for sRGB and R16_UINT for packed5551.
                // Sampling/rendering still require the logical format; exact mutable format lists are checked later.
                D3D12_FEATURE_DATA_FORMAT_SUPPORT storage{storage_format};
                const UINT storage_required = required | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
                if (FAILED(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &storage, sizeof(storage))) ||
                    (UINT(storage.Support1) & storage_required) != storage_required ||
                    !(UINT(storage.Support2) & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;
            } else {
                required |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
                if (!(UINT(format.Support2) & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;
            }
        }
        if ((UINT(format.Support1) & required) != required) return false;
    }
    d = native;
    return true;
}

bool observed_linear_texture_enabled(const D3D12_RESOURCE_DESC& desc, bool allocation_query = false) {
    char mode[32]{};
    return GetEnvironmentVariableA("LEONIDA_XBOX_TEXTURE_LAYOUT", mode, sizeof(mode)) == 15 &&
           std::strcmp(mode, "observed_r8_4ks") == 0 &&
           (rt::d3d12::title_linear_texture_shape(desc) || rt::d3d12::title_default_bc_texture_shape(desc) ||
            rt::d3d12::title_display_bc_texture_shape(desc) ||
            (allocation_query && (rt::d3d12::title_sampled_4ks_texture_shape(desc) ||
                                  rt::d3d12::title_implicit_sampled_4ks_query(desc))));
}

// PR #7201's console slot uses packed 56-byte DESC entries, not the PC's 64-byte DESC1 array. Both hidden-return
// pointer orders are accepted, but the input representation remains the console one in either order. This bridge
// does not infer console tiling. Unchanged PC descriptions and the opt-in native target profile reach the backend.
void trace_small_texture_query(unsigned slot, const D3D12_RESOURCE_DESC* descs, UINT count,
                               const D3D12_RESOURCE_ALLOCATION_INFO& result) {
    static const bool trace = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_TRACE_TEXTURE_RANGES", value, 2) == 1 && value[0] == L'1';
    }();
    if (!trace || count != 1 || !descs || descs->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        descs->Height > 16 || descs->Width < 32) return;
    std::fprintf(stderr, "[rt] d3d12_xs: texture allocation slot=%u size=%llux%u format=%u layout=0x%x "
                 "bytes=%llu alignment=%llu\n", slot, static_cast<unsigned long long>(descs->Width),
                 descs->Height, unsigned(descs->Format), unsigned(descs->Layout),
                 static_cast<unsigned long long>(result.SizeInBytes), static_cast<unsigned long long>(result.Alignment));
}

std::uint64_t proxy_get_resource_allocation_info2(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2,
                                                  std::uint64_t a3, std::uint64_t a4, std::uint64_t a5,
                                                  std::uint64_t caller) {
    note_slot(108);
    using Pin = std::unique_ptr<DeviceProxy, decltype(&release_proxy_reference)>;
    Pin first(pin_live_device(self), &release_proxy_reference);
    Pin second(pin_live_device(reinterpret_cast<const void*>(a1)), &release_proxy_reference);
    // Exactly one argument must be an actual live device. Two devices cannot also be writable result storage.
    if (static_cast<bool>(first) == static_cast<bool>(second)) return 0;
    DeviceProxy* proxy = first ? first.get() : second.get();
    auto* result = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(first ? a1 : reinterpret_cast<std::uint64_t>(self));
    if (!result) return 0;
    *result = {UINT64_MAX, 0};  // SizeInBytes is the API's failure indicator; this is never a usable allocation.
    const auto result_word = reinterpret_cast<std::uint64_t>(result);
    const UINT count = static_cast<UINT>(a3);
    const auto* descs = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4);
    if (count && !descs) return result_word;
    Microsoft::WRL::ComPtr<ID3D12Device8> device;
    if (FAILED(proxy->real->QueryInterface(IID_PPV_ARGS(device.GetAddressOf()))) || !device) return result_word;
    std::vector<D3D12_RESOURCE_DESC1> expanded;
    try {
        expanded.resize(count);
    } catch (const std::bad_alloc&) {
        return result_word;
    } catch (const std::length_error&) {
        return result_word;
    }
    static_assert(sizeof(D3D12_RESOURCE_DESC) == 56);
    static_assert(sizeof(D3D12_RESOURCE_DESC1) == 64);
    const auto diagnose = [&](UINT index, const char* reason) {
        static std::atomic<unsigned> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) >= 32 && !d3d12_trace::enabled()) return;
        const auto& d = descs[index];
        std::fprintf(stderr, "[rt] d3d12_xs: allocation108 refused reason=%s caller=%s index=%u/%u dimension=%u "
                             "alignment=%llu width=%llu height=%u depth_or_array=%u mips=%u format=%u "
                             "samples=%u quality=%u layout=0x%x flags=0x%x size=%llu result_alignment=%llu\n",
            reason, rt::shims::module_offset(caller).c_str(), index, count, static_cast<unsigned>(d.Dimension),
            static_cast<unsigned long long>(d.Alignment), static_cast<unsigned long long>(d.Width), d.Height,
            static_cast<unsigned>(d.DepthOrArraySize), static_cast<unsigned>(d.MipLevels),
            static_cast<unsigned>(d.Format), d.SampleDesc.Count, d.SampleDesc.Quality,
            static_cast<unsigned>(d.Layout), static_cast<unsigned>(d.Flags),
            static_cast<unsigned long long>(result->SizeInBytes), static_cast<unsigned long long>(result->Alignment));
    };
    constexpr std::uint32_t kKnownPcFlags = 0x5ff;  // includes tight alignment 0x400, excludes unknown 0x200.
    for (UINT i = 0; i < count; ++i) {
        auto d = descs[i];
        const bool linear_texture_profile = observed_linear_texture_enabled(d, true);
        // Query the real native render target's size without claiming that its console metadata is PC memory.
        // Actual title-address creation and raw aliases remain independently checked by the placement adapter.
        const bool native_profile = normalize_observed_native_texture(*device.Get(), d);
        const bool console_allocation = native_profile && (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            rt::d3d12::title_linear_color_layout(descs[i]) || rt::d3d12::title_depth16_metadata(descs[i]) ||
            rt::d3d12::title_explicit_4k_storage(descs[i]) || rt::d3d12::title_storage_array_profile(descs[i]) ||
            rt::d3d12::title_default_storage_profile(descs[i]) || rt::d3d12::title_linear_volume_storage(descs[i]) ||
            rt::d3d12::title_rt_storage_profile(descs[i]) ||
            (rt::d3d12::title_native_rt_target(descs[i]) && UINT(descs[i].Flags) == 0xc00001u) ||
            rt::d3d12::title_d16_target(descs[i]) || rt::d3d12::title_combined_depth_metadata(descs[i]) ||
            rt::d3d12::title_additional_depth_target(descs[i]));
        if (console_allocation || linear_texture_profile) {
            // The Vulkan resource owns separate storage. The title allocator still needs the raw layout.
            d = descs[i];
        } else if (native_profile) {
            static std::atomic<bool> announced{false};
            if (!announced.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr, "[rt] d3d12_xs: EXPERIMENTAL allocation108 native color target sizing; "
                                     "console metadata and raw aliases remain unsupported\n");
        }
        if (!linear_texture_profile && !console_allocation &&
            (static_cast<UINT>(d.Layout) > D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE ||
            (static_cast<UINT>(d.Flags) & ~kKnownPcFlags))) {
            diagnose(i, "console layout or flags");
            return result_word;
        }
        std::memcpy(&expanded[i], &d, sizeof(d));
        expanded[i].SamplerFeedbackMipRegion = {};
    }
    *result = device->GetResourceAllocationInfo2(static_cast<UINT>(a2), count, count ? expanded.data() : nullptr,
                                                 reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(a5));
    trace_small_texture_query(108, descs, count, *result);
    if (result->SizeInBytes == UINT64_MAX)
        for (UINT i = 0; i < count; ++i) diagnose(i, "native allocation query");
    return result_word;
}

// Device slot 26, GetCustomHeapProperties, as the retail executable calls it. Measured on 1.0.2.1 under
// gta6rt --run with a register probe in this slot's thunk: three calls right after device creation, each with the
// device proxy in RDX and not in RCX, R8 = 1 and R9 = 1, 2, 3 (node mask 1; DEFAULT, UPLOAD, READBACK). So the
// buffer for the returned D3D12_HEAP_PROPERTIES is in RCX and the device in RDX. That is how x64 passes them to a
// plain function that takes the device as its first argument and returns a struct; a PC C++ method has the device
// in RCX and the buffer in RDX. The forwarder took RCX for the proxy, read the PC vtable out of the title's stack
// buffer and faulted. The buffer's address goes back in RAX, as that convention returns it; what the title does with
// RAX is not measured.
// Slot 25 returns a 16-byte allocation-info structure. Console callers put the hidden output before this,
// as for slot 26; PC callers retain this before the output. Identify and pin the device before dereferencing it.
std::uint64_t proxy_get_resource_allocation_info(DeviceProxy* self, std::uint64_t a1, UINT node_mask,
                                                UINT count, const D3D12_RESOURCE_DESC* descriptions) {
    note_slot(25);
    using Pin = std::unique_ptr<DeviceProxy, decltype(&release_proxy_reference)>;
    Pin first(pin_live_device(self), &release_proxy_reference);
    Pin second(pin_live_device(reinterpret_cast<const void*>(a1)), &release_proxy_reference);
    if (static_cast<bool>(first) == static_cast<bool>(second)) return 0;
    DeviceProxy* proxy = first ? first.get() : second.get();
    auto* out = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(first ? a1 : reinterpret_cast<std::uint64_t>(self));
    if (!out) return 0;
    using Fn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(ID3D12Device*, D3D12_RESOURCE_ALLOCATION_INFO*,
                                                       UINT, UINT, const D3D12_RESOURCE_DESC*);
    reinterpret_cast<Fn>(proxy->real_vtable[25])(proxy->real, out, node_mask, count, descriptions);
    trace_small_texture_query(25, descriptions, count, *out);
    return reinterpret_cast<std::uint64_t>(out);
}

std::uint64_t proxy_get_custom_heap_properties(DeviceProxy* self, std::uint64_t a1,
                                               UINT node_mask, D3D12_HEAP_TYPE heap_type) {
    note_slot(26);
    using Pin = std::unique_ptr<DeviceProxy, decltype(&release_proxy_reference)>;
    Pin first(pin_live_device(self), &release_proxy_reference);
    Pin second(pin_live_device(reinterpret_cast<const void*>(a1)), &release_proxy_reference);
    if (static_cast<bool>(first) == static_cast<bool>(second)) return 0;
    DeviceProxy* proxy = first ? first.get() : second.get();
    auto* out = reinterpret_cast<D3D12_HEAP_PROPERTIES*>(first ? a1 : reinterpret_cast<std::uint64_t>(self));
    if (!out) return 0;
    // The PC x64 member ABI receives its hidden return pointer after this. Passing the caller's buffer directly
    // keeps the existing PC-order forwarding contract as well as the console-order pointer swap.
    using Fn = D3D12_HEAP_PROPERTIES*(WINAPI*)(ID3D12Device*, D3D12_HEAP_PROPERTIES*, UINT, D3D12_HEAP_TYPE);
    reinterpret_cast<Fn>(proxy->real_vtable[26])(proxy->real, out, node_mask, heap_type);
    return reinterpret_cast<std::uint64_t>(out);
}

// Device slot 133 places a resource at title-owned backing: (va, desc, initial layout, clear, riid, out).
// The observed 0xFFFFFFFF is D3D12_BARRIER_LAYOUT_UNDEFINED, not a legacy D3D12_RESOURCE_STATES value. Keep it
// and the optional optimized clear value for the backend; a texture's native layout and aliases need their own
// validation. Only the established buffer path translates console high flags and forces ROW_MAJOR.
std::atomic<PlacedAtVaHandler> g_placed_at_va{nullptr};
std::atomic<PlacedAtVaPagesHook> g_placed_at_va_pages{nullptr};

bool clear_placed_resource_output(void** out) {
    __try {
        *out = nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool snapshot_placed_resource_input(const D3D12_RESOURCE_DESC* desc, const IID* iid,
                                     const D3D12_CLEAR_VALUE* clear, void** out,
                                     D3D12_RESOURCE_DESC* desc_copy, IID* iid_copy,
                                     D3D12_CLEAR_VALUE* clear_copy) {
    __try {
        *out = nullptr;
        if (!desc || !iid) return false;
        std::memcpy(desc_copy, desc, sizeof(*desc_copy));
        std::memcpy(iid_copy, iid, sizeof(*iid_copy));
        if (clear) std::memcpy(clear_copy, clear, sizeof(*clear_copy));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT proxy_create_placed_resource_x(DeviceProxy* self, void* va, const D3D12_RESOURCE_DESC* xbox_desc,
                                       std::uint32_t initial_layout, const D3D12_CLEAR_VALUE* clear,
                                       const IID* riid, void** out) {
    note_slot(133);
    const PlacedAtVaHandler handler = g_placed_at_va.load(std::memory_order_acquire);
    if (!handler) {
        if (out && !clear_placed_resource_output(out)) return E_POINTER;
        return E_NOTIMPL;
    }
    if (!out) return E_POINTER;
    D3D12_RESOURCE_DESC pc_desc{};
    D3D12_CLEAR_VALUE clear_copy{};
    IID requested{};
    if (!snapshot_placed_resource_input(xbox_desc, riid, clear, out, &pc_desc, &requested, &clear_copy))
        return E_POINTER;
    if (!va) return E_INVALIDARG;
    if (pc_desc.Dimension < D3D12_RESOURCE_DIMENSION_BUFFER ||
        pc_desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D) return E_INVALIDARG;
    static std::atomic<unsigned> texture_stacks{0};
    if (pc_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        texture_stacks.fetch_add(1, std::memory_order_relaxed) < 2) {
        void* frames[12]{};
        const USHORT count = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
        std::fprintf(stderr, "[rt] d3d12_xs: slot133 texture call stack");
        for (USHORT i = 0; i < count; ++i)
            std::fprintf(stderr, " %s", rt::shims::module_offset(reinterpret_cast<std::uint64_t>(frames[i])).c_str());
        std::fprintf(stderr, "\n");
    }
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    static std::atomic<unsigned> logged_buffers{0}, logged_textures{0};
    auto& logged = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? logged_buffers : logged_textures;
    static const unsigned range_limit = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_TRACE_TEXTURE_RANGES", value, 2) == 1 && value[0] == L'1' ? 8192u : 4u;
    }();
    if (logged.fetch_add(1, std::memory_order_relaxed) < range_limit) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot133 va=%p dimension=%u size=%llux%ux%u mips=%u format=%u samples=%u/%u "
                     "texture_layout=0x%x xbox_flags=0x%x initial_layout=0x%x clear=%u clear_format=%u\n",
                     va, static_cast<unsigned>(pc_desc.Dimension), static_cast<unsigned long long>(pc_desc.Width),
                     pc_desc.Height, pc_desc.DepthOrArraySize, pc_desc.MipLevels, static_cast<unsigned>(pc_desc.Format),
                     pc_desc.SampleDesc.Count, pc_desc.SampleDesc.Quality, static_cast<unsigned>(pc_desc.Layout),
                     xbox_flags, initial_layout, static_cast<unsigned>(clear != nullptr),
                     static_cast<unsigned>(clear_copy.Format));
    }
    if (pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (initial_layout != UINT32_MAX || clear) return E_INVALIDARG;
    } else {
        // These are the public enhanced-barrier layouts, not texture-memory swizzle modes. Unknown values
        // must not silently turn into COMMON; the backend checks the known layout against resource capabilities.
        if (initial_layout != UINT32_MAX && initial_layout > D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON)
            return E_NOTIMPL;
        if (clear && !(pc_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) return E_INVALIDARG;
    }
    // An integer mask: a D3D12_RESOURCE_FLAGS constant past the SDK's last enumerator is not a constant expression
    // for clang-cl with SDK 10.0.26100 (found in review). The cast is applied to the masked value at run time.
    // The PC flags up to RAYTRACING_ACCELERATION_STRUCTURE (0x100) and USE_TIGHT_ALIGNMENT (0x400); 0x200 is
    // dropped, as committed_resource_refusal says the host refuses it (found in review).
    constexpr std::uint32_t kPcResourceFlags = 0x5ff;
    const bool linear_texture_profile = observed_linear_texture_enabled(pc_desc);
    const bool linear_color_layout = rt::d3d12::title_linear_color_layout(pc_desc);
    const bool explicit_4k_storage = rt::d3d12::title_explicit_4k_storage(pc_desc);
    const bool storage_array_profile = rt::d3d12::title_storage_array_profile(pc_desc);
    const bool default_storage_profile = rt::d3d12::title_default_storage_profile(pc_desc);
    const bool linear_volume_storage = rt::d3d12::title_linear_volume_storage(pc_desc);
    const bool rt_storage_profile = rt::d3d12::title_rt_storage_profile(pc_desc);
    const auto guest_desc = pc_desc;
    const bool combined_depth_metadata = rt::d3d12::title_combined_depth_metadata(pc_desc);
    const bool additional_depth_target = rt::d3d12::title_additional_depth_target(pc_desc);
    const bool native_texture_profile = normalize_observed_native_texture(*self->real, pc_desc);
    if (native_texture_profile && (explicit_4k_storage || storage_array_profile || default_storage_profile ||
                                  linear_volume_storage || rt_storage_profile)) pc_desc = guest_desc;
    if (native_texture_profile && xbox_flags == 0xc00001u && rt::d3d12::title_native_rt_target(pc_desc))
        pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags);  // backend keeps the guest budget separate
    if (native_texture_profile && (combined_depth_metadata || additional_depth_target))
        pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags);  // preserve the recognized guest initial-layout contract
    if (native_texture_profile && linear_color_layout)
        pc_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(rt::d3d12::kTitleLinearLayout);
    if (!linear_texture_profile && !native_texture_profile)
        pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlags);
    // The narrow CPU-owned image path needs an explicit readonly contract. Unknown console flags cannot be
    // erased to make such a texture look readonly; buffer handling retains its established flag translation.
    if (pc_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && xbox_flags != static_cast<UINT>(pc_desc.Flags) &&
        !native_texture_profile && !linear_texture_profile)
        return E_NOTIMPL;
    if (pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) pc_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (const auto pages = g_placed_at_va_pages.load(std::memory_order_acquire)) {
            if (!pages(va, static_cast<std::size_t>(pc_desc.Width)))
                std::fprintf(stderr, "[rt] d3d12_xs: slot 133 page commit failed; resource handler will validate backing\n");
        }
    }
    const HRESULT hr = handler(self->real, va, pc_desc, initial_layout, clear ? &clear_copy : nullptr, requested, out);
    if (FAILED(hr) && pc_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        static std::atomic<unsigned> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 24)
            std::fprintf(stderr, "[rt] d3d12_xs: slot133 native refused hr=0x%08lx va=%p dimension=%u "
                                 "size=%llux%ux%u alignment=%llu mips=%u format=%u samples=%u/%u layout=0x%x "
                                 "raw_flags=0x%x pc_flags=0x%x initial=0x%x clear=%u clear_format=%u\n",
                static_cast<unsigned long>(hr), va, unsigned(pc_desc.Dimension),
                static_cast<unsigned long long>(pc_desc.Width), pc_desc.Height, unsigned(pc_desc.DepthOrArraySize),
                static_cast<unsigned long long>(pc_desc.Alignment),
                unsigned(pc_desc.MipLevels), unsigned(pc_desc.Format), pc_desc.SampleDesc.Count,
                pc_desc.SampleDesc.Quality, unsigned(pc_desc.Layout), xbox_flags, unsigned(pc_desc.Flags),
                initial_layout, unsigned(clear != nullptr), unsigned(clear ? clear_copy.Format : DXGI_FORMAT_UNKNOWN));
    }
    return hr;
}

// A measured console producer writes three compute group counts, consumed by Compute PSOs through argument
// type105. Keep that signature distinct after translating its payload to the PC dispatch representation: the
// COM recorder admits only the observed single-command/no-count-buffer execution form. Native PC callers and
// other console descriptor shapes retain their existing validation/refusal.
HRESULT proxy_create_command_signature(DeviceProxy* self, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                        ID3D12RootSignature* root, const IID* riid, void** out) {
    note_slot(41);
    if (out) *out = nullptr;
    if (!riid) return E_POINTER;
    constexpr std::uint32_t console_groups = 105;
    std::uint32_t raw_type = 0;
    if (self->console_lists && desc && desc->NumArgumentDescs && desc->pArgumentDescs) {
        static_assert(sizeof(desc->pArgumentDescs[0].Type) == sizeof(raw_type));
        // Console values exceed the PC SDK enum's range; inspect the representation without loading that enum.
        std::memcpy(&raw_type, &desc->pArgumentDescs[0].Type, sizeof(raw_type));
    }
    if (!self->console_lists || !desc || !desc->NumArgumentDescs || !desc->pArgumentDescs ||
        raw_type != console_groups)
        return self->real->CreateCommandSignature(desc, root, *riid, out);
    if (desc->NumArgumentDescs != 1 || desc->ByteStride != sizeof(D3D12_DISPATCH_ARGUMENTS) ||
        desc->NodeMask != 0 || root)
        return E_NOTIMPL;
    // The descriptor's first extension word is explicitly zero in the measured creation form.
    std::uint32_t extension = 0;
    std::memcpy(&extension, reinterpret_cast<const std::byte*>(desc->pArgumentDescs) + sizeof(console_groups),
                sizeof(extension));
    if (extension != 0) return E_NOTIMPL;
    if (!out) return E_UNEXPECTED;
#ifdef LEONIDA_VULKAN_DEVICE
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    auto translated = *desc;
    translated.pArgumentDescs = &argument;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature;
    HRESULT hr = self->real->CreateCommandSignature(&translated, nullptr, IID_PPV_ARGS(&signature));
    if (FAILED(hr)) return hr;
    if (!rt::d3d12::tag_console_group_dispatch_signature(signature.Get())) return E_NOTIMPL;
    hr = signature->QueryInterface(*riid, out);
    if (SUCCEEDED(hr)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: console indirect105 admitted as a single compute group-count record\n");
    }
    return hr;
#else
    return E_NOTIMPL;
#endif
}

bool snapshot_srv_at_address(const void* resource, const void* view, D3D12_RESOURCE_DESC* resource_copy,
                             D3D12_SHADER_RESOURCE_VIEW_DESC* view_copy, std::uint64_t* address) {
    static_assert(sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) == 0x28);
    __try {
        std::memcpy(resource_copy, resource, sizeof(*resource_copy));
        std::memcpy(view_copy, view, sizeof(*view_copy));
        std::memcpy(address, static_cast<const std::uint8_t*>(view) + 0x28, sizeof(*address));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool snapshot_native_pipeline_header(const void* source, std::uint64_t* words, const IID* iid, IID* iid_copy) {
    __try {
        std::memcpy(words, source, 16 * sizeof(*words));
        std::memcpy(iid_copy, iid, sizeof(*iid_copy));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool clear_native_pipeline_output(void** out) {
    __try {
        *out = nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Microsoft's public GDK sample constructs D3D12XBOX_DESERIALIZE_GRAPHICS_PIPELINE_STATE from root signature,
// packet/metadata pairs and per-stage GPU-instruction/metadata pairs, then calls this three-argument method:
// https://github.com/microsoft/Xbox-GDK-Samples/blob/b1e77b6ac10f95b07296025fda171ca2ddc42dbe/Samples/Graphics/HlslCompile/AcoPipelineState.cpp
// The observed packet is still opaque here: shader fingerprints alone cannot recover fixed-function PSO state.
std::uint64_t proxy_unmodelled_native_pipeline(DeviceProxy* self, const std::uint64_t* arguments) {
    const auto description = arguments[0], iid = arguments[1], out = arguments[2], caller = arguments[3];
    note_slot(57);
    std::uint64_t words[16]{};
    IID requested{};
    const bool readable = snapshot_native_pipeline_header(reinterpret_cast<const void*>(description), words,
        reinterpret_cast<const IID*>(iid), &requested);
    const bool pipeline_iid = readable && IsEqualGUID(requested, __uuidof(ID3D12PipelineState));
    static std::atomic<unsigned> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 2) {
        std::fprintf(stderr, "[rt] d3d12_xs: native pipeline slot57 header readable=%u pipeline_state_iid=%u "
                             "description=0x%llx iid=0x%llx out=0x%llx\n",
                     static_cast<unsigned>(readable),
                     static_cast<unsigned>(pipeline_iid),
                     description, iid, out);
        if (readable) {
            std::fprintf(stderr, "[rt] d3d12_xs: native pipeline slot57 header words");
            for (std::uint64_t word : words) std::fprintf(stderr, " %016llx", word);
            std::fprintf(stderr, "\n");
            // Identify the six observed spans by framing/hash only. Raw programs and state stay out of logs.
            for (unsigned part = 0; part < 6; ++part) {
                const std::uint64_t address = words[1 + 2 * part], size = words[2 + 2 * part];
                if (!address || size < 4 || size > (1u << 20)) continue;
                try {
                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                    SIZE_T copied = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), bytes.data(),
                                            bytes.size(), &copied) || copied != bytes.size()) continue;
                    std::uint32_t magic = 0;
                    std::memcpy(&magic, bytes.data(), sizeof(magic));
                    std::fprintf(stderr, "[rt] d3d12_xs: native pipeline slot57 part%u size=%llu first_word=0x%08x sha256=%s\n",
                                 part, size, magic, leonida::sha256_hex(bytes).c_str());
                    if ((part == 2 || part == 4) && (magic & 0xffff0000u) == 0xd5800000u) {
                        unsigned records = 0;
                        for (std::size_t at = 0; at + 12 <= bytes.size() && records < 8; at += 12) {
                            std::uint32_t tag = 0;
                            std::memcpy(&tag, bytes.data() + at, sizeof(tag));
                            if ((tag & 0xffff0000u) != 0xd5800000u) break;
                            ++records;
                        }
                        std::fprintf(stderr, "[rt] d3d12_xs: native pipeline slot57 part%u leading_d580_records=%u\n",
                                     part, records);
                        const std::span<const std::uint8_t> program(bytes.data(), bytes.size());
                        for (std::size_t start : {0u, 24u, 36u, 40u, 48u, 64u}) {
                            if (start >= program.size()) continue;
                            std::size_t end = 0;
                            for (std::size_t at = start; at + 4 <= program.size(); at += 4) {
                                std::uint32_t instruction = 0;
                                std::memcpy(&instruction, program.data() + at, sizeof(instruction));
                                if (instruction == 0xbf810000u) end = at + 4;
                            }
                            std::fprintf(stderr, "[rt] d3d12_xs: native pipeline slot57 part%u suffix_start=%zu "
                                                 "bytes=%zu sha256=%s\n", part, start, program.size() - start,
                                         leonida::sha256_hex(program.subspan(start)).c_str());
                            if (end > start)
                                std::fprintf(stderr, "[rt] d3d12_xs: native pipeline slot57 part%u candidate_start=%zu "
                                                     "candidate_end=%zu bytes=%zu instruction_sha256=%s\n",
                                             part, start, end, end - start,
                                             leonida::sha256_hex(program.subspan(start, end - start)).c_str());
                        }
                    }
                } catch (const std::bad_alloc&) {
                    break;
                }
            }
        }
    }
#ifdef LEONIDA_VULKAN_DEVICE
    char mode[32]{};
    if (GetEnvironmentVariableA("LEONIDA_XBOX_NATIVE_PIPELINES", mode, sizeof(mode)) == 15 &&
        std::strcmp(mode, "refuse_observed") == 0 && pipeline_iid && words[0] && words[1] && words[2] && out &&
        rt::shims::module_offset(caller) == "game_scarlett_master.exe+0x247757e") {
        bool bounded = true;
        for (unsigned part = 0; part < 6; ++part) {
            const std::uint64_t address = words[1 + 2 * part], size = words[2 + 2 * part];
            bounded &= (address != 0) == (size != 0);
            bounded &= size <= (64u << 20) && size % 4 == 0 && address <= UINT64_MAX - size;
        }
        if (bounded && clear_native_pipeline_output(reinterpret_cast<void**>(out))) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr, "[rt] d3d12_xs: EXPERIMENTAL native pipeline cache precreation refused with "
                                     "E_NOTIMPL; no pipeline object created; title handles the cache miss\n");
            return hr_to_u64(E_NOTIMPL);
        }
    }
#endif
    // The measured DeserializeGraphicsPipelineStateX call requests a PipelineState. Forwarding it to PC slot 57,
    // CreateLifetimeTracker, reports an unrelated failure and cannot deserialize the title's native pipeline.
    rt::shims::unimplemented_proxy_effect("device", 57, caller, reinterpret_cast<std::uint64_t>(self),
                                           description, iid, out);
    return hr_to_u64(E_NOTIMPL);
}

bool snapshot_sampler_x(const void* source, std::uint32_t* words) {
    __try {
        std::memcpy(words, source, 27 * sizeof(*words));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool publish_page_pool_handle(std::uintptr_t* out, std::uintptr_t value) {
    __try {
        *out = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT proxy_register_page_pool(DeviceProxy* self, std::uintptr_t address, UINT pages, std::uintptr_t* out) {
    note_slot(74);
    constexpr std::uintptr_t page_bytes = 64 * 1024;
    if (!out) return E_POINTER;
    if (!address || address % page_bytes || !pages || pages > (UINTPTR_MAX - address) / page_bytes)
        return E_INVALIDARG;
    const auto end = address + std::uintptr_t(pages) * page_bytes;
    for (auto at = address; at < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return E_INVALIDARG;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize > UINTPTR_MAX - base || base + mbi.RegionSize <= at) return E_INVALIDARG;
        at = base + mbi.RegionSize;
    }
    try {
        std::lock_guard lock(self->page_pool_mutex);
        if (self->page_pools.size() >= 4096) return E_OUTOFMEMORY;
        auto handle = g_next_page_pool.load(std::memory_order_relaxed);
        for (;;) {
            if (!handle || handle == UINTPTR_MAX) return E_OUTOFMEMORY;
            if (g_next_page_pool.compare_exchange_weak(handle, handle + 1, std::memory_order_relaxed)) break;
        }
        // Registration records already-mapped caller backing; resources import it lazily through
        // CreatePlacedResourceX. Registration and removal do not allocate, remap or free the title's pages.
        self->page_pools.emplace(handle, DeviceProxy::PagePool{address, pages});
        if (!publish_page_pool_handle(out, handle)) {
            self->page_pools.erase(handle);
            return E_POINTER;
        }
        if (d3d12_trace::enabled())
            std::fprintf(stderr, "[rt] d3d12_xs: registered page pool %zu, %u mapped 64 KiB pages at %p\n",
                         handle, pages, reinterpret_cast<void*>(address));
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

HRESULT proxy_unregister_page_pool(DeviceProxy* self, std::uintptr_t handle) {
    note_slot(75);
    std::lock_guard lock(self->page_pool_mutex);
    return self->page_pools.erase(handle) ? S_OK : E_INVALIDARG;
}

std::uint64_t proxy_create_sampler_x(DeviceProxy* self, std::uint64_t description, std::uint64_t destination,
                                      std::uint64_t caller) {
    note_slot(61);
    std::uint32_t words[27]{};
    HRESULT hr = E_NOTIMPL;
#ifdef LEONIDA_VULKAN_DEVICE
    if (snapshot_sampler_x(reinterpret_cast<const void*>(description), words)) {
        hr = rt::d3d12::create_console_sampler(self->real, words,
            translate_cpu_handle(self, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(destination)}));
    }
#endif
    d3d12_trace::result("Xbox CreateSamplerX (device slot 61)", hr);
    if (FAILED(hr)) {
        static std::atomic<unsigned> refused{0};
        if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::fprintf(stderr, "[rt] d3d12_xs: refused samplerX words");
            for (std::uint32_t word : words) std::fprintf(stderr, " %08x", word);
            std::fprintf(stderr, "\n");
        }
        rt::shims::unimplemented_proxy_effect("device", 61, caller, reinterpret_cast<std::uint64_t>(self),
                                               description, destination, 0);
    }
    return 0;  // console method returns void
}

std::uint64_t proxy_create_srv_at_address(DeviceProxy* self, const std::uint64_t* arguments) {
    const auto resource = arguments[0], view = arguments[1], destination = arguments[2], caller = arguments[3];
    note_slot(70);
    D3D12_RESOURCE_DESC resource_copy{};
    D3D12_SHADER_RESOURCE_VIEW_DESC view_copy{};
    std::uint64_t address = 0;
    HRESULT hr = E_NOTIMPL;
#ifdef LEONIDA_VULKAN_DEVICE
    if (snapshot_srv_at_address(reinterpret_cast<const void*>(resource), reinterpret_cast<const void*>(view),
                                &resource_copy, &view_copy, &address))
        hr = rt::d3d12::create_shader_resource_view_at_address(
            self->real, resource_copy, view_copy, address, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(destination)});
#endif
    d3d12_trace::result("Xbox CreateShaderResourceView at address (device slot 70)", hr);
    if (FAILED(hr)) {
        static std::atomic<unsigned> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr, "[rt] d3d12_xs: refused address SRV resource dim=%u size=%llux%u layers=%u mips=%u "
                                 "format=%u flags=0x%x layout=0x%x VA=0x%llx view_dim=%u view_format=%u\n",
                         static_cast<unsigned>(resource_copy.Dimension), resource_copy.Width, resource_copy.Height,
                         resource_copy.DepthOrArraySize, resource_copy.MipLevels, static_cast<unsigned>(resource_copy.Format),
                         static_cast<unsigned>(resource_copy.Flags), static_cast<unsigned>(resource_copy.Layout), address,
                         static_cast<unsigned>(view_copy.ViewDimension), static_cast<unsigned>(view_copy.Format));
        rt::shims::unimplemented_proxy_effect("device", 70, caller, reinterpret_cast<std::uint64_t>(self),
                                               resource, view, destination);
    }
    return hr_to_u64(hr);
}

// Device slot 26, GetCustomHeapProperties, as the retail executable calls it. Measured on 1.0.2.1 under
// gta6rt --run with a register probe in this slot's thunk: three calls right after device creation, each with the
// device proxy in RDX and not in RCX, R8 = 1 and R9 = 1, 2, 3 (node mask 1; DEFAULT, UPLOAD, READBACK). So the
// buffer for the returned D3D12_HEAP_PROPERTIES is in RCX and the device in RDX. That is how x64 passes them to a
// plain function that takes the device as its first argument and returns a struct; a PC C++ method has the device
// in RCX and the buffer in RDX. The forwarder took RCX for the proxy, read the PC vtable out of the title's stack
// buffer and faulted. The buffer's address goes back in RAX, as that convention returns it; what the title does with
// RAX is not measured.
constexpr unsigned kCreateCommittedResource3X = 127;
constexpr unsigned kPcCreateCommittedResource3 = 76;  // ID3D12Device10, the PC device's 77th method
constexpr std::uint32_t kPcResourceFlagsMask =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
    D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
    D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

HRESULT proxy_create_committed_resource3_x(DeviceProxy* self, std::uint64_t heap_word, std::uint64_t heap_flags,
                                           std::uint64_t desc_word, std::uint64_t layout_word,
                                           std::uint64_t clear_word, std::uint64_t session_word,
                                           std::uint64_t castable_count, std::uint64_t castable_formats,
                                           std::uint64_t riid_word, std::uint64_t out_word) {
    note_slot(kCreateCommittedResource3X);
    auto* const out = reinterpret_cast<void**>(out_word);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    const auto* const xbox_desc = reinterpret_cast<const D3D12_RESOURCE_DESC1*>(desc_word);
    const auto* const riid = reinterpret_cast<const IID*>(riid_word);
    if (!out || !xbox_desc || !riid) return E_POINTER;
    if (self->slot_count <= kPcCreateCommittedResource3) {  // the PC device predates ID3D12Device10
        static std::atomic<bool> warned{false};
        if (first_call(warned))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: device slot %u (%s) needs ID3D12Device10 and the PC device has %u slots; "
                         "returning E_NOTIMPL\n",
                         kCreateCommittedResource3X, slot_name(kCreateCommittedResource3X), self->slot_count);
        return E_NOTIMPL;
    }

    D3D12_RESOURCE_DESC1 pc_desc = *xbox_desc;
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlagsMask);

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 127 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x, xbox flags "
                     "0x%x -> PC flags 0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout),
                     xbox_flags, static_cast<unsigned>(pc_desc.Flags));

    using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                const D3D12_RESOURCE_DESC1*, std::uint32_t, const D3D12_CLEAR_VALUE*,
                                ID3D12ProtectedResourceSession*, std::uint32_t, const DXGI_FORMAT*, const IID*,
                                void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[kPcCreateCommittedResource3]);
    return create(self->real, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(heap_word),
                  static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(heap_flags)), &pc_desc,
                  static_cast<std::uint32_t>(layout_word), reinterpret_cast<const D3D12_CLEAR_VALUE*>(clear_word),
                  reinterpret_cast<ID3D12ProtectedResourceSession*>(session_word),
                  static_cast<std::uint32_t>(castable_count), reinterpret_cast<const DXGI_FORMAT*>(castable_formats),
                  riid, out);
}

template <unsigned Slot>
std::uint64_t WINAPI proxy_slot(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    [[maybe_unused]] rt::d3d12::XboxResourceCall<Slot == 25 || Slot == 27 || Slot == 29 || Slot == 76 ||
                                                 Slot == 108 || Slot == 127 || Slot == 133> resource_call;
    if constexpr (Slot == 57) {
        if (self->console_lists) {
            // The native call has four register arguments including this. Keep the diagnostic caller in owned
            // storage: adding it as a fifth helper argument could overwrite the title's frame in a tail call.
            const std::uint64_t arguments[] = {a1, a2, a3, reinterpret_cast<std::uint64_t>(_ReturnAddress())};
            return proxy_unmodelled_native_pipeline(self, arguments);
        }
    }
    if constexpr (Slot == 74) {
        if (self->console_lists)
            return hr_to_u64(d3d12_trace::result("Xbox RegisterPagePool (device slot 74)",
                proxy_register_page_pool(self, static_cast<std::uintptr_t>(a1), static_cast<UINT>(a2),
                                           reinterpret_cast<std::uintptr_t*>(a3))));
    }
    if constexpr (Slot == 75) {
        if (self->console_lists)
            return hr_to_u64(d3d12_trace::result("Xbox UnregisterPagePool (device slot 75)",
                proxy_unregister_page_pool(self, static_cast<std::uintptr_t>(a1))));
    }
    if constexpr (Slot == 61) {
        if (self->console_lists)
            return proxy_create_sampler_x(self, a1, a2, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    }
    if constexpr (Slot == 70) {
        if (self->console_lists) {
            const std::uint64_t arguments[] = {a1, a2, a3, reinterpret_cast<std::uint64_t>(_ReturnAddress())};
            return proxy_create_srv_at_address(self, arguments);
        }
    }
    if constexpr (Slot == 0) {
        return hr_to_u64(proxy_query_interface(self, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return proxy_add_ref(self);
    } else if constexpr (Slot == 2) {
        return proxy_release(self);
    } else if constexpr (Slot == 8) {
        return hr_to_u64(proxy_create_command_queue(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                    reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 9) {
        return hr_to_u64(proxy_create_command_allocator(self, a1, reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 12) {
        return hr_to_u64(proxy_create_command_list(self, a1, a2, a3, a4, reinterpret_cast<const IID*>(a5),
                                                   reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 36) {
        return hr_to_u64(proxy_create_fence(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                            reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 41) {
        return hr_to_u64(proxy_create_command_signature(self,
            reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(a1),
            reinterpret_cast<ID3D12RootSignature*>(a2), reinterpret_cast<const IID*>(a3),
            reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 32) {
        return hr_to_u64(proxy_open_shared_handle(self, a1, reinterpret_cast<const IID*>(a2),
                                                  reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 31 || Slot == 34 || Slot == 35 || Slot == 45 || Slot == 46 || Slot == 50) {
        return hr_to_u64(proxy_unwrapping_call<Slot>(self, a1, a2, a3, a4, a5));
    } else if constexpr (Slot == 75) {
        return hr_to_u64(proxy_create_command_queue1(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                                     reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 14) {
        return hr_to_u64(proxy_create_descriptor_heap(
            self, reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(a1),
            reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 17) {
        proxy_create_cbv(self, reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(a1),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)});
        return 0;
    } else if constexpr (Slot == 18) {
        proxy_create_srv(self, reinterpret_cast<ID3D12Resource*>(a1),
                         reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(a2),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)});
        return 0;
    } else if constexpr (Slot == 19) {
        proxy_create_uav(self, reinterpret_cast<ID3D12Resource*>(a1), reinterpret_cast<ID3D12Resource*>(a2),
                         reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(a3),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a4)});
        return 0;
    } else if constexpr (Slot == 20) {
        proxy_create_rtv(self, reinterpret_cast<ID3D12Resource*>(a1),
                         reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(a2),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)});
        return 0;
    } else if constexpr (Slot == 21) {
        proxy_create_dsv(self, reinterpret_cast<ID3D12Resource*>(a1),
                         reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(a2),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)});
        return 0;
    } else if constexpr (Slot == 22) {
        proxy_create_sampler(self, reinterpret_cast<const D3D12_SAMPLER_DESC*>(a1),
                             D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)});
        return 0;
    } else if constexpr (Slot == 23) {
        proxy_copy_descriptors(self, static_cast<UINT>(a1), reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a2),
                               reinterpret_cast<const UINT*>(a3), static_cast<UINT>(a4),
                               reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a5),
                               reinterpret_cast<const UINT*>(a6),
                               static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(static_cast<UINT>(a7)));
        return 0;
    } else if constexpr (Slot == 24) {
        proxy_copy_descriptors_simple(self, static_cast<UINT>(a1),
                                      D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)},
                                      D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)},
                                      static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(static_cast<UINT>(a4)));
        return 0;
    } else if constexpr (Slot == 25) {
        return proxy_get_resource_allocation_info(self, a1, static_cast<UINT>(a2), static_cast<UINT>(a3),
                                                  reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4));
    } else if constexpr (Slot == 26) {
        return proxy_get_custom_heap_properties(self, a1, static_cast<UINT>(a2),
                                                 static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3)));
    } else if constexpr (Slot == 62) {
        return hr_to_u64(proxy_create_compute_pipeline_state(
            self, reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(a1), a2, a3,
            reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5)));
    } else if constexpr (Slot == 67) {
        return hr_to_u64(proxy_create_compute_pipeline_from_native_code(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 127) {
        if (!self->console_lists)
            return hr_to_u64(proxy_create_committed_resource3_x(self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10));
        return hr_to_u64(proxy_create_resource_layout(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1), static_cast<D3D12_HEAP_FLAGS>(a2),
            reinterpret_cast<const D3D12_RESOURCE_DESC1*>(a3), static_cast<D3D12_BARRIER_LAYOUT>(a4),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<ID3D12ProtectedResourceSession*>(a6),
            static_cast<UINT32>(a7), reinterpret_cast<const DXGI_FORMAT*>(a8), reinterpret_cast<const IID*>(a9),
            reinterpret_cast<void**>(a10)));
    } else if constexpr (Slot == 133) {
        static std::atomic<unsigned> callers{0};
        if (callers.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr, "[rt] d3d12_xs: slot133 caller=%s\n",
                         rt::shims::module_offset(reinterpret_cast<std::uint64_t>(_ReturnAddress())).c_str());
        return hr_to_u64(proxy_create_placed_resource_x(self, reinterpret_cast<void*>(a1),
                                                        reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2),
                                                        static_cast<std::uint32_t>(a3),
                                                        reinterpret_cast<const D3D12_CLEAR_VALUE*>(a4),
                                                        reinterpret_cast<const IID*>(a5),
                                                        reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 70) {
        proxy_create_srv_from_raw_x(self, a1, a2, a3);
        return 0;
    } else if constexpr (Slot == 76) {
        return hr_to_u64(proxy_create_resource(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1),
            static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(a2)),
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3),
            static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(a4)),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<const IID*>(a6),
            reinterpret_cast<void**>(a7)));
    } else if constexpr (Slot == 86) {
        return hr_to_u64(proxy_create_pipeline_state(self, reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DESC*>(a1),
                                                     reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 101) {
        return hr_to_u64(proxy_create_state_object(self, reinterpret_cast<const D3D12_STATE_OBJECT_DESC*>(a1),
                                                   reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 108) {
        return proxy_get_resource_allocation_info2(self, a1, a2, a3, a4, a5,
            reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    } else {
        if constexpr (Slot == 49) {
            // The observed Xbox call has no established PC signature. In particular R8=1 cannot be a PC REFIID.
            rt::shims::unimplemented_proxy_effect("device", Slot, reinterpret_cast<std::uint64_t>(_ReturnAddress()),
                reinterpret_cast<std::uint64_t>(self), a1, a2, a3);
        }
        note_slot(Slot);
        // Xbox device slots, before the PC vtable is indexed: the public HLE's 78/79/80 would otherwise reach
        // CreateReservedResource2/CreateSampler2 or fall past an 80-slot table, and 48/73 would reach unrelated PC
        // methods. The check is before the slot-count refusal because slot 80 is outside a PC ID3D12Device11 table.
        const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
        if (Slot == routes.frame_interval) {
            log_xbox_route_once(Slot, "SetFrameIntervalX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_set_frame_interval_x(self, a1, a2, a3, a4));
        }
        if (Slot == routes.schedule_event) {
            log_xbox_route_once(Slot, "ScheduleFrameEventX", a1, a2, a3, a4, a5);
            if (a3) return hr_to_u64(E_NOTIMPL);  // only the observed null event/ancillary pointer is modeled
            return hr_to_u64(proxy_schedule_frame_event_x(self, a1, a2, a4));
        }
        if (Slot == routes.wait_event) {
            log_xbox_route_once(Slot, "WaitFrameEventX", a1, a2, a3, a4, a5);
            if (a3) return hr_to_u64(E_NOTIMPL);
            return hr_to_u64(proxy_wait_frame_event_x(self, a1, a2, a4, a5));
        }
        if (Slot == routes.frame_statistics) {
            HRESULT hr = S_OK;
            if (call_presentation(self, [&](const PresentationHandler& p) {
                    if (!p.get_frame_statistics) return false;
                    log_xbox_route_once(Slot, "GetFrameStatisticsX", a1, a2, a3, a4, a5);
                    hr = p.get_frame_statistics(p.context, a1, static_cast<UINT>(a2), reinterpret_cast<UINT*>(a3),
                                                reinterpret_cast<void*>(a4));
                    return true;
                }))
                return hr_to_u64(hr);
            // No presenter: no statistics yet (S_FALSE with a zero count, the "not available yet" answer in
            // docs/d3d12_vulkan.md), and never the PC slot 81 (ID3D12Device13::OpenExistingHeapFromAddress1).
            log_xbox_route_once(Slot, "GetFrameStatisticsX", a1, a2, a3, a4, a5);
            if (auto* count = reinterpret_cast<UINT*>(a3)) *count = 0;
            return hr_to_u64(S_FALSE);
        }
        if (Slot == routes.queue_x) {
            log_xbox_route_once(Slot, "CreateCommandQueueX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_create_command_queue_xbox(self, reinterpret_cast<const std::uint32_t*>(a1),
                                                             reinterpret_cast<const IID*>(a2),
                                                             reinterpret_cast<void**>(a3)));
        }
        if (Slot == routes.list_x) {
            log_xbox_route_once(Slot, "CreateCommandListX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_create_command_list_x(self, a1, a2, a3, reinterpret_cast<const IID*>(a4),
                                                         reinterpret_cast<void**>(a5)));
        }
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));

        // The PC vtable entry, called with the PC device as `this`. Integer/pointer arguments, the hidden
        // struct-return pointer and any stack arguments keep their positions; XMM registers are untouched.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&proxy_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_vtable() {
    static void** vtable = [] {
        static void* storage[kMaxSlots];
        fill_vtable(storage, std::make_index_sequence<kMaxSlots>{});
        return storage;
    }();
    return vtable;
}

bool supports(ID3D12Device* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// Highest device interface the PC runtime exposes; its cumulative slot count (from d3d12.h) bounds forwarding.
// Windows SDK 10.0.26100 defines ID3D12Device through ID3D12Device14 only; there is no Device15/Device16 header.
unsigned detect_slot_count(ID3D12Device* real) {
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device14))) return 83;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device13))) return 82;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device12))) return 81;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device11))) return 80;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device10))) return 79;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device9))) return 76;
#endif
    if (supports(real, __uuidof(ID3D12Device8))) return 73;
    if (supports(real, __uuidof(ID3D12Device7))) return 68;
    if (supports(real, __uuidof(ID3D12Device6))) return 66;
    if (supports(real, __uuidof(ID3D12Device5))) return 65;
    if (supports(real, __uuidof(ID3D12Device4))) return 57;
    if (supports(real, __uuidof(ID3D12Device3))) return 51;
    if (supports(real, __uuidof(ID3D12Device2))) return 48;
    if (supports(real, __uuidof(ID3D12Device1))) return 47;
    return 44;
}

}  // namespace

HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out, bool console_lists) {
    if (!real) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    DeviceProxy* proxy = new (std::nothrow) DeviceProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_vtable();
    proxy->real = real;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->slot_count = detect_slot_count(real);
    proxy->console_lists = console_lists;
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->dxgi_vtable = get_dxgi_vtable();
    proxy->adapter_provider = nullptr;
    proxy->gpu_thread_priority.store(0, std::memory_order_relaxed);
    proxy->max_frame_latency.store(3, std::memory_order_relaxed);
    register_live_device(proxy);

    const unsigned slot_count = proxy->slot_count;
    HRESULT hr = proxy_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its reference to `real`.
        unregister_live_device(proxy);
        delete proxy;
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: device proxy %p wraps PC device %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), slot_count);
    proxy_release(proxy);  // drop the creation reference; `out` holds the caller's reference
    return S_OK;
}

D3D_FEATURE_LEVEL max_feature_level(ID3D12Device* device) {
    if (!device) return D3D_FEATURE_LEVEL(0);
    static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                                D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{UINT(std::size(kLevels)), kLevels, D3D_FEATURE_LEVEL(0)};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels))))
        return D3D_FEATURE_LEVEL(0);
    return levels.MaxSupportedFeatureLevel;
}

HRESULT create_host_device(HostCreateDeviceFn create, IUnknown* adapter, ID3D12Device** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!create) return E_NOTIMPL;
    ID3D12Device* device = nullptr;
    const HRESULT hr = create(adapter, kHostDeviceFloor, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (FAILED(hr) || !device) {
        if (device) device->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }
    const D3D_FEATURE_LEVEL level = max_feature_level(device);
    std::fprintf(stderr, "[rt] d3d12_xs: host device created at minimum feature level 11_0; highest level %u_%u\n",
                 unsigned(level) >> 12, (unsigned(level) >> 8) & 0xf);
    *out = device;
    return S_OK;
}

bool is_device_proxy(const void* device) {
    return device && *reinterpret_cast<void* const*>(device) == reinterpret_cast<void*>(get_vtable());
}

void* device_proxy_from_dxgi_device(void* dxgi_device) {
    if (!dxgi_device || *static_cast<void***>(dxgi_device) != get_dxgi_vtable()) return nullptr;
    return dxgi_owner(dxgi_device);
}

void* device_proxy_primary(void* device) {
    if (is_device_proxy(device)) return device;
    return device_proxy_from_dxgi_device(device);
}

bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        // Not installing one silently would leave the host's adapter in its place.
        std::fprintf(stderr, "[rt] d3d12_xs: set_device_adapter_provider: %p is not a device proxy; no provider set\n",
                     device);
        return false;
    }
    proxy->adapter_provider = provider;
    return true;
}

bool is_heap_proxy(const void* heap) {
    return heap && *reinterpret_cast<void* const*>(heap) == reinterpret_cast<void*>(get_heap_vtable());
}

ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap) {
    if (!is_heap_proxy(heap)) return nullptr;
    return static_cast<const HeapProxy*>(heap)->real;
}

void set_descriptor_heap_allocator(DescriptorHeapBackingAlloc alloc, DescriptorHeapBackingFree free) {
    std::lock_guard lock(heap_alloc_mutex());
    g_heap_alloc = alloc;
    g_heap_free = free;
    g_heap_alloc_explicit = (alloc != nullptr || free != nullptr);
}

void set_default_descriptor_heap_allocator(DescriptorHeapBackingAlloc alloc, DescriptorHeapBackingFree free) {
    std::lock_guard lock(heap_alloc_mutex());
    if (g_heap_alloc_explicit) return;
    g_heap_alloc = alloc;
    g_heap_free = free;
}

D3D12_CPU_DESCRIPTOR_HANDLE translate_cpu_descriptor_handle(const void* device, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const auto* proxy = is_device_proxy(device) ? static_cast<const DeviceProxy*>(device) : nullptr;
    return translate_cpu_handle(proxy, handle);
}

unsigned device_proxy_slot_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->slot_count;
}

void set_placed_at_va_pages_hook(PlacedAtVaPagesHook hook) {
    g_placed_at_va_pages.store(hook, std::memory_order_release);
}

void set_console_descriptor_heap_handler(ConsoleDescriptorHeapHandler handler) {
    g_console_descriptor_heap.store(handler, std::memory_order_release);
}

bool set_presentation_handler(void* device, const PresentationHandler* handler) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) return false;
    AcquireSRWLockExclusive(&proxy->presentation_lock);
    proxy->presentation = handler ? *handler : PresentationHandler{};
    ReleaseSRWLockExclusive(&proxy->presentation_lock);
    return true;
}

void set_placed_at_va_handler(PlacedAtVaHandler handler) {
    g_placed_at_va.store(handler, std::memory_order_release);
}

ID3D12Device* device_proxy_real_device(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->real;
}

bool device_slot_overridden(unsigned slot) {
    if (slot <= 2 || slot == 8 || slot == 9 || slot == 12 || slot == 14 || (slot >= 17 && slot <= 26) || slot == 31 || slot == 32 ||
        slot == 34 || slot == 35 || slot == 36 || slot == 45 || slot == 46 || slot == 50 || slot == 57 || slot == 61 || slot == 62 ||
        slot == 67 || slot == 70 || slot == 74 || slot == 75 || slot == 76 || slot == 86 || slot == 101 || slot == 108 || slot == 127 || slot == 133)
        return true;
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    return slot == routes.frame_interval || slot == routes.schedule_event || slot == routes.wait_event ||
           slot == routes.frame_statistics || slot == routes.queue_x || slot == routes.list_x;
}

bool is_queue_proxy(const void* queue) { return is_child_proxy<ChildKind::queue>(queue); }

bool is_list_proxy(const void* list) { return is_list_proxy_impl(list); }

ID3D12CommandList* list_proxy_real_list(const void* list) {
    if (!is_list_proxy_impl(list)) return nullptr;
    return static_cast<const ListProxy*>(list)->real;
}

const char* list_tail_slot_name(unsigned slot, unsigned char console_layout) {
    return list_tail_name(slot, console_layout);
}
std::uint64_t list_slot_hits(unsigned slot) {
    return slot < kListSlots ? ListStats::hits[slot].load(std::memory_order_relaxed) : 0;
}
bool is_fence_proxy(const void* fence) { return is_child_proxy<ChildKind::fence>(fence); }
bool is_allocator_proxy(const void* allocator) { return is_child_proxy<ChildKind::allocator>(allocator); }

ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator) {
    if (!is_allocator_proxy(allocator)) return nullptr;
    return reinterpret_cast<ID3D12CommandAllocator*>(static_cast<const ChildProxy*>(allocator)->real);
}

ID3D12CommandQueue* queue_proxy_real_queue(const void* queue) {
    if (!is_queue_proxy(queue)) return nullptr;
    return reinterpret_cast<ID3D12CommandQueue*>(static_cast<const ChildProxy*>(queue)->real);
}

ID3D12Fence* fence_proxy_real_fence(const void* fence) {
    if (!is_fence_proxy(fence)) return nullptr;
    return reinterpret_cast<ID3D12Fence*>(static_cast<const ChildProxy*>(fence)->real);
}

unsigned queue_proxy_slot_count(const void* queue) {
    return is_queue_proxy(queue) ? static_cast<const ChildProxy*>(queue)->slot_count : 0;
}

unsigned fence_proxy_slot_count(const void* fence) {
    return is_fence_proxy(fence) ? static_cast<const ChildProxy*>(fence)->slot_count : 0;
}

unsigned queue_proxy_table_size() { return kQueueSlots; }
unsigned fence_proxy_table_size() { return kFenceSlots; }
unsigned allocator_proxy_table_size() { return kAllocatorSlots; }
unsigned allocator_proxy_slot_count(const void* allocator) {
    return is_allocator_proxy(allocator) ? static_cast<const ChildProxy*>(allocator)->slot_count : 0;
}

unsigned long command_object_proxy_ref_count(const void* object) {
    if (!is_queue_proxy(object) && !is_fence_proxy(object) && !is_allocator_proxy(object)) return 0;
    return static_cast<const ChildProxy*>(object)->refs.load(std::memory_order_relaxed);
}

std::uint64_t queue_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool queue_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t fence_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool fence_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t queue_table_overruns() {
    return ChildStats<ChildKind::queue>::overruns.load(std::memory_order_relaxed);
}

std::uint64_t fence_table_overruns() {
    return ChildStats<ChildKind::fence>::overruns.load(std::memory_order_relaxed);
}

bool command_object_tables_guarded() {
    get_child_vtable<ChildKind::queue>();
    get_child_vtable<ChildKind::fence>();
    get_child_vtable<ChildKind::allocator>();
    return ChildStats<ChildKind::queue>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::fence>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::allocator>::table_end.load(std::memory_order_acquire);
}

std::uint64_t allocator_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool allocator_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t allocator_table_overruns() {
    return ChildStats<ChildKind::allocator>::overruns.load(std::memory_order_relaxed);
}

bool heap_slot_overridden(unsigned slot) {
    return slot <= 2 || slot == 7 || slot == 9 || slot == 10;
}

std::uint64_t device_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kMaxSlots ? g_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool device_slot_first_call_seen(unsigned slot) {
    return slot < kMaxSlots && g_first[slot].load(std::memory_order_relaxed);
}

unsigned long device_proxy_ref_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->refs.load(std::memory_order_relaxed);
}

std::uint64_t heap_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kHeapSlots ? g_heap_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool heap_slot_first_call_seen(unsigned slot) {
    return slot < kHeapSlots && g_heap_first[slot].load(std::memory_order_relaxed);
}

bool is_command_list_proxy(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

ID3D12GraphicsCommandList* command_list_proxy_real_list(const void* list) {
    if (!is_command_list_proxy(list)) return nullptr;
    return static_cast<const ListProxy*>(list)->real;
}

unsigned command_list_proxy_table_size() { return kListSlots; }

bool list_slot_first_call_seen(unsigned slot) {
    return slot < kListSlots && ListStats::first[slot].load(std::memory_order_relaxed);
}

void reset_device_proxy_stats() {
    start_hit_counts();
    for (unsigned slot = 0; slot < kMaxSlots; ++slot) {
        g_hits[slot].store(0, std::memory_order_relaxed);
        g_first[slot].store(false, std::memory_order_relaxed);
        g_beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kHeapSlots; ++slot) {
        g_heap_hits[slot].store(0, std::memory_order_relaxed);
        g_heap_first[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kListSlots; ++slot) {
        ListStats::hits[slot].store(0, std::memory_order_relaxed);
        ListStats::first[slot].store(false, std::memory_order_relaxed);
    }
    reset_child_stats<ChildKind::queue>();
    reset_child_stats<ChildKind::fence>();
    reset_child_stats<ChildKind::allocator>();
}

}  // namespace rt::d3d12_xs
