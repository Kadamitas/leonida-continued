// xmem.dll's physical pages through the registered shims, as a title calls them: XMemAllocatePhysicalPages,
// XMemFreePhysicalPages, XMemMapPhysicalPages, XMemMapPhysicalPagesScatter, the 64 KiB-page reserve-only
// XMemVirtualAlloc they are mapped into and the title's VirtualFree that releases it. The call shapes are the ones in
// xmem_physical.cpp's header comment; everything here is synthetic.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "shims.h"
#include "host_texture_read.h"
#include "xmem_physical.h"
#include "xmem_pages.h"

namespace {

using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using AllocatePhysicalFn = BOOL(WINAPI*)(ULONG64, ULONG_PTR*, ULONG_PTR*);
using FreePhysicalFn = BOOL(WINAPI*)(ULONG_PTR, ULONG_PTR*);
using MapPhysicalFn = PVOID(WINAPI*)(PVOID, ULONG_PTR, ULONG_PTR*);
using MapScatterFn = BOOL(WINAPI*)(PVOID*, ULONG_PTR, ULONG_PTR*);
using VirtualFreeFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);

constexpr SIZE_T kPage = 0x10000;
constexpr DWORD kPhysicalReserve = MEM_RESERVE | 0x20400000;  // MEM_LARGE_PAGES | MEM_PHYSICAL: 64 KiB pages
constexpr DWORD kGraphicsNoAccess = 0x800;
constexpr DWORD kGraphicsReadWrite = 0x2000;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

struct Api {
    XMemVirtualAllocFn virtual_alloc;
    AllocatePhysicalFn allocate;
    FreePhysicalFn free_pages;
    MapPhysicalFn map;
    MapScatterFn scatter;
    VirtualFreeFn virtual_free;
};

uint8_t* at(void* base, size_t page) { return static_cast<uint8_t*>(base) + page * kPage; }

MEMORY_BASIC_INFORMATION query(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(p, &mbi, sizeof(mbi));
    return mbi;
}

bool committed(const void* p) { return query(p).State == MEM_COMMIT; }

void fill(uint8_t* p, uint8_t value) { std::memset(p, value, kPage); }

bool all_equal(const uint8_t* p, uint8_t value) {
    return std::all_of(p, p + kPage, [value](uint8_t b) { return b == value; });
}

void test_allocate(const Api& x) {
    ULONG_PTR count = 4;
    ULONG_PTR frames[4] = {};
    check(x.allocate(0x280, &count, frames) == TRUE, "XMemAllocatePhysicalPages(0x280, 4) returns TRUE");
    check(count == 4, "the page count comes back unchanged");
    bool shape = true;
    for (const ULONG_PTR f : frames) shape &= f != 0 && f % 16 == 0 && (f & ~0x1FFFFF0ull) == 0;
    check(shape, "every frame number is a nonzero multiple of 16 inside 25 bits");
    std::vector<ULONG_PTR> sorted(frames, frames + 4);
    std::sort(sorted.begin(), sorted.end());
    check(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "the four frame numbers differ");

    ULONG_PTR none = 0;
    check(x.allocate(0x180, &none, nullptr) == TRUE && none == 0, "a count of 0 succeeds with no array");
    check(x.allocate(0x280, nullptr, frames) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "a null count is ERROR_INVALID_PARAMETER");
    check(x.free_pages(4, frames) == TRUE, "XMemFreePhysicalPages frees them");
    check(x.free_pages(4, frames) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "freeing them a second time is ERROR_INVALID_PARAMETER");
}

void test_oversized_counts(const Api& x) {
    constexpr ULONG_PTR oversized = (std::numeric_limits<ULONG_PTR>::max)();
    ULONG_PTR count = oversized;
    ULONG_PTR frame = 0x55;
    bool threw = false;
    BOOL result = TRUE;
    try {
        result = x.allocate(0x280, &count, &frame);
    } catch (...) {
        threw = true;
    }
    check(!threw && result == FALSE && count == 0 && frame == 0x55 && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
          "SIZE_MAX allocation count fails without writing the small output array or throwing");

    threw = false;
    try {
        result = x.free_pages(oversized, &frame);
    } catch (...) {
        threw = true;
    }
    check(!threw && result == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "SIZE_MAX free count fails before reading the small input array without throwing");

    void* address = reinterpret_cast<void*>(kPage);
    threw = false;
    try {
        result = x.scatter(&address, oversized, &frame);
    } catch (...) {
        threw = true;
    }
    check(!threw && result == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "SIZE_MAX scatter count fails before reading its small input arrays without throwing");

    threw = false;
    try {
        result = x.scatter(&address, oversized, nullptr);
    } catch (...) {
        threw = true;
    }
    check(!threw && result == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "SIZE_MAX scatter unmap count fails without allocating or throwing");

    threw = false;
    void* mapped = address;
    try {
        mapped = x.map(address, oversized, &frame);
    } catch (...) {
        threw = true;
    }
    check(!threw && mapped == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
          "SIZE_MAX contiguous map count fails without reading its small input array or throwing");
}

