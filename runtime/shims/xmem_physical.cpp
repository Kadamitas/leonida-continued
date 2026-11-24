// xmem.dll's physical pages: XMemAllocatePhysicalPages, XMemFreePhysicalPages, XMemMapPhysicalPages and
// XMemMapPhysicalPagesScatter, and the reservations they are mapped into.
//
// The shapes come from the retail executable's call sites (1.0.2.1; four callers of the allocation, three of the free,
// eleven of the map and two of the scatter form). They follow the Win32 AWE functions with the process handle left
// out and an attribute word added:
//   BOOL  XMemAllocatePhysicalPages(ULONG64 attributes, ULONG_PTR* count, ULONG_PTR* page_array)
//           count is in/out, in 64 KiB pages; the callers require TRUE and the count written back unchanged.
//           Measured attribute words: 0x180, 0x280, 0x380 (not decoded; recorded in the startup trace).
//   BOOL  XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* page_array)        count by value
//   PVOID XMemMapPhysicalPages(PVOID va, ULONG_PTR count, ULONG_PTR* page_array)
//           the callers test the full 64-bit result for non-null; a null page_array unmaps the range.
//   BOOL  XMemMapPhysicalPagesScatter(PVOID* va_array, ULONG_PTR count, ULONG_PTR* page_array)
//           a null page_array unmaps every listed page; a caller writes 0 entries for pages it leaves unmapped.
// The pages are mapped into ranges reserved with XMemVirtualAlloc(nullptr, size, MEM_RESERVE | MEM_LARGE_PAGES |
// MEM_PHYSICAL, attributes, protect), MEM_LARGE_PAGES | MEM_PHYSICAL being the console's 64 KiB page mode; the
// executable makes two 24 GiB ones at startup and smaller ones per pool. It frees them with VirtualFree.
//
// The page-array entries are frame numbers the title treats as opaque, except that one caller rebuilds an entry as a
// 64 KiB page index times 16, masked to 25 bits. So an entry here is a 4 KiB frame number of a 64 KiB page: (slot + 1)
// * 16, never 0 (0 means "unmapped" in the scatter form), and the store has at most 2^21 - 1 slots (128 GiB).
//
// The host has no user-mode physical pages without SeLockMemoryPrivilege (AWE), and AWE pages cannot be mapped at two
// addresses at once, which the console allows (the two 24 GiB reservations differ only in their attribute word and
// CPU protection). So a page is a 64 KiB slot of a pagefile-backed section (sections of 512 slots), and a mapping is a
// view of that slot: the reservation is a placeholder (VirtualAlloc2 with MEM_RESERVE_PLACEHOLDER), a page is split
// out of it and replaced with a view (MapViewOfFile3 with MEM_REPLACE_PLACEHOLDER), and unmapping puts the
// placeholder back (UnmapViewOfFile2 with MEM_PRESERVE_PLACEHOLDER). One page can then sit at any number of addresses
// and every view sees the same bytes, as on the console. New pages read as zero, a freed page is zeroed before it is
// handed out again, and freeing a page unmaps it everywhere (as FreeUserPhysicalPages does).
//
// Not done: the GPU side. These pages are CPU memory only; the D3D12 layer does not see them yet. #5381's
// PhysicalPageStore is the same mechanism with a device-local half and dirty tracking, and can take this store's
// sections. Committing ordinary memory inside a physical reservation fails (ERROR_INVALID_ADDRESS), as on Win32.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "shims.h"
#include "startup_trace.h"
#include "xmem_physical.h"
#include "xmem_control_pages.h"
#include "xmem.h"
#include "xmem_pages.h"

