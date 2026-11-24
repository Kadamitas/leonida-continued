#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace leonida {

// Draw coverage and conditional rendering are unknown on the CPU. Keep those command observations distinct
// from unconditional color clears, image copies, and resolves. None identifies which physical guest bytes changed.
enum class AttachmentWrite { clear_full, clear_rects, copy, resolve, possible, count };
struct AttachmentWriteTrace : std::enable_shared_from_this<AttachmentWriteTrace> {
    std::array<std::uint64_t, unsigned(AttachmentWrite::count)> last_ticket{};  // protected by the submission mutex
    std::atomic<bool> incomplete{false};

};

struct AttachmentWriteBatch {
    struct Item { std::shared_ptr<AttachmentWriteTrace> trace; unsigned kinds; };
    std::vector<Item> items;
    void add(AttachmentWriteTrace* trace, AttachmentWrite kind) {
        if (!trace) return;
        const unsigned bit = 1u << unsigned(kind);
        for (auto& item : items) if (item.trace.get() == trace) { item.kinds |= bit; return; }
        try { items.push_back({trace->shared_from_this(), bit}); }
        catch (const std::bad_alloc&) { trace->incomplete.store(true, std::memory_order_relaxed); }
    }
    void submit(std::uint64_t ticket) const {
        for (const auto& item : items)
            for (unsigned kind = 0; kind < item.trace->last_ticket.size(); ++kind)
                if (item.kinds & (1u << kind)) item.trace->last_ticket[kind] = ticket;
    }
};

} // namespace leonida