void test_host_failure(const Api& x) {
    const auto alloc2 = reinterpret_cast<decltype(&VirtualAlloc2)>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
    void* const region = x.virtual_alloc(nullptr, 2 * kPage, kPhysicalReserve, 0x21, PAGE_READWRITE);
    ULONG_PTR count = 2;
    ULONG_PTR frames[2]{};
    const bool setup = alloc2 && region && x.allocate(0x280, &count, frames) && x.map(region, 2, frames) == region;
    check(setup, "host-failure setup");
    if (!setup) return;
    at(region, 0)[0] = 0x11;
    at(region, 1)[0] = 0x22;
    SYSTEM_INFO host{};
    GetSystemInfo(&host);
    DWORD old = 0;
    check(VirtualProtect(region, host.dwPageSize, PAGE_READONLY, &old) == TRUE,
          "restrict the first host page of the original view");

    void* const second = at(region, 1);
    // Occupy the second placeholder so Win32 rejects the replacement even
    // though both guest addresses and frame handles pass validation.
    check(x.map(second, 1, nullptr) == second, "restore an isolated placeholder for the second page");
    void* const obstruction = alloc2(nullptr, second, kPage, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                                     PAGE_READWRITE, nullptr, 0);
    check(obstruction == second, "occupy the second placeholder with native memory");
    if (!obstruction) return;

    ULONG_PTR remap[2]{frames[1], frames[0]};
    const auto contiguous = x.map(region, 2, remap);
    const auto contiguous_error = GetLastError();
    check(contiguous == nullptr && contiguous_error != ERROR_SUCCESS, "Win32 rejects the second contiguous map");
    check(committed(region) && at(region, 0)[0] == 0x11, "contiguous map leaves first page intact on host failure");
    check(query(region).Protect == PAGE_READONLY && query(at(region, 0) + host.dwPageSize).Protect == PAGE_READWRITE,
          "contiguous rollback restores the original per-page protection");

    check(x.map(region, 1, frames) == region, "restore original first page for the scatter test");
    check(VirtualProtect(region, host.dwPageSize, PAGE_READONLY, &old) == TRUE,
          "restrict the original view for the scatter test");
    void* addresses[3]{region, region, second};
    ULONG_PTR scatter_frames[3]{frames[1], 0, frames[0]};
    const auto scatter = x.scatter(addresses, 3, scatter_frames);
    const auto scatter_error = GetLastError();
    check(scatter == FALSE && scatter_error != ERROR_SUCCESS, "Win32 rejects the final scatter map");
    check(committed(region) && at(region, 0)[0] == 0x11,
          "scatter rollback restores the original mapping when an address appears twice");
    check(query(region).Protect == PAGE_READONLY && query(at(region, 0) + host.dwPageSize).Protect == PAGE_READWRITE,
          "scatter rollback restores the original per-page protection");

    check(VirtualFree(obstruction, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == TRUE, "restore native placeholder");
    check(x.free_pages(2, frames) == TRUE && x.virtual_free(region, 0, MEM_RELEASE) == TRUE, "release pages and region");
}

void test_unmap_failure(const Api& x) {
    void* const region = x.virtual_alloc(nullptr, 2 * kPage, kPhysicalReserve, 0x21, PAGE_READWRITE);
    void* const alias = x.virtual_alloc(nullptr, 2 * kPage, kPhysicalReserve, 0x23, PAGE_READWRITE);
    ULONG_PTR count = 2;
    ULONG_PTR frames[2]{};
    const bool setup = region && alias && x.allocate(0x280, &count, frames) &&
                       x.map(region, 2, frames) == region && x.map(alias, 2, frames) == alias;
    check(setup, "failed-unmap setup has two pages with two aliases each");
    if (!setup) return;
    at(region, 0)[0] = 0x31;
    at(region, 1)[0] = 0x42;
    SYSTEM_INFO host{};
    GetSystemInfo(&host);
    DWORD old = 0;
    check(VirtualProtect(region, host.dwPageSize, PAGE_READONLY, &old) == TRUE,
          "restrict part of a view before failed freeing");

    // Fail the last alias after earlier views have already been removed. The
    // test-only native seam leaves actual Windows views in place on failure.
    rt::xmem::fail_physical_unmap_once_for_test(at(alias, 1));
    const BOOL freed = x.free_pages(2, frames);
    const DWORD free_error = GetLastError();
    check(freed == FALSE && free_error == ERROR_BUSY, "failed physical free preserves the native unmap error");
    check(committed(region) && committed(at(region, 1)) && committed(alias) && committed(at(alias, 1)) &&
              at(region, 0)[0] == 0x31 && at(alias, 1)[0] == 0x42,
          "failed freeing restores every earlier alias and its bytes");
    check(query(region).Protect == PAGE_READONLY && query(at(region, 0) + host.dwPageSize).Protect == PAGE_READWRITE,
          "failed freeing restores per-host-page protections");
    check(x.map(alias, 2, frames) == alias, "failed freeing keeps both frame handles allocated");

    ULONG_PTR probe_count = 1;
    ULONG_PTR probe = 0;
    const bool allocated_probe = x.allocate(0x280, &probe_count, &probe) == TRUE;
    check(allocated_probe && probe != frames[0] && probe != frames[1],
          "a failed free never recycles either live frame number");
    if (allocated_probe) check(x.free_pages(1, &probe) == TRUE, "release the probe page");

    rt::xmem::fail_physical_unmap_once_for_test(at(region, 1));
    const BOOL released = x.virtual_free(region, 0, MEM_RELEASE);
    const DWORD release_error = GetLastError();
    check(released == FALSE && release_error == ERROR_BUSY, "failed reservation release preserves the unmap error");
    check(committed(region) && committed(at(region, 1)) && at(region, 0)[0] == 0x31 && at(region, 1)[0] == 0x42,
          "failed reservation release restores its earlier views");
    check(query(region).Protect == PAGE_READONLY, "failed reservation release restores its CPU protection");

    rt::xmem::fail_physical_release_once_for_test(MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    check(x.virtual_free(region, 0, MEM_RELEASE) == FALSE && GetLastError() == ERROR_BUSY,
          "failed placeholder coalescing returns the native error");
    check(committed(region) && committed(at(region, 1)) && at(region, 0)[0] == 0x31 && at(region, 1)[0] == 0x42 &&
              query(region).Protect == PAGE_READONLY,
          "failed coalescing retains the whole reservation and restores its views");

    rt::xmem::fail_physical_release_once_for_test(MEM_RELEASE);
    check(x.virtual_free(region, 0, MEM_RELEASE) == FALSE && GetLastError() == ERROR_BUSY,
          "failed final placeholder release returns the native error");
    check(committed(region) && committed(at(region, 1)) && at(region, 0)[0] == 0x31 && at(region, 1)[0] == 0x42 &&
              query(region).Protect == PAGE_READONLY,
          "failed final release restores views after the placeholders were coalesced");
    check(x.virtual_free(region, 0, MEM_RELEASE) == TRUE, "reservation release succeeds when retried");
    check(committed(alias) && at(alias, 0)[0] == 0x31, "releasing one reservation preserves the other alias");
    check(x.free_pages(2, frames) == TRUE && !committed(alias) && !committed(at(alias, 1)),
          "physical free succeeds on retry and removes all remaining aliases");
    check(x.virtual_free(alias, 0, MEM_RELEASE) == TRUE, "release the remaining alias reservation");
}

void test_map(const Api& x) {
    void* rw = x.virtual_alloc(nullptr, 16 * kPage, kPhysicalReserve, 0x21, PAGE_READWRITE);
    void* alias = x.virtual_alloc(nullptr, 16 * kPage, kPhysicalReserve, 0x23, PAGE_READWRITE | kGraphicsReadWrite);
    void* gpu_only = x.virtual_alloc(nullptr, 16 * kPage, kPhysicalReserve, 0x3, PAGE_NOACCESS | kGraphicsNoAccess);
    check(rw && alias && gpu_only, "three 64 KiB-page physical reservations (attributes 0x21, 0x23, 0x3)");
    if (!rw || !alias || !gpu_only) return;
    check(reinterpret_cast<uintptr_t>(rw) % kPage == 0, "a physical reservation is 64 KiB aligned");
    check(!committed(rw), "nothing is accessible in a reservation before a page is mapped");

    ULONG_PTR count = 4;
    ULONG_PTR frames[4] = {};
    check(x.allocate(0x380, &count, frames) == TRUE, "allocate four pages");

    check(x.map(rw, 4, frames) == rw, "XMemMapPhysicalPages returns the address it mapped at");
    bool rw_ok = true;
    for (size_t i = 0; i < 4; ++i) rw_ok &= committed(at(rw, i)) && query(at(rw, i)).Protect == PAGE_READWRITE;
    check(rw_ok, "the four mapped pages are read-write");
    check(all_equal(at(rw, 0), 0) && all_equal(at(rw, 3), 0), "new pages read as zero");
    for (size_t i = 0; i < 4; ++i) fill(at(rw, i), static_cast<uint8_t>(0x10 + i));
    check(!committed(at(rw, 4)), "the page after the mapped range stays unmapped");

    // The same pages at a second address, in another order: both views see one set of bytes.
    ULONG_PTR reversed[4] = {frames[3], frames[2], frames[1], frames[0]};
    check(x.map(at(alias, 8), 4, reversed) == at(alias, 8), "the same pages map at a second address");
    check(all_equal(at(alias, 8), 0x13) && all_equal(at(alias, 11), 0x10), "the second view reads the first's bytes");
    fill(at(alias, 9), 0x77);
    check(all_equal(at(rw, 2), 0x77), "a write through the second view shows in the first");

    check(x.map(gpu_only, 1, frames) == gpu_only, "a page maps into the PAGE_NOACCESS reservation");
    check(committed(gpu_only) && query(gpu_only).Protect == PAGE_NOACCESS,
          "there it keeps the CPU protection, PAGE_NOACCESS (the graphics bit is dropped)");

    const auto* plane = at(gpu_only, 0) + kPage - 7;
    auto view = rt::xmem::find_physical_cpu_view(plane, 16);
    check(!view.address && view.mapped_pages == 1 && view.total_pages == 2,
          "a linear plane spanning a physical hole has no writable CPU view");
    check(x.map(at(gpu_only, 1), 1, frames + 1) == at(gpu_only, 1), "map both synthetic plane pages");
    view = rt::xmem::find_physical_cpu_view(plane, 16);
    check(view.address == at(rw, 0) + kPage - 7 && view.mapped_pages == 2 && view.total_pages == 2,
          "the CPU view preserves both the physical page order and the unaligned byte offset");
    if (view.address) {
        static_cast<uint8_t*>(view.address)[9] = 0x69;
        std::byte pixels[16]{};
        check(at(rw, 1)[2] == 0x69 && at(alias, 10)[2] == 0x69 &&
              rt::xmem::snapshot_sparse_texture(plane, pixels) == rt::xmem::SparseTextureSnapshot::ready &&
              pixels[9] == std::byte{0x69}, "actual CPU writes are visible through every alias and the GPU snapshot");
        check(rt::xmem::compare_sparse_texture(plane, pixels) == rt::xmem::SparseTextureSnapshot::unchanged,
              "an unchanged physical range compares without an output buffer");
        at(rw, 1)[2] = 0x11;
        check(rt::xmem::compare_sparse_texture(plane, pixels) == rt::xmem::SparseTextureSnapshot::ready &&
              pixels[9] == std::byte{0x69}, "a CPU alias write is detected without changing the previous snapshot");
    }
    check(x.map(at(rw, 1), 1, frames + 2) == at(rw, 1), "change the writable mapping's second physical page");
    view = rt::xmem::find_physical_cpu_view(plane, 16);
    check(!view.address && view.mapped_pages == 2 && view.total_pages == 2,
          "a fragmented or reordered CPU view cannot stand in for a contiguous texture");
    check(query(gpu_only).Protect == PAGE_NOACCESS && query(at(gpu_only, 1)).Protect == PAGE_NOACCESS,
          "looking up a CPU alias retains both original GPU page protections");
    check(x.map(at(rw, 1), 1, frames + 1) == at(rw, 1) && x.map(at(gpu_only, 1), 1, nullptr) == at(gpu_only, 1),
          "restore the synthetic mappings");

    // A bad frame number leaves the range as it was.
    ULONG_PTR bad[2] = {frames[0], 17};
    check(x.map(at(rw, 0), 2, bad) == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
          "a frame number that was never handed out is ERROR_INVALID_PARAMETER");
    check(all_equal(at(rw, 1), 0x11), "and the mapping it would have replaced is untouched");
    check(x.map(at(rw, 14), 4, frames) == nullptr, "a range past the reservation's end fails");
    static uint8_t not_reserved[2 * kPage];
    void* outside = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(not_reserved) + kPage - 1) & ~(kPage - 1));
    check(x.map(outside, 1, frames) == nullptr, "an address outside every physical reservation fails");
    check(x.map(at(rw, 0) + 0x1000, 1, frames) == nullptr, "an address that is not 64 KiB aligned fails");

    // Remapping a page replaces what was there.
    check(x.map(at(rw, 0), 1, &frames[3]) == at(rw, 0) && all_equal(at(rw, 0), 0x13),
          "mapping another page over a mapped one replaces it");

    check(x.map(at(rw, 0), 4, nullptr) == at(rw, 0), "a null page array unmaps the range");
    check(!committed(at(rw, 0)) && !committed(at(rw, 3)), "the unmapped pages are inaccessible again");
    check(all_equal(at(alias, 11), 0x10), "unmapping one view leaves the other");
    check(x.map(at(rw, 0), 1, &frames[0]) == at(rw, 0) && all_equal(at(rw, 0), 0x10),
          "a page maps again where it was unmapped");

    // Scatter: listed addresses, 0 entries unmap.
    void* vas[3] = {at(rw, 5), at(rw, 12), at(rw, 0)};
    ULONG_PTR scatter_frames[3] = {frames[1], frames[2], 0};
    check(x.scatter(vas, 3, scatter_frames) == TRUE, "XMemMapPhysicalPagesScatter returns TRUE");
    check(all_equal(at(rw, 5), 0x11) && all_equal(at(rw, 12), 0x77), "it maps each page at its own address");
    check(!committed(at(rw, 0)), "a 0 entry unmaps its address");
    check(x.scatter(vas, 2, nullptr) == TRUE && !committed(at(rw, 5)) && !committed(at(rw, 12)),
          "a null page array unmaps every listed address");

    // Freeing a page unmaps it wherever it is, and it comes back zeroed.
    check(x.free_pages(4, frames) == TRUE, "free the four pages while they are mapped");
    check(!committed(at(alias, 8)) && !committed(at(alias, 11)) && !committed(gpu_only),
          "freeing unmaps the pages in every reservation");
    ULONG_PTR again_count = 4;
    ULONG_PTR again[4] = {};
    check(x.allocate(0x280, &again_count, again) == TRUE, "allocate four pages again");
    check(x.map(rw, 4, again) == rw, "map them");
    bool zero = true;
    for (size_t i = 0; i < 4; ++i) zero &= all_equal(at(rw, i), 0);
    check(zero, "reused pages read as zero");

    ULONG_PTR twice[2] = {again[0], again[0]};
    check(x.free_pages(2, twice) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "freeing one frame number twice in one call is refused");
    check(committed(rw), "and frees nothing");

    check(x.virtual_free(at(rw, 1), 0, MEM_RELEASE) == FALSE, "VirtualFree inside a physical reservation fails");
    check(x.virtual_free(rw, 0, MEM_RELEASE) == TRUE, "VirtualFree(base, 0, MEM_RELEASE) releases it");
    check(query(rw).State == MEM_FREE && query(at(rw, 7)).State == MEM_FREE && query(at(rw, 15)).State == MEM_FREE,
          "the whole range is free afterwards, views and split placeholders included");
    check(x.map(rw, 1, again) == nullptr, "nothing maps into a released reservation");
    check(x.free_pages(4, again) == TRUE, "the pages that were mapped there can still be freed");
    check(x.virtual_free(alias, 0, MEM_RELEASE) == TRUE && x.virtual_free(gpu_only, 0, MEM_RELEASE) == TRUE,
          "release the other two");

    // Plain memory still goes to the host.
    void* plain = VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(plain && x.virtual_free(plain, 0, MEM_RELEASE) == TRUE && query(plain).State == MEM_FREE,
          "VirtualFree of ordinary memory is the host's");
}