namespace {

constexpr SIZE_T kPage = 0x10000;
constexpr ULONG_PTR kFramesPerPage = kPage / 0x1000;
constexpr uint32_t kChunkPages = 512;
constexpr uint32_t kMaxSlots = (1u << 21) - 1;

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*,
                                        ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

// Looked up rather than imported, so the runtime still starts where they are missing (before Windows 10 1803, or a
// Wine without placeholders); physical reservations are then ordinary ones and nothing can be mapped into them.
struct PlaceholderApi {
    VirtualAlloc2Fn alloc2 = nullptr;
    MapViewOfFile3Fn map3 = nullptr;
    UnmapViewOfFile2Fn unmap2 = nullptr;
    bool ok() const { return alloc2 && map3 && unmap2; }
};

const PlaceholderApi& api() {
    static const PlaceholderApi a = [] {
        PlaceholderApi r;
        if (HMODULE k = GetModuleHandleW(L"kernelbase.dll")) {
            r.alloc2 = reinterpret_cast<VirtualAlloc2Fn>(reinterpret_cast<void*>(GetProcAddress(k, "VirtualAlloc2")));
            r.map3 = reinterpret_cast<MapViewOfFile3Fn>(reinterpret_cast<void*>(GetProcAddress(k, "MapViewOfFile3")));
            r.unmap2 =
                reinterpret_cast<UnmapViewOfFile2Fn>(reinterpret_cast<void*>(GetProcAddress(k, "UnmapViewOfFile2")));
        }
        return r;
    }();
    return a;
}

struct Chunk {
    HANDLE section;
    uint8_t* view;  // the store's own view of the whole section, for zeroing a reused page
};

struct Slot {
    bool allocated : 1 = false;
    bool needs_zero : 1 = false;
    bool committed : 1 = false;
    std::uint16_t physical_store_writes = 0;
    std::uint32_t allocation_epoch = 0;
    std::vector<uintptr_t> mapped_at;
};

struct Region {
    SIZE_T pages = 0;
    DWORD protect = PAGE_NOACCESS;
    std::vector<DWORD> page_protect;  // subranges can select distinct CPU protections
    std::vector<uint32_t> page;     // slot + 1 mapped at each page, 0 for none
    std::vector<uint8_t> isolated;  // the page is a placeholder or view of its own
};

std::mutex g_mutex;
std::vector<Chunk> g_chunks;
std::vector<Slot> g_slots;
std::vector<uint32_t> g_free;            // free slots, the next one at the back
std::map<uintptr_t, std::size_t> g_import_pins;  // fixed guest page -> live coherent import count
size_t g_committed_slots = 0;            // backed slots in the section store, including freed slots retained for reuse
std::map<uintptr_t, Region> g_regions;  // physical reservations by base

#ifdef LEONIDA_XMEM_TEST_HOOKS
void* g_fail_unmap_once = nullptr;
DWORD g_fail_release_once = 0;
#endif

BOOL unmap_view(void* address) {
#ifdef LEONIDA_XMEM_TEST_HOOKS
    if (address == g_fail_unmap_once) {
        g_fail_unmap_once = nullptr;
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
#endif
    return api().unmap2(GetCurrentProcess(), address, MEM_PRESERVE_PLACEHOLDER);
}

BOOL free_placeholder(void* address, SIZE_T size, DWORD free_type) {
#ifdef LEONIDA_XMEM_TEST_HOOKS
    if (free_type == g_fail_release_once) {
        g_fail_release_once = 0;
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
#endif
    return VirtualFree(address, size, free_type);
}

ULONG_PTR frame_of(uint32_t slot) { return (static_cast<ULONG_PTR>(slot) + 1) * kFramesPerPage; }

bool slot_of(ULONG_PTR frame, uint32_t& slot) {
    if (frame == 0 || frame % kFramesPerPage != 0) return false;
    const ULONG_PTR index = frame / kFramesPerPage - 1;
    if (index >= g_slots.size() || !g_slots[index].allocated) return false;
    slot = static_cast<uint32_t>(index);
    return true;
}

template <typename Container>
void reserve_metadata(Container& entries, size_t needed, size_t limit) {
    const size_t capacity = entries.capacity();
    if (needed > capacity)
        entries.reserve((std::max)(needed, (std::min)(limit, capacity + (capacity + 1) / 2)));
}

// In this mode a physical-page request reserves space; a later map or direct page write can fail for lack
// of system commit. Every published native view is still fully committed before the guest can use it.
bool commit_on_map() {
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("LEONIDA_XMEM_COMMIT_ON_MAP", value, sizeof(value)) == 1 && value[0] == '1';
    }();
    return enabled;
}

rt::xmem::PhysicalStoreStats physical_store_stats() noexcept {
    const std::lock_guard lock(g_mutex);
    return {g_slots.size(), g_slots.size() - g_free.size(), g_committed_slots, commit_on_map()};
}

// Called under g_mutex, within one 32 MiB section. Batch contiguous uncommitted slots into one native call.
bool commit_slots(uint32_t first, uint32_t count) {
    const uint32_t end = first + count;
    while (first < end) {
        if (g_slots[first].committed) {
            ++first;
            continue;
        }
        uint32_t stop = first + 1;
        while (stop < end && !g_slots[stop].committed) ++stop;
        auto* const backing = g_chunks[first / kChunkPages].view + std::size_t(first % kChunkPages) * kPage;
        if (!VirtualAlloc(backing, std::size_t(stop - first) * kPage, MEM_COMMIT, PAGE_READWRITE)) return false;
        const auto previous = g_committed_slots;
        g_committed_slots += stop - first;
        // One report per GiB of backing in the opt-in mode. A page mapped into multiple guest apertures counts once.
        if (previous / 16384 != g_committed_slots / 16384)
            std::fprintf(stderr, "[rt] xmem: physical store slots capacity=%zu allocated=%zu committed=%zu commit_on_map=1\n",
                         g_slots.size(), g_slots.size() - g_free.size(), g_committed_slots);
        for (; first < stop; ++first) g_slots[first].committed = true;
    }
    return true;
}

// encoded_slot returns slot+1, or zero for a placeholder. Real contiguous pages share a single
// commit operation unless they cross a backing section. The caller already validated the entire request.
template <typename EncodedSlot>
bool prepare_slots(std::size_t count, EncodedSlot encoded_slot) {
    if (!commit_on_map()) return true;
    for (std::size_t i = 0; i < count;) {
        const uint32_t encoded = encoded_slot(i++);
        if (!encoded) continue;
        const uint32_t first = encoded - 1;
        uint32_t run = 1;
        while (i < count && run < kChunkPages - first % kChunkPages && encoded_slot(i) == encoded + run) {
            ++run;
            ++i;
        }
        if (!commit_slots(first, run)) return false;
    }
    return true;
}

// Adds sections until `count` slots are free.
bool grow(size_t count) {
    if (count <= g_free.size()) return true;
    constexpr size_t max_chunks = kMaxSlots / kChunkPages;
    const size_t old_chunks = g_chunks.size();
    const size_t additional = 1 + (count - g_free.size() - 1) / kChunkPages;
    if (additional > max_chunks - old_chunks) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    const size_t needed = old_chunks + additional;
    // Reserve metadata in bulk. Only the requested sections acquire native backing.
    reserve_metadata(g_chunks, needed, max_chunks);
    reserve_metadata(g_slots, needed * kChunkPages, max_chunks * kChunkPages);
    reserve_metadata(g_free, g_free.size() + additional * kChunkPages, max_chunks * kChunkPages);
    g_slots.resize(needed * kChunkPages);
    const DWORD mode = commit_on_map() ? SEC_RESERVE : SEC_COMMIT;
    while (g_chunks.size() < needed) {
        constexpr uint64_t bytes = static_cast<uint64_t>(kChunkPages) * kPage;
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | mode,
                                            static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
        void* view = section ? MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0) : nullptr;
        if (!view) {
            const DWORD error = GetLastError();
            if (section) CloseHandle(section);
            // None of these sections has published a frame number or a guest view yet.
            while (g_chunks.size() > old_chunks) {
                const Chunk& chunk = g_chunks.back();
                UnmapViewOfFile(chunk.view);
                CloseHandle(chunk.section);
                g_chunks.pop_back();
            }
            g_slots.resize(old_chunks * kChunkPages);
            SetLastError(error);
            return false;
        }
        g_chunks.push_back({section, static_cast<uint8_t*>(view)});
    }
    for (size_t chunk = old_chunks; chunk < needed; ++chunk) {
        const uint32_t first = static_cast<uint32_t>(chunk * kChunkPages);
        for (uint32_t i = kChunkPages; i-- > 0;) {
            g_slots[first + i].committed = mode == SEC_COMMIT;
            g_free.push_back(first + i);
        }
    }
    if (mode == SEC_COMMIT) g_committed_slots += additional * kChunkPages;
    return true;
}

Region* region_of(uintptr_t va, ULONG_PTR count, uintptr_t& base, size_t& index) {
    auto it = g_regions.upper_bound(va);
    if (it == g_regions.begin()) return nullptr;
    --it;
    base = it->first;
    Region& r = it->second;
    if ((va - base) / kPage >= r.pages) return nullptr;
    index = (va - base) / kPage;
    if (count > r.pages - index) return nullptr;
    return &r;
}

uintptr_t page_address(uintptr_t base, size_t index) { return base + index * kPage; }

// Splits page `index` out of the placeholder that holds it, unless it already is a placeholder of its own.
bool isolate(Region& r, uintptr_t base, size_t index) {
    if (r.isolated[index]) return true;
    const uintptr_t a = page_address(base, index);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != a || end != a + kPage) {
        if (!VirtualFree(reinterpret_cast<void*>(a), kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return false;
    }
    r.isolated[index] = 1;
    return true;
}

void forget_mapping(uint32_t slot, uintptr_t a) {
    auto& at = g_slots[slot].mapped_at;
    at.erase(std::remove(at.begin(), at.end(), a), at.end());
}

bool unmap_page(Region& r, uintptr_t base, size_t index) {
    if (r.page[index] == 0) return true;
    const uintptr_t a = page_address(base, index);
    if (!unmap_view(reinterpret_cast<void*>(a))) return false;
    forget_mapping(r.page[index] - 1, a);
    r.page[index] = 0;
    return true;
}

bool map_page(Region& r, uintptr_t base, size_t index, uint32_t slot) {
    if (r.page[index] == slot + 1) return true;
    // Allocate space for the new record before removing the old view. Undoing
    // a replacement can reuse the capacity retained by the old record.
    auto& mappings = g_slots[slot].mapped_at;
    if (mappings.size() == mappings.capacity()) mappings.reserve(mappings.size() * 2 + 1);
    if (!unmap_page(r, base, index) || !isolate(r, base, index)) return false;
    void* const a = reinterpret_cast<void*>(page_address(base, index));
    const Chunk& chunk = g_chunks[slot / kChunkPages];
    const ULONG64 offset = static_cast<ULONG64>(slot % kChunkPages) * kPage;
    const DWORD protect = r.page_protect[index];
    void* view = api().map3(chunk.section, GetCurrentProcess(), a, offset, kPage, MEM_REPLACE_PLACEHOLDER, protect,
                            nullptr, 0);
    if (!view && protect != PAGE_READWRITE) {
        // A protection the view cannot be created with directly (PAGE_NOACCESS on some hosts): map it read-write and
        // narrow it. The placeholder is still in place after the failed call.
        view = api().map3(chunk.section, GetCurrentProcess(), a, offset, kPage, MEM_REPLACE_PLACEHOLDER,
                          PAGE_READWRITE, nullptr, 0);
        DWORD old = 0;
        if (view && !VirtualProtect(view, kPage, protect, &old)) {
            const DWORD error = GetLastError();
            if (!unmap_view(view)) {
                mappings.push_back(page_address(base, index));
                r.page[index] = slot + 1;
            }
            SetLastError(error);
            return false;
        }
    }
    if (!view) return false;
    mappings.push_back(page_address(base, index));
    r.page[index] = slot + 1;
    return true;
}

struct ProtectionRange {
    void* address;
    SIZE_T size;
    DWORD protection;
};

struct MappingChange {
    Region* region;
    uintptr_t base;
    size_t index;
    uint32_t slot;
    uint32_t previous_slot = 0;
    std::vector<ProtectionRange> previous_protections;
};

// A scatter request can repeat pages and addresses, so the number of physical
// slots is not its limit. Only reject sizes the backing containers cannot hold.
bool mapping_count_fits(ULONG_PTR count) {
    return count <= std::vector<uint32_t>{}.max_size() && count <= std::vector<MappingChange>{}.max_size();
}

bool capture_protections(MappingChange& change) {
    const uintptr_t base = page_address(change.base, change.index);
    const uintptr_t end = base + kPage;
    for (uintptr_t address = base; address < end;) {
        MEMORY_BASIC_INFORMATION info{};
        auto* const ptr = reinterpret_cast<void*>(address);
        if (!VirtualQuery(ptr, &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT || info.Type != MEM_MAPPED || info.BaseAddress != ptr ||
            info.AllocationBase != reinterpret_cast<void*>(base) || info.RegionSize == 0 ||
            info.RegionSize > end - address) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return false;
        }
        if (info.Protect != change.region->page_protect[change.index])
            change.previous_protections.push_back({ptr, info.RegionSize, info.Protect});
        address += info.RegionSize;
    }
    return true;
}

bool set_mapping(const MappingChange& change, uint32_t slot) {
    if (slot == 0) return unmap_page(*change.region, change.base, change.index);
    return map_page(*change.region, change.base, change.index, slot - 1);
}

bool restore_mappings(const std::vector<MappingChange>& changes, size_t undo) {
    bool restored = true;
    while (undo != 0) {
        const auto& change = changes[--undo];
        if (change.region->page[change.index] == change.previous_slot) continue;
        if (!set_mapping(change, change.previous_slot)) {
            restored = false;
            continue;
        }
        for (const auto& range : change.previous_protections) {
            DWORD old = 0;
            if (!VirtualProtect(range.address, range.size, range.protection, &old)) restored = false;
        }
    }
    return restored;
}

// Restore earlier views if any native operation fails. Capture each old view
// in order so scatter requests with repeated addresses retain their semantics.
bool apply_mappings(std::vector<MappingChange>& changes) {
    // Test the entire batch before an alias or its native page protections can change. The expected
    // slot cannot change inside this batch either, as any operation replacing it is rejected here.
    if (!g_import_pins.empty()) {
        for (const auto& change : changes) {
            if (change.region->page[change.index] != change.slot &&
                g_import_pins.contains(page_address(change.base, change.index))) {
                SetLastError(ERROR_BUSY);
                return false;
            }
        }
    }
    // A failed backing commit leaves all previously published guest views intact.
    if (!prepare_slots(changes.size(), [&](std::size_t i) { return changes[i].slot; })) return false;
    size_t undo = 0;
    DWORD error = ERROR_SUCCESS;
    try {
        for (auto& change : changes) {
            change.previous_slot = change.region->page[change.index];
            if (change.previous_slot != change.slot && change.previous_slot != 0 && !capture_protections(change)) {
                error = GetLastError();
                break;
            }
            ++undo;  // A failed replacement may already have removed its old view.
            if (!set_mapping(change, change.slot)) {
                error = GetLastError();
                if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
                break;
            }
        }
        if (error == ERROR_SUCCESS && undo == changes.size()) return true;
    } catch (const std::bad_alloc&) {
        error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::length_error&) {
        error = ERROR_INVALID_PARAMETER;
    }

    const bool restored = restore_mappings(changes, undo);
    if (!restored) std::fprintf(stderr, "[rt] xmem: could not restore all views after a physical mapping failed\n");
    SetLastError(error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE);
    return false;
}

// The first allocation for each distinct attribute word, to the startup trace (gta6rt --trace).
void note_allocation(uint64_t attributes, ULONG_PTR count, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(attributes).second) return;
    }
    rt::startup_trace::event(
        "xmem_physical",
        "\"api\":\"XMemAllocatePhysicalPages\",\"attributes\":" +
            rt::startup_trace::quote(rt::startup_trace::hex(attributes)) +
            ",\"pages\":" + std::to_string(static_cast<uint64_t>(count)) + ",\"caller\":" +
            rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller))));
}

