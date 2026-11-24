// The xmem shim's physical pages, for the other shims that are handed page numbers instead of addresses (console
// DirectStorage: DSTORAGE_REQUEST_OPTIONS::{Source,Destination}IsPhysicalPages).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rt::xmem {

// The size of one page XMemAllocatePhysicalPages hands out (shims/xmem_physical.cpp).
constexpr size_t kPhysicalPageBytes = 0x10000;

// Copies `size` bytes from `source` into the physical pages page_array names: the data starts `offset` bytes into
// page_array[0] and runs on through the entries after it, (offset + size) rounded up to whole pages of them. The
// pages are written through their backing, so it makes no difference whether a page is mapped, mapped at several
// addresses or not mapped at all, and a mapped view sees the bytes.
//
// Every page number is checked before the first byte is written. false, with nothing written: a null argument, an
// offset of a page or more, or an entry that is not a live page of XMemAllocatePhysicalPages (0 included; in the map
// calls 0 means "unmap", which is not a place to write to). `page_array` and `source` must be readable: a caller that
// takes them from a title copies them first. Keep both spans stable during this call; source must be a separate
// staging buffer rather than an alias of the physical pages. With commit-on-map enabled, the store commits the
// entire validated destination before the first write, and returns false if backing cannot be committed.
bool write_physical_pages(const uint64_t* page_array, size_t offset, const void* source, size_t size);

// Reads the same page-number/offset range into separate writable staging.
// Every page ID is validated under the page-store lock before the first output
// byte is changed, and the lock is retained through the copy. False leaves the
// staging untouched. Guest mappings are unnecessary. The caller provides a
// stable readable page array and a separate writable destination of `size`
// bytes; a zero-sized read succeeds without touching either pointer.
bool read_physical_pages(const uint64_t* page_array, size_t offset, void* destination, size_t size);

struct PhysicalPageAliasRange {
    std::uintptr_t destination;
    std::uintptr_t source;
    std::size_t page_count;
    bool repeat_source = false;
};

enum class SparseTextureSnapshot { untracked, ready, failed, unchanged };

struct PhysicalStoreStats {
    std::size_t capacity_slots = 0;
    std::size_t allocated_slots = 0;
    std::size_t committed_slots = 0;
    bool commit_on_map = false;
};

struct PhysicalCpuView {
    void* address = nullptr;
    std::size_t mapped_pages = 0;
    std::size_t total_pages = 0;
};

enum class PhysicalPageState : std::uint8_t { unavailable, reserved, mapped, inconsistent };
struct PhysicalPageIdentity {
    PhysicalPageState state = PhysicalPageState::unavailable;
    std::uint64_t frame = 0;
    std::uint32_t allocation_epoch = 0;  // Saturating, so UINT32_MAX cannot distinguish later allocations.
    std::uint16_t physical_store_writes = 0;  // Saturating writes through write_physical_pages to this whole page.
};

// A successful pin fixes this guest page's frame until release, including through free/unmap requests.
struct PhysicalImportOps {
    void* (*pin)(const void*, std::size_t) noexcept;
    void (*release)(const void*) noexcept;
};

enum class TextureCoverage { sparse, fully_mapped };

// A sampled view overlaps an otherwise unwritten native render target. Every source page must
// remain mapped; the overlapping frames must still match the identities established at placement.
struct PhysicalTextureProof {
    std::uintptr_t overlap_start;
    std::span<const PhysicalPageIdentity> overlap_pages;
};

struct SparseTextureMismatch {
    std::size_t offset = 0;
    std::size_t bytes = 0;
};

namespace detail {
using PhysicalStoreStatsFn = PhysicalStoreStats (*)() noexcept;
inline constinit std::atomic<PhysicalStoreStatsFn> physical_store_stats_handler{nullptr};
using PhysicalPageAliasFn = bool (*)(std::span<const PhysicalPageAliasRange>) noexcept;
using PhysicalPageIdentityFn = PhysicalPageIdentity (*)(const void*) noexcept;
using PhysicalCpuViewFn = PhysicalCpuView (*)(const void*, std::size_t) noexcept;
using SparseTextureSnapshotFn = SparseTextureSnapshot (*)(const void*, std::size_t, std::span<std::byte>,
                                                         std::span<const std::byte>, SparseTextureMismatch*,
                                                         TextureCoverage, const PhysicalTextureProof*) noexcept;
// The device proxy is also compiled without the physical store. Constant initialization keeps those targets
// independent of xmem_physical.cpp; the real store publishes its handler during shim registration.
inline constinit std::atomic<PhysicalPageAliasFn> physical_page_alias_handler{nullptr};
inline constinit std::atomic<const PhysicalImportOps*> physical_import_ops{nullptr};
inline constinit std::atomic<PhysicalPageIdentityFn> physical_page_identity_handler{nullptr};
inline constinit std::atomic<PhysicalCpuViewFn> physical_cpu_view_handler{nullptr};
inline constinit std::atomic<SparseTextureSnapshotFn> sparse_texture_snapshot_handler{nullptr};
}  // namespace detail