// The startup reservations are 24 GiB; the page tables of a reservation that size must not cost more than a few MB,
// and a page at its far end maps like any other.
void test_large_reservation(const Api& x) {
    constexpr SIZE_T size = 24ull << 30;
    void* big = x.virtual_alloc(nullptr, size, kPhysicalReserve, 0x21, PAGE_READWRITE);
    check(big != nullptr, "a 24 GiB physical reservation");
    if (!big) return;
    ULONG_PTR count = 1;
    ULONG_PTR frame = 0;
    check(x.allocate(0x280, &count, &frame) == TRUE, "allocate one page");
    uint8_t* last = static_cast<uint8_t*>(big) + size - kPage;
    check(x.map(last, 1, &frame) == last, "it maps at the last page of the reservation");
    last[kPage - 1] = 0x5a;
    check(last[kPage - 1] == 0x5a, "and is writable there");
    check(x.free_pages(1, &frame) == TRUE && x.virtual_free(big, 0, MEM_RELEASE) == TRUE, "free and release");
}

// Many pages at once, as the startup pools ask for (a few hundred MB each).
void test_bulk(const Api& x) {
    constexpr ULONG_PTR pages = 2048;  // 128 MiB, four sections
    void* region = x.virtual_alloc(nullptr, pages * kPage, kPhysicalReserve, 0x23, PAGE_READWRITE | kGraphicsReadWrite);
    std::vector<ULONG_PTR> frames(pages);
    ULONG_PTR count = pages;
    check(region && x.allocate(0x280, &count, frames.data()) == TRUE && count == pages, "allocate 2048 pages");
    if (!region || count != pages) return;
    check(x.map(region, pages, frames.data()) == region, "map 128 MiB in one call");
    bool ok = true;
    for (ULONG_PTR i = 0; i < pages; ++i) at(region, i)[0] = static_cast<uint8_t>(i);
    for (ULONG_PTR i = 0; i < pages; ++i) ok &= at(region, i)[0] == static_cast<uint8_t>(i);
    check(ok, "every page holds its own byte");
    check(x.free_pages(pages, frames.data()) == TRUE && x.virtual_free(region, 0, MEM_RELEASE) == TRUE,
          "free and release");
}

