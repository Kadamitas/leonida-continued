#pragma once

// Allocation provenance for the measured startup control page. An epoch follows the title allocation, not any
// short-lived Vulkan import of its bytes. Only the exact successful XMem profile records entries; a title free
// revokes them before attempting to change the mapping. No resources, host pointers or imports are retained.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rt::shims::control_pages {
namespace detail {

struct Allocation {
    std::uintptr_t base = 0;
    std::size_t bytes = 0;
    std::uint64_t epoch = 0;
    bool copy_arena = false;
    unsigned pins = 0;
};

struct State {
    std::atomic_flag mutex = ATOMIC_FLAG_INIT;
    std::array<Allocation, 64> allocations{};
    std::uint64_t next_epoch = 1;
};

// Inline storage is shared by the allocation shim, free shim and backend across translation units. Fixed storage
// makes registration/revocation allocation-free, and has no process-exit container destructor racing title work.
inline State state;

class Lock {
public:
    Lock() noexcept { while (state.mutex.test_and_set(std::memory_order_acquire)) {} }
    ~Lock() { state.mutex.clear(std::memory_order_release); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

inline bool overlaps(const Allocation& entry, std::uintptr_t base, std::size_t bytes) noexcept {
    return entry.epoch && (base <= entry.base ? entry.base - base < bytes : base - entry.base < entry.bytes);
}

}  // namespace detail

inline std::uint64_t allocation_id(std::uintptr_t base) noexcept {
    const detail::Lock lock;
    for (const auto& entry : detail::state.allocations)
        if (entry.epoch && !entry.copy_arena && entry.base == base) return entry.epoch;
    return 0;
}

// A title free/decommit revokes under this lock before it changes the Windows mapping. Hold the lock through the
// load so even a recycled address can never make an old generation wait dereference released or replacement memory.
// A null output only rechecks the allocation after a satisfied wait, whose word may subsequently regress.
inline bool snapshot_generation(std::uint64_t base, std::uint64_t expected, std::uint64_t* observed) noexcept {
    if constexpr (!std::atomic_ref<std::uint64_t>::is_always_lock_free) return false;
    if (!base || !expected || base > UINTPTR_MAX || base % std::atomic_ref<std::uint64_t>::required_alignment)
        return false;
    const detail::Lock lock;
    for (const auto& entry : detail::state.allocations) {
        if (entry.epoch != expected || entry.copy_arena || entry.base != base || entry.bytes < sizeof(std::uint64_t))
            continue;
        if (observed) {
            auto* word = reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(base));
            *observed = std::atomic_ref<std::uint64_t>(*word).load(std::memory_order_acquire);
        }
        return true;
    }
    return false;
}

// Returns zero when no bounded slot/unique epoch is available; the allocation then has no control-page role.
inline std::uint64_t note_allocation(std::uintptr_t base, std::size_t bytes, bool copy_arena = false) noexcept {
    if (!base || !bytes || bytes > std::numeric_limits<std::uintptr_t>::max() - base) return 0;
    const detail::Lock lock;
    for (const auto& entry : detail::state.allocations)
        if (entry.pins && detail::overlaps(entry, base, bytes)) return 0;
    for (auto& entry : detail::state.allocations)
        if (detail::overlaps(entry, base, bytes)) entry = {};
    if (!detail::state.next_epoch) return 0;  // never reuse an epoch, including after uint64 wrap
    for (auto& entry : detail::state.allocations) {
        if (entry.epoch) continue;
        const auto epoch = detail::state.next_epoch++;
        entry = {base, bytes, epoch, copy_arena};
        return epoch;
    }
    return 0;
}

// A console raw copy can import only a span wholly in the same live write-back allocation.
inline std::uint64_t pin_copy_arena(std::uintptr_t address, std::size_t bytes) noexcept {
    const detail::Lock lock;
    for (auto& entry : detail::state.allocations) {
        if (entry.copy_arena && entry.epoch && address >= entry.base && address - entry.base < entry.bytes &&
            bytes && bytes <= entry.bytes - (address - entry.base) && entry.pins != (std::numeric_limits<unsigned>::max)()) {
            ++entry.pins;
            return entry.epoch;
        }
    }
    return 0;
}

inline void unpin_copy_arena(std::uint64_t epoch) noexcept {
    const detail::Lock lock;
    for (auto& entry : detail::state.allocations)
        if (entry.epoch == epoch && entry.pins) { --entry.pins; return; }
}

// A zero-size RELEASE/DECOMMIT refers to the containing allocation. Invalid wrapped ranges conservatively
// revoke every role; a failed native free may lose eligibility but can never preserve stale provenance.
inline bool revoke(std::uintptr_t address, std::size_t bytes) noexcept {
    const detail::Lock lock;
    const bool wrapped = bytes > std::numeric_limits<std::uintptr_t>::max() - address;
    for (const auto& entry : detail::state.allocations) {
        const bool contains = address >= entry.base && address - entry.base < entry.bytes;
        if (entry.pins && (wrapped || (bytes ? detail::overlaps(entry, address, bytes) : contains))) return false;
    }
    for (auto& entry : detail::state.allocations) {
        if (!entry.epoch) continue;
        const bool contains = address >= entry.base && address - entry.base < entry.bytes;
        if (wrapped || (bytes ? detail::overlaps(entry, address, bytes) : contains)) entry = {};
    }
    return true;
}

}  // namespace rt::shims::control_pages
