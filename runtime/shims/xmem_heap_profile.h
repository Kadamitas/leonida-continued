#pragma once

#include <windows.h>
#include <psapi.h>
#include <malloc.h>
#include <atomic>
#include <cstdio>
#include <cstdint>

namespace rt::xmem::heap_profile {

inline bool enabled() {
    static const bool on = [] {
        const DWORD previous_error = GetLastError();
        char option[2]{};
        const bool set = GetEnvironmentVariableA("LEONIDA_TRACE_HEAP_OWNERS", option, sizeof(option)) == 1 && option[0] == '1';
        SetLastError(previous_error);
        return set;
    }();
    return on;
}

// Query only the current process. This reports the whole reservation, including uncommitted holes.
inline bool large_private_region(const void* address, WIN32_MEMORY_REGION_INFORMATION& region) {
    static const auto query = reinterpret_cast<decltype(&QueryVirtualMemoryInformation)>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryVirtualMemoryInformation"));
    return query && query(GetCurrentProcess(), address, MemoryRegionInfo, &region, sizeof(region), nullptr) &&
        region.Private && region.RegionSize >= 256ull * 1024 * 1024;
}

inline std::atomic<unsigned long long> allocation_count{}, sample_count{};

inline void sample_allocation(const char* api, const void* address, size_t bytes, uint64_t attributes, const void* caller) {
    if (!enabled() || !address) return;
    const auto sequence = allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (sequence >= 16 && (sequence & 63) != 0 && bytes < 1024 * 1024) return;
    const DWORD previous_error = GetLastError();
    sample_count.fetch_add(1, std::memory_order_relaxed);
    WIN32_MEMORY_REGION_INFORMATION region{};
    if (large_private_region(address, region)) {
        // Results from _aligned_malloc have not reached the caller yet, so their reservation cannot race a free.
        static std::atomic<void*> seen[128]{};
        for (auto& slot : seen) {
            void* earlier = slot.load(std::memory_order_relaxed);
            if (!earlier && slot.compare_exchange_strong(earlier, region.AllocationBase, std::memory_order_relaxed)) {
                std::fprintf(stderr, "[rt] xmem: heap owner api=%s crt=%p base=%p reserve=%zu commit=%zu "
                    "block=%p bytes=%zu attributes=0x%llx caller=%p\n", api, reinterpret_cast<void*>(_get_heap_handle()),
                    region.AllocationBase, region.RegionSize, region.CommitSize, address, bytes,
                    static_cast<unsigned long long>(attributes), caller);
                break;
            }
            if (earlier == region.AllocationBase) break;
        }
    }
    SetLastError(previous_error);
}

inline void report_heap(HANDLE heap) {
    // Only inspect known process-lifetime heaps. Other DLLs can destroy handles returned by GetProcessHeaps.
    static const auto summarize = reinterpret_cast<decltype(&HeapSummary)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
    HEAP_SUMMARY totals{};
    totals.cb = sizeof(totals);
    const bool summary_ok = summarize && summarize(heap, 0, &totals);

    WIN32_MEMORY_REGION_INFORMATION regions[32]{};
    (void)large_private_region(heap, regions[0]);  // Resolve the optional Windows API before taking the heap lock.
    unsigned count = 0, entries = 0;
    bool complete = false;
    // Explicitly bound the walk. No runtime logging or heap allocation while the heap is locked.
    if (HeapLock(heap)) {
        const auto start = GetTickCount64();
        PROCESS_HEAP_ENTRY entry{};
        for (; entries < 65536; ++entries) {
            if ((entries & 127) == 0 && GetTickCount64() - start >= 25) break;
            if (!HeapWalk(heap, &entry)) {
                complete = GetLastError() == ERROR_NO_MORE_ITEMS;
                break;
            }
            if ((entry.wFlags & PROCESS_HEAP_REGION) && count < 32 && large_private_region(entry.lpData, regions[count]))
                ++count;
        }
        HeapUnlock(heap);
    }
    std::fprintf(stderr, "[rt] xmem: heap summary heap=%p valid=%u allocated=%zu committed=%zu reserved=%zu "
        "walk_entries=%u walk_complete=%u large_regions=%u\n", heap, unsigned(summary_ok), totals.cbAllocated,
        totals.cbCommitted, totals.cbReserved, entries, unsigned(complete), count);
    for (unsigned i = 0; i < count; ++i)
        std::fprintf(stderr, "[rt] xmem: heap region heap=%p base=%p reserve=%zu commit=%zu\n",
            heap, regions[i].AllocationBase, regions[i].RegionSize, regions[i].CommitSize);
}

inline std::atomic<bool> reported{};

inline void report() {
    if (!enabled()) return;
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    const DWORD previous_error = GetLastError();
    const HANDLE process_heap = GetProcessHeap();
    const HANDLE crt_heap = reinterpret_cast<HANDLE>(_get_heap_handle());
    std::fprintf(stderr, "[rt] xmem: heap census process=%p crt=%p total_heaps=%lu xmem_allocations=%llu samples=%llu\n",
        process_heap, crt_heap, GetProcessHeaps(0, nullptr), allocation_count.load(), sample_count.load());
    report_heap(process_heap);
    if (crt_heap && crt_heap != process_heap) report_heap(crt_heap);
    SetLastError(previous_error);
}

// Catch late pressure even when reducing arena size moves the first refusal past the original Vulkan import.
inline void check_placement() {
    if (!enabled() || reported.load(std::memory_order_relaxed)) return;
    static std::atomic<unsigned> placements{};
    if (placements.fetch_add(1, std::memory_order_relaxed) % 128) return;
    const DWORD previous_error = GetLastError();
    PROCESS_MEMORY_COUNTERS_EX process{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) &&
        process.PrivateUsage >= 36ull * 1024 * 1024 * 1024)
        report();
    SetLastError(previous_error);
}

}  // namespace rt::xmem::heap_profile