// The measured startup creates one parent placeholder and chooses several
// reserve-only physical subranges. Selection must retain neighbouring mappings
// and the parent's lifetime, with protection belonging to each selected page.
void test_placeholder_windows(const Api& x) {
    constexpr DWORD create = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    constexpr DWORD select = MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | 0x20400000;
    void* const parent = x.virtual_alloc(nullptr, 12 * kPage, create, 7, PAGE_NOACCESS);
    check(parent != nullptr, "explicit parent placeholder reservation succeeds");
    if (!parent) return;
    uint8_t* const left = at(parent, 2);
    uint8_t* const right = at(parent, 7);
    check(x.virtual_alloc(left, 3 * kPage, select, 0x27, PAGE_READWRITE | kGraphicsReadWrite) == left,
          "select a writable physical window inside its parent");
    check(x.virtual_alloc(right, 2 * kPage, select, 0x27, PAGE_READONLY) == right,
          "select an independent read-only window without discarding the first");
    ULONG_PTR count = 1;
    ULONG_PTR frame = 0;
    const bool allocated = x.allocate(0x280, &count, &frame) == TRUE;
    check(allocated, "allocate the page for the placeholder windows");
    if (!allocated) {
        x.virtual_free(parent, 0, MEM_RELEASE);
        return;
    }
    const bool mapped = x.map(left, 1, &frame) == left && x.map(right, 1, &frame) == right;
    check(mapped, "both selected windows map the same physical page");
    if (mapped) {
        left[0] = 0x63;
        check(right[0] == 0x63, "selected windows retain real shared page contents");
        check(query(left).Protect == PAGE_READWRITE && query(right).Protect == PAGE_READONLY,
              "selected windows apply distinct CPU protections");
        check(x.virtual_alloc(left, kPage, select, 0x27, PAGE_READONLY) == nullptr &&
                  GetLastError() == ERROR_INVALID_ADDRESS,
              "selection refuses an already mapped page");
        check(left[0] == 0x63 && query(left).Protect == PAGE_READWRITE,
              "a refused selection preserves the live page and protection");
        check(x.virtual_alloc(at(parent, 11), 2 * kPage, select, 0x27, PAGE_READWRITE) == nullptr &&
                  GetLastError() == ERROR_INVALID_ADDRESS,
              "selection cannot extend beyond the parent reservation");
        check(query(parent).State == MEM_RESERVE && query(at(parent, 11)).State == MEM_RESERVE,
              "unselected prefix and suffix remain reserved");
    }
    check(x.virtual_alloc(left + 1, kPage, select, 0x27, PAGE_READWRITE) == nullptr &&
              GetLastError() == ERROR_INVALID_PARAMETER,
          "physical window selection rejects an unaligned address");
    check(x.virtual_free(parent, 0, MEM_RELEASE) == TRUE,
          "releasing the parent removes all selected windows and placeholders");
    check(query(parent).State == MEM_FREE && query(left).State == MEM_FREE && query(right).State == MEM_FREE,
          "the whole parent range is released together");
    check(x.free_pages(1, &frame) == TRUE, "the page can be freed after releasing its parent");
}