BOOL WINAPI XMemAllocatePhysicalPages(ULONG64 attributes, ULONG_PTR* count, ULONG_PTR* page_array) {
    note_allocation(attributes, count ? *count : 0, _ReturnAddress());
    if (!count || (*count != 0 && !page_array)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const ULONG_PTR wanted = *count;
    try {
        const std::lock_guard lock(g_mutex);
        if (wanted > kMaxSlots || !grow(static_cast<size_t>(wanted))) {
            *count = 0;
            if (wanted > kMaxSlots) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        for (ULONG_PTR i = 0; i < wanted; ++i) {
            const uint32_t slot = g_free.back();
            g_free.pop_back();
            Slot& s = g_slots[slot];
            if (s.needs_zero) {
                std::memset(g_chunks[slot / kChunkPages].view + static_cast<size_t>(slot % kChunkPages) * kPage, 0,
                            kPage);
                s.needs_zero = false;
            }
            s.allocated = true;
            if (s.allocation_epoch != UINT32_MAX) ++s.allocation_epoch;
            s.physical_store_writes = 0;
            page_array[i] = frame_of(slot);
        }
        return TRUE;
    } catch (const std::bad_alloc&) {
        *count = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        *count = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* page_array) {
    if (count != 0 && !page_array) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    try {
        const std::lock_guard lock(g_mutex);
        // Every entry must name a distinct allocated slot. Reject an impossible
        // count before allocating a vector or reading the caller's array.
        if (count > g_slots.size()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        std::vector<uint32_t> slots(static_cast<size_t>(count));
        for (ULONG_PTR i = 0; i < count; ++i) {
            if (!slot_of(page_array[i], slots[i])) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
        }
        std::vector<uint32_t> sorted = slots;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        g_free.reserve(g_free.size() + slots.size());
        std::vector<MappingChange> changes;
        for (const uint32_t slot : slots) {
            for (const uintptr_t a : g_slots[slot].mapped_at) {
                uintptr_t base = 0;
                size_t index = 0;
                Region* const region = region_of(a, 1, base, index);
                if (!region || region->page[index] != slot + 1) {
                    SetLastError(ERROR_INVALID_ADDRESS);
                    return FALSE;
                }
                changes.push_back({region, base, index, 0});
            }
        }
        // A failed unmap must not recycle a still-live slot. Journal every alias
        // before touching any, then restore the batch on native failure.
        if (!apply_mappings(changes)) return FALSE;
        for (const uint32_t slot : slots) {
            Slot& s = g_slots[slot];
            s.allocated = false;
            s.needs_zero = s.committed;
            g_free.push_back(slot);
        }
        return TRUE;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

// Checks every entry before anything changes, so a bad frame number leaves the mappings as they were.
bool resolve_frames(ULONG_PTR count, const ULONG_PTR* page_array, std::vector<uint32_t>& slots) {
    slots.assign(static_cast<size_t>(count), 0);
    if (!page_array) return true;
    for (ULONG_PTR i = 0; i < count; ++i) {
        if (page_array[i] == 0) continue;
        uint32_t slot = 0;
        if (!slot_of(page_array[i], slot)) return false;
        slots[i] = slot + 1;
    }
    return true;
}

PVOID WINAPI XMemMapPhysicalPages(PVOID va, ULONG_PTR count, ULONG_PTR* page_array) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(va);
    if (!api().ok()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    if (!va || a % kPage != 0 || count == 0 || !mapping_count_fits(count)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    try {
        const std::lock_guard lock(g_mutex);
        uintptr_t base = 0;
        size_t first = 0;
        Region* r = region_of(a, count, base, first);
        std::vector<uint32_t> slots;
        if (!r || !resolve_frames(count, page_array, slots)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        std::vector<MappingChange> changes;
        changes.reserve(count);
        for (ULONG_PTR i = 0; i < count; ++i) changes.push_back({r, base, first + i, slots[i]});
        return apply_mappings(changes) ? va : nullptr;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    } catch (const std::length_error&) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
}

BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* va_array, ULONG_PTR count, ULONG_PTR* page_array) {
    if (!api().ok()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if ((count != 0 && !va_array) || !mapping_count_fits(count)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    try {
        const std::lock_guard lock(g_mutex);
        std::vector<uint32_t> slots;
        if (!resolve_frames(count, page_array, slots)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        std::vector<MappingChange> changes;
        changes.reserve(count);
        for (ULONG_PTR i = 0; i < count; ++i) {
            const uintptr_t a = reinterpret_cast<uintptr_t>(va_array[i]);
            uintptr_t base = 0;
            size_t index = 0;
            Region* const region = a % kPage == 0 ? region_of(a, 1, base, index) : nullptr;
            if (!region) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            changes.push_back({region, base, index, slots[i]});
        }
        return apply_mappings(changes) ? TRUE : FALSE;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

DWORD shared_view_protect(DWORD protect) {
    // The backing sections and their permanent CPU aliases are write-back. All aliases must use that same cache
    // policy; mixed write-combined views also cannot use the coherent external-host-memory GPU import path.
    // Preserve access restrictions and keep the guest's requested cache policy in the allocation trace.
    if (protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] xmem: shared physical-page views use write-back caching for coherent aliases\n");
    }
    return protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
}

void* reserve_physical(SIZE_T size, DWORD protect) {
    protect = shared_view_protect(protect);
    if (!api().ok()) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr,
                         "[rt] xmem: no VirtualAlloc2/MapViewOfFile3 placeholders on this host; physical-page "
                         "reservations are ordinary ones and XMemMapPhysicalPages fails\n");
        }
        return VirtualAlloc(nullptr, size, MEM_RESERVE, protect);
    }
    if (size == 0 || size > (std::numeric_limits<SIZE_T>::max)() - (kPage - 1)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    size = (size + kPage - 1) & ~(kPage - 1);
    void* p = api().alloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                           nullptr, 0);
    if (!p) return nullptr;
    try {
        const std::lock_guard lock(g_mutex);
        Region r;
        r.pages = size / kPage;
        r.protect = protect;
        r.page_protect.assign(r.pages, protect);
        r.page.assign(r.pages, 0);
        r.isolated.assign(r.pages, 0);
        g_regions.emplace(reinterpret_cast<uintptr_t>(p), std::move(r));
    } catch (const std::bad_alloc&) {
        VirtualFree(p, 0, MEM_RELEASE);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    } catch (const std::length_error&) {
        VirtualFree(p, 0, MEM_RELEASE);
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return p;
}

void* reserve_placeholder(void* address, SIZE_T size, DWORD allocation_type, DWORD protect) {
    protect = shared_view_protect(protect);
    constexpr DWORD physical_select = MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | 0x20400000;
    if (!api().ok()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    if (allocation_type == (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER)) {
        // This is the form measured at the title's call site. Explicit-address
        // creation and private/committed replacements need separate semantics.
        if (address || protect != PAGE_NOACCESS) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        return reserve_physical(size, PAGE_NOACCESS);
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    if (allocation_type != physical_select || !address || a % kPage != 0 || size == 0 ||
        size > (std::numeric_limits<SIZE_T>::max)() - (kPage - 1)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const SIZE_T pages = ((size + kPage - 1) & ~(kPage - 1)) / kPage;
    try {
        const std::lock_guard lock(g_mutex);
        uintptr_t base = 0;
        size_t first = 0;
        Region* const region = region_of(a, pages, base, first);
        if (!region) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        // Validate the full selection before changing any metadata. Do not
        // reclassify live views or erase the records of unaffected neighbours.
        for (size_t i = first; i < first + pages; ++i) {
            if (region->page[i] != 0) {
                SetLastError(ERROR_INVALID_ADDRESS);
                return nullptr;
            }
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(page_address(base, i)), &info, sizeof(info))) return nullptr;
            if (info.State != MEM_RESERVE) {
                SetLastError(ERROR_INVALID_ADDRESS);
                return nullptr;
            }
        }
        std::fill(region->page_protect.begin() + first, region->page_protect.begin() + first + pages, protect);
        return address;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

// Merge adjacent placeholders without making any part of the region available
// to unrelated allocations. Called only after all its mapped views are removed.
bool coalesce_region(uintptr_t base, SIZE_T size) {
    SIZE_T merged = 0;
    while (merged < size) {
        auto* const address = reinterpret_cast<void*>(base + merged);
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(address, &info, sizeof(info))) return false;
        if (info.State != MEM_RESERVE || info.AllocationBase != address || info.RegionSize == 0 ||
            info.RegionSize > size - merged) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return false;
        }
        const SIZE_T next_size = merged + info.RegionSize;
        if (merged != 0 && !free_placeholder(reinterpret_cast<void*>(base), next_size,
                                             MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) return false;
        merged = next_size;
    }
    return true;
}

// Unmaps every page of a physical reservation and releases its placeholders. Called with g_mutex held.
bool release_region(std::map<uintptr_t, Region>::iterator it) {
    const uintptr_t base = it->first;
    Region& r = it->second;
    std::vector<MappingChange> changes;
    for (size_t i = 0; i < r.pages; ++i) {
        if (r.page[i] != 0) changes.push_back({&r, base, i, 0});
    }
    if (!apply_mappings(changes)) return false;
    // Coalescing changes placeholder boundaries. A failed release can still
    // restore each view, but isolate() must query and split the new boundaries.
    std::fill(r.isolated.begin(), r.isolated.end(), uint8_t{0});
    if (!coalesce_region(base, r.pages * kPage) || !free_placeholder(reinterpret_cast<void*>(base), 0, MEM_RELEASE)) {
        const DWORD error = GetLastError();
        if (!restore_mappings(changes, changes.size()))
            std::fprintf(stderr, "[rt] xmem: could not restore all views after a physical reservation release failed\n");
        SetLastError(error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE);
        return false;
    }
    g_regions.erase(it);
    return true;
}

// kernel32!VirtualFree as a title calls it: a physical reservation is a set of placeholders and views, which the
// host's VirtualFree(base, 0, MEM_RELEASE) would release only the first of. Everything else goes to the host.
BOOL WINAPI TitleVirtualFree(LPVOID address, SIZE_T size, DWORD free_type) {
    if ((free_type & (MEM_DECOMMIT | MEM_RELEASE)) &&
        !rt::shims::control_pages::revoke(reinterpret_cast<std::uintptr_t>(address), size)) {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    try {
        const std::lock_guard lock(g_mutex);
        auto it = g_regions.upper_bound(a);
        if (it != g_regions.begin()) {
            --it;
            if (a - it->first < it->second.pages * kPage) {
                if (free_type == (MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    Region& region = it->second;
                    const SIZE_T offset = a - it->first;
                    if (a % kPage || !size || size % kPage || size > region.pages * kPage - offset) {
                        SetLastError(ERROR_INVALID_PARAMETER);
                        return FALSE;
                    }
                    const size_t first = offset / kPage;
                    const size_t count = size / kPage;
                    for (size_t i = first; i < first + count; ++i) {
                        if (region.page[i] != 0) {
                            SetLastError(ERROR_INVALID_ADDRESS);
                            return FALSE;
                        }
                    }
                    // This splits the native placeholder without releasing its address space. Keep the logical
                    // parent and page ownership; isolate() will query the new native boundaries when mapping pages.
                    if (!VirtualFree(address, size, free_type)) return FALSE;
                    std::fill(region.isolated.begin() + first, region.isolated.begin() + first + count, uint8_t{0});
                    return TRUE;
                }
                if (a == it->first && size == 0 && free_type == MEM_RELEASE) {
                    if (release_region(it)) return TRUE;
                    return FALSE;
                }
                // Decommitting or releasing part of a physical reservation fails on Win32 as well.
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
        }
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return VirtualFree(address, size, free_type);
}

bool alias_physical_page_ranges(std::span<const rt::xmem::PhysicalPageAliasRange> ranges) noexcept {
    // Bound even repeated destinations by the store's physical capacity, before allocating the journal.
    if (ranges.size() > kMaxSlots) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    size_t total = 0;
    for (const auto& range : ranges) {
        if (range.page_count > kMaxSlots - total) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        total += range.page_count;
        if (!range.page_count) continue;
        const size_t source_pages = range.repeat_source ? 1 : range.page_count;
        if (!range.destination || range.destination % kPage || range.source % kPage ||
            range.page_count > (UINTPTR_MAX - range.destination) / kPage ||
            (range.source && source_pages > (UINTPTR_MAX - range.source) / kPage)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
    }
    if (!total) return true;
    if (!api().ok()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    try {
        const std::lock_guard lock(g_mutex);
        std::vector<MappingChange> changes;
        changes.reserve(total);
        for (const auto& range : ranges) {
            if (!range.page_count) continue;
            uintptr_t destination_base = 0;
            size_t destination_first = 0;
            Region* const destination = region_of(range.destination, range.page_count,
                                                   destination_base, destination_first);
            uintptr_t source_base = 0;
            size_t source_first = 0;
            Region* const source = range.source ? region_of(range.source,
                range.repeat_source ? 1 : range.page_count, source_base, source_first) : nullptr;
            if (!destination || (range.source && !source)) {
                SetLastError(ERROR_INVALID_ADDRESS);
                return false;
            }
            for (size_t i = 0; i < range.page_count; ++i) {
                uint32_t slot = 0;
                if (source) {
                    const size_t index = source_first + (range.repeat_source ? 0 : i);
                    slot = source->page[index];
                    if (!slot || slot > g_slots.size() || !g_slots[slot - 1].allocated) {
                        SetLastError(ERROR_INVALID_ADDRESS);
                        return false;
                    }
                    // Check that the recorded source is still a real native mapped view. Its protection does not
                    // transfer: destination mappings retain the destination reservation's own CPU protection.
                    if (!range.repeat_source || i == 0) {
                        MappingChange observed{source, source_base, index, slot};
                        if (!capture_protections(observed)) return false;
                    }
                }
                changes.push_back({destination, destination_base, destination_first + i, slot});
            }
        }
        return apply_mappings(changes);
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    } catch (const std::length_error&) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
}

void* pin_physical_import(const void* source, std::size_t size) noexcept {
    const auto start = reinterpret_cast<uintptr_t>(source);
    const auto page = start & ~(kPage - 1);
    const auto offset = start - page;
    if (!start || !size || size > kPage - offset || size > UINTPTR_MAX - start) return nullptr;
    const std::lock_guard lock(g_mutex);
    uintptr_t base = 0;
    size_t index = 0;
    const auto* region = region_of(page, 1, base, index);
    if (!region) return nullptr;
    const auto entry = region->page[index];
    if (!entry || entry > g_slots.size() || !g_slots[entry - 1].allocated) return nullptr;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(source, &info, sizeof(info)) || info.AllocationBase != reinterpret_cast<void*>(page) ||
        info.State != MEM_COMMIT || info.Type != MEM_MAPPED || info.Protect != PAGE_NOACCESS) return nullptr;
    const auto region_start = reinterpret_cast<uintptr_t>(info.BaseAddress);
    if (region_start > start || info.RegionSize > UINTPTR_MAX - region_start ||
        region_start + info.RegionSize < start + size) return nullptr;
    try {
        auto& count = g_import_pins[page];
        if (count == SIZE_MAX) return nullptr;
        ++count;
    } catch (const std::bad_alloc&) { return nullptr; }
    const auto slot = entry - 1;
    return g_chunks[slot / kChunkPages].view + std::size_t(slot % kChunkPages) * kPage + offset;
}

void release_physical_import(const void* source) noexcept {
    const auto page = reinterpret_cast<uintptr_t>(source) & ~(kPage - 1);
    const std::lock_guard lock(g_mutex);
    const auto it = g_import_pins.find(page);
    if (it != g_import_pins.end() && !--it->second) g_import_pins.erase(it);
}

constexpr rt::xmem::PhysicalImportOps import_ops{pin_physical_import, release_physical_import};

rt::xmem::PhysicalPageIdentity physical_page_identity(const void* source) noexcept {
    using State = rt::xmem::PhysicalPageState;
    const auto page = reinterpret_cast<uintptr_t>(source) & ~(kPage - 1);
    const std::lock_guard lock(g_mutex);
    uintptr_t base = 0;
    size_t index = 0;
    const auto* region = region_of(page, 1, base, index);
    if (!region) return {};
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(source, &info, sizeof(info))) return {State::inconsistent};
    const auto entry = region->page[index];
    if (!entry) return {info.State == MEM_RESERVE ? State::reserved : State::inconsistent};
    if (entry > g_slots.size() || !g_slots[entry - 1].allocated || info.State != MEM_COMMIT ||
        info.Type != MEM_MAPPED || reinterpret_cast<uintptr_t>(info.AllocationBase) != page ||
        (info.Protect & PAGE_GUARD)) return {State::inconsistent};
    const auto& slot = g_slots[entry - 1];
    return {State::mapped, frame_of(entry - 1), slot.allocation_epoch, slot.physical_store_writes};
}

rt::xmem::PhysicalCpuView find_physical_cpu_view(const void* source, std::size_t size) noexcept {
    rt::xmem::PhysicalCpuView view;
    const auto address = reinterpret_cast<uintptr_t>(source);
    if (!address || !size || size > UINTPTR_MAX - address) return view;
    const auto first_page = address & ~(kPage - 1);
    const auto count = (address + size - 1 - first_page) / kPage + 1;
    const std::lock_guard lock(g_mutex);
    uintptr_t base = 0;
    size_t first = 0;
    Region* origin = region_of(first_page, count, base, first);
    if (!origin) return view;
    view.total_pages = count;
    for (size_t i = 0; i < count; ++i) {
        const auto slot = origin->page[first + i];
        if (slot && slot <= g_slots.size() && g_slots[slot - 1].allocated) ++view.mapped_pages;
    }
    if (view.mapped_pages != count) return view;

    const auto offset = address - first_page;
    auto native_view = [&](uintptr_t start, bool writable) {
        const auto end = start + size;
        for (auto cursor = start; cursor < end;) {
            MEMORY_BASIC_INFORMATION info{};
            const auto page = cursor & ~(kPage - 1);
            if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) || info.State != MEM_COMMIT ||
                info.Type != MEM_MAPPED || reinterpret_cast<uintptr_t>(info.AllocationBase) != page ||
                (writable && ((info.Protect & PAGE_GUARD) || (info.Protect & 0xff) != PAGE_READWRITE))) return false;
            const auto region = reinterpret_cast<uintptr_t>(info.BaseAddress);
            if (region > cursor || info.RegionSize > UINTPTR_MAX - region || region + info.RegionSize <= cursor)
                return false;
            cursor = (std::min)(end, region + info.RegionSize);
        }
        return true;
    };
    if (native_view(address, true)) {
        view.address = const_cast<void*>(source);
        return view;
    }
    if (!native_view(address, false)) return view;
    for (const auto candidate : g_slots[origin->page[first] - 1].mapped_at) {
        if (candidate == first_page) continue;
        uintptr_t alias_base = 0;
        size_t index = 0;
        Region* alias = region_of(candidate, count, alias_base, index);
        if (!alias || candidate > UINTPTR_MAX - offset || size > UINTPTR_MAX - (candidate + offset) ||
            !std::equal(origin->page.begin() + first, origin->page.begin() + first + count, alias->page.begin() + index))
            continue;
        const auto start = candidate + offset;
        if (native_view(start, true)) {
            view.address = reinterpret_cast<void*>(start);
            break;
        }
    }
    return view;
}

rt::xmem::SparseTextureSnapshot snapshot_sparse_texture(const void* source, std::size_t size,
                                                       std::span<std::byte> destination,
                                                       std::span<const std::byte> previous,
                                                       rt::xmem::SparseTextureMismatch* mismatch) noexcept {
    using Result = rt::xmem::SparseTextureSnapshot;
    if (!size) return Result::ready;
    const auto start = reinterpret_cast<uintptr_t>(source);
    if (!start || size > UINTPTR_MAX - start) return Result::failed;
    const auto end = start + size;
    if (!destination.empty()) {
        const auto output = reinterpret_cast<uintptr_t>(destination.data());
        if (!output || destination.size() != size || size > UINTPTR_MAX - output ||
            (start < output + size && output < end)) return Result::failed;
    }
    const std::lock_guard lock(g_mutex);
    const auto next = g_regions.upper_bound(start);
    auto found = next;
    bool tracked = false;
    if (found != g_regions.begin()) {
        --found;
        tracked = (start - found->first) / kPage < found->second.pages;
    }
    if (!tracked) {
        // An ordinary read must not straddle a tracked reservation without its mapping lock.
        return next != g_regions.end() && next->first < end ? Result::failed : Result::untracked;
    }
    const auto base = found->first;
    const auto& region = found->second;
    if (region.pages > (UINTPTR_MAX - base) / kPage || region.page.size() != region.pages ||
        size > region.pages * kPage - (start - base)) return Result::failed;
    // Validate ownership and the real native mapping for every source piece before changing output.
    // PAGE_NOACCESS is valid for the title's GPU-only aliases; it is not changed or read through here.
    MEMORY_BASIC_INFORMATION info{};
    auto native_end = start;
    for (auto cursor = start; cursor < end;) {
        const auto index = (cursor - base) / kPage;
        const auto page_base = page_address(base, index);
        const auto piece_end = (std::min)(end, page_base + kPage);
        const auto slot = region.page[index];
        if (slot && (slot > g_slots.size() || !g_slots[slot - 1].allocated ||
            (slot - 1) / kChunkPages >= g_chunks.size() || !g_chunks[(slot - 1) / kChunkPages].view))
            return Result::failed;
        while (cursor < piece_end) {
            // An unbroken reservation can cover many empty slots. One native query describes the entire
            // run; each slot's allocation, state and protection are still checked under the mapping lock.
            if (cursor == native_end) {
                if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info))) return Result::failed;
                const auto native_base = reinterpret_cast<uintptr_t>(info.BaseAddress);
                if (native_base > cursor || !info.RegionSize || info.RegionSize > UINTPTR_MAX - native_base ||
                    native_base + info.RegionSize <= cursor) return Result::failed;
                native_end = native_base + info.RegionSize;
            }
            if (slot ? (info.State != MEM_COMMIT || info.Type != MEM_MAPPED ||
                        info.AllocationBase != reinterpret_cast<void*>(page_base) || (info.Protect & PAGE_GUARD)) :
                       info.State != MEM_RESERVE) return Result::failed;
            cursor = (std::min)(piece_end, native_end);
        }
    }
    if (destination.empty() && previous.empty()) return Result::ready;
    // Merge consecutive live physical slots into bulk reads, without crossing a permanent backing chunk.
    // Zero runs contain only metadata-confirmed holes whose native reservation was checked above.
    const auto visit_runs = [&](auto&& visit) {
        for (auto cursor = start; cursor < end;) {
            const auto index = (cursor - base) / kPage;
            const auto slot = region.page[index];
            const auto offset = (cursor - base) % kPage;
            auto bytes = (std::min)(std::size_t(end - cursor), std::size_t(kPage - offset));
            auto following = index + 1;
            while (bytes < end - cursor && following < region.pages) {
                const auto next_slot = region.page[following];
                const auto delta = following - index;
                if (slot ? (!next_slot || next_slot != slot + delta ||
                            (next_slot - 1) / kChunkPages != (slot - 1) / kChunkPages) : next_slot != 0) break;
                const auto extra = (std::min)(std::size_t(end - cursor - bytes), std::size_t(kPage));
                bytes += extra;
                ++following;
            }
            const uint8_t* from = nullptr;
            if (slot) {
                const auto physical = slot - 1;
                from = g_chunks[physical / kChunkPages].view +
                    std::size_t(physical % kChunkPages) * kPage + offset;
            }
            if (!visit(cursor - start, from, bytes)) return false;
            cursor += bytes;
        }
        return true;
    };
    // These permanent chunk views belong to the physical store; the mapping lock also protects
    // direct page writes and alias changes. Compare the actual bytes even on GPU-only guest mappings.
    if (previous.size() == size && visit_runs([&](std::size_t at, const uint8_t* from, std::size_t bytes) {
        if (from && !mismatch) return std::memcmp(from, previous.data() + at, bytes) == 0;
        static constexpr std::byte zero_page[kPage]{};
        while (bytes) {
            const auto count = (std::min)(bytes, sizeof(zero_page));
            const void* backing = from ? static_cast<const void*>(from) : zero_page;
            if (std::memcmp(previous.data() + at, backing, count) != 0) {
                if (mismatch) *mismatch = {at, count};
                return false;
            }
            if (from) from += count;
            at += count;
            bytes -= count;
        }
        return true;
    })) return Result::unchanged;
    if (destination.empty()) return Result::ready;
    const bool copied = visit_runs([&](std::size_t at, const uint8_t* from, std::size_t bytes) {
        if (!from) {
            std::memset(destination.data() + at, 0, bytes);
            return true;
        }
        // The checked run stays inside our permanent, readable section view while g_mutex holds the mapping.
        std::memcpy(destination.data() + at, from, bytes);
        return true;
    });
    return copied ? Result::ready : Result::failed;
}

const bool g_registered = (rt::xmem::set_physical_reserve(&reserve_physical),
                           rt::xmem::set_placeholder_reserve(&reserve_placeholder),
                           rt::xmem::detail::physical_store_stats_handler.store(&physical_store_stats, std::memory_order_release),
                           rt::xmem::detail::physical_page_alias_handler.store(&alias_physical_page_ranges,
                               std::memory_order_release),
                           rt::xmem::detail::physical_import_ops.store(&import_ops, std::memory_order_release),
                           rt::xmem::detail::physical_page_identity_handler.store(&physical_page_identity,
                               std::memory_order_release),
                           rt::xmem::detail::physical_cpu_view_handler.store(&find_physical_cpu_view,
                               std::memory_order_release),
                           rt::xmem::detail::sparse_texture_snapshot_handler.store(&snapshot_sparse_texture,
                               std::memory_order_release), true);

}  // namespace

// DirectStorage's staged destination writer. Every slot has a permanent writable backing view; writing it changes
// every mapped alias immediately, including pages which had no guest mapping when the request was submitted.
// Hold the page-store lock through validation and copying so a page cannot be freed/reused between those steps.
// The caller owns stable page numbers and a separate readable staging buffer (xmem_pages.h).
bool rt::xmem::write_physical_pages(const uint64_t* page_array, size_t offset, const void* source, size_t size) {
    if (size == 0) return true;
    if (!page_array || !source || offset >= kPage ||
        size > (std::numeric_limits<size_t>::max)() - offset - (kPage - 1)) return false;
    const size_t count = (offset + size + kPage - 1) / kPage;
    const std::lock_guard lock(g_mutex);
    // Reject every invalid number before changing any bytes, including the validated prefix.
    for (size_t i = 0; i < count; ++i) {
        uint32_t slot;
        if (!slot_of(page_array[i], slot)) return false;
    }
    static const bool trace_writes = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_TRACE_TEXTURE_RANGES", value, 2) == 1 && value[0] == L'1';
    }();
    auto* from = static_cast<const uint8_t*>(source);
    if (!prepare_slots(count, [&](std::size_t i) { return static_cast<uint32_t>(page_array[i] / kFramesPerPage); }))
        return false;
    for (size_t i = 0; i < count; ++i) {
        uint32_t slot;
        if (!slot_of(page_array[i], slot)) std::terminate();  // cannot change while g_mutex is held
        const size_t bytes = (std::min)(size, kPage - offset);
        auto* destination = g_chunks[slot / kChunkPages].view + static_cast<size_t>(slot % kChunkPages) * kPage;
        std::memcpy(destination + offset, from, bytes);
        if (trace_writes && g_slots[slot].physical_store_writes != UINT16_MAX)
            ++g_slots[slot].physical_store_writes;
        from += bytes;
        size -= bytes;
        offset = 0;
    }
    return true;
}

// Source-page requests take a snapshot of the same permanent backing. The
// stable page array and separate output belong to the caller, not to guest
// mappings. Validation/copy share the allocation/free lock, as for writes.
bool rt::xmem::read_physical_pages(const uint64_t* page_array, size_t offset, void* destination, size_t size) {
    if (size == 0) return true;
    if (!page_array || !destination || offset >= kPage ||
        size > (std::numeric_limits<size_t>::max)() - offset - (kPage - 1)) return false;
    const size_t count = (offset + size + kPage - 1) / kPage;
    const std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < count; ++i) {
        uint32_t slot;
        if (!slot_of(page_array[i], slot)) return false;
    }
    auto* to = static_cast<uint8_t*>(destination);
    for (size_t i = 0; i < count; ++i) {
        uint32_t slot;
        if (!slot_of(page_array[i], slot)) std::terminate();  // stable IDs and g_mutex make this invariant
        const size_t bytes = (std::min)(size, kPage - offset);
        const auto* source = g_chunks[slot / kChunkPages].view + static_cast<size_t>(slot % kChunkPages) * kPage;
        // A never-committed slot has had neither a guest mapping nor a direct write. Its contents are zero.
        if (!g_slots[slot].committed) std::memset(to, 0, bytes);
        else std::memcpy(to, source + offset, bytes);
        to += bytes;
        size -= bytes;
        offset = 0;
    }
    return true;
}

#ifdef LEONIDA_XMEM_TEST_HOOKS
void rt::xmem::fail_physical_unmap_once_for_test(void* address) {
    const std::lock_guard lock(g_mutex);
    g_fail_unmap_once = address;
}

void rt::xmem::fail_physical_release_once_for_test(DWORD free_type) {
    const std::lock_guard lock(g_mutex);
    g_fail_release_once = free_type;
}
#endif

#define TITLE_VIRTUAL_FREE(dll) {dll, "VirtualFree", reinterpret_cast<void*>(&TitleVirtualFree)}

SHIM_TABLE(xmem_physical,
    {"xmem.dll", "XMemAllocatePhysicalPages", reinterpret_cast<void*>(&XMemAllocatePhysicalPages)},
    {"xmem.dll", "XMemFreePhysicalPages", reinterpret_cast<void*>(&XMemFreePhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPages", reinterpret_cast<void*>(&XMemMapPhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPagesScatter", reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter)},
    TITLE_VIRTUAL_FREE("kernel32.dll"),
    TITLE_VIRTUAL_FREE("kernelbase.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-0.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-1.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-2.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-3.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-4.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-5.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-6.dll"),
);

// This store commits its section views when pages are mapped. The upstream first-touch hook therefore validates
// that the complete range is already committed; it must not commit private memory over a missing page mapping.
bool rt::xmem::commit_physical_range(const void* address, std::size_t size) {
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    if (!cursor || !size || size > ~std::uintptr_t(0) - cursor) return false;
    const auto end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize > ~std::uintptr_t(0) - base) return false;
        const auto next = base + mbi.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
