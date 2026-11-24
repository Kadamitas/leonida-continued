#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace leonida {

// Optimal images and transfer buffers have separate pools. A slot returns to its pool only
// after its bound resource is destroyed by the device's GPU retirement queue.
struct TitleMemoryPage {
    VkDevice device{};
    VkDeviceMemory memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkUnmapMemory unmap_memory{};
    void* mapped = nullptr;
    std::uint32_t type = 0, capacity = 0, allocated = 0;
    std::mutex mutex;
    std::vector<std::uint32_t> returned;

    ~TitleMemoryPage() {
        if (mapped) unmap_memory(device, memory);
        if (memory) free_memory(device, memory, nullptr);
    }

    bool take(std::uint32_t& slot) {
        std::lock_guard lock(mutex);
        if (!returned.empty()) {
            slot = returned.back();
            returned.pop_back();
        } else if (allocated < capacity) {
            slot = allocated++;
        } else {
            return false;
        }
        return true;
    }

    void release(std::uint32_t slot) {
        std::lock_guard lock(mutex);
        returned.push_back(slot);  // the page reserves capacity at creation
    }
};

template<class Handle, class Destroy>
struct PooledTitleResource {
    PooledTitleResource() = default;
    PooledTitleResource(const PooledTitleResource&) = delete;
    PooledTitleResource& operator=(const PooledTitleResource&) = delete;

    std::shared_ptr<TitleMemoryPage> page;
    std::uint32_t slot = 0;
    VkDeviceSize offset = 0;
    Handle handle{};
    Destroy destroy{};

    ~PooledTitleResource() {
        if (handle) destroy(page->device, handle, nullptr);
        if (page) page->release(slot);
    }
};

template<class Handle, class Bind, class Destroy>
class TitleResourcePool {
public:
    // The caller has already excluded dedicated-only resources. All allocations in one page have
    // identical stride, compatible memory types and non-overlapping, individually aligned ranges.
    template<class Allocate>
    std::unique_ptr<PooledTitleResource<Handle, Destroy>> bind(
        VkDevice device, Handle handle, const VkMemoryRequirements& req, Bind bind_memory, Destroy destroy,
        PFN_vkFreeMemory free_memory, Allocate allocate, PFN_vkMapMemory map = nullptr, PFN_vkUnmapMemory unmap = nullptr) {
        // Short, CPU-filled texture rows are numerous. Limit unused mapped capacity in their
        // last page; large video uploads already justify an individual mapping.
        const VkDeviceSize page_bytes = map ? VkDeviceSize{256} << 10 : VkDeviceSize{4} << 20;
        const VkDeviceSize largest_slot = page_bytes / 2;
        const VkDeviceSize needed = std::max(req.size, req.alignment);
        if (!req.size || !std::has_single_bit(req.alignment) || needed > largest_slot || !req.memoryTypeBits) return {};
        // Eight size classes per power of two keep padding below one eighth (besides required alignment).
        const VkDeviceSize quantum = std::max(req.alignment, std::max(VkDeviceSize{1}, std::bit_floor(needed) / 8));
        const VkDeviceSize stride = (needed + quantum - 1) & ~(quantum - 1);
        if (bool(map) != bool(unmap)) return {};
        auto allocation = std::make_unique<PooledTitleResource<Handle, Destroy>>();
        std::lock_guard lock(mutex_);
        auto& pages = classes_[stride];
        std::erase_if(pages, [](const auto& page) { return page.expired(); });
        // A rarely used size class can otherwise reserve several MiB for a single small image. Grow only
        // when compatible pages of this class actually fill; fully released classes start small again.
        VkDeviceSize slots = std::clamp<VkDeviceSize>((VkDeviceSize{512} << 10) / stride, 1, 4);
        for (const auto& weak : pages) {
            auto page = weak.lock();
            if (!page || !(req.memoryTypeBits & (1u << page->type)) || bool(page->mapped) != bool(map)) continue;
            if (page->take(allocation->slot)) {
                allocation->page = std::move(page);
                break;
            }
            slots = std::max(slots, 2 * VkDeviceSize(page->capacity));
        }
        if (!allocation->page) {
            const auto capacity = std::min({slots, VkDeviceSize{256}, page_bytes / stride});
            auto page = std::make_shared<TitleMemoryPage>();
            page->device = device;
            page->free_memory = free_memory;
            page->capacity = static_cast<std::uint32_t>(capacity);
            page->returned.reserve(page->capacity);
            page->memory = allocate(capacity * stride, page->type);
            if (!page->memory) return {};
            if (map) {
                void* mapped = nullptr;
                if (map(device, page->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return {};
                page->mapped = mapped;
                page->unmap_memory = unmap;
            }
            pages.push_back(page);
            page->take(allocation->slot);
            allocation->page = std::move(page);
        }
        allocation->offset = stride * allocation->slot;
        if (bind_memory(device, handle, allocation->page->memory, allocation->offset) != VK_SUCCESS) return {};
        allocation->destroy = destroy;
        allocation->handle = handle;
        return allocation;
    }

private:
    std::mutex mutex_;
    std::map<VkDeviceSize, std::vector<std::weak_ptr<TitleMemoryPage>>> classes_;
};

using PooledTitleImage = PooledTitleResource<VkImage, PFN_vkDestroyImage>;
using TitleImagePool = TitleResourcePool<VkImage, PFN_vkBindImageMemory, PFN_vkDestroyImage>;
using PooledStagingBuffer = PooledTitleResource<VkBuffer, PFN_vkDestroyBuffer>;
using TitleStagingPool = TitleResourcePool<VkBuffer, PFN_vkBindBufferMemory, PFN_vkDestroyBuffer>;

}  // namespace leonida