void test_placeholder_protections(const Api& x) {
    constexpr DWORD words[] = {0x1004, 0x1404, 0x8404, 0x2404, 0x2004};
    constexpr SIZE_T pages = sizeof(words) / sizeof(words[0]);
    constexpr DWORD create = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    constexpr DWORD select = MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | 0x20400000;
    void* const parent = x.virtual_alloc(nullptr, pages * kPage, create, 7, PAGE_NOACCESS);
    check(parent != nullptr, "reserve the five measured-protection windows");
    if (!parent) return;
    ULONG_PTR count = pages;
    ULONG_PTR frames[pages]{};
    const bool allocated = x.allocate(0x280, &count, frames) == TRUE;
    check(allocated, "allocate the five measured-protection pages");
    if (allocated) {
        for (SIZE_T i = 0; i < pages; ++i) {
            void* const page = at(parent, i);
            const bool selected = x.virtual_alloc(page, kPage, select, 0x27, words[i]) == page;
            check(selected, "select a window with a measured graphics protection word");
            const bool mapped = selected && x.map(page, 1, &frames[i]) == page;
            check(mapped, "map its physical page with the selected CPU protection");
            if (mapped) {
                const DWORD expected = (words[i] & PAGE_WRITECOMBINE) ? PAGE_READWRITE | PAGE_WRITECOMBINE : PAGE_READWRITE;
                check(query(page).Protect == expected, "the mapped page keeps its CPU write-combining policy");
                auto* bytes = static_cast<volatile uint8_t*>(page);
                bytes[0] = static_cast<uint8_t>(0x50 + i);
                MemoryBarrier();
                check(bytes[0] == static_cast<uint8_t>(0x50 + i), "the measured-protection page is writable and readable");
            }
        }
        check(x.free_pages(pages, frames) == TRUE, "free the five measured-protection pages");
    }
    check(x.virtual_free(parent, 0, MEM_RELEASE) == TRUE, "release the measured-protection parent");
}

