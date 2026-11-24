#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace leonida {

// The resource lock protects the stable resource-map nodes. Several imports may alias the same VA.
template<class Entry>
class HostImageIndex {
public:
    struct Node {
        std::uint64_t address;
        Entry* entry;
    };

    bool empty() const { return nodes_.empty(); }

    void insert(std::uint64_t address, Entry* entry) {
        if (nodes_.empty() || nodes_.back().address <= address) {
            nodes_.push_back({address, entry});
            return;
        }
        const auto after = std::upper_bound(nodes_.begin(), nodes_.end(), address,
            [](std::uint64_t key, const Node& node) { return key < node.address; });
        nodes_.insert(after, {address, entry});
    }

    void erase(std::uint64_t address, Entry* entry) {
        const auto first = lower(address);
        for (auto it = first; it != nodes_.end() && it->address == address; ++it) {
            if (it->entry == entry) {
                nodes_.erase(it);
                return;
            }
        }
    }

    // The caller collects distinct nonzero addresses. Address zero is an import with unknown VA and must
    // be visited even on a bounded submission. Two phases reuse the selection under the same resource lock.
    std::span<const Node> select(bool all, std::span<const std::uint64_t> addresses,
                                 std::vector<Node>& scratch) const {
        scratch.clear();
        if (all || nodes_.empty()) return nodes_;
        const auto append = [&](std::uint64_t address) {
            for (auto it = lower(address); it != nodes_.end() && it->address == address; ++it)
                scratch.push_back(*it);
        };
        if (!nodes_.front().address) append(0);
        for (const auto address : addresses) append(address);
        return scratch;
    }

private:
    auto lower(std::uint64_t address) const {
        return std::lower_bound(nodes_.begin(), nodes_.end(), address,
            [](const Node& node, std::uint64_t key) { return node.address < key; });
    }

    std::vector<Node> nodes_;
};

} // namespace leonida