inline PhysicalStoreStats physical_store_stats() noexcept {
    const auto handler = detail::physical_store_stats_handler.load(std::memory_order_acquire);
    return handler ? handler() : PhysicalStoreStats{};
}

// Aliases live 64 KiB physical mappings, or unmaps destinations when source is zero. All non-zero sources must
// already be mapped in this store and every destination must be in one of its physical reservations. Source
// slots are captured for the whole batch before any destination changes, including overlapping ranges. Repeated
// sources use one page for the entire destination range. Native failures use the store's transactional rollback;
// no bytes are copied. The caller supplies a stable range array. False also means the store is not linked.
inline bool alias_physical_page_ranges(std::span<const PhysicalPageAliasRange> ranges) noexcept {
    const auto handler = detail::physical_page_alias_handler.load(std::memory_order_acquire);
    return handler && handler(ranges);
}

// Optional on targets built without the XMem page store. Its permanent write-back mapping owns the
// actual bytes; this interface never changes guest protections or returns a guest-managed alias.
inline const PhysicalImportOps* physical_import_ops() noexcept {
    return detail::physical_import_ops.load(std::memory_order_acquire);
}

// Debug identity at this instant. It does not pin the page or observe direct writes through a mapped alias.
inline PhysicalPageIdentity physical_page_identity(const void* source) noexcept {
    const auto handler = detail::physical_page_identity_handler.load(std::memory_order_acquire);
    return handler ? handler(source) : PhysicalPageIdentity{};
}

// Look up an existing guest-owned writable mapping of the exact same physical pages in the same order.
// The returned pointer follows the mapping lifetime of those pages. Holes and GPU-only pages without a
// contiguous CPU alias have no writable view. This lookup does not map, write, or change page protection.
inline PhysicalCpuView find_physical_cpu_view(const void* source, std::size_t size) noexcept {
    const auto handler = detail::physical_cpu_view_handler.load(std::memory_order_acquire);
    return handler ? handler(source, size) : PhysicalCpuView{};
}

// Snapshot a sampled texture in one physical reservation, serialized with mapping/free and physical writes.
// Confirmed mapped slots are read through the store's permanent backing, including GPU-only guest aliases;
// only confirmed unmapped placeholders become zero. No guest protections or mappings are changed.
// The destination is separate, writable caller-owned staging. On failed it must be discarded; untracked leaves
// it untouched and permits the caller's ordinary strict read. A missing store also returns untracked.
// With an equally sized previous snapshot, unchanged means every byte still matches and destination was not touched.
// On ready, an optional mismatch identifies a block that differed during the comparison. Recheck the captured bytes
// before treating the block as dirty: a writer through a CPU alias can change the data before the copy.
inline SparseTextureSnapshot snapshot_sparse_texture(const void* source, std::span<std::byte> destination,
                                                     std::span<const std::byte> previous = {},
                                                     SparseTextureMismatch* mismatch = nullptr,
                                                     const PhysicalTextureProof* proof = nullptr) noexcept {
    if (mismatch) *mismatch = {};
    const auto handler = detail::sparse_texture_snapshot_handler.load(std::memory_order_acquire);
    return handler ? handler(source, destination.size(), destination, previous, mismatch,
                             TextureCoverage::sparse, proof) : SparseTextureSnapshot::untracked;
}

// Validate the same physical mappings before allocating a texture, without copying its contents.
inline SparseTextureSnapshot validate_sparse_texture(const void* source, std::size_t size,
                                                     TextureCoverage coverage = TextureCoverage::sparse,
                                                     const PhysicalTextureProof* proof = nullptr) noexcept {
    const auto handler = detail::sparse_texture_snapshot_handler.load(std::memory_order_acquire);
    return handler ? handler(source, size, {}, {}, nullptr, coverage, proof) : SparseTextureSnapshot::untracked;
}

// Compare all current physical bytes, including holes, before reserving scratch for a refresh.
// Ready means a difference was observed. A later capture validates the mapping again.
inline SparseTextureSnapshot compare_sparse_texture(const void* source, std::span<const std::byte> previous,
                                                    const PhysicalTextureProof* proof = nullptr) noexcept {
    const auto handler = detail::sparse_texture_snapshot_handler.load(std::memory_order_acquire);
    return handler ? handler(source, previous.size(), {}, previous, nullptr,
                             TextureCoverage::sparse, proof) : SparseTextureSnapshot::untracked;
}

}  // namespace rt::xmem