void test_backing_reads(const Api& x) {
    ULONG_PTR count = 3, frames[3]{};
    const bool allocated = x.allocate(0x280, &count, frames) && count == 3;
    check(allocated, "allocate physical source pages for backing reads");
    if (!allocated) return;
    const auto* pages = reinterpret_cast<const uint64_t*>(frames);
    std::vector<uint8_t> input(3 * kPage);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>(i * 17 + (i / kPage) * 29 + 5);
    check(rt::xmem::write_physical_pages(pages, 0, input.data(), input.size()),
          "initialize physical backing without guest mappings");

    const uint64_t order[3] = {frames[2], frames[0], frames[1]};
    constexpr size_t offset = kPage - 1, size = kPage + 2;
    std::vector<uint8_t> staged(size + 32, 0xa5), want(size);
    want[0] = input.back();
    std::memcpy(want.data() + 1, input.data(), kPage);
    want.back() = input[kPage];
    check(rt::xmem::read_physical_pages(order, offset, staged.data() + 16, size),
          "read three unmapped pages in caller order from offset 65535");
    check(std::equal(want.begin(), want.end(), staged.begin() + 16),
          "backing read returns each expected byte across both page boundaries");
    check(std::all_of(staged.begin(), staged.begin() + 16, [](uint8_t b) { return b == 0xa5; }) &&
              std::all_of(staged.end() - 16, staged.end(), [](uint8_t b) { return b == 0xa5; }),
          "backing read preserves output canaries");

    const uint64_t repeated[2] = {frames[1], frames[1]};
    std::vector<uint8_t> repeated_out(kPage + 10);
    check(rt::xmem::read_physical_pages(repeated, 0, repeated_out.data(), repeated_out.size()),
          "source page IDs may repeat without requiring duplicate physical allocations");
    check(std::equal(input.begin() + kPage, input.begin() + 2 * kPage, repeated_out.begin()) &&
              std::equal(input.begin() + kPage, input.begin() + kPage + 10, repeated_out.begin() + kPage),
          "repeated source IDs read the same page for each array entry");

    auto* window = static_cast<uint8_t*>(x.virtual_alloc(nullptr, 2 * kPage, kPhysicalReserve, 1, PAGE_READWRITE));
    ULONG_PTR aliases[2] = {frames[0], frames[0]};
    const bool mapped = window && x.map(window, 2, aliases) == window;
    check(mapped, "map two CPU aliases of one source page");
    if (mapped) {
        window[kPage + 13] = 0xe2;
        uint8_t byte = 0;
        check(window[13] == 0xe2 && rt::xmem::read_physical_pages(pages, 13, &byte, 1) && byte == 0xe2,
              "backing reads observe a write through either mapped CPU alias");
        check(x.map(window, 2, nullptr) == window, "unmap both source aliases");
        byte = 0;
        check(rt::xmem::read_physical_pages(pages, 13, &byte, 1) && byte == 0xe2,
              "backing remains readable after every guest alias is unmapped");
    }

    const uint64_t invalid[3] = {frames[2], frames[0], 0};
    std::fill(staged.begin(), staged.end(), uint8_t{0xa5});
    check(!rt::xmem::read_physical_pages(invalid, offset, staged.data() + 16, size),
          "a bad final source page rejects the whole read");
    check(std::all_of(staged.begin(), staged.end(), [](uint8_t b) { return b == 0xa5; }),
          "source validation completes before changing even the staging prefix");
    const uint64_t foreign = 0xfffffffffffffff0ull;
    check(!rt::xmem::read_physical_pages(&foreign, 0, staged.data(), 1), "a foreign source page is refused");
    check(!rt::xmem::read_physical_pages(nullptr, 0, staged.data(), 1) &&
              !rt::xmem::read_physical_pages(pages, 0, nullptr, 1), "nonempty backing reads require both spans");
    const auto* tiny = reinterpret_cast<const uint64_t*>(uintptr_t{1});
    check(!rt::xmem::read_physical_pages(tiny, kPage, staged.data(), 1),
          "an invalid source offset fails before an array read");
    check(!rt::xmem::read_physical_pages(tiny, 0, staged.data(), (std::numeric_limits<size_t>::max)()),
          "SIZE_MAX source size fails before an array read or count overflow");
    check(rt::xmem::read_physical_pages(nullptr, kPage, nullptr, 0), "an empty backing read touches neither pointer");
    check(x.free_pages(3, frames) == TRUE, "free the source pages after the backing reads");
    check(!rt::xmem::read_physical_pages(pages, 0, staged.data(), 1) && staged[0] == 0xa5,
          "a freed source page fails without changing its output");
    if (window) check(x.virtual_free(window, 0, MEM_RELEASE) == TRUE, "release the source alias window");
}

void test_ordinary_texture_read() {
    using Read = rt::HostTextureRead;
    constexpr std::size_t chunk = 1024 * 1024;
    constexpr auto size = 3 * chunk + 123;
    auto* data = static_cast<std::byte*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(data != nullptr, "allocate ordinary texture pages");
    if (!data) return;
    std::vector<std::byte> previous(size), scratch(size);
    SIZE_T read = 0;
    auto result = rt::read_host_texture(data, scratch, {}, read);
    check(result == Read::captured && read == size && scratch == previous, "first read copies the complete texture");
    result = rt::read_host_texture(data, scratch, previous, read);
    check(result == Read::unchanged && read == size, "all chunks matched, including the final partial chunk");
    for (const auto at : {std::size_t(0), chunk, 2 * chunk, size - 1}) {
        data[at] ^= std::byte{0x55};
        const auto changed = rt::read_host_texture(data, scratch, previous, read);
        check(changed == Read::captured && read == size && std::memcmp(data, scratch.data(), size) == 0,
              "a change at any chunk boundary returns a fresh full capture");
        previous = scratch;
    }
    DWORD old{};
    const auto page = data + 2 * chunk;
    if (VirtualProtect(page, 4096, PAGE_NOACCESS, &old)) {
        result = rt::read_host_texture(data, scratch, previous, read);
        check(result == Read::failed && read < size && query(page).Protect == PAGE_NOACCESS,
              "an unreadable late chunk fails and retains its protection");
        data[0] ^= std::byte{1};
        result = rt::read_host_texture(data, scratch, previous, read);
        check(result == Read::failed && read < size, "an unreadable page after the first change also fails");
        VirtualProtect(page, 4096, old, &old);
        result = rt::read_host_texture(data, scratch, previous, read);
        check(result == Read::captured && read == size && std::memcmp(data, scratch.data(), size) == 0,
              "retry after failure returns exact current source bytes");
    } else {
        check(false, "set an ordinary texture page inaccessible");
    }
    if (VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &old)) {
        const BOOL baseline = ReadProcessMemory(GetCurrentProcess(), data, scratch.data(), size, &read);
        const DWORD baseline_protection = query(page).Protect;
        DWORD saved{};
        if (VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &saved)) {
            result = rt::read_host_texture(data, scratch, previous, read);
            check(!baseline && result == Read::failed && query(page).Protect == baseline_protection,
                  "guard-page failure retains the same protection as a whole-range checked read");
        } else check(false, "restore guard for the chunked read");
    } else check(false, "set an ordinary texture guard page");
    VirtualFree(data, 0, MEM_RELEASE);
}

}  // namespace

int main() {
    std::printf("xmem_physical_test: xmem.dll physical pages\n");
    test_ordinary_texture_read();
    Api x{};
    x.virtual_alloc = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    x.allocate = reinterpret_cast<AllocatePhysicalFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    x.free_pages = reinterpret_cast<FreePhysicalFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    x.map = reinterpret_cast<MapPhysicalFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    x.scatter = reinterpret_cast<MapScatterFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPagesScatter"));
    x.virtual_free = reinterpret_cast<VirtualFreeFn>(rt::shims::find("api-ms-win-core-memory-l1-1-0.dll", "VirtualFree"));
    check(x.virtual_alloc && x.allocate && x.free_pages && x.map && x.scatter && x.virtual_free,
          "the registry serves the four physical-page exports, XMemVirtualAlloc and the title's VirtualFree");
    check(rt::shims::find("kernel32.dll", "VirtualFree") == reinterpret_cast<void*>(x.virtual_free),
          "kernel32.dll!VirtualFree is the same shim");
    if (x.virtual_alloc && x.allocate && x.free_pages && x.map && x.scatter && x.virtual_free) {
        test_allocate(x);
        test_oversized_counts(x);
        test_map(x);
        test_large_reservation(x);
        test_bulk(x);
        test_host_failure(x);
        test_unmap_failure(x);
        test_placeholder_windows(x);
        test_placeholder_protections(x);
        test_backing_reads(x);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
