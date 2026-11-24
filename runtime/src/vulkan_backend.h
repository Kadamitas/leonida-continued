#pragma once

// The execution side of the D3D12 layer, without COM: the per-device Vulkan context (resources, layout tracker,
// image views), queues that execute command lists, and timeline-semaphore fences. The COM objects in
// shims/d3d12_com.cpp are thin wrappers over these. Needs Vulkan 1.2 timeline semaphores; rectangle clears and
// enhanced barriers need the Vulkan 1.3 functions (dynamicRendering, synchronization2) to be enabled, as core
// features or through their KHR extensions on a 1.2 device (vulkan13_features.h).

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "binary_occlusion_spirv.h"
#include "buffer_va_index.h"
#include "image_view_cache.h"
#include "occlusion_sum_spirv.h"
#include "queue_holds.h"
#include "retire_tickets.h"
#include "statistics_sum_spirv.h"
#include "vulkan13_features.h"
#include "vulkan_depth24.h"
#include "vulkan_indirect_dispatch_count.h"
#include "vulkan_scratch.h"
#include "vulkan_title_image_pool.h"
#include "vulkan_replay.h"
#include "queue_families.h"
#include "resource_tiling.h"
#include "xdxr_build.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include "frame_profile.h"
#include "host_shadow_profile.h"
#include "host_image_index.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace leonida {

// A CPU-written Vulkan-address instance pointer array, kept until the submission that builds with it completes.
// Completed stages are pooled per queue and reused, so a per-frame TLAS build does not allocate memory each frame.
struct KhrInstanceStage {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    std::uint64_t size = 0;  // bytes available in buffer, so a larger later list can be refused for a new stage
};

// One per-queue query pool of VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, retained on the
// CommandSlot until its submission completes and then recycled to the queue pool.
struct KhrAsQueryPool {
    VkQueryPool pool = VK_NULL_HANDLE;
    std::uint32_t capacity = 0;
    std::uint32_t used = 0;
};

// VK_EXT_debug_utils labels: an instance extension, so a device has them only when its instance enabled it.
struct DebugLabelFunctions {
    PFN_vkCmdInsertDebugUtilsLabelEXT cmd_insert = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT cmd_end = nullptr;
    PFN_vkQueueInsertDebugUtilsLabelEXT queue_insert = nullptr;
    PFN_vkQueueBeginDebugUtilsLabelEXT queue_begin = nullptr;
    PFN_vkQueueEndDebugUtilsLabelEXT queue_end = nullptr;

    bool complete() const { return cmd_insert && cmd_begin && cmd_end && queue_insert && queue_begin && queue_end; }
};

struct VulkanDeviceFunctions {
#define LEONIDA_VK_DEVICE_FUNCTIONS(X)                                                                         \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer)         \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateSemaphore)     \
    X(vkDestroySemaphore) X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores) X(vkSignalSemaphore)               \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2)                \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering)         \
    X(vkCmdClearAttachments) X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage)                         \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                  \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)  \
    X(vkUnmapMemory) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage)  \
    X(vkCmdResolveImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCreateSampler) X(vkDestroySampler)        \
    X(vkCmdBeginConditionalRenderingEXT) X(vkCmdEndConditionalRenderingEXT) X(vkCreateQueryPool)               \
    X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdWriteTimestamp)    \
    X(vkCmdCopyQueryPoolResults) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirectCount)    \
    X(vkCmdDrawIndexedIndirectCount) X(vkCmdDispatchIndirect) X(vkGetBufferDeviceAddress)                      \
    X(vkGetAccelerationStructureBuildSizesKHR) X(vkCreateAccelerationStructureKHR)                             \
    X(vkDestroyAccelerationStructureKHR) X(vkGetAccelerationStructureDeviceAddressKHR)                         \
    X(vkCmdBuildAccelerationStructuresKHR) X(vkCmdCopyAccelerationStructureKHR)                                \
    X(vkCmdWriteAccelerationStructuresPropertiesKHR)                                                           \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)                   \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                          \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule)      \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)            \
    X(vkCmdPushConstants) X(vkCmdDispatch)                                                                     \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor)                  \
    X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount)                                                   \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetStencilReference)                          \
    X(vkCmdSetBlendConstants) X(vkCmdDraw)                                                                              \
    X(vkCmdDrawIndexed) X(vkCreateGraphicsPipelines) X(vkQueueBindSparse) X(vkGetImageSparseMemoryRequirements)   \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache)                                                        \
    X(vkCreateEvent) X(vkDestroyEvent) X(vkResetEvent) X(vkCmdSetEvent) X(vkCmdWaitEvents) X(vkCmdDrawMeshTasksEXT) \
    X(vkCmdDrawMeshTasksIndirectEXT) \
    X(vkCreateRenderPass2) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)               \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)
#define LEONIDA_VK_MEMBER(name) PFN_##name name = nullptr;
    LEONIDA_VK_DEVICE_FUNCTIONS(LEONIDA_VK_MEMBER)
#undef LEONIDA_VK_MEMBER

    // The Vulkan 1.3 entry points may be null on older devices, the extension ones (conditional rendering, mesh
    // shaders) when the extension is not enabled, and the query functions are optional (a list that uses queries
    // without them is unsupported). The compute and descriptor functions are needed for normalized or summed
    // occlusion results.
    bool load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
#define LEONIDA_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(leonida::device_proc_or_khr(get, device, #name));
        LEONIDA_VK_DEVICE_FUNCTIONS(LEONIDA_VK_LOAD)
#undef LEONIDA_VK_LOAD
        return vkCreateCommandPool && vkDestroyCommandPool && vkAllocateCommandBuffers && vkResetCommandBuffer &&
               vkBeginCommandBuffer && vkEndCommandBuffer && vkQueueSubmit && vkQueueWaitIdle && vkCreateSemaphore &&
               vkDestroySemaphore && vkGetSemaphoreCounterValue && vkWaitSemaphores && vkSignalSemaphore &&
               vkCreateImageView && vkDestroyImageView && vkCmdPipelineBarrier && vkCmdClearColorImage &&
               vkCmdClearDepthStencilImage && vkCmdClearAttachments && vkDeviceWaitIdle && vkCreateImage &&
               vkDestroyImage && vkGetImageMemoryRequirements && vkBindImageMemory && vkCreateBuffer &&
               vkDestroyBuffer && vkGetBufferMemoryRequirements && vkBindBufferMemory && vkAllocateMemory &&
               vkFreeMemory && vkMapMemory && vkUnmapMemory && vkCmdCopyBuffer && vkCmdCopyBufferToImage &&
               vkCmdCopyImageToBuffer && vkCmdCopyImage && vkCmdResolveImage && vkCmdFillBuffer &&
               vkCmdUpdateBuffer && vkCmdBindPipeline && vkCmdPushConstants && vkCmdDispatch &&
               vkCreateShaderModule && vkDestroyShaderModule && vkCreatePipelineLayout &&
               vkDestroyPipelineLayout && vkCreateComputePipelines && vkDestroyPipeline &&
               vkGetBufferDeviceAddress && vkCmdBindIndexBuffer && vkCmdSetViewport && vkCmdSetScissor &&
               vkCmdSetStencilReference && vkCmdSetBlendConstants && vkCmdDraw && vkCmdDrawIndexed &&
               vkCreateGraphicsPipelines;
    }

    // Graphics pipelines also need these Vulkan 1.3 entry points (dynamic rendering, extended dynamic state).
    bool graphics_complete() const {
        return vkCmdBeginRendering && vkCmdEndRendering && vkCmdBindVertexBuffers2 && vkCmdSetPrimitiveTopology &&
               vkCmdSetPrimitiveRestartEnable;
    }
};

// Both kinds of compute resolve use the same storage buffer, descriptor set and pipeline layout.
// The destination needs transfer usage only. Pipelines are created on first use.
class OcclusionResolve {
public:
    OcclusionResolve(const VulkanDeviceFunctions& vk, VkDevice device) : vk_(vk), device_(device) {}
    OcclusionResolve(const OcclusionResolve&) = delete;
    OcclusionResolve& operator=(const OcclusionResolve&) = delete;

    ~OcclusionResolve() {
        if (sum_pipeline_) vk_.vkDestroyPipeline(device_, sum_pipeline_, nullptr);
        if (binary_pipeline_) vk_.vkDestroyPipeline(device_, binary_pipeline_, nullptr);
        if (statistics_pipeline_) vk_.vkDestroyPipeline(device_, statistics_pipeline_, nullptr);
        if (layout_) vk_.vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (pool_) vk_.vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (set_layout_) vk_.vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        if (scratch_) vk_.vkDestroyBuffer(device_, scratch_, nullptr);
        if (memory_) vk_.vkFreeMemory(device_, memory_, nullptr);
    }

    bool initialize(const VkPhysicalDeviceMemoryProperties& memory) {
        if (!vk_.vkCreateDescriptorSetLayout || !vk_.vkDestroyDescriptorSetLayout ||
            !vk_.vkCreateDescriptorPool || !vk_.vkDestroyDescriptorPool || !vk_.vkAllocateDescriptorSets ||
            !vk_.vkUpdateDescriptorSets || !vk_.vkCreatePipelineLayout || !vk_.vkDestroyPipelineLayout ||
            !vk_.vkCreateShaderModule || !vk_.vkDestroyShaderModule || !vk_.vkCreateComputePipelines ||
            !vk_.vkDestroyPipeline || !vk_.vkCmdBindPipeline || !vk_.vkCmdBindDescriptorSets ||
            !vk_.vkCmdPushConstants || !vk_.vkCmdDispatch)
            return false;

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = scratch_size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vk_.vkCreateBuffer(device_, &buffer, nullptr, &scratch_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk_.vkGetBufferMemoryRequirements(device_, scratch_, &requirements);
        auto type = memory.memoryTypeCount;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            if (type == memory.memoryTypeCount) type = i;
            if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                type = i;
                break;
            }
        }
        if (type == memory.memoryTypeCount) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (vk_.vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS ||
            vk_.vkBindBufferMemory(device_, scratch_, memory_, 0) != VK_SUCCESS)
            return false;

        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_layout.bindingCount = 1;
        set_layout.pBindings = &binding;
        if (vk_.vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vk_.vkCreateDescriptorPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set.descriptorPool = pool_;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &set_layout_;
        if (vk_.vkAllocateDescriptorSets(device_, &set, &set_) != VK_SUCCESS) return false;
        const VkDescriptorBufferInfo storage{scratch_, 0, scratch_size};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set_;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &storage;
        vk_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

        // The count, and for statistics_sum.comp whether the segments are mesh-pool results.
        const VkPushConstantRange count{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(std::uint32_t)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &set_layout_;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &count;
        return vk_.vkCreatePipelineLayout(device_, &layout, nullptr, &layout_) == VK_SUCCESS;
    }

    bool prepare(bool needs_sum, bool needs_binary, bool needs_statistics_sum = false,
                 bool needs_sum_binary = false) {
        // The composite (sum then normalize, D3D-252) uses both pipelines.
        needs_sum = needs_sum || needs_sum_binary;
        needs_binary = needs_binary || needs_sum_binary;
        return (!needs_sum || sum_pipeline_ || create_pipeline(occlusion_sum_spirv, sum_pipeline_)) &&
               (!needs_binary || binary_pipeline_ || create_pipeline(binary_occlusion_spirv, binary_pipeline_)) &&
               (!needs_statistics_sum || statistics_pipeline_ ||
                create_pipeline(statistics_sum_spirv, statistics_pipeline_));
    }

    // Adds `count` pipeline-statistics results into one D3D12_QUERY_DATA_PIPELINE_STATISTICS at dst + offset: the
    // segments of a statistics query that was split around helper dispatches (D3D-162) or mesh draws (D3D-291).
    // Segment i is in `mesh_queries` when mesh[i] is set (two counters: fragment and compute; see
    // statistics_sum.comp), otherwise in `queries` (all eleven). A mesh segment's clipping invocations come from
    // `primitives_queries`, the mesh-primitives count of that segment (D3D-MESH-CLIPPING-COUNTER); it is null when
    // the device has no meshShaderQueries, and the field then reads 0.
    void sum_statistics(VkCommandBuffer cb, VkQueryPool queries, VkQueryPool mesh_queries,
                        VkQueryPool primitives_queries, const std::uint8_t* mesh, std::uint32_t first,
                        std::uint32_t count, VkBuffer dst, VkDeviceSize offset) const {
        constexpr VkDeviceSize result = statistics_counters * word_size;
        constexpr VkDeviceSize mesh_slot = 7 * word_size;  // PSInvocations, where the mesh pool's first counter goes
        constexpr VkDeviceSize clipping_slot = 5 * word_size;  // CInvocations, where the mesh-primitives count goes
        begin_scratch_write(cb);
        vk_.vkCmdFillBuffer(cb, scratch_, 0, result, 0);
        bind(cb, statistics_pipeline_);
        while (count) {
            const bool in_mesh = mesh && mesh[first];
            std::uint32_t chunk = 1;
            while (chunk < std::min(count, statistics_capacity) && bool(mesh && mesh[first + chunk]) == in_mesh)
                ++chunk;
            if (in_mesh) {
                // The mesh pool holds only the fragment and compute counters, so the clipping field of a mesh
                // segment reads 0 unless the mesh-primitives count fills it (D3D-MESH-CLIPPING-COUNTER).
                vk_.vkCmdFillBuffer(cb, scratch_, result, chunk * result, 0);
                barrier(cb, transfer, transfer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            }
            vk_.vkCmdCopyQueryPoolResults(cb, in_mesh ? mesh_queries : queries, first, chunk, scratch_,
                                          result + (in_mesh ? mesh_slot : 0), result,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (in_mesh && primitives_queries)
                vk_.vkCmdCopyQueryPoolResults(cb, primitives_queries, first, chunk, scratch_, result + clipping_slot,
                                              result, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer | compute, compute, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    shader_access);
            dispatch(cb, chunk, 1, in_mesh);
            first += chunk;
            count -= chunk;
            if (count) barrier(cb, compute, transfer, shader_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        copy_result(cb, dst, offset, result);
    }

    void sum(VkCommandBuffer cb, VkQueryPool queries, std::uint32_t first, std::uint32_t count,
             VkBuffer dst, VkDeviceSize offset) const {
        begin_scratch_write(cb);
        vk_.vkCmdFillBuffer(cb, scratch_, 0, word_size, 0);
        bind(cb, sum_pipeline_);
        while (count) {
            const auto chunk = std::min(count, sum_capacity);
            vk_.vkCmdCopyQueryPoolResults(cb, queries, first, chunk, scratch_, word_size, word_size,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer | compute, compute, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    shader_access);
            dispatch(cb, chunk, 1);
            first += chunk;
            count -= chunk;
            if (count) barrier(cb, compute, transfer, shader_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        copy_result(cb, dst, offset, word_size);
    }

    // Sums `count` binary-occlusion segments into one value and normalizes that total in place to 0 or 1: the
    // result of a binary query whose interval spans more than one physical segment (overlapping queries, D3D-252).
    void sum_binary(VkCommandBuffer cb, VkQueryPool queries, std::uint32_t first, std::uint32_t count,
                    VkBuffer dst, VkDeviceSize offset) const {
        begin_scratch_write(cb);
        vk_.vkCmdFillBuffer(cb, scratch_, 0, word_size, 0);
        bind(cb, sum_pipeline_);
        auto tail = first, left = count;
        while (left) {
            const auto chunk = std::min(left, sum_capacity);
            vk_.vkCmdCopyQueryPoolResults(cb, queries, tail, chunk, scratch_, word_size, word_size,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer | compute, compute, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    shader_access);
            dispatch(cb, chunk, 1);
            tail += chunk;
            left -= chunk;
            if (left) barrier(cb, compute, transfer, shader_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        barrier(cb, compute, compute, VK_ACCESS_SHADER_WRITE_BIT, shader_access);
        bind(cb, binary_pipeline_);
        dispatch(cb, 1, 1);
        copy_result(cb, dst, offset, word_size);
    }

    void normalize(VkCommandBuffer cb, VkQueryPool queries, std::uint32_t first, std::uint32_t count,
                   VkBuffer dst, VkDeviceSize offset) const {
        bind(cb, binary_pipeline_);
        while (count) {
            const auto chunk = std::min(count, binary_capacity);
            const VkDeviceSize bytes = VkDeviceSize(chunk) * word_size;
            begin_scratch_write(cb);
            vk_.vkCmdCopyQueryPoolResults(cb, queries, first, chunk, scratch_, 0, word_size,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer, compute, VK_ACCESS_TRANSFER_WRITE_BIT, shader_access);
            dispatch(cb, chunk, (chunk + binary_group_size - 1) / binary_group_size);
            copy_result(cb, dst, offset, bytes);
            first += chunk;
            count -= chunk;
            offset += bytes;
        }
    }

private:
    bool create_pipeline(std::span<const std::uint32_t> code, VkPipeline& out) const {
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = code.size_bytes();
        module.pCode = code.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vk_.vkCreateShaderModule(device_, &module, nullptr, &shader) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        pipeline.layout = layout_;
        VkPipeline handle = VK_NULL_HANDLE;
        const VkResult result = vk_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &handle);
        vk_.vkDestroyShaderModule(device_, shader, nullptr);
        if (result == VK_SUCCESS) out = handle;
        else if (handle) vk_.vkDestroyPipeline(device_, handle, nullptr);
        return result == VK_SUCCESS;
    }

    void bind(VkCommandBuffer cb, VkPipeline pipeline) const {
        vk_.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vk_.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    }

    void dispatch(VkCommandBuffer cb, std::uint32_t chunk, std::uint32_t groups, bool mesh = false) const {
        const std::uint32_t range[2] = {chunk, mesh ? 1u : 0u};
        vk_.vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(range), range);
        vk_.vkCmdDispatch(cb, groups, 1, 1);
    }

    void begin_scratch_write(VkCommandBuffer cb) const {
        // A sum or a normalization in an earlier submission may still use this buffer.
        barrier(cb, transfer | compute, transfer,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | shader_access,
                VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    void copy_result(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize bytes) const {
        barrier(cb, compute, transfer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy copy{0, offset, bytes};
        vk_.vkCmdCopyBuffer(cb, scratch_, dst, 1, &copy);
    }

    void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                 VkAccessFlags before, VkAccessFlags after) const {
        VkBufferMemoryBarrier memory{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        memory.srcAccessMask = before;
        memory.dstAccessMask = after;
        memory.srcQueueFamilyIndex = memory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.buffer = scratch_;
        memory.size = scratch_size;
        vk_.vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 1, &memory, 0, nullptr);
    }

    static constexpr auto transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    static constexpr auto compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    static constexpr VkAccessFlags shader_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    static constexpr std::uint32_t sum_capacity = 256;  // one more word holds the running sum
    static constexpr std::uint32_t binary_capacity = 1024;
    static constexpr std::uint32_t binary_group_size = 64;  // local_size_x in binary_occlusion.comp
    static constexpr std::uint32_t statistics_counters = 11;  // local_size_x in statistics_sum.comp
    static constexpr std::uint32_t statistics_capacity = 64;  // segments per pass; 11 more words hold the sums
    static constexpr VkDeviceSize word_size = sizeof(std::uint64_t);
    static constexpr VkDeviceSize scratch_size =
        std::max({sum_capacity + 1, binary_capacity, (statistics_capacity + 1) * statistics_counters}) * word_size;
    const VulkanDeviceFunctions& vk_;
    VkDevice device_;
    VkBuffer scratch_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline sum_pipeline_ = VK_NULL_HANDLE;
    VkPipeline binary_pipeline_ = VK_NULL_HANDLE;
    VkPipeline statistics_pipeline_ = VK_NULL_HANDLE;
};

struct SparseResource;

// Zero-allocation O(1) Ring Buffer deferred deletion queue tied to the device timeline semaphore.
class DeferredDeletionQueue {
public:
    enum class Kind : std::uint8_t {
        bindless_slot,
        image_view,
        image,
        pooled_title_image,
        pooled_staging_buffer,
        sparse_resource,
        buffer,
        device_memory,
        acceleration_structure,
        framebuffer,
    };

    struct Item {
        std::uint64_t ticket = 0;
        std::uint64_t handle = 0;  // for bindless_slot: (uint64_t(dimension) << 32) | index
        Kind kind = Kind::image_view;
    };

    using BindlessReleaseFn = void (*)(void* user, std::uint32_t dimension, std::uint32_t index);

    explicit DeferredDeletionQueue(std::size_t initial_capacity = 256) : pending_(next_pow2(initial_capacity)) {}

    void set_bindless_release(BindlessReleaseFn fn, void* user) {
        std::lock_guard lock(mutex_);
        bindless_release_fn_ = fn;
        bindless_release_user_ = user;
    }

    void push(std::uint64_t retire_ticket, Kind kind, std::uint64_t handle) {
        if (!handle && kind != Kind::bindless_slot) return;
        std::lock_guard lock(mutex_);
        if (count_ == pending_.size()) {
            const std::size_t old_cap = pending_.size();
            std::vector<Item> grown(old_cap * 2);
            const std::size_t first = old_cap - head_;
            std::memcpy(grown.data(), pending_.data() + head_, first * sizeof(Item));
            if (head_ > 0) std::memcpy(grown.data() + first, pending_.data(), head_ * sizeof(Item));
            pending_ = std::move(grown);
            head_ = 0;
            tail_ = count_;
        }
        pending_[tail_] = {retire_ticket, handle, kind};
        tail_ = (tail_ + 1) & (pending_.size() - 1);
        ++count_;
    }

    // Drains all items with ticket <= current_ticket in O(k), invoking vkDestroy* outside the lock.
    // Uses a stack-local batch vector so concurrent drain() calls from multiple queues never race.
    void drain(std::uint64_t current_ticket, VkDevice device, const VulkanDeviceFunctions& vk) {
        BindlessReleaseFn release_fn = nullptr;
        void* release_user = nullptr;
        std::vector<Item> to_destroy;
        {
            std::lock_guard lock(mutex_);
            if (count_ == 0 || pending_[head_].ticket > current_ticket) return;
            release_fn = bindless_release_fn_;
            release_user = bindless_release_user_;
            const std::size_t mask = pending_.size() - 1;
            while (count_ > 0 && pending_[head_].ticket <= current_ticket) {
                to_destroy.push_back(pending_[head_]);
                head_ = (head_ + 1) & mask;
                --count_;
            }
            if (count_ == 0) {
                head_ = 0;
                tail_ = 0;
            }
        }

        for (const Item& item : to_destroy) {
            switch (item.kind) {
            case Kind::bindless_slot:
                if (release_fn) {
                    release_fn(release_user, static_cast<std::uint32_t>(item.handle >> 32),
                               static_cast<std::uint32_t>(item.handle & 0xFFFFFFFFu));
                }
                break;
            case Kind::image_view:
                vk.vkDestroyImageView(device, reinterpret_cast<VkImageView>(item.handle), nullptr);
                break;
            case Kind::image:
                vk.vkDestroyImage(device, reinterpret_cast<VkImage>(item.handle), nullptr);
                break;
            case Kind::pooled_title_image:
                delete reinterpret_cast<PooledTitleImage*>(item.handle);
                break;
            case Kind::pooled_staging_buffer:
                delete reinterpret_cast<PooledStagingBuffer*>(item.handle);
                break;
            case Kind::sparse_resource:
                // Releases the resource and its page backing together after outstanding submissions.
                delete reinterpret_cast<std::shared_ptr<SparseResource>*>(item.handle);
                break;
            case Kind::buffer:
                vk.vkDestroyBuffer(device, reinterpret_cast<VkBuffer>(item.handle), nullptr);
                break;
            case Kind::device_memory:
                vk.vkFreeMemory(device, reinterpret_cast<VkDeviceMemory>(item.handle), nullptr);
                break;
            case Kind::acceleration_structure:
                if (vk.vkDestroyAccelerationStructureKHR)
                    vk.vkDestroyAccelerationStructureKHR(
                        device, reinterpret_cast<VkAccelerationStructureKHR>(item.handle), nullptr);
                break;
            case Kind::framebuffer:
                vk.vkDestroyFramebuffer(device, reinterpret_cast<VkFramebuffer>(item.handle), nullptr);
                break;
            }
        }
    }

private:
    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 16;
        while (p < n) p <<= 1;
        return p;
    }

    std::mutex mutex_;
    std::vector<Item> pending_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
    BindlessReleaseFn bindless_release_fn_ = nullptr;
    void* bindless_release_user_ = nullptr;
};

// Centralized single-thread fence dispatcher using Vulkan 1.2 vkWaitSemaphores(VK_SEMAPHORE_WAIT_ANY_BIT).
// Eliminates per-fence OS threads and 10ms polling: a synthetic timeline semaphore (wakeup_sem_) at index 0
// wakes vkWaitSemaphores immediately whenever a new wait, a released fence, or shutdown arrives while waiting_ == true.
inline void publish_fence_completion(std::atomic<std::uint64_t>& value, std::uint64_t completed) noexcept {
    auto previous = value.load(std::memory_order_relaxed);
    while (previous < completed &&
           !value.compare_exchange_weak(previous, completed, std::memory_order_release, std::memory_order_relaxed)) {}
}

class FenceWaitDispatcher {
public:
    struct Entry {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        std::uint64_t value = 0;
        void* event_handle = nullptr;
        std::function<void()> callback;
    };

    // A dispatch runs outside mutex_. Each entry in a ready batch gets its own slot: a callback may re-entrantly
    // call cancel() for a later entry in the same batch, which marks that entry cancelled so it is skipped before
    // it can use the semaphore. A callback running on another thread must still be waited for.
    // A cancelled entry still in pending_ is dispatched by cancel() itself (callback and bookkeeping both run);
    // one already in a ready batch is skipped and only its event is signalled. The held Signal op's count is
    // therefore not decremented for a skipped batch entry. That is safe because only ~Fence cancels a wait, but
    // keep it in mind if cancel() gains other callers.
    struct DispatchSlot {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        std::thread::id owner;
        bool active = false;     // guarded by mutex_
        bool cancelled = false;  // guarded by mutex_
    };

    using SignalEventFn = void (*)(void* event_handle);

    // Longest single vkWaitSemaphores call in run(): bounds how long the thread can sit in an uninterruptible wait.
    static constexpr std::uint64_t kWaitSliceNs = 100'000'000;  // 100 ms

    FenceWaitDispatcher(VkDevice device, const VulkanDeviceFunctions& vk, std::atomic<bool>& device_lost)
        : device_(device), vk_(vk), device_lost_(device_lost) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        if (vk_.vkCreateSemaphore(device_, &info, nullptr, &wakeup_sem_) != VK_SUCCESS)
            wakeup_sem_ = VK_NULL_HANDLE;
        semaphores_.reserve(64);
        values_.reserve(64);
    }

    ~FenceWaitDispatcher() {
        stop();
        if (wakeup_sem_) vk_.vkDestroySemaphore(device_, wakeup_sem_, nullptr);
    }

    FenceWaitDispatcher(const FenceWaitDispatcher&) = delete;
    FenceWaitDispatcher& operator=(const FenceWaitDispatcher&) = delete;

    void set_signal_event_fn(SignalEventFn fn) {
        std::lock_guard lock(mutex_);
        signal_event_ = fn;
    }

    void enqueue(VkSemaphore sem, std::uint64_t value, void* event_handle) {
        if (!sem || !event_handle) return;
        SignalEventFn immediate_fn = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (stop_ || device_lost_.load(std::memory_order_acquire)) {
                immediate_fn = signal_event_;
            } else {
                pending_.push_back({sem, value, event_handle, {}});
                if (!thread_.joinable() && wakeup_sem_) thread_ = std::thread([this] { run(); });
                signal_wakeup_locked();
            }
        }
        if (immediate_fn) {
            immediate_fn(event_handle);
            return;
        }
        cv_.notify_one();
    }

    void enqueue_callback(VkSemaphore sem, std::uint64_t value, std::function<void()> cb) {
        if (!sem || !cb) return;
        std::function<void()> immediate_cb;
        {
            std::lock_guard lock(mutex_);
            if (stop_ || device_lost_.load(std::memory_order_acquire)) {
                immediate_cb = std::move(cb);
            } else {
                pending_.push_back({sem, value, nullptr, std::move(cb)});
                if (!thread_.joinable() && wakeup_sem_) thread_ = std::thread([this] { run(); });
                signal_wakeup_locked();
            }
        }
        if (immediate_cb) {
            immediate_cb();
            return;
        }
        cv_.notify_one();
    }

    // The caller keeps the cell and semaphore alive until cancel() returns. The worker publishes only native
    // counter observations; a queue's promised signal value must never be stored into this CPU-readable mirror.
    bool watch_completion(VkSemaphore sem, std::atomic<std::uint64_t>& value) {
        if (!sem || !wakeup_sem_ || !vk_.vkGetSemaphoreCounterValue || !vk_.vkWaitSemaphores) return false;
        {
            std::lock_guard lock(mutex_);
            if (stop_) return false;
            if (device_lost_.load(std::memory_order_acquire)) {
                publish_fence_completion(value, UINT64_MAX);
                return true;
            }
            for (const auto& watch : watchers_)
                if (watch.semaphore == sem) return watch.value == &value;
            std::uint64_t completed = 0;
            const VkResult queried = vk_.vkGetSemaphoreCounterValue(device_, sem, &completed);
            if (queried == VK_ERROR_DEVICE_LOST) {
                device_lost_.store(true, std::memory_order_release);
                publish_fence_completion(value, UINT64_MAX);
                publish_lost_watchers_locked();
                signal_wakeup_locked();
                cv_.notify_all();
                return true;
            }
            if (queried != VK_SUCCESS) return false;
            if (completed == UINT64_MAX) {
                publish_fence_completion(value, completed);
                return true;  // no larger native timeline value can be awaited
            }
            try {
                // Reserve before storing the caller's pointer or starting the worker. A failed registration
                // cannot leave a watcher behind, including when the OS cannot create the dispatch thread.
                semaphores_.reserve(1 + pending_.size() + watchers_.size() + 1);
                values_.reserve(1 + pending_.size() + watchers_.size() + 1);
                watchers_.push_back({sem, &value, completed});
                try {
                    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
                } catch (...) {
                    watchers_.pop_back();
                    return false;
                }
            } catch (const std::bad_alloc&) {
                return false;
            } catch (const std::length_error&) {
                return false;
            }
            publish_fence_completion(value, completed);
            signal_wakeup_locked();
        }
        cv_.notify_one();
        return true;
    }

    // Wakes run() when another path marks the device lost so pending waits complete and stop()/cancel() never hang.
    void notify_device_lost() {
        {
            std::lock_guard lock(mutex_);
            signal_wakeup_locked();
        }
        cv_.notify_all();
    }

    // Completes the pending waits for `sem` (a released D3D12 fence counts as reached, as on the PC runtime) and
    // blocks until both a vkWaitSemaphores on `sem` and any callback already dispatched for it have finished, before
    // the caller destroys the VkSemaphore.
    void cancel(VkSemaphore sem) {
        if (!sem) return;
        std::vector<Entry> cancelled;
        SignalEventFn fn = nullptr;
        {
            std::unique_lock lock(mutex_);
            fn = signal_event_;
            // No mirror pointer can be queried or published after this erasure. The wait snapshot may still
            // name its semaphore; the existing epoch join below retires that snapshot before destruction.
            std::erase_if(watchers_, [sem](const auto& watch) { return watch.semaphore == sem; });
            std::size_t write = 0;
            for (std::size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i].semaphore == sem) {
                    cancelled.push_back(std::move(pending_[i]));
                } else {
                    if (write != i) pending_[write] = std::move(pending_[i]);
                    ++write;
                }
            }
            pending_.resize(write);
            // Entries already taken out of pending_ but not yet started belong to a live ready batch. Mark them
            // cancelled so dispatch skips them; this is how a re-entrant cancel() from another callback in the
            // same batch is completed without waiting on the callback that is already on this thread's stack.
            for (const auto& slot : dispatching_) {
                if (slot->semaphore == sem && !slot->active) slot->cancelled = true;
            }
            if (thread_.joinable() && (waiting_ || callback_on_other_thread(sem))) {
                const std::uint64_t target_epoch = epoch_ + 1;
                signal_wakeup_locked();
                epoch_cv_.wait(lock, [this, sem, target_epoch] {
                    return (!waiting_ || epoch_ >= target_epoch) && !callback_on_other_thread(sem);
                });
            }
        }
        dispatch_entries(cancelled, fn);
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (stop_) return;
            stop_ = true;
            watchers_.clear();
            signal_wakeup_locked();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    struct CompletionWatch {
        VkSemaphore semaphore;
        std::atomic<std::uint64_t>* value;
        std::uint64_t observed;
    };

    void publish_lost_watchers_locked() {
        for (const auto& watch : watchers_) publish_fence_completion(*watch.value, UINT64_MAX);
        watchers_.clear();
    }

    static void dispatch_entries(std::vector<Entry>& entries, SignalEventFn signal_event) {
        for (Entry& e : entries) {
            if (e.callback) e.callback();
            else if (signal_event && e.event_handle) signal_event(e.event_handle);
        }
    }

    // True only for a callback already running on another thread. A slot that has not started yet is cancelled by
    // cancel() and skipped, so it never uses a destroyed semaphore and must not hold up the cancelling caller.
    bool callback_on_other_thread(VkSemaphore sem) const {
        const std::thread::id me = std::this_thread::get_id();
        return std::any_of(dispatching_.begin(), dispatching_.end(), [&](const auto& d) {
            return d->semaphore == sem && d->active && d->owner != me;
        });
    }

    // mutex_ is held on entry and again on return. Recording every entry before unlocking closes the window where
    // run() has taken them out of pending_ but has not started the callback, yet cancel() can return and let the
    // caller destroy the semaphore. Each entry has its own slot so a cancel() for a later entry in the same batch
    // skips that entry instead of being mistaken for the callback already on the cancelling thread's stack.
    void dispatch_outside_lock(std::unique_lock<std::mutex>& lock, std::vector<Entry>& entries, SignalEventFn fn) {
        const std::thread::id me = std::this_thread::get_id();
        std::vector<std::shared_ptr<DispatchSlot>> slots;
        slots.reserve(entries.size());
        for (const Entry& e : entries) {
            auto slot = std::make_shared<DispatchSlot>();
            slot->semaphore = e.semaphore;
            slot->owner = me;
            slots.push_back(slot);
            dispatching_.push_back(slot);
        }
        lock.unlock();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            Entry& e = entries[i];
            DispatchSlot& slot = *slots[i];
            bool skip = false;
            {
                std::lock_guard guard(mutex_);
                skip = slot.cancelled;
                if (!skip) slot.active = true;
            }
            if (skip) {
                // A re-entrant cancel() removed this wait; still complete its event so no waiter is left behind.
                if (fn && e.event_handle) fn(e.event_handle);
                continue;
            }
            if (e.callback) e.callback();
            else if (fn && e.event_handle) fn(e.event_handle);
            {
                std::lock_guard guard(mutex_);
                slot.active = false;
                const auto it = std::find(dispatching_.begin(), dispatching_.end(), slots[i]);
                if (it != dispatching_.end()) dispatching_.erase(it);
            }
            epoch_cv_.notify_all();
        }
        lock.lock();
        for (const auto& slot : slots) {
            const auto it = std::find(dispatching_.begin(), dispatching_.end(), slot);
            if (it != dispatching_.end()) dispatching_.erase(it);
        }
        epoch_cv_.notify_all();
    }

    // Only signals wakeup_sem_ when run() is actively waiting (waiting_ == true) on wakeup_val_ + 1,
    // eliminating any lost-wakeup race between snapshot construction and vkWaitSemaphores. Keep signaling even if
    // device_lost_ is already set so a blocked vkWaitSemaphores wakes cleanly for cancel()/stop().
    void signal_wakeup_locked() {
        if (!waiting_ || !wakeup_sem_) return;
        const std::uint64_t next = ++wakeup_val_;
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore = wakeup_sem_;
        si.value = next;
        vk_.vkSignalSemaphore(device_, &si);
    }

    void run() {
        std::unique_lock lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] {
                return stop_ || !pending_.empty() || !watchers_.empty() || device_lost_.load(std::memory_order_acquire);
            });
            if (device_lost_.load(std::memory_order_acquire)) {
                publish_lost_watchers_locked();
                std::vector<Entry> drained = std::move(pending_);
                pending_.clear();
                dispatch_outside_lock(lock, drained, signal_event_);
                return;
            }
            if (stop_) return;

            semaphores_.clear();
            values_.clear();
            semaphores_.push_back(wakeup_sem_);
            values_.push_back(wakeup_val_ + 1);
            for (const Entry& e : pending_) {
                semaphores_.push_back(e.semaphore);
                values_.push_back(e.value);
            }
            for (const auto& watch : watchers_) {
                semaphores_.push_back(watch.semaphore);
                values_.push_back(watch.observed + 1);
            }

            // The native wait must keep a private immutable snapshot. Registration may reserve the staging
            // vectors while this mutex is unlocked, but cannot invalidate these local vectors' Vulkan pointers.
            auto wait_semaphores = std::move(semaphores_);
            auto wait_values = std::move(values_);

            waiting_ = true;
            lock.unlock();

            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.flags = VK_SEMAPHORE_WAIT_ANY_BIT;
            wi.semaphoreCount = static_cast<std::uint32_t>(wait_semaphores.size());
            wi.pSemaphores = wait_semaphores.data();
            wi.pValues = wait_values.data();
            // A bounded slice, not UINT64_MAX. Under GPU paravirtualization (measured in Windows Sandbox on an
            // RTX 5060 Ti) this wait is a kernel wait the OS cannot interrupt: a process that exits while a wait
            // is pending (d3d12_queue_wait_timeout's _Exit, or quitting with a fence outstanding) never finishes
            // exiting, and TerminateProcess cannot end it either. A timeout returns here and the loop re-arms the
            // same wait; completions still wake it at once through the ANY wait.
            const VkResult res = vk_.vkWaitSemaphores(device_, &wi, kWaitSliceNs);

            lock.lock();
            waiting_ = false;
            if (semaphores_.capacity() < wait_semaphores.capacity()) semaphores_.swap(wait_semaphores);
            if (values_.capacity() < wait_values.capacity()) values_.swap(wait_values);
            ++epoch_;
            epoch_cv_.notify_all();

            if (res == VK_ERROR_DEVICE_LOST || device_lost_.load(std::memory_order_acquire)) {
                device_lost_.store(true, std::memory_order_release);
                publish_lost_watchers_locked();
                std::vector<Entry> drained = std::move(pending_);
                pending_.clear();
                dispatch_outside_lock(lock, drained, signal_event_);
                return;
            }
            if (stop_) return;

            bool watch_lost = false;
            std::size_t watch_write = 0;
            for (std::size_t i = 0; i < watchers_.size(); ++i) {
                auto watch = watchers_[i];
                std::uint64_t completed = 0;
                const VkResult queried = vk_.vkGetSemaphoreCounterValue(device_, watch.semaphore, &completed);
                if (queried == VK_ERROR_DEVICE_LOST) { watch_lost = true; break; }
                if (queried != VK_SUCCESS) {
                    // Keep the last proven value on a non-device-loss error. Do not leave a permanently ready
                    // stale threshold spinning the worker or publish an invented terminal completion value.
                    static std::atomic<unsigned> failures{0};
                    if (failures.fetch_add(1, std::memory_order_relaxed) < 8)
                        std::fprintf(stderr, "[rt] fence: completion mirror stopped after native counter query %d\n",
                                     int(queried));
                    continue;
                }
                publish_fence_completion(*watch.value, completed);
                if (completed == UINT64_MAX) continue;
                watch.observed = completed;
                watchers_[watch_write++] = watch;
            }
            if (watch_lost) {
                device_lost_.store(true, std::memory_order_release);
                publish_lost_watchers_locked();
                std::vector<Entry> drained = std::move(pending_);
                pending_.clear();
                dispatch_outside_lock(lock, drained, signal_event_);
                return;
            }
            watchers_.resize(watch_write);

            std::vector<Entry> ready;
            bool lost = false;
            std::size_t write = 0;
            for (std::size_t i = 0; i < pending_.size(); ++i) {
                std::uint64_t done = 0;
                const VkResult cr = vk_.vkGetSemaphoreCounterValue(device_, pending_[i].semaphore, &done);
                if (cr == VK_ERROR_DEVICE_LOST) {
                    lost = true;
                    for (std::size_t j = i; j < pending_.size(); ++j) ready.push_back(std::move(pending_[j]));
                    break;
                }
                if (cr == VK_SUCCESS && done >= pending_[i].value) {
                    ready.push_back(std::move(pending_[i]));
                } else {
                    if (write != i) pending_[write] = std::move(pending_[i]);
                    ++write;
                }
            }
            if (lost) {
                pending_.resize(write);
                for (Entry& e : pending_) ready.push_back(std::move(e));
                pending_.clear();
                device_lost_.store(true, std::memory_order_release);
                publish_lost_watchers_locked();
                dispatch_outside_lock(lock, ready, signal_event_);
                return;
            }
            pending_.resize(write);
            if (!ready.empty()) {
                dispatch_outside_lock(lock, ready, signal_event_);
                if (stop_) return;
            }
        }
    }

    VkDevice device_;
    const VulkanDeviceFunctions& vk_;
    std::atomic<bool>& device_lost_;
    std::vector<CompletionWatch> watchers_;  // cells are accessed only while mutex_ is held
    SignalEventFn signal_event_ = nullptr;
    VkSemaphore wakeup_sem_ = VK_NULL_HANDLE;
    std::uint64_t wakeup_val_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable epoch_cv_;
    std::vector<Entry> pending_;
    std::vector<std::shared_ptr<DispatchSlot>> dispatching_;  // ready-batch callbacks outside mutex_
    std::vector<VkSemaphore> semaphores_;
    std::vector<std::uint64_t> values_;
    std::uint64_t epoch_ = 0;
    bool waiting_ = false;
    bool stop_ = false;
    std::thread thread_;
};

// A range of a D3D12 heap allocation. Mappings and in-flight unbinds keep the arena range alive.
struct TileHeapAllocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    std::uint32_t memory_type = 0;
};

struct TileMapping {
    std::shared_ptr<const TileHeapAllocation> heap;
    VkDeviceSize offset = 0;
    bool operator==(const TileMapping&) const = default;
};

struct SparseResource {
    SparseResource(VkDevice device, const VulkanDeviceFunctions& vk) : device(device), vk(vk) {}
    SparseResource(const SparseResource&) = delete;
    SparseResource& operator=(const SparseResource&) = delete;
    ~SparseResource() {
        if (image) vk.vkDestroyImage(device, image, nullptr);
        if (buffer) vk.vkDestroyBuffer(device, buffer, nullptr);
    }

    VkDevice device;
    const VulkanDeviceFunctions& vk;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkExtent2D image_extent{};
    std::uint64_t tile_count = 0;
    std::uint32_t memory_types = 0;
    std::map<std::uint64_t, TileMapping> mapped;
};

struct TileRegion {
    std::uint64_t first = 0;
    std::uint64_t count = 0;
};

enum class TileRangeMode { sequential, unmap, skip, reuse };

struct TileRange {
    TileRangeMode mode;
    std::uint64_t first;
    std::uint64_t count;
};

// Per device: what the queues share. Resources are registered by their stable ID (the COM resource pointer).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // intentional alignas(64) cache-line padding to prevent false sharing
#endif
class VulkanContext {
public:
    // mesh_queries: VK_EXT_mesh_shader's meshShaderQueries feature is enabled on `device`, so a mesh segment's
    // clipping invocations can come from a mesh-primitives query (D3D-MESH-CLIPPING-COUNTER). Set by the device
    // that created the context.
    bool mesh_queries = false;
    // conditional_rendering: VK_EXT_conditional_rendering is enabled on `device` (SetPredication).
    // ray_tracing: VK_KHR_acceleration_structure + VK_KHR_ray_query are enabled on `device`.
    // xdxr: acceleration-structure builds write XDXR BVH4 records (xdxr_build.h) instead; needs bufferDeviceAddress.
    VulkanContext(PFN_vkGetDeviceProcAddr get, VkDevice device, bool separate_depth_stencil_layouts,
                  bool conditional_rendering = false, bool ray_tracing = false, bool xdxr = false)
        : device_(device), tracker_(separate_depth_stencil_layouts), ray_tracing_(ray_tracing) {
        loaded_ = vk.load(get, device);
        replay_.pipeline_barrier = vk.vkCmdPipelineBarrier;
        replay_.clear_color_image = vk.vkCmdClearColorImage;
        replay_.clear_depth_stencil_image = vk.vkCmdClearDepthStencilImage;
        replay_.pipeline_barrier2 = vk.vkCmdPipelineBarrier2;
        replay_.begin_rendering = vk.vkCmdBeginRendering;
        replay_.end_rendering = vk.vkCmdEndRendering;
        replay_.clear_attachments = vk.vkCmdClearAttachments;
        replay_.copy_buffer = vk.vkCmdCopyBuffer;
        replay_.copy_buffer_to_image = vk.vkCmdCopyBufferToImage;
        replay_.copy_image_to_buffer = vk.vkCmdCopyImageToBuffer;
        replay_.copy_image = vk.vkCmdCopyImage;
        replay_.resolve_image = vk.vkCmdResolveImage;
        replay_.fill_buffer = vk.vkCmdFillBuffer;
        replay_.update_buffer = vk.vkCmdUpdateBuffer;
        replay_.draw_indirect = vk.vkCmdDrawIndirect;
        replay_.draw_indexed_indirect = vk.vkCmdDrawIndexedIndirect;
        replay_.draw_indirect_count = vk.vkCmdDrawIndirectCount;
        replay_.draw_indexed_indirect_count = vk.vkCmdDrawIndexedIndirectCount;
        replay_.dispatch_indirect = vk.vkCmdDispatchIndirect;
        if (conditional_rendering) {  // a driver may hand out these pointers even when the extension is off
            replay_.begin_conditional_rendering = vk.vkCmdBeginConditionalRenderingEXT;
            replay_.end_conditional_rendering = vk.vkCmdEndConditionalRenderingEXT;
        }
        replay_.reset_query_pool = vk.vkCmdResetQueryPool;
        replay_.begin_query = vk.vkCmdBeginQuery;
        replay_.end_query = vk.vkCmdEndQuery;
        replay_.write_timestamp = vk.vkCmdWriteTimestamp;
        replay_.copy_query_pool_results = vk.vkCmdCopyQueryPoolResults;
        replay_.bind_pipeline = vk.vkCmdBindPipeline;
        replay_.push_constants = vk.vkCmdPushConstants;
        replay_.bind_descriptor_sets = vk.vkCmdBindDescriptorSets;
        replay_.dispatch = vk.vkCmdDispatch;
        replay_.compute_pipeline = [this](std::uintptr_t id) -> const ComputePipeline* {
            const auto it = pipelines_.find(id);
            return it == pipelines_.end() ? nullptr : &it->second;
        };
        replay_.bind_vertex_buffers2 = vk.vkCmdBindVertexBuffers2;
        replay_.bind_index_buffer = vk.vkCmdBindIndexBuffer;
        replay_.set_viewport = vk.vkCmdSetViewport;
        replay_.set_scissor = vk.vkCmdSetScissor;
        replay_.set_viewport_with_count = vk.vkCmdSetViewportWithCount;
        replay_.set_scissor_with_count = vk.vkCmdSetScissorWithCount;
        replay_.set_primitive_topology = vk.vkCmdSetPrimitiveTopology;
        replay_.set_primitive_restart_enable = vk.vkCmdSetPrimitiveRestartEnable;
        replay_.set_stencil_reference = vk.vkCmdSetStencilReference;
        replay_.set_blend_constants = vk.vkCmdSetBlendConstants;
        replay_.draw = vk.vkCmdDraw;
        replay_.draw_mesh_tasks = vk.vkCmdDrawMeshTasksEXT;  // null unless VK_EXT_mesh_shader is enabled
        replay_.draw_mesh_tasks_indirect = vk.vkCmdDrawMeshTasksIndirectEXT;
        replay_.draw_indexed = vk.vkCmdDrawIndexed;
        replay_.graphics_pipeline = [this](std::uintptr_t id) -> const GraphicsPipeline* {
            const auto it = graphics_pipelines_.find(id);
            return it == graphics_pipelines_.end() ? nullptr : &it->second;
        };
        // Called with resource_mutex_ held. A binary search of the buffers' VA ranges (buffer_va_index.h), kept in step
        // with resources_ by set_resource_locked and remove_resource.
        // A range that runs past its buffer is cut at the buffer's end (`size` is updated): D3D12 reads past a view's
        // buffer as out of bounds, which is no reason to drop the submission.
        replay_.buffer_at = [this](std::uint64_t va, std::uint64_t& size, VkBuffer& buffer, VkDeviceSize& offset) {
            BufferVaIndex<ResourceId>::Hit hit{};
            if (!buffer_vas_.find(va, hit, size)) return false;  // prefers an aliasing buffer that holds all of it
            const ResourceBinding* b = resolve(hit.id);
            if (!b) return false;
            buffer = b->buffer;
            offset = b->buffer_offset + (va - b->va);
            size = std::min<std::uint64_t>(size, b->size - (va - b->va));
            return true;
        };
        // Host reads of small CPU-written inputs. Unlike buffer_at, the whole range must be in one CPU-writable
        // mapping: a partial read would see bytes the application has not written through the D3D12 API. The
        // binding's host pointer is null on DEFAULT and READBACK heaps, whose mapped bytes the GPU can write.
        replay_.host_at = [this](std::uint64_t va, std::uint64_t bytes, const std::uint8_t*& host) {
            BufferVaIndex<ResourceId>::Hit hit{};
            if (!buffer_vas_.find(va, hit, bytes)) return HostReadStatus::absent;
            const ResourceBinding* b = resolve(hit.id);
            if (!b) return HostReadStatus::absent;
            if (!b->host) return HostReadStatus::not_cpu_written;
            const std::uint64_t offset = va - b->va;
            if (offset > b->size || bytes > b->size - offset) return HostReadStatus::absent;
            host = b->host + offset;
            return HostReadStatus::ok;
        };
        if (xdxr && loaded_) {
            xdxr_ = std::make_unique<XdxrBuilder>(get, device);
            if (xdxr_->loaded()) {
                replay_.xdxr_check = [](const BuildAccelerationStructure& b) {
                    XdxrLayout layout;
                    // An update needs ALLOW_UPDATE (D3D12 requires it on the build and the update): a structure
                    // built without it may have paired leaves, which a refit would rewrite as wrong geometry.
                    return xdxr_inputs(b, nullptr, 0) && layout.compute(xdxr_elements(b), b.is_tlas, xdxr_box16(b)) &&
                           b.dst_va && b.scratch_va && (!b.is_tlas || !b.instance_count || b.instances_va) &&
                           (!b.update || (b.build_flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0);
                };
                replay_.xdxr_record = [this](VkCommandBuffer cb, const BuildAccelerationStructure& b) {
                    // Every address is a console VA in the GPU address arena: the device address is heap_base + VA,
                    // and the BLAS addresses in instance descs get the same bias.
                    const std::uint64_t base = replay_.heap_base;
                    XdxrLayout layout;
                    layout.compute(xdxr_elements(b), b.is_tlas, xdxr_box16(b));
                    // PERFORM_UPDATE refits the source (the destination itself when no source is given, as D3D12
                    // allows for an in-place update); a full build otherwise.
                    const std::uint64_t from = b.update ? base + (b.src_va ? b.src_va : b.dst_va) : 0;
                    if (b.is_tlas) {
                        xdxr_->record_tlas(cb, layout, b.instances_va ? base + b.instances_va : 0, base + b.dst_va,
                                           base + b.scratch_va, base, from);
                    } else {
                        std::vector<XdxrTriangles> geometries;
                        xdxr_inputs(b, &geometries, base);
                        xdxr_->record_blas(cb, layout, geometries, base + b.dst_va, base + b.scratch_va, from,
                                           xdxr_pair(b));
                    }
                };
                replay_.xdxr_copy = [this](VkCommandBuffer cb, std::uint64_t src, std::uint64_t dst, std::uint64_t bytes) {
                    const std::uint64_t base = replay_.heap_base;
                    xdxr_->record_copy(cb, base + src, base + dst, bytes);
                };
                replay_.xdxr_postbuild = [this](VkCommandBuffer cb, std::uint64_t src, std::uint64_t dest) {
                    const std::uint64_t base = replay_.heap_base;
                    xdxr_->record_postbuild(cb, base + src, base + dest);
                };
            } else {
                xdxr_.reset();
            }
        }
        if (!xdxr_ && ray_tracing_ && vk.vkCmdBuildAccelerationStructuresKHR) {
            replay_.build_acceleration_structures = vk.vkCmdBuildAccelerationStructuresKHR;
            replay_.copy_acceleration_structure = vk.vkCmdCopyAccelerationStructureKHR;
            replay_.write_acceleration_structures_properties = vk.vkCmdWriteAccelerationStructuresPropertiesKHR;
            replay_.resolve_acceleration_structure = [this](std::uint64_t va) {
                const auto it = acceleration_structures_.find(va);
                return it == acceleration_structures_.end() ? VK_NULL_HANDLE : it->second;
            };
        }
        replay_.image_view = [this](const ResourceBinding& r, const VkImageSubresourceRange& range) {
            return view(r, range);
        };
        if (vk.vkCreateRenderPass2 && vk.vkDestroyRenderPass && vk.vkCreateFramebuffer && vk.vkDestroyFramebuffer &&
            vk.vkCmdBeginRenderPass && vk.vkCmdEndRenderPass) {
            replay_.begin_render_pass = vk.vkCmdBeginRenderPass;
            replay_.end_render_pass = vk.vkCmdEndRenderPass;
            replay_.depth_resolve_framebuffer = [this](const ResourceBinding& source, VkImageView from,
                                                       VkImageLayout from_layout, const ResourceBinding& target,
                                                       VkImageView to, VkImageLayout to_layout,
                                                       VkResolveModeFlagBits mode, VkExtent2D extent,
                                                       VkRenderPass& pass) {
                return depth_resolve_framebuffer(source, from, from_layout, target, to, to_layout, mode, extent, pass);
            };
        }
        replay_.query_pool = [this](QueryHeapId id) -> QueryPool* {  // called with resource_mutex_ held
            const auto it = query_pools_.find(id);
            return it == query_pools_.end() ? nullptr : &it->second;
        };
        replay_.separate_depth_stencil_layouts = separate_depth_stencil_layouts;
        if (loaded_) {
            fence_dispatcher_ = std::make_unique<FenceWaitDispatcher>(device_, vk, device_lost_);
        }
    }

    ~VulkanContext() {
        fence_dispatcher_.reset();
        deletion_queue_.drain(UINT64_MAX, device_, vk);
        for (const auto& [key, framebuffer] : resolve_framebuffers_)
            vk.vkDestroyFramebuffer(device_, framebuffer, nullptr);
        for (const auto& [key, pass] : resolve_passes_) vk.vkDestroyRenderPass(device_, pass, nullptr);
        views_.for_each([&](VkImageView view) { vk.vkDestroyImageView(device_, view, nullptr); });
    }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool loaded() const { return loaded_; }
    // Call before any submission; the feature must actually be enabled on the logical device.
    void set_indirect_first_instance(bool enabled) { replay_.indirect_first_instance = enabled; }
    void configure_predication(PFN_vkGetDeviceProcAddr get, const VkPhysicalDeviceMemoryProperties& memory,
                               VkDeviceSize max_range, std::uint32_t family) {
        std::unique_lock lock(resource_mutex_);
        predicate_family_ = family;
        predicate_reducer_ = std::make_unique<PredicateReducer>(get, device_, memory, max_range);
    }
    // Configure before submitting work, only when storageBuffer8BitAccess is enabled on the device.
    void configure_predicated_copy(PFN_vkGetDeviceProcAddr get, const VkPhysicalDeviceLimits& limits) {
        std::unique_lock lock(resource_mutex_);
        predicated_copier_ = std::make_unique<PredicatedBufferCopier>(get, device_, limits);
    }
    // Counts for DISPATCH-only ExecuteIndirect are read by a helper compute shader from GPU memory.
    // Requires Vulkan bufferDeviceAddress, already enabled for the D3D12 address arena.
    void configure_counted_dispatch(PFN_vkGetDeviceProcAddr get, const VkPhysicalDeviceMemoryProperties& memory,
                                    const VkPhysicalDeviceLimits& limits) {
        std::unique_lock lock(resource_mutex_);
        counted_dispatch_ = std::make_unique<IndirectDispatchCounter>(get, device_, memory, limits);
    }
    VkDevice device() const { return device_; }

    // Debug markers become labels from here on; call before any queue or list uses the context. Incomplete sets
    // are ignored, so markers stay no-ops.
    void enable_debug_labels(const DebugLabelFunctions& labels) {
        if (!labels.complete()) return;
        labels_ = labels;
        replay_.insert_label = labels.cmd_insert;
        replay_.begin_label = labels.cmd_begin;
        replay_.end_label = labels.cmd_end;
    }
    bool debug_labels() const { return labels_.complete(); }

    // Registers a resource in its D3D12 initial state (a legacy state; images get the matching layout).
    // The image must already be in that layout, e.g. transitioned from UNDEFINED at creation.
    void add_resource(ResourceId id, const ResourceBinding& binding) {
        std::unique_lock lock(resource_mutex_);
        set_resource_locked(id, binding);
    }
    void add_resource(ResourceId id, const ResourceBinding& binding, VkImageLayout current_layout) {
        std::unique_lock rlock(resource_mutex_);
        if (binding.attachment_write_trace) trace_native_attachments_ = true;
        set_resource_locked(id, binding);
        if (binding.image) {
            std::lock_guard tlock(tracker_mutex_);
            tracker_.track(id, binding, current_layout);
        }
    }

    // A shader can access a storage image or an initially unbound sparse image before any command names it.
    // The first shader submission moves it to GENERAL, the descriptor's layout, ahead of its lists.
    void note_shader_image(ResourceId id) {
        std::lock_guard lock(storage_mutex_);
        storage_pending_.insert(id);
    }

    // A device-local buffer D3D12 says starts zeroed (D3D-71): the host cannot write it, so the next submission on
    // any queue fills it with zeros ahead of its lists. That submission publishes the same initialisation semaphore as
    // the storage images above, so a list on another queue that uses the buffer first waits for the fill.
    void zero_on_next_submit(ResourceId id, VkBuffer buffer) {
        std::lock_guard lock(storage_mutex_);
        zero_pending_.push_back({id, buffer});
    }

    void add_sparse_resource(ResourceId id, const ResourceBinding& binding, std::shared_ptr<SparseResource> sparse) {
        std::unique_lock lock(resource_mutex_);
        if (!binding.image) {
            auto [it, inserted] = sparse_resources_.try_emplace(id, std::move(sparse));
            try {
                set_resource_locked(id, binding);
            } catch (...) {
                if (inserted) sparse_resources_.erase(it);
                throw;
            }
            return;
        }
        set_resource_locked(id, binding);
        sparse_resources_.emplace(id, std::move(sparse));
        if (binding.image) {
            std::lock_guard tlock(tracker_mutex_);
            tracker_.track(id, binding, VK_IMAGE_LAYOUT_UNDEFINED);
        }
    }

    // External-host imports detach before waiting: a concurrent unrelated execution must not capture another
    // global host-image barrier after the retirement snapshot. The tracker lock joins CPU-side submit/ticket
    // publication by executions which already captured the binding, then views can be destroyed after the wait.
    void remove_resource(ResourceId id, bool immediate_views = false) {
        VkImage img = VK_NULL_HANDLE;
        std::shared_ptr<SparseResource> sparse_resource;
        {
            std::unique_lock rlock(resource_mutex_);
            const auto it = resources_.find(id);
            if (it == resources_.end()) return;
            img = it->second.image;
            unindex_buffer(id, it->second);
            if (it->second.tracked_host_image())
                host_images_.erase(it->second.host_image_address, &*it);
            resources_.erase(it);
            replay_.has_host_readonly_images = !host_images_.empty();
            if (auto sparse = sparse_resources_.find(id); sparse != sparse_resources_.end()) {
                sparse_resource = std::move(sparse->second);
                sparse_resources_.erase(sparse);
            }
        }
        {
            std::lock_guard lock(storage_mutex_);  // a buffer destroyed before any submission is not filled
            std::erase_if(zero_pending_, [id](const PendingZero& zero) { return zero.id == id; });
        }
        if (immediate_views) {
            { std::lock_guard tlock(tracker_mutex_); tracker_.forget(id); }
            wait_retire_ticket(submitted_retire_ticket());
        }
        if (img) {
            std::lock_guard vlock(views_mutex_);
            for (auto f = resolve_framebuffers_.begin(); f != resolve_framebuffers_.end();) {
                if (std::get<0>(f->first) == img || std::get<1>(f->first) == img) {
                    if (immediate_views) vk.vkDestroyFramebuffer(device_, f->second, nullptr);
                    else defer_destroy(DeferredDeletionQueue::Kind::framebuffer, f->second);
                    f = resolve_framebuffers_.erase(f);
                } else {
                    ++f;
                }
            }
            views_.erase_image(img, [&](VkImageView view) {
                if (immediate_views) vk.vkDestroyImageView(device_, view, nullptr);
                else defer_destroy(DeferredDeletionQueue::Kind::image_view, view);
            });
        }
        if (sparse_resource)
            defer_destroy(DeferredDeletionQueue::Kind::sparse_resource,
                          new std::shared_ptr<SparseResource>(std::move(sparse_resource)));
        std::lock_guard tlock(tracker_mutex_);
        tracker_.forget(id);
    }

    // A query heap, by its stable ID (the COM query heap pointer). The pool stays the caller's to destroy, after
    // remove_query_pool.
    void add_query_pool(QueryHeapId id, const QueryPool& pool) {
        std::unique_lock lock(resource_mutex_);
        query_pools_[id] = pool;
    }
    void remove_query_pool(QueryHeapId id) {
        // Lock hierarchy: resource_mutex_ -> tracker_mutex_. Holding tracker_mutex_ before erasing keeps the
        // QueryPool node alive until any in-flight execute() that unlocked resource_mutex_ finishes QueryReplay::commit().
        std::unique_lock lock(resource_mutex_);
        std::lock_guard tlock(tracker_mutex_);
        query_pools_.erase(id);
    }
    // The heap's d3d12_query_heap type, or ~0u for a heap that is not registered.
    std::uint32_t query_heap_type(QueryHeapId id) {
        std::shared_lock lock(resource_mutex_);   // query_pools_ is guarded by resource_mutex_, like add/remove
        const auto it = query_pools_.find(id);
        return it == query_pools_.end() ? ~0u : it->second.heap_type;
    }

    // VkPhysicalDeviceLimits::timestampPeriod (nanoseconds per tick), or 0 when the queue writes no timestamps
    // (timestampValidBits 0). Set by the device before any queue uses it.
    double timestamp_period() const { return timestamp_period_; }
    void set_timestamp_period(double period) { timestamp_period_ = period; }
    // Actual VkQueueFamilyProperties, populated before queues are created. An absent family has no proved support.
    std::uint32_t timestamp_valid_bits(std::uint32_t family) const {
        const auto found = timestamp_valid_bits_.find(family);
        return found == timestamp_valid_bits_.end() ? 0 : found->second;
    }
    void set_timestamp_valid_bits(std::uint32_t family, std::uint32_t bits) { timestamp_valid_bits_[family] = bits; }
    // True where a timestamp resolve must be ordered explicitly after the timestamps it copies (MoltenVK on Apple
    // GPUs, docs/d3d12_vulkan.md section 4.6). Set by the device before any queue uses it.
    bool order_timestamp_copies() const { return order_timestamp_copies_; }
    void set_order_timestamp_copies(bool order) { order_timestamp_copies_ = order; }

    // The device address of the GPU address arena: console VAs are offsets from it (docs section 3.3).
    // execute() copies replay_ under a shared resource_mutex_, so writers hold it uniquely.
    void set_heap_base(std::uint64_t heap_base) {
        std::unique_lock lock(resource_mutex_);
        replay_.heap_base = heap_base;
    }

    // Zeroed memory for unbound vertex and index buffers to read (ReplayFunctions::zero_buffer).
    void set_zero_page(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
        std::unique_lock lock(resource_mutex_);
        replay_.zero_buffer = buffer;
        replay_.zero_offset = offset;
        replay_.zero_size = size;
    }

    void set_depth_range_unrestricted(bool enabled) {
        std::unique_lock lock(resource_mutex_);
        replay_.depth_range_unrestricted = enabled;
    }

    // Physical-device COLOR_ATTACHMENT format check for reinterpreted UAV clears.
    void set_color_attachment_format(std::function<bool(VkFormat)> check) {
        std::unique_lock lock(resource_mutex_);
        replay_.color_attachment_format = std::move(check);
    }
    std::uint64_t heap_base() {
        std::shared_lock lock(resource_mutex_);
        return replay_.heap_base;
    }

    bool contains_buffer_range(std::uint64_t address, std::uint64_t bytes) {
        std::shared_lock lock(resource_mutex_);
        BufferVaIndex<ResourceId>::Hit hit{};
        return buffer_vas_.find(address, hit, bytes) && hit.size - (address - hit.start) >= bytes;
    }

    // Pipelines are registered by an opaque id (the PSO object) for as long as the PSO lives.
    void add_pipeline(std::uintptr_t id, const ComputePipeline& pipeline) {
        std::unique_lock lock(resource_mutex_);
        pipelines_[id] = pipeline;
    }
    void remove_pipeline(std::uintptr_t id) {
        std::unique_lock lock(resource_mutex_);
        pipelines_.erase(id);
        graphics_pipelines_.erase(id);
    }
    void add_graphics_pipeline(std::uintptr_t id, const GraphicsPipeline& pipeline) {
        std::unique_lock lock(resource_mutex_);
        graphics_pipelines_[id] = pipeline;
    }
    // Configure before queue creation, when the selected queue family supports compute.
    void set_occlusion_resolve_memory(const VkPhysicalDeviceMemoryProperties& memory) {
        occlusion_resolve_memory_ = memory;
    }

    // For tests and traces.
    VkImageLayout layout(ResourceId id, std::uint32_t subresource) {
        std::lock_guard lock(tracker_mutex_);
        return tracker_.layout(id, subresource);
    }

    bool has_resource(ResourceId id) {
        std::shared_lock lock(resource_mutex_);
        return resources_.find(id) != resources_.end();
    }

    bool ray_tracing_supported() const { return ray_tracing_ && vk.vkCmdBuildAccelerationStructuresKHR != nullptr; }
    bool xdxr_acceleration_structures() const { return xdxr_ != nullptr; }

    // Whether a build's inner nodes are BOX16 (64 bytes, half boxes rounded outward) instead of BOX32: bottom-level
    // builds with MINIMIZE_MEMORY (VK LOW_MEMORY). A top-level build keeps BOX32: it is in world space, where half
    // precision would make boxes metres too large a few kilometres from the origin. The flags of an update must match
    // its build's (D3D12), so prebuild info, the build and its refits agree. WARNING: an update that breaks that rule
    // (MINIMIZE_MEMORY on one and not the other) is refitted with the wrong layout and writes past the structure; it
    // cannot be detected when the command is recorded.
    static bool xdxr_box16(const BuildAccelerationStructure& b) {
        return !b.is_tlas && (b.build_flags & VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR) != 0;
    }
    // Whether a bottom-level build pairs sibling triangles into TRI0/TRI1 leaves: not when the structure may be
    // updated (ALLOW_UPDATE), since a refit rewrites the leaves one triangle each and would leave every TRI1 child
    // reading wrong vertices. xdxr_check refuses an update without ALLOW_UPDATE for that reason. Pairing changes
    // neither the layout nor the tree, so prebuild info does not depend on it.
    static bool xdxr_pair(const BuildAccelerationStructure& b) {
        return !b.is_tlas && !b.update && (b.build_flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) == 0;
    }
    // Triangles (BLAS) or instances (TLAS) a build has: what XdxrLayout is computed from, for the build and for
    // GetRaytracingAccelerationStructurePrebuildInfo.
    static std::uint32_t xdxr_elements(const BuildAccelerationStructure& b) {
        if (b.is_tlas) return b.instance_count;
        std::uint64_t n = 0;
        for (const auto& t : b.triangles) n += (t.index_type ? t.index_count : t.vertex_count) / 3;
        return n > 0xffffffffu ? 0xffffffffu : std::uint32_t(n);
    }
    // The geometries as the builder reads them, with `base` added to their VAs; false for an input it cannot read
    // (a vertex format other than R32G32B32_FLOAT or R32G32_FLOAT, an index type other than none/R16/R32).
    static bool xdxr_inputs(const BuildAccelerationStructure& b, std::vector<XdxrTriangles>* out, std::uint64_t base) {
        if (b.is_tlas) return true;
        for (const auto& t : b.triangles) {
            if (t.vertex_format != VK_FORMAT_R32G32B32_SFLOAT && t.vertex_format != VK_FORMAT_R32G32_SFLOAT) return false;
            if (t.index_type > 2) return false;
            const std::uint32_t count = (t.index_type ? t.index_count : t.vertex_count) / 3;
            if (count && !t.vertex_va) return false;
            if (count && t.index_type && !t.index_va) return false;
            if (!out) continue;
            XdxrTriangles g;
            g.vertices = t.vertex_va ? base + t.vertex_va : 0;
            g.vertex_stride = t.vertex_stride;
            g.xy_only = t.vertex_format == VK_FORMAT_R32G32_SFLOAT;
            g.indices = t.index_type && t.index_va ? base + t.index_va : 0;
            g.index_type = t.index_type;
            g.triangle_count = count;
            g.transform = t.transform_va ? base + t.transform_va : 0;
            g.flags = t.flags | (t.opaque ? 1u : 0u);
            out->push_back(g);
        }
        return true;
    }

    void register_acceleration_structure(std::uint64_t va, VkAccelerationStructureKHR as) {
        if (!va || !as) return;
        std::unique_lock lock(resource_mutex_);
        acceleration_structures_[va] = as;
    }

    void unregister_acceleration_structure(std::uint64_t va) {
        if (!va) return;
        std::unique_lock lock(resource_mutex_);
        acceleration_structures_.erase(va);
    }

    VkAccelerationStructureKHR resolve_acceleration_structure(std::uint64_t va) {
        std::shared_lock lock(resource_mutex_);
        const auto it = acceleration_structures_.find(va);
        return it == acceleration_structures_.end() ? VK_NULL_HANDLE : it->second;
    }

    // `prepare` is called at the start of the command buffer of every ExecuteCommandLists, before the lists are
    // written: the device brings the bindless set 0 of the translated shaders up to date there (vulkan_bindless.h)
    // and records what the set needs first. When it fails, nothing of that call is executed.
    void set_transfer_only_family(std::uint32_t family) { transfer_only_family_ = family; }

    void set_bindless_prepare(std::function<bool(VkCommandBuffer)> prepare) {
        std::unique_lock lock(resource_mutex_);
        bindless_prepare_ = std::move(prepare);
    }

    // D24_UNORM_S8_UINT stored as D32: copies of its depth plane run a compute pass (vulkan_depth24.h). `allocate`
    // gives the scratch buffers device-local memory. Without this such a copy is refused as unsupported.
    void enable_depth24(PFN_vkGetDeviceProcAddr get, Depth24Converter::Allocate allocate,
                       std::uint64_t max_storage_range = 1u << 27) {
        std::unique_lock lock(resource_mutex_);
        depth24_ = std::make_unique<Depth24Converter>(get, device_, std::move(allocate), max_storage_range);
        if (!depth24_->loaded()) {
            depth24_.reset();
            return;
        }
        replay_.depth24_prepare = [this](Depth24Job& job, std::vector<Depth24Job>& parts) { return depth24_->prepare(job, parts); };
        replay_.depth24_run = [this](VkCommandBuffer command_buffer, const Depth24Job& job) {
            depth24_->run(command_buffer, job);
        };
    }

    // Copies the replay stages through a buffer (depth <-> colour, vulkan_replay.h) take device-local scratch from
    // `allocate`. Without this such a copy is refused as unsupported.
    void enable_copy_scratch(PFN_vkGetDeviceProcAddr get, ScratchBuffers::Allocate allocate) {
        std::unique_lock lock(resource_mutex_);
        scratch_ = std::make_unique<ScratchBuffers>(get, device_, std::move(allocate));
        if (!scratch_->loaded()) scratch_.reset();
    }

    // Called after that command buffer's submit: true when it was accepted, false when vkQueueSubmit failed. The
    // set keeps its upload pending in the second case, because the copy never reached the GPU.
    void set_bindless_commit(std::function<void(bool)> commit) {
        std::unique_lock lock(resource_mutex_);
        bindless_commit_ = std::move(commit);
    }

    // The set 0 the commands of this ExecuteCommandLists bind, read after `prepare` succeeded and under the same
    // locks (a flush may have made a new set). Without it the pipelines' own set is bound.
    void set_bindless_current(std::function<VkDescriptorSet()> current) {
        std::unique_lock lock(resource_mutex_);
        bindless_current_ = std::move(current);
    }

    void set_bindless_release(DeferredDeletionQueue::BindlessReleaseFn fn, void* user) {
        deletion_queue_.set_bindless_release(fn, user);
    }

    // The ticket of the latest submission: everything submitted so far is at or below it, and a resource that any
    // of that work may use can be freed once completed_retire_ticket() has reached it. Tickets run over all queues,
    // each recorded with its queue's timeline value, so this waits for every queue's timeline to pass what was
    // submitted to it, and never makes a queue wait.
    std::uint64_t submitted_retire_ticket() const { return submitted_ticket_.load(std::memory_order_acquire); }

    template<typename VkHandle>
    void defer_destroy(DeferredDeletionQueue::Kind kind, VkHandle handle) {
        const auto raw = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
        if (!raw) return;
        std::lock_guard lock(submit_mutex_);
        const std::uint64_t safe_ticket = submitted_ticket_.load(std::memory_order_acquire) + 1;
        deletion_queue_.push(safe_ticket, kind, raw);
    }

    void defer_destroy(DeferredDeletionQueue::Kind kind, std::uint64_t raw_handle) {
        if (!raw_handle && kind != DeferredDeletionQueue::Kind::bindless_slot) return;
        std::lock_guard lock(submit_mutex_);
        const std::uint64_t safe_ticket = submitted_ticket_.load(std::memory_order_acquire) + 1;
        deletion_queue_.push(safe_ticket, kind, raw_handle);
    }

    // Waits (not for the device: for the queues' timelines) until every submission up to `ticket` is done, then
    // returns the completed ticket. For a caller whose list of retired objects has grown past what it will hold.
    std::uint64_t wait_retire_ticket(std::uint64_t ticket) {
        std::vector<std::pair<VkSemaphore, std::uint64_t>> pending;
        {
            std::lock_guard lock(submit_mutex_);
            if (is_device_lost()) return 0;
            pending = retire_tickets_.pending_through(ticket);
        }
        if (!pending.empty()) {
            std::vector<VkSemaphore> semaphores;
            std::vector<std::uint64_t> values;
            for (const auto& [semaphore, value] : pending) {
                semaphores.push_back(semaphore);
                values.push_back(value);
            }
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = std::uint32_t(semaphores.size());
            wait.pSemaphores = semaphores.data();
            wait.pValues = values.data();
            vk.vkWaitSemaphores(device_, &wait, UINT64_MAX);  // outside submit_mutex_: a signal may need it
        }
        return completed_retire_ticket();
    }

    // The bindless flush of this ExecuteCommandLists started a new set generation, whose table upload is in its
    // command buffer: called from the `prepare` callback (tracker_mutex_ held), so execute() makes every other
    // queue's next submission wait for this one, which binds the new set.
    void note_bindless_upload() { bindless_upload_pending_ = true; }

    std::uint64_t completed_retire_ticket() {
        bool lost = false;
        std::uint64_t ticket = 0;
        {
            std::lock_guard lock(submit_mutex_);
            if (is_device_lost()) return 0;
            ticket = retire_tickets_.completed([this, &lost](VkSemaphore semaphore, std::uint64_t value) {
                std::uint64_t done = 0;
                const VkResult result = vk.vkGetSemaphoreCounterValue(device_, semaphore, &done);
                if (result == VK_ERROR_DEVICE_LOST) lost = true;
                return result == VK_SUCCESS && done >= value;
            });
        }
        // Outside submit_mutex_, as in submit(): notifying the fence dispatcher can run callbacks that submit.
        // A DEVICE_LOST seen here is the first Vulkan operation to discover it; it must not be mistaken for
        // "this ticket has not completed".
        if (lost) mark_device_lost();
        return lost ? 0 : ticket;
    }

    void drain_deleted_resources(std::uint64_t current_ticket) {
        deletion_queue_.drain(current_ticket, device_, vk);
    }

    void report_attachment_writes(std::uint64_t owner, const AttachmentWriteTrace& trace) {
        std::unique_lock lock(submit_mutex_, std::try_to_lock);
        if (!lock || is_device_lost()) {
            std::fprintf(stderr, "[rt] d3d12: native RT replay owner=0x%llx retirement=unavailable\n",
                         static_cast<unsigned long long>(owner));
            return;
        }
        constexpr const char* names[] = {"clear_full", "clear_rects", "copy", "resolve", "possible_draw_or_predicated_clear"};
        for (unsigned i = 0; i < trace.last_ticket.size(); ++i) {
            const auto ticket = trace.last_ticket[i];
            const auto retired = retire_tickets_.observe(ticket, [&](VkSemaphore semaphore, std::uint64_t value)
                -> std::optional<bool> {
                std::uint64_t done = 0;
                if (vk.vkGetSemaphoreCounterValue(device_, semaphore, &done) != VK_SUCCESS) return {};
                return done >= value;
            });
            std::fprintf(stderr, "[rt] d3d12: native RT replay owner=0x%llx kind=%s last_global_ticket=%llu "
                "retirement=%s incomplete=%u\n", static_cast<unsigned long long>(owner), names[i],
                static_cast<unsigned long long>(ticket), !ticket ? "none_recorded" : !retired ? "unknown" :
                *retired ? "retired" : "pending", unsigned(trace.incomplete.load(std::memory_order_relaxed)));
        }
    }

    // Both methods are called with submit_mutex_ held. A queue's own progress timeline is
    // monotonic even when another queue completes first; global tickets are retired in order.
    void record_submit_locked(VkSemaphore progress, std::uint64_t value) {
        const auto ticket = submitted_ticket_.fetch_add(1, std::memory_order_acq_rel) + 1;
        retire_tickets_.record(ticket, progress, value);
    }
    void retire_queue_idle_locked(VkSemaphore progress) {
        retire_tickets_.queue_idle(progress);
    }

    VulkanDeviceFunctions vk;

    // Vulkan requires external synchronisation of a VkQueue; every D3D12 queue of the device may share one.
    std::mutex& submit_mutex() { return submit_mutex_; }
    std::uintptr_t frame_draw_image_id(ResourceId id) {
        std::shared_lock lock(resource_mutex_);
        const auto* binding = resolve(id);
        return binding ? binding->draw_activity_image : 0;
    }

    FrameDrawActivity& frame_draw_activity() { return frame_draw_activity_; }

    void mark_device_lost() noexcept {
        device_lost_.store(true, std::memory_order_release);
        if (fence_dispatcher_) {
            try {
                fence_dispatcher_->notify_device_lost();
            } catch (...) {
            }
        }
    }
    bool is_device_lost() const noexcept { return device_lost_.load(std::memory_order_acquire); }

    void set_signal_event_fn(FenceWaitDispatcher::SignalEventFn fn) {
        if (fence_dispatcher_) fence_dispatcher_->set_signal_event_fn(fn);
    }
    void enqueue_fence_wait(VkSemaphore sem, std::uint64_t value, void* event_handle) {
        if (fence_dispatcher_) fence_dispatcher_->enqueue(sem, value, event_handle);
    }
    void enqueue_fence_callback(VkSemaphore sem, std::uint64_t value, std::function<void()> cb) {
        if (fence_dispatcher_) fence_dispatcher_->enqueue_callback(sem, value, std::move(cb));
    }
    bool watch_fence_completion(VkSemaphore sem, std::atomic<std::uint64_t>& value) {
        return fence_dispatcher_ && fence_dispatcher_->watch_completion(sem, value);
    }
    void cancel_fence_waits(VkSemaphore sem) {
        if (fence_dispatcher_) fence_dispatcher_->cancel(sem);
    }

    // Cross-queue Wait-before-Signal: per fence, the highest value a Signal was submitted for, and the queues that
    // hold batches until one is (queue_holds.h).
    QueueHolds<VkSemaphore>& holds() { return holds_; }

private:
    friend class VulkanQueue;
    friend class VulkanPresenter;

    // buffer_at finds a buffer by VA: one with no image, a VkBuffer, a VA and bytes (what the linear search it replaced
    // accepted). Both are called with resource_mutex_ held uniquely.
    static bool addressable(const ResourceBinding& b) { return !b.image && b.buffer && b.va && b.size; }
    void unindex_buffer(ResourceId id, const ResourceBinding& b) {
        if (addressable(b)) buffer_vas_.erase(b.va, id);
    }
    // resources_[id] = binding, with the VA index kept in step (a re-registered ID drops its old range first).
    void set_resource_locked(ResourceId id, ResourceBinding binding) {
        auto [it, inserted] = resources_.try_emplace(id);
        if (binding.image && FrameDrawActivity::enabled()) {
            binding.draw_activity_image = inserted || it->second.image != binding.image
                ? ++next_activity_image_ : it->second.draw_activity_image;
        }
        const bool was_host_image = !inserted && it->second.tracked_host_image();
        const bool host_image = binding.tracked_host_image();
        const bool changed_key = it->second.host_image_address != binding.host_image_address;
        bool appended = false;
        try {
            if (host_image && (!was_host_image || changed_key)) {
                host_images_.insert(binding.host_image_address, &*it);
                appended = true;
            }
            if (addressable(binding)) buffer_vas_.insert(binding.va, binding.size, id);
        } catch (...) {
            if (appended) host_images_.erase(binding.host_image_address, &*it);
            if (inserted) resources_.erase(it);
            throw;
        }
        // A same-VA insert updates its size in place; removing the old key would remove the new range.
        if (addressable(it->second) && (!addressable(binding) || it->second.va != binding.va))
            unindex_buffer(id, it->second);
        if (was_host_image && (!host_image || changed_key)) host_images_.erase(it->second.host_image_address, &*it);
        static_assert(std::is_nothrow_move_assignable_v<ResourceBinding>);
        it->second = std::move(binding);
        replay_.has_host_readonly_images = !host_images_.empty();
    }

    const ResourceBinding* resolve(ResourceId id) const {
        const auto it = resources_.find(id);
        if (it == resources_.end()) return nullptr;
        if (auto* memory = it->second.deferred_memory; memory && !memory->ensure()) return nullptr;
        return &it->second;
    }

    // Called from prepare; protected by views_mutex_ so concurrent shared_lock readers are safe.
    VkImageView view(const ResourceBinding& r, const VkImageSubresourceRange& range) {
        std::lock_guard lock(views_mutex_);
        const ViewCache::Key key{r.image, range.aspectMask, range.baseMipLevel, range.baseArrayLayer, range.layerCount,
                                 static_cast<std::uint32_t>(r.format)};
        if (const VkImageView cached = views_.find(key)) return cached;
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = r.image;
        info.viewType = r.line ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;  // 1D images take no 2D view
        info.format = r.format;
        info.subresourceRange = range;
        // Attachment views need only color-attachment usage. Inheriting STORAGE usage from a mutable/extended
        // resource (typeless views such as RGBA8 sRGB, or R32_UINT-backed R9G9B9E5 UAV images) would require unrelated usages.
        VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (r.typeless || (range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && r.color_attachment))
            info.pNext = &usage;
        VkImageView view = VK_NULL_HANDLE;
        if (vk.vkCreateImageView(device_, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        try {
            views_.insert(key, view);
        } catch (const std::bad_alloc&) {
            vk.vkDestroyImageView(device_, view, nullptr);
            return VK_NULL_HANDLE;
        }
        return view;
    }

    // The render pass and framebuffer that resolve one layer of a depth-only multisampled image into one layer of
    // another: views `from` and `to` (from view()), in the layouts given. Used where the dynamic-rendering resolve
    // reads the wrong source layer (D3D-417; vulkan_replay.h says when). Both objects are cached: a pass by what it
    // describes, a framebuffer by its images and views, until remove_resource retires either image.
    VkFramebuffer depth_resolve_framebuffer(const ResourceBinding& source, VkImageView from, VkImageLayout from_layout,
                                            const ResourceBinding& target, VkImageView to, VkImageLayout to_layout,
                                            VkResolveModeFlagBits mode, VkExtent2D extent, VkRenderPass& pass) {
        std::lock_guard lock(views_mutex_);
        const auto pass_key = std::make_tuple(source.format, source.samples, from_layout, to_layout, mode);
        if (const auto it = resolve_passes_.find(pass_key); it != resolve_passes_.end()) {
            pass = it->second;
        } else {
            VkAttachmentDescription2 attachments[2]{};
            for (auto& attachment : attachments) {
                attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
                attachment.format = source.format;
                attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;      // the source is read; the destination keeps
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // what is outside the render area
                attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // depth-only formats
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            }
            attachments[0].samples = static_cast<VkSampleCountFlagBits>(source.samples);
            attachments[0].initialLayout = attachments[0].finalLayout = from_layout;
            attachments[1].initialLayout = attachments[1].finalLayout = to_layout;
            const VkAttachmentReference2 from_reference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
                                                        from_layout, VK_IMAGE_ASPECT_DEPTH_BIT};
            const VkAttachmentReference2 to_reference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, to_layout,
                                                      VK_IMAGE_ASPECT_DEPTH_BIT};
            VkSubpassDescriptionDepthStencilResolve resolve{
                VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
            resolve.depthResolveMode = mode;
            resolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
            resolve.pDepthStencilResolveAttachment = &to_reference;
            VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
            subpass.pNext = &resolve;
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &from_reference;
            VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
            info.attachmentCount = 2;
            info.pAttachments = attachments;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            if (vk.vkCreateRenderPass2(device_, &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
            resolve_passes_[pass_key] = pass;
        }
        const auto key = std::make_tuple(source.image, target.image, from, to, pass);
        if (const auto it = resolve_framebuffers_.find(key); it != resolve_framebuffers_.end()) return it->second;
        const VkImageView views[] = {from, to};
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = pass;
        info.attachmentCount = 2;
        info.pAttachments = views;
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vk.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) return VK_NULL_HANDLE;
        resolve_framebuffers_[key] = framebuffer;
        return framebuffer;
    }

    VkDevice device_;
    bool loaded_ = false;
    bool ray_tracing_ = false;
    std::unique_ptr<XdxrBuilder> xdxr_;   // set: acceleration structures are XDXR BVH4 records
    std::atomic<bool> device_lost_{false};
    alignas(64) std::shared_mutex resource_mutex_;  // shared for prepare lookups, unique for add/remove
    alignas(64) std::mutex tracker_mutex_;
    alignas(64) std::mutex views_mutex_;
    alignas(64) std::mutex submit_mutex_;
    FrameDrawActivity frame_draw_activity_;
    std::uintptr_t next_activity_image_ = 0;  // under resource_mutex_
    RetireTickets<VkSemaphore> retire_tickets_;
    bool trace_native_attachments_ = false;  // protected by resource_mutex_
    std::atomic<std::uint64_t> submitted_ticket_{0};
    DeferredDeletionQueue deletion_queue_;
    std::unique_ptr<FenceWaitDispatcher> fence_dispatcher_;
    QueueHolds<VkSemaphore> holds_;
    std::unordered_map<ResourceId, ResourceBinding> resources_;
    // Node addresses survive unordered_map rehash; resource_mutex_ protects the index and its lifetime.
    using HostImages = HostImageIndex<decltype(resources_)::value_type>;
    HostImages host_images_;
    BufferVaIndex<ResourceId> buffer_vas_;  // the addressable buffers of resources_, by VA range (buffer_at)
    std::unordered_map<ResourceId, std::shared_ptr<SparseResource>> sparse_resources_;
    std::unordered_map<std::uintptr_t, ComputePipeline> pipelines_;
    std::unordered_map<std::uintptr_t, GraphicsPipeline> graphics_pipelines_;
    std::unordered_map<std::uint64_t, VkAccelerationStructureKHR> acceleration_structures_;
    LayoutTracker tracker_;
    std::mutex storage_mutex_;  // a leaf lock: guards storage_pending_ and zero_pending_ only
    std::unordered_set<ResourceId> storage_pending_;
    struct PendingZero {
        ResourceId id;
        VkBuffer buffer;
    };
    std::vector<PendingZero> zero_pending_;  // zero_on_next_submit
    // The last submission that initialized storage images (its queue's progress semaphore and ticket) and a counter
    // that says which one: every other queue's next submission waits for it, so no queue uses such an image before
    // the zero-fill that put it in GENERAL has run.
    VkSemaphore storage_init_semaphore_ = VK_NULL_HANDLE;
    std::uint64_t storage_init_value_ = 0;
    std::uint64_t storage_init_version_ = 0;
    using ViewCache = ImageViewCache<VkImage, VkImageView>;
    ViewCache views_;
    // depth_resolve_framebuffer's caches, under views_mutex_ like views_.
    std::map<std::tuple<VkFormat, std::uint32_t, VkImageLayout, VkImageLayout, VkResolveModeFlagBits>, VkRenderPass>
        resolve_passes_;
    std::map<std::tuple<VkImage, VkImage, VkImageView, VkImageView, VkRenderPass>, VkFramebuffer> resolve_framebuffers_;
    std::function<bool(VkCommandBuffer)> bindless_prepare_;
    std::function<VkDescriptorSet()> bindless_current_;
    bool bindless_upload_pending_ = false;  // tracker_mutex_
    // The last submission that carried a new generation's table upload (its queue's progress semaphore and value), and
    // a counter that says which one: every other queue's next submission waits for it (storage_mutex_).
    VkSemaphore bindless_upload_semaphore_ = VK_NULL_HANDLE;
    std::uint64_t bindless_upload_value_ = 0;
    std::uint64_t bindless_upload_version_ = 0;
    std::unique_ptr<Depth24Converter> depth24_;
    std::unique_ptr<ScratchBuffers> scratch_;  // copies staged through a buffer (depth <-> colour)
    std::function<void(bool)> bindless_commit_;
    std::unordered_map<QueryHeapId, QueryPool> query_pools_;  // element pointers stay valid across inserts
    double timestamp_period_ = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> timestamp_valid_bits_;
    bool order_timestamp_copies_ = false;
    VkPhysicalDeviceMemoryProperties occlusion_resolve_memory_{};
    ReplayFunctions replay_;
    std::unique_ptr<PredicateReducer> predicate_reducer_;
    std::unique_ptr<PredicatedBufferCopier> predicated_copier_;
    std::unique_ptr<IndirectDispatchCounter> counted_dispatch_;
    std::uint32_t predicate_family_ = UINT32_MAX;
    std::uint32_t transfer_only_family_ = UINT32_MAX;
    DebugLabelFunctions labels_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// A Vulkan timeline semaphore with D3D12 fence semantics for the host side.
class VulkanFence {
public:
    VulkanFence(VulkanContext& context, std::uint64_t initial) : context_(context) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = initial;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        if (context.vk.vkCreateSemaphore(context.device(), &info, nullptr, &semaphore_) != VK_SUCCESS)
            semaphore_ = VK_NULL_HANDLE;
        if (semaphore_) context.holds().add_fence(semaphore_, initial);
    }
    ~VulkanFence() {
        if (!semaphore_) return;
        context_.holds().remove_fence(semaphore_);  // batches held on it can never run: dropped and reported
        context_.vk.vkDestroySemaphore(context_.device(), semaphore_, nullptr);
    }
    VulkanFence(const VulkanFence&) = delete;
    VulkanFence& operator=(const VulkanFence&) = delete;

    VkSemaphore semaphore() const { return semaphore_; }

    std::uint64_t completed() const {
        if (context_.is_device_lost()) return UINT64_MAX;
        std::uint64_t value = 0;
        const VkResult result = context_.vk.vkGetSemaphoreCounterValue(context_.device(), semaphore_, &value);
        if (result == VK_ERROR_DEVICE_LOST) {
            context_.mark_device_lost();
            return UINT64_MAX;
        }
        if (result != VK_SUCCESS) return 0;
        return value;
    }

    // ID3D12Fence::Signal from the CPU. Vulkan timelines only increase: a value at or below the current one is
    // ignored (D3D12 would set it), and the caller is told.
    bool signal(std::uint64_t value) {
        // The dispatcher may be testing a failed counter query. Do not call completed() here:
        // it marks the context lost on a query error and prevents the dispatcher from waking.
        if (context_.is_device_lost()) return false;
        std::uint64_t current = 0;
        if (context_.vk.vkGetSemaphoreCounterValue(context_.device(), semaphore_, &current) == VK_SUCCESS &&
            value <= current)
            return false;
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = semaphore_;
        info.value = value;
        const VkResult result = context_.vk.vkSignalSemaphore(context_.device(), &info);
        if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
        if (result != VK_SUCCESS) return false;
        context_.holds().signaled(semaphore_, value);  // queues that held a batch for this value run it now
        context_.holds().pump_ready();
        return true;
    }

    // true when the value was reached within the timeout.
    bool wait(std::uint64_t value, std::uint64_t timeout_ns) const {
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &semaphore_;
        info.pValues = &value;
        const VkResult result = context_.vk.vkWaitSemaphores(context_.device(), &info, timeout_ns);
        if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
        return result == VK_SUCCESS;
    }

private:
    VulkanContext& context_;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
};

// One D3D12 command queue on one VkQueue. D3D12 lets one thread use a queue at a time per call and the COM wrapper
// serialises its calls; state_mutex_ additionally serialises them against the thread that releases a held batch
// (Wait-before-Signal, queue_holds.h), the only other thread that touches a queue.
//
// A Wait on a fence value no Signal has been submitted for yet holds the next batch (ExecuteCommandLists or Signal)
// on the CPU, with everything the queue is asked to do after it, instead of replaying it against layouts from before
// the work it waits for: Vulkan itself would accept the wait (timeline semaphores allow wait-before-signal), but the
// layout tracker follows replay order. Holding on the CPU also keeps a Wait and the Signal that satisfies it from
// landing on one VkQueue in the wrong order, which would never make progress on the GPU.
class VulkanQueue : private QueueHolds<VkSemaphore>::Holder {
public:
    enum class QueueKind : std::uint8_t { direct, compute, copy };

    // The most command buffers a queue holds at once. At that many, a caller that outruns the GPU waits for the oldest
    // submitted one to finish (before taking any context lock, see wait_for_room) instead of allocating without bound. The
    // cap holds while the GPU is slow too: a wait that times out is retried, in kQueuedWaitNs slices so a device lost
    // meanwhile ends it, and a device lost makes execute() submit nothing.
    static constexpr std::size_t kMaxQueuedCommandBuffers = 64;
    static constexpr std::uint64_t kQueuedWaitNs = 5'000'000'000;

    static constexpr VkPipelineStageFlags2 queue_supported_stages(QueueKind kind) noexcept {
        switch (kind) {
        case QueueKind::copy:
            return VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case QueueKind::compute:
            return VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case QueueKind::direct:
        default:
            return VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        }
    }

    VulkanQueue(VulkanContext& context, VkQueue queue, std::uint32_t family, QueueKind kind = QueueKind::direct)
        : VulkanQueue(context, queue, family, kind, true) {}

private:
    friend class VulkanPresenter;
    // A presenter's private uploader uses the same VkQueue and retirement machinery, with no guest waits.
    VulkanQueue(VulkanContext& context, VkQueue queue, std::uint32_t family, QueueKind kind, bool guest_queue)
        : context_(context), queue_(queue), kind_(kind), family_(family), guest_queue_(guest_queue) {
        auto& vk = context.vk;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family;
        ok_ = vk.vkCreateCommandPool(context.device(), &pool, nullptr, &pool_) == VK_SUCCESS;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        ok_ = ok_ && vk.vkCreateSemaphore(context.device(), &info, nullptr, &progress_) == VK_SUCCESS;
        try {
            if (guest_queue_) context_.holds().attach(this);
        } catch (...) {
            // A failed registration leaves no queue to run the destructor for these Vulkan handles.
            if (progress_) vk.vkDestroySemaphore(context.device(), progress_, nullptr);
            if (pool_) vk.vkDestroyCommandPool(context.device(), pool_, nullptr);
            throw;
        }
    }

public:
    ~VulkanQueue() {
        auto& vk = context_.vk;
        if (guest_queue_) {
            context_.holds().detach(this);  // no thread releases into this queue from here on
            report_held_at_teardown();
            context_.holds().pump_ready();  // presents queued behind what was just dropped can run
        }
        if (queue_) {
            std::lock_guard lock(context_.submit_mutex());
            for (; open_labels_; --open_labels_) context_.labels_.queue_end(queue_);
            const VkResult idle_result = vk.vkQueueWaitIdle(queue_);
            if (idle_result == VK_SUCCESS) {
                if (progress_) context_.retire_queue_idle_locked(progress_);
            } else {
                context_.mark_device_lost();
            }
        }
        if (progress_) {
            std::lock_guard lock(context_.tracker_mutex_);
            if (context_.depth24_) context_.depth24_->forget(progress_);  // its scratch is idle now
            if (context_.scratch_) context_.scratch_->forget(progress_);
        }
        if (progress_) {
            // This queue is idle, so whatever initialisation it published (storage images, zero-filled buffers) has
            // finished: the next submission on another queue must not wait on a semaphore that is about to go.
            std::lock_guard lock(context_.storage_mutex_);
            if (context_.storage_init_semaphore_ == progress_) context_.storage_init_semaphore_ = VK_NULL_HANDLE;
        }
        if (pool_) vk.vkDestroyCommandPool(context_.device(), pool_, nullptr);
        if (progress_) vk.vkDestroySemaphore(context_.device(), progress_, nullptr);
        destroy_khr_stages(khr_stage_pool_);
        destroy_khr_as_queries(khr_as_query_pool_);
        for (auto& slot : buffers_) {
            destroy_khr_stages(slot.khr_instances);
            destroy_khr_as_queries(slot.khr_as_queries);
            destroy_timestamp_queries(slot.timestamp_queries);
            free_events_.insert(free_events_.end(), slot.events.begin(), slot.events.end());
        }
        for (VkEvent event : free_events_) vk.vkDestroyEvent(context_.device(), event, nullptr);
    }

    VulkanQueue(const VulkanQueue&) = delete;
    VulkanQueue& operator=(const VulkanQueue&) = delete;

    bool ok() const { return ok_; }

    template<typename Range, typename Apply>
    bool alias_host_pages(std::span<const Range> ranges, std::size_t page_size, Apply apply) {
        std::lock_guard state(state_mutex_);
        if (!ok_ || context_.is_device_lost() || must_hold()) return false;
        // Consume any earlier queue waits whose signals have actually been submitted. No memory is remapped
        // behind a hold whose producer still needs to call into this queue.
        if (!waits_.empty() && !submit(VK_NULL_HANDLE, VK_NULL_HANDLE, 0)) return false;
        std::unique_lock resources(context_.resource_mutex_);
        std::lock_guard tracker(context_.tracker_mutex_);
        for (const auto& range : ranges) {
            if (!page_size || range.page_count > (UINT64_MAX - range.destination) / page_size) return false;
            const std::uint64_t end = range.destination + range.page_count * page_size;
            for (const auto& [id, binding] : context_.resources_) {
                if (binding.imported_host_size && range.destination < binding.imported_host_base + binding.imported_host_size &&
                    binding.imported_host_base < end) return false;
            }
        }
        // No new resource-using submission can enter while the physical views change. Vulkan imports must keep
        // their original host backing intact, even when every earlier access has completed.
        const auto ticket = context_.submitted_retire_ticket();
        if (ticket && context_.wait_retire_ticket(ticket) < ticket) return false;
        return apply();
    }

    // Entries kept between executions. Execute only uses the first lists.size(); a larger count is retained storage,
    // not work the next submission will replay (vk_queue_test checks both halves of that contract).
    std::size_t prepared_entries() const { return prepared_.size(); }

    template<typename GetHeap>
    bool update_tiles(ResourceId id, std::span<const TileRegion> regions,
                      std::span<const TileRange> ranges, GetHeap&& get_heap) {
        std::lock_guard state(state_mutex_);  // outermost: held batches are released from other threads
        std::unique_lock lock(context_.resource_mutex_);
        const auto it = context_.sparse_resources_.find(id);
        if (it == context_.sparse_resources_.end()) return false;
        const auto& sparse = it->second;
        for (const auto& region : regions)
            if (!valid_tiles(*sparse, region)) return false;
        const auto heap = get_heap(sparse->image != VK_NULL_HANDLE, sparse->memory_types);

        std::map<std::uint64_t, TileMapping> changes;
        std::size_t region_index = 0;
        std::uint64_t consumed = 0;
        for (const auto& range : ranges) {
            const bool backed = range.mode == TileRangeMode::sequential || range.mode == TileRangeMode::reuse;
            const std::uint64_t heap_count = heap ? heap->size / tile_size_in_bytes : 0;
            const std::uint64_t required = range.mode == TileRangeMode::reuse ? 1 : range.count;
            if (backed && (!heap || range.first > heap_count || required > heap_count - range.first)) return false;
            for (std::uint64_t i = 0; i < range.count;) {
                if (region_index == regions.size()) return false;
                const auto& region = regions[region_index];
                const auto count = std::min(range.count - i, region.count - consumed);
                const auto first = region.first + consumed;
                if (range.mode == TileRangeMode::unmap) {
                    const auto end = first + count;
                    for (auto old = sparse->mapped.lower_bound(first); old != sparse->mapped.end() && old->first < end; ++old)
                        changes[old->first] = {};
                    for (auto change = changes.lower_bound(first); change != changes.end() && change->first < end; ++change)
                        change->second = {};
                } else if (backed) {
                    for (std::uint64_t j = 0; j < count; ++j) {
                        const auto page = range.first + (range.mode == TileRangeMode::sequential ? i + j : 0);
                        changes[first + j] = {heap, heap->offset + page * tile_size_in_bytes};
                    }
                }
                i += count;
                consumed += count;
                if (consumed == region.count) {
                    ++region_index;
                    consumed = 0;
                }
            }
        }
        return region_index == regions.size() && bind_tiles(id, sparse, changes);
    }

    bool copy_tile_mappings(ResourceId dst, std::span<const TileRegion> dst_regions, ResourceId src,
                            std::span<const TileRegion> src_regions) {
        std::lock_guard state(state_mutex_);
        std::unique_lock lock(context_.resource_mutex_);
        const auto source = context_.sparse_resources_.find(src), target = context_.sparse_resources_.find(dst);
        if (source == context_.sparse_resources_.end() || target == context_.sparse_resources_.end() ||
            bool(source->second->image) != bool(target->second->image) || src_regions.size() != dst_regions.size())
            return false;
        // Read the complete source mapping before changing any destination slot, including copies within one resource.
        std::map<std::uint64_t, TileMapping> changes;
        for (std::size_t row = 0; row < src_regions.size(); ++row) {
            const auto from = src_regions[row], to = dst_regions[row];
            if (!valid_tiles(*source->second, from) || !valid_tiles(*target->second, to) || from.count != to.count)
                return false;
            for (std::uint64_t i = 0; i < from.count; ++i) {
                const auto page = source->second->mapped.find(from.first + i);
                changes.emplace(to.first + i, page == source->second->mapped.end() ? TileMapping{} : page->second);
            }
        }
        return bind_tiles(dst, target->second, changes);
    }

    // ExecuteCommandLists: every list is checked before any is written, so a failure submits nothing and leaves
    // the layout tracker as it was. Pending Wait()s are attached to this submission. When one of them (or an
    // earlier held operation) waits for a fence value no Signal has been submitted for, the lists are copied and
    // held instead, and the call succeeds: the batch is replayed by the thread that submits the Signal (or the CPU
    // Signal), which is also where its errors are reported (stderr, and take_held_failure()).
    //
    // `pin` returns what must stay alive through GPU retirement, including an immediately submitted batch. The
    // application may Reset/release its lists as soon as this call returns. Held batches also retain the snapshot
    // while waiting for their submission.
    using PinFn = std::function<std::shared_ptr<void>()>;

    ReplayResult execute(std::span<const CommandList* const> lists, const PinFn& pin = {}) {
        ReplayResult result = ReplayResult::success;
        bool held = false;
        std::vector<std::shared_ptr<void>> retired;  // destroyed after state_mutex_ is released
        std::vector<std::shared_ptr<void>> direct_pins;
        {
            std::lock_guard state(state_mutex_);
            if (!must_hold()) {
                if (pin) direct_pins.push_back(pin());
                const auto before = submitted_;
                result = execute_guest_batch(lists, direct_pins);
                retire_pins(direct_pins, submitted_ > before ? submitted_ : 0);
            } else if (context_.is_device_lost()) {
                result = ReplayResult::unsupported;
            } else {
                Deferred op;
                op.kind = Deferred::Kind::batch;
                op.lists.reserve(lists.size());
                for (const CommandList* list : lists) op.lists.push_back(*list);
                if (pin) op.pins.push_back(pin());
                hold(std::move(op));
                held = true;
            }
            drain_pins(retired);
        }
        retired.clear();
        if (held) context_.holds().pump_ready();  // a Signal may have been submitted since the check
        return result;
    }

    // ID3D12CommandQueue::Signal: the fence reaches `value` after all work submitted so far. Held behind earlier
    // held operations, so the fence does not run ahead of the work it follows. `pin` (called only when the Signal is
    // held) keeps the fence object alive until it has been submitted, as D3D12 does.
    bool signal(VkSemaphore fence, std::uint64_t value, const PinFn& pin = {}) {
        bool ok = true;
        std::vector<std::shared_ptr<void>> retired;
        {
            std::lock_guard state(state_mutex_);
            if (must_hold()) {
                Deferred op;
                op.kind = Deferred::Kind::signal;
                op.fence = fence;
                op.value = value;
                if (pin) op.pins.push_back(pin());
                hold(std::move(op));
            } else {
                ok = submit(VK_NULL_HANDLE, fence, value);
            }
            drain_pins(retired);
        }
        retired.clear();
        context_.holds().pump_ready();  // this Signal, or the hold above, may have made a held batch ready
        return ok;
    }

    // ID3D12CommandQueue::Wait: later submissions wait for the fence. A wait in a Vulkan batch only holds back that
    // batch, so it is kept and attached to the next submission (held on the CPU if no Signal reaches it yet).
    // `pin` returns what keeps the fence object alive until then (a D3D12 queue holds its fences).
    void wait(VkSemaphore fence, std::uint64_t value, const PinFn& pin = {}) {
        std::vector<std::shared_ptr<void>> retired;  // fences whose submission has completed: released unlocked
        {
            std::lock_guard state(state_mutex_);
            waits_.push_back(fence);
            wait_values_.push_back(value);
            if (pin) wait_pins_.push_back(pin());
            drain_pins(retired);
        }
    }

    // The first failure of a held ExecuteCommandLists since the last call (what the call would have returned had it
    // not been held), or success. A Signal behind a failed batch still signals, as when the failure is not held: the
    // work did not run, the application is told here and on stderr, and nothing waits forever.
    ReplayResult take_held_failure() {
        return static_cast<ReplayResult>(held_failure_.exchange(static_cast<int>(ReplayResult::success)));
    }

    // ID3D12CommandQueue::SetMarker/BeginEvent/EndEvent: queue labels, between the submissions around them. An end
    // with no begin open on this queue is dropped, as Vulkan requires a matching begin. No-ops without the extension.
    void marker(DebugMarker::Kind kind, const char* label) {
        std::vector<std::shared_ptr<void>> retired;
        std::lock_guard state(state_mutex_);
        drain_pins(retired);  // destroyed after the lock (reverse declaration order)
        if (!held_.empty()) {  // behind the held work, so behind it in the queue's labels too
            Deferred op;
            op.kind = Deferred::Kind::marker;
            op.marker_kind = kind;
            op.label = label ? label : "";
            hold(std::move(op));
            return;
        }
        marker_now(kind, label);
    }

    bool idle() {
        std::lock_guard lock(context_.submit_mutex());
        return context_.vk.vkQueueWaitIdle(queue_) == VK_SUCCESS;
    }

private:
    // Completed stages, reused by later ARRAY_OF_POINTERS builds. Only execute_now() touches the pool, and it holds
    // state_mutex_ (including the deferred/held release paths), so no extra lock is needed.
    std::vector<KhrInstanceStage> khr_stage_pool_;
    std::vector<KhrAsQueryPool> khr_as_query_pool_;

    void destroy_khr_stages(std::vector<KhrInstanceStage>& stages) {
        for (const auto& stage : stages) {
            if (stage.memory) context_.vk.vkFreeMemory(context_.device(), stage.memory, nullptr);
            if (stage.buffer) context_.vk.vkDestroyBuffer(context_.device(), stage.buffer, nullptr);
        }
        stages.clear();
    }

    // A stage returns to the pool only after its submission has completed, so rewriting it cannot race the GPU.
    void recycle_khr_stages(std::vector<KhrInstanceStage>& stages) {
        for (auto& stage : stages) khr_stage_pool_.push_back(std::move(stage));
        stages.clear();
    }

    void destroy_khr_as_queries(std::vector<KhrAsQueryPool>& pools) {
        if (context_.vk.vkDestroyQueryPool) {
            for (const auto& entry : pools) {
                if (entry.pool) context_.vk.vkDestroyQueryPool(context_.device(), entry.pool, nullptr);
            }
        }
        pools.clear();
    }

    void recycle_khr_as_queries(std::vector<KhrAsQueryPool>& pools) {
        for (auto& entry : pools) {
            entry.used = 0;
            khr_as_query_pool_.push_back(entry);
        }
        pools.clear();
    }

    void destroy_timestamp_queries(std::vector<VkQueryPool>& pools) {
        for (VkQueryPool pool : pools)
            if (pool) context_.vk.vkDestroyQueryPool(context_.device(), pool, nullptr);
        pools.clear();
    }

    bool allocate_khr_as_queries(std::uint32_t count, std::vector<KhrAsQueryPool>& active, VkQueryPool& out_pool,
                                 std::uint32_t& out_first) {
        if (!count || !context_.vk.vkCreateQueryPool || !context_.vk.vkDestroyQueryPool) return false;
        if (!active.empty()) {
            auto& back = active.back();
            if (back.used <= back.capacity && count <= back.capacity - back.used) {
                out_pool = back.pool;
                out_first = back.used;
                back.used += count;
                return true;
            }
        }
        for (auto it = khr_as_query_pool_.begin(); it != khr_as_query_pool_.end(); ++it) {
            if (it->capacity >= count) {
                KhrAsQueryPool entry = *it;
                khr_as_query_pool_.erase(it);
                entry.used = count;
                active.push_back(entry);
                out_pool = entry.pool;
                out_first = 0;
                return true;
            }
        }
        const std::uint32_t capacity = std::max<std::uint32_t>(count, 16u);
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        info.queryCount = capacity;
        VkQueryPool pool = VK_NULL_HANDLE;
        if (context_.vk.vkCreateQueryPool(context_.device(), &info, nullptr, &pool) != VK_SUCCESS || !pool)
            return false;
        active.push_back({pool, capacity, count});
        out_pool = pool;
        out_first = 0;
        return true;
    }

    // A KHR TLAS built with D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS holds console VAs in its pointer list; Vulkan
    // reads those 8-byte entries as device addresses. Stage a translated copy in our own host-visible buffer instead
    // of editing the guest's input. The caller keeps the stage until the submission that builds with it completes.
    ReplayResult stage_khr_instances(const ReplayFunctions& vk, const BuildAccelerationStructure& build,
                                     std::vector<KhrInstanceStage>& stages, VkDeviceAddress& address) {
        const auto& funcs = context_.vk;
        if (!funcs.vkCreateBuffer || !funcs.vkDestroyBuffer || !funcs.vkGetBufferMemoryRequirements ||
            !funcs.vkAllocateMemory || !funcs.vkFreeMemory || !funcs.vkBindBufferMemory || !funcs.vkMapMemory ||
            !funcs.vkUnmapMemory || !funcs.vkGetBufferDeviceAddress || !vk.host_at || !vk.buffer_at ||
            !build.instance_count)
            return ReplayResult::unsupported;
        const std::uint64_t bytes = 8ull * build.instance_count;
        const std::uint8_t* guest = nullptr;
        const HostReadStatus status = vk.host_at(build.instances_va, bytes, guest);
        if (status == HostReadStatus::not_cpu_written) return ReplayResult::unsupported;
        if (status != HostReadStatus::ok) return ReplayResult::invalid_copy;

        KhrInstanceStage stage{};
        const auto discard = [&] {
            if (stage.memory) funcs.vkFreeMemory(context_.device(), stage.memory, nullptr);
            if (stage.buffer) funcs.vkDestroyBuffer(context_.device(), stage.buffer, nullptr);
        };
        // Reuse a completed stage that is already large enough. A per-frame TLAS rebuild then reuses the same buffer
        // and memory instead of making a new vkAllocateMemory call every frame.
        for (auto it = khr_stage_pool_.begin(); it != khr_stage_pool_.end(); ++it) {
            if (it->size >= bytes) {
                stage = std::move(*it);
                khr_stage_pool_.erase(it);
                break;
            }
        }
        if (!stage.buffer) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            if (funcs.vkCreateBuffer(context_.device(), &info, nullptr, &stage.buffer) != VK_SUCCESS)
                return ReplayResult::unsupported;
            VkMemoryRequirements requirements{};
            funcs.vkGetBufferMemoryRequirements(context_.device(), stage.buffer, &requirements);
            const auto& memory = context_.occlusion_resolve_memory_;
            constexpr VkMemoryPropertyFlags host_flags =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            std::uint32_t type = memory.memoryTypeCount;
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if (!(requirements.memoryTypeBits & (1u << i))) continue;
                if ((memory.memoryTypes[i].propertyFlags & host_flags) == host_flags) {
                    type = i;
                    break;
                }
            }
            if (type == memory.memoryTypeCount) {
                discard();
                return ReplayResult::unsupported;
            }
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
            flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            if (funcs.vkAllocateMemory(context_.device(), &allocation, nullptr, &stage.memory) != VK_SUCCESS ||
                funcs.vkBindBufferMemory(context_.device(), stage.buffer, stage.memory, 0) != VK_SUCCESS) {
                discard();
                return ReplayResult::unsupported;
            }
            stage.size = bytes;
        }

        void* mapped = nullptr;
        if (funcs.vkMapMemory(context_.device(), stage.memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            discard();
            return ReplayResult::unsupported;
        }
        // Validate and translate in one pass straight into the staging buffer; no second vector of addresses.
        for (std::uint32_t i = 0; i < build.instance_count; ++i) {
            std::uint64_t va = 0;
            std::memcpy(&va, guest + 8ull * i, sizeof(va));
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            std::uint64_t room = 64;  // one D3D12_RAYTRACING_INSTANCE_DESC
            // No overflow check on heap_base + va: a buffer outside the arena whose device address lies below the
            // arena's (an UPLOAD heap in system memory without Resizable BAR) has a VA that wraps (address - base),
            // and the unsigned sum wraps back to its device address, as the replay's other VA translations do.
            if (!va || !vk.buffer_at(va, room, buffer, offset) || room < 64) {
                funcs.vkUnmapMemory(context_.device(), stage.memory);
                discard();
                return ReplayResult::invalid_copy;
            }
            const std::uint64_t device_address = vk.heap_base + va;
            std::memcpy(static_cast<std::uint8_t*>(mapped) + 8ull * i, &device_address, sizeof(device_address));
        }
        funcs.vkUnmapMemory(context_.device(), stage.memory);
        VkBufferDeviceAddressInfo device_address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        device_address.buffer = stage.buffer;
        stage.address = funcs.vkGetBufferDeviceAddress(context_.device(), &device_address);
        if (!stage.address) {
            discard();
            return ReplayResult::unsupported;
        }
        stages.push_back(std::move(stage));
        address = stages.back().address;
        return ReplayResult::success;
    }

    FrameProfile submit_profile_;
    FrameProfile record_profile_;
    HostShadowProfile shadow_profile_;
    std::vector<const DescriptorHeapMirror*> bound_descriptor_heaps_;
    std::vector<VulkanContext::HostImages::Node> selected_host_images_;
    struct GenerationCell {
        std::uint64_t* word;
        ResourceId resource;
        std::uint64_t allocation;

        bool operator==(const GenerationCell&) const = default;
    };

    // Called under resource_mutex_. A retained host import must still refer to the live control allocation.
    std::optional<GenerationCell> generation_cell(std::uint64_t address, std::uint64_t* observed,
                                                   const GenerationCell* expected = nullptr) const {
        if constexpr (!std::atomic_ref<std::uint64_t>::is_always_lock_free) return std::nullopt;
        if (!address || address % sizeof(std::uint64_t) ||
            address % std::atomic_ref<std::uint64_t>::required_alignment ||
            address > UINT64_MAX - sizeof(std::uint64_t)) return std::nullopt;
        BufferVaIndex<ResourceId>::Hit hit{};
        if (!context_.buffer_vas_.find(address, hit, sizeof(std::uint64_t))) return std::nullopt;
        const ResourceBinding* binding = context_.resolve(hit.id);
        if (!binding || binding->image || !binding->buffer || !binding->coherent_host_import ||
            !binding->host_readable || !binding->host || address < binding->va) return std::nullopt;
        const std::uint64_t offset = address - binding->va;
        if (offset > binding->size || sizeof(std::uint64_t) > binding->size - offset) return std::nullopt;
        const auto* host = binding->host + offset;
        if (reinterpret_cast<std::uintptr_t>(host) != address) return std::nullopt;
        GenerationCell cell{reinterpret_cast<std::uint64_t*>(const_cast<std::uint8_t*>(host)),
                            hit.id, binding->control_allocation};
        if (expected && cell != *expected) return std::nullopt;
        if (binding->snapshot_control_generation) {
            if (address != binding->va ||
                !binding->snapshot_control_generation(address, cell.allocation, observed)) return std::nullopt;
        } else {
            // The portable Vulkan interface also accepts caller-owned test/import storage. A binding that
            // advertises revocable XMem backing must always supply the locked snapshot operation.
            if (cell.allocation) return std::nullopt;
            if (observed) *observed = std::atomic_ref<std::uint64_t>(*cell.word).load(std::memory_order_acquire);
        }
        return cell;
    }

    std::optional<GenerationCell> await_generation(const HostMemoryAcquire& acquire) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        std::uint64_t observed = 0;
        bool waited = false;
        std::optional<GenerationCell> first;
        do {
            if (context_.is_device_lost()) return std::nullopt;
            {
                std::shared_lock lock(context_.resource_mutex_);
                const auto cell = generation_cell(acquire.address, &observed, first ? &*first : nullptr);
                if (!cell) return std::nullopt;
                if (!first) first = cell;
                if (observed >= acquire.reference) {
                    static std::atomic<unsigned> traces{0};
                    if (traces.fetch_add(1, std::memory_order_relaxed) < 12)
                        std::fprintf(stderr, "[rt] VulkanQueue: CPU generation acquired after prefix "
                                             "reference=%llu observed=%llu waited=%u\n",
                            static_cast<unsigned long long>(acquire.reference),
                            static_cast<unsigned long long>(observed), unsigned(waited));
                    return cell;
                }
            }
            // Allow the CPU publisher and other queues to make progress without retaining either Vulkan lock.
            waited = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        std::fprintf(stderr, "[rt] VulkanQueue: CPU generation publication timed out reference=%llu observed=%llu\n",
                     static_cast<unsigned long long>(acquire.reference), static_cast<unsigned long long>(observed));
        return std::nullopt;
    }

    ReplayResult execute_guest_batch(std::span<const CommandList* const> lists, std::vector<std::shared_ptr<void>>& pins) {
        const auto split = std::find_if(lists.begin(), lists.end(), [](const auto* list) {
            return list->ordered_acquires() != 0;
        });
        if (split == lists.end()) return execute_now(lists, pins);
        for (const auto* list : lists) {
            if (!list->closed()) return ReplayResult::list_open;
            if (list->queries_open()) return ReplayResult::invalid_query;
        }
        if (std::any_of(split + 1, lists.end(), [](const auto* list) { return list->ordered_acquires() != 0; }))
            return ReplayResult::unsupported;
        CommandList before, after;
        if (!(*split)->split_generation_wait(before, after)) return ReplayResult::unsupported;
        std::vector<const CommandList*> prefix(lists.begin(), split);
        if (!before.commands().empty()) prefix.push_back(&before);
        std::vector<const CommandList*> suffix{&after};
        suffix.insert(suffix.end(), split + 1, lists.end());

        // Only state_mutex_ is held between executes. Other guest queues and the CPU publisher can make
        // progress. The prefix consumes the queue's original waits and retains its own real native ticket.
        const auto before_ticket = submitted_;
        const auto failure = [&](ReplayResult result) {
            if (before_ticket == submitted_) return result;
            std::fprintf(stderr, "[rt] VulkanQueue: generation wait suffix refused (%d) after prefix ticket %llu; "
                                 "stopping the guest device\n", int(result), static_cast<unsigned long long>(submitted_));
            context_.mark_device_lost();
            return ReplayResult::partial_submission;
        };
        try {
            if (!prefix.empty()) {
                const auto result = execute_now(prefix, pins);
                if (result != ReplayResult::success) return failure(result);
            } else if (!waits_.empty() && !submit(VK_NULL_HANDLE, VK_NULL_HANDLE, 0)) {
                return failure(ReplayResult::unsupported);
            }
            if (submitted_) {
                const VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &progress_, &submitted_};
                VkResult result;
                do {
                    if (context_.is_device_lost()) return failure(ReplayResult::unsupported);
                    result = context_.vk.vkWaitSemaphores(context_.device(), &wait, 100'000'000);
                } while (result == VK_TIMEOUT);
                if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
                if (result != VK_SUCCESS) return failure(ReplayResult::unsupported);
            }
            const auto& acquire = std::get<HostMemoryAcquire>(after.commands().front());
            const auto latched = await_generation(acquire);
            if (!latched) return failure(ReplayResult::unsupported);
            const auto result = execute_now(suffix, pins, {}, &*latched);
            if (result != ReplayResult::success) return failure(result);
            static std::atomic<unsigned> traces{0};
            if (traces.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr, "[rt] VulkanQueue: ordered generation wait retired prefix lists=%zu writes=%zu; "
                    "suffix submitted ticket=%llu\n", prefix.size(), before.commands().size(),
                    static_cast<unsigned long long>(submitted_));
            return result;
        } catch (const std::bad_alloc&) {
            return failure(ReplayResult::unsupported);
        }
    }

    ReplayResult execute_now(std::span<const CommandList* const> lists, std::vector<std::shared_ptr<void>>& pins,
                             std::span<const ResourceId> present_sources = {},
                             const GenerationCell* latched_generation = nullptr) {
        const auto profile_start = FrameProfile::now();
        if (context_.is_device_lost() || !wait_for_room()) return ReplayResult::unsupported;
        if (kind_ != QueueKind::direct)
            for (const auto* list : lists)
                for (const auto& command : list->commands())
                    if (std::holds_alternative<ClearRenderTarget>(command) ||
                        std::holds_alternative<ClearDepthStencil>(command))
                        return ReplayResult::unsupported;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        struct KhrGuard {
            VulkanQueue* queue;
            std::vector<KhrInstanceStage> stages;
            std::vector<KhrAsQueryPool> as_queries;
            ~KhrGuard() {
                queue->recycle_khr_stages(stages);
                queue->recycle_khr_as_queries(as_queries);
            }
        } khr_guard{this};
        // These pools belong to this execution, not the recorded list. Before a command slot exists, no submitted
        // work can use them; after handoff, the slot retains them until its own timeline says the GPU is finished.
        struct TimestampGuard {
            VulkanQueue* queue;
            std::vector<VkQueryPool> pools;
            ~TimestampGuard() { queue->destroy_timestamp_queries(pools); }
        } timestamp_guard{this};
        std::function<void(bool)> bindless_commit;
        // Lock hierarchy: resource_mutex_ -> tracker_mutex_ -> submit_mutex_.
        // Shared read-lock allows concurrent prepare() across queues; tracker_mutex_ is acquired lazily during
        // prepare() only if a list actually converts a depth24_as_float copy, and otherwise right before
        // bindless_prepare_/emit() through submit() so bindless flushes, layout emits, and submissions stay in order.
        std::shared_lock resource_lock(context_.resource_mutex_);
        std::unique_lock tracker_lock(context_.tracker_mutex_, std::defer_lock);
        // Declared after tracker_lock so any early return after depth24_->prepare() retires uncommitted scratch/pools
        // with ticket 0 while tracker_lock is still held.
        struct Depth24Guard {
            Depth24Converter* d24 = nullptr;
            VkSemaphore progress = VK_NULL_HANDLE;
            bool active = false;
            ~Depth24Guard() {
                if (active && d24) d24->retire(progress, 0);
            }
        } depth24_guard{context_.depth24_.get(), progress_, false};
        struct ScratchGuard {  // the same for the copy scratch
            ScratchBuffers* scratch = nullptr;
            VkSemaphore progress = VK_NULL_HANDLE;
            bool active = false;
            ~ScratchGuard() {
                if (active && scratch) scratch->retire(progress, 0);
            }
        } scratch_guard{context_.scratch_.get(), progress_, false};
        const bool transfer_only = !guest_queue_ || family_ == context_.transfer_only_family_;
        if (!transfer_only) bindless_commit = context_.bindless_commit_;
        // Kept between executions: prepare refills each list's entries in the storage the last execution left, where
        // making them fresh for every command was about half the layer's CPU cost per command (replay_cpu_bench).
        auto& prepared = prepared_;
        // Keep the storage from a larger batch. resize() down would destroy the unused entries and their vectors, so
        // a queue that sees 8 lists and then 1 in every frame would rebuild them on each execution. The loops below
        // use only the first lists.size() entries, so stale entries are never prepared, queried or emitted.
        if (prepared.size() < lists.size()) prepared.resize(lists.size());
        ReplayFunctions replay = context_.replay_;
        std::optional<FrameDrawTargets> frame_draws;
        if (FrameDrawActivity::enabled()) replay.frame_draws = &frame_draws.emplace();
        std::optional<AttachmentWriteBatch> attachment_writes;
        if (context_.trace_native_attachments_) replay.attachment_writes = &attachment_writes.emplace();
        replay.execution_pins = &pins;
        HostImageReads image_reads;
        if (!context_.host_images_.empty()) replay.image_reads = &image_reads;
        // prepare() holds resource_mutex_ shared. Validate the exact live coherent import and read the CPU's real
        // word on every execution. The startup floor is stable. Later generations may regress:
        // observe them only at their logical execution boundary, after all queue predecessors retire.
        replay.host_memory_acquire = [this, &lists, latched_generation](const HostMemoryAcquire& acquire) {
            const auto [address, reference] = acquire;
            if (reference != 1) {
                if (reference <= 1 || !starts_with_acquire(lists, acquire) || !latched_generation ||
                    !generation_cell(address, nullptr, latched_generation)) {
                    std::fprintf(stderr, "[rt] VulkanQueue: generation wait has no matching retired-prefix acquire\n");
                    return false;
                }
                // A satisfied wait is one-shot. The CPU can publish an older generation again after this point.
                return true;
            }
            std::uint64_t observed = 0;
            if (!generation_cell(address, &observed)) return false;
            static std::atomic<unsigned> observations{0};
            if (observations.fetch_add(1, std::memory_order_relaxed) < 16)
                std::fprintf(stderr, "[rt] VulkanQueue: startup host acquire address=0x%llx reference=%llu "
                                     "observed=%llu satisfied=%u\n",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(reference),
                    static_cast<unsigned long long>(observed), unsigned(observed >= reference));
            return observed >= reference;
        };
        const auto& timestamp_vk = context_.vk;
        const double timestamp_period = context_.timestamp_period();
        // The timestamp write itself can run on a transfer-only queue, but vkCmdResetQueryPool cannot. Keep this
        // command-reset implementation off that family until a host-reset path is explicitly supported.
        if (!transfer_only && context_.timestamp_valid_bits(family_) && timestamp_period > 0 &&
            std::isfinite(timestamp_period) && timestamp_vk.vkCreateQueryPool && timestamp_vk.vkDestroyQueryPool &&
            timestamp_vk.vkCmdResetQueryPool &&
            timestamp_vk.vkCmdWriteTimestamp && timestamp_vk.vkCmdCopyQueryPoolResults) {
            replay.timestamp_query = [this, &timestamp_guard]() -> VkQueryPool {
                VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                info.queryType = VK_QUERY_TYPE_TIMESTAMP;
                info.queryCount = 1;
                VkQueryPool pool = VK_NULL_HANDLE;
                if (context_.vk.vkCreateQueryPool(context_.device(), &info, nullptr, &pool) != VK_SUCCESS)
                    return VK_NULL_HANDLE;
                try {
                    timestamp_guard.pools.push_back(pool);
                } catch (const std::bad_alloc&) {
                    context_.vk.vkDestroyQueryPool(context_.device(), pool, nullptr);
                    return VK_NULL_HANDLE;
                }
                return pool;
            };
        } else {
            replay.timestamp_query = {};
        }
        replay.queue_stages = kind_ == QueueKind::copy      ? detail::queue_stages_copy
                              : kind_ == QueueKind::compute ? detail::queue_stages_compute
                                                            : 0;
        replay.prepare_khr_instances = [this, &replay, &khr_guard](const BuildAccelerationStructure& build,
                                                                   VkDeviceAddress& address) {
            return stage_khr_instances(replay, build, khr_guard.stages, address);
        };
        if (replay.write_acceleration_structures_properties) {
            replay.prepare_khr_postbuild = [this, &khr_guard](std::uint32_t count, VkQueryPool& pool,
                                                              std::uint32_t& first) {
                return allocate_khr_as_queries(count, khr_guard.as_queries, pool, first);
            };
        }
        std::uint64_t captures = 0;
        bool runs_shaders = false, has_commands = false;
        const auto explicit_import = [&](ResourceId id) {
            if (!id) return;
            if (const auto* image = context_.resolve(id); image && image->host_readonly_image) {
                if (image->host_image_address) image_reads.add(image->host_image_address);
                else { image_reads.all = true; image_reads.reasons |= HostImageReads::unnamed; }
            }
        };
        for (const auto* list : lists)
            for (const auto& command : list->commands()) {
                if (const auto* p = std::get_if<Predication>(&command); p && p->buffer) ++captures;
                runs_shaders |= std::holds_alternative<Dispatch>(command) || std::holds_alternative<Draw>(command) ||
                                std::holds_alternative<ExecuteIndirect>(command);
                has_commands = true;
                if (!replay.image_reads || image_reads.all) continue;
                // Shader keys are collected during prepare from the frozen table. Other image readers
                // name their resources directly. Preserve the full refresh for unknown operations.
                if (const auto [dst, src] = copy_ids(command); dst || src) {
                    explicit_import(src);
                    explicit_import(dst);
                } else if (const auto* draw = std::get_if<Draw>(&command)) {
                    if (draw->views)
                        for (const auto& target : draw->views->render_targets) explicit_import(target.resource);
                    explicit_import(draw->depth.resource);
                } else if (const auto* barrier = std::get_if<TextureBarrier>(&command)) {
                    explicit_import(barrier->resource);
                } else if (const auto* barrier = std::get_if<TransitionBarrier>(&command)) {
                    explicit_import(barrier->resource);
                } else if (const auto* clear = std::get_if<ClearRenderTarget>(&command)) {
                    explicit_import(clear->target.resource);
                } else if (const auto* clear = std::get_if<ClearDepthStencil>(&command)) {
                    explicit_import(clear->target.resource);
                } else if (const auto* barrier = std::get_if<UavBarrier>(&command)) {
                    // A global UAV visibility barrier reads no texture contents.
                    explicit_import(barrier->resource);
                } else {
                    image_reads.all = !std::holds_alternative<Dispatch>(command) &&
                        !std::holds_alternative<WriteTimestamp>(command) &&
                        !std::holds_alternative<WriteImmediate>(command) &&
                        !std::holds_alternative<CopyAddress>(command) &&
                        !std::holds_alternative<HostMemoryAcquire>(command) &&
                        !std::holds_alternative<DebugMarker>(command) &&
                        !std::holds_alternative<GlobalBarrier>(command) &&
                        !std::holds_alternative<BufferBarrier>(command);
                    static_assert(std::variant_size_v<Command> <= 58);
                    if (image_reads.all) image_reads.reasons |= std::uint64_t{1} << command.index();
                }
            }
        // An empty internal submission deliberately initializes a newly imported native image.
        if (!has_commands) { image_reads.all = true; image_reads.reasons |= HostImageReads::empty; }
        std::unique_ptr<PredicateReducer::Batch> batch;
        std::unique_ptr<PredicatedBufferCopier::Batch> copy_batch;
        std::unique_ptr<IndirectDispatchCounter::Batch> counted_batch;
        if (captures) {
            if (kind_ == QueueKind::copy || !context_.predicate_reducer_ || family_ != context_.predicate_family_)
                return ReplayResult::unsupported;
            // The helper's lazy pipeline is shared by queues. Serialize preparation with the tracker lock,
            // retaining main's resource -> tracker -> submit lock order through emit and submission.
            if (!tracker_lock.owns_lock()) tracker_lock.lock();
            batch = context_.predicate_reducer_->batch(captures);
            if (!batch) return ReplayResult::unsupported;
            replay.prepare_predicate = [&](VkBuffer buffer, VkDeviceSize offset, PredicateCapture& out) {
                return batch->prepare(buffer, offset, out);
            };
            replay.capture_predicate = [&](VkCommandBuffer cb, const PredicateCapture& p) {
                context_.predicate_reducer_->capture(cb, p);
            };
            if (context_.predicated_copier_) {
                copy_batch = context_.predicated_copier_->batch();
                replay.prepare_predicated_copy = [&](VkBuffer source, VkBuffer destination, VkDeviceSize src,
                                                     VkDeviceSize dst, VkDeviceSize size,
                                                     std::vector<PredicatedCopyDispatch>& out) {
                    return copy_batch->prepare(source, destination, src, dst, size, out);
                };
                replay.emit_predicated_copy = [&](VkCommandBuffer cb, const std::vector<PredicatedCopyDispatch>& jobs) {
                    context_.predicated_copier_->emit(cb, jobs);
                };
            }
        }
        if (context_.scratch_) {
            replay.copy_scratch = [this, &tracker_lock, &scratch_guard](VkDeviceSize bytes, VkBuffer& buffer,
                                                                        VkDeviceSize& offset) {
                if (!tracker_lock.owns_lock()) tracker_lock.lock();
                if (!scratch_guard.active) {
                    context_.scratch_->collect();
                    scratch_guard.active = true;
                }
                return context_.scratch_->get(bytes, buffer, offset);
            };
        }
        if (kind_ != QueueKind::copy && context_.counted_dispatch_) {
            replay.prepare_counted_dispatch = [this, &tracker_lock, &counted_batch](
                VkBuffer args, VkDeviceSize args_offset, VkBuffer count, VkDeviceSize count_offset,
                std::uint32_t stride, std::uint32_t maximum, CountedDispatch& out) {
                // Pipeline creation is lazy and shared across queues; serialize it with other replay preparation.
                if (!tracker_lock.owns_lock()) tracker_lock.lock();
                if (!counted_batch) counted_batch = context_.counted_dispatch_->batch();
                return counted_batch->prepare(args, args_offset, count, count_offset, stride, maximum, out);
            };
            replay.emit_counted_dispatch = [this](VkCommandBuffer cb, const CountedDispatch& job) {
                context_.counted_dispatch_->emit(cb, job);
            };
        }
        if (context_.depth24_ && !transfer_only) {
            replay.depth24_prepare = [this, &tracker_lock, &depth24_guard](Depth24Job& job,
                                                                           std::vector<Depth24Job>& parts) {
                if (!tracker_lock.owns_lock()) tracker_lock.lock();
                if (!depth24_guard.active) {
                    context_.depth24_->collect();
                    depth24_guard.active = true;
                }
                return context_.depth24_->prepare(job, parts);
            };
        }
        const auto& occlusion_memory = context_.occlusion_resolve_memory_;
        const bool can_resolve_occlusion = occlusion_memory.memoryTypeCount != 0;
        if (can_resolve_occlusion) {
            replay.sum_occlusion = [this](VkCommandBuffer cb, VkQueryPool pool, std::uint32_t first,
                                          std::uint32_t count, VkBuffer dst, VkDeviceSize offset) {
                occlusion_resolve_->sum(cb, pool, first, count, dst, offset);
            };
            replay.resolve_binary_occlusion = [this](VkCommandBuffer cb, VkQueryPool pool, std::uint32_t first,
                                                      std::uint32_t count, VkBuffer dst, VkDeviceSize offset) {
                occlusion_resolve_->normalize(cb, pool, first, count, dst, offset);
            };
            replay.sum_statistics = [this](VkCommandBuffer cb, VkQueryPool pool, VkQueryPool mesh_pool,
                                           VkQueryPool primitives_pool, const std::uint8_t* mesh,
                                           std::uint32_t first, std::uint32_t count, VkBuffer dst,
                                           VkDeviceSize offset) {
                occlusion_resolve_->sum_statistics(cb, pool, mesh_pool, primitives_pool, mesh, first, count, dst,
                                                  offset);
            };
            replay.sum_binary_occlusion = [this](VkCommandBuffer cb, VkQueryPool pool, std::uint32_t first,
                                                 std::uint32_t count, VkBuffer dst, VkDeviceSize offset) {
                occlusion_resolve_->sum_binary(cb, pool, first, count, dst, offset);
            };
        }
        const auto resolve = [this](ResourceId id) { return context_.resolve(id); };
        std::uint32_t segments = 0, statistics_segments = 0;
        bool has_queries = false;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            const auto result = prepare(*lists[i], resolve, replay, prepared[i], can_resolve_occlusion);
            if (result != ReplayResult::success) return result;
            if (prepared[i].segment_count > UINT32_MAX - segments ||
                prepared[i].statistics_segment_count > UINT32_MAX - statistics_segments)
                return ReplayResult::unsupported;
            segments += prepared[i].segment_count;
            statistics_segments += prepared[i].statistics_segment_count;
            has_queries = has_queries || !prepared[i].queries.empty();
        }
        const auto segment_pool = [this](VkQueryType type, std::uint32_t count,
                                         std::shared_ptr<OcclusionSegmentPool>& out) {
            const auto& vk = context_.vk;
            if (!vk.vkCreateQueryPool || !vk.vkDestroyQueryPool) return false;
            out = std::make_shared<OcclusionSegmentPool>();
            out->device = context_.device();
            out->destroy = vk.vkDestroyQueryPool;
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = type;
            info.queryCount = count;
            // The counters of a D3D12 pipeline-statistics heap, in D3D12_QUERY_DATA_PIPELINE_STATISTICS order.
            if (type == VK_QUERY_TYPE_PIPELINE_STATISTICS) info.pipelineStatistics = 0x7ff;
            return vk.vkCreateQueryPool(out->device, &info, nullptr, &out->pool) == VK_SUCCESS;
        };
        std::shared_ptr<OcclusionSegmentPool> physical, statistics;
        if (segments && !segment_pool(VK_QUERY_TYPE_OCCLUSION, segments, physical)) return ReplayResult::unsupported;
        // Without the compute helper a split query could not be summed: leave statistics queries whole, as before
        // (QueryReplay::plan refuses a list whose statistics query holds a mesh draw then).
        if (statistics_segments && can_resolve_occlusion) {
            // Vulkan forbids legacy clipping counters while mesh work executes (07076). Native D3D12 counts
            // clipped/culled mesh primitives in those fields; neither zero nor MSPrimitives is an equivalent.
            // Refuse the complete submission before emit until those counters have a faithful implementation.
            for (std::size_t i = 0; i < lists.size(); ++i)
                if (std::find(prepared[i].statistics_mesh.begin(), prepared[i].statistics_mesh.end(), 1) !=
                    prepared[i].statistics_mesh.end()) return ReplayResult::unsupported;
            if (!segment_pool(VK_QUERY_TYPE_PIPELINE_STATISTICS, statistics_segments, statistics))
                return ReplayResult::unsupported;
        }
        // QueryReplay::plan() reads QueryPool::ended / occlusion and QueryReplay::commit() writes them after
        // resource_lock is unlocked. Hold tracker_mutex_ across plan -> emit -> submit -> commit so concurrent queues
        // see submission-order query state and remove_query_pool() waits until commit finishes.
        if (has_queries && !tracker_lock.owns_lock()) tracker_lock.lock();
        QueryReplay queries;
        std::uint32_t base = 0, statistics_base = 0;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            auto& list = prepared[i];
            const auto result = queries.plan(list, physical, base, can_resolve_occlusion, statistics, statistics_base);
            if (result != ReplayResult::success) return result;
            base += list.segment_count;
            statistics_base += list.statistics_segment_count;
        }
        if (queries.needs_sum || queries.needs_binary || queries.needs_statistics_sum || queries.needs_sum_binary) {
            if (!occlusion_resolve_) {
                auto helper = std::make_unique<OcclusionResolve>(context_.vk, context_.device());
                if (!helper->initialize(occlusion_memory)) return ReplayResult::unsupported;
                occlusion_resolve_ = std::move(helper);
            }
            if (!occlusion_resolve_->prepare(queries.needs_sum, queries.needs_binary, queries.needs_statistics_sum,
                                             queries.needs_sum_binary))
                return ReplayResult::unsupported;
        }

        // The resource lock keeps the selected map nodes alive through both shadow passes. Allocate
        // the bounded selection before acquiring a command slot, so a failed allocation can be retried.
        std::span<const VulkanContext::HostImages::Node> selected;
        try {
            if (present_sources.empty()) {
                selected = context_.host_images_.select(image_reads.all, image_reads.addresses, selected_host_images_);
            } else {
                selected_host_images_.clear();
                for (auto id : present_sources) {
                    const auto it = context_.resources_.find(id);
                    if (it != context_.resources_.end() && it->second.host_readonly_image &&
                        std::none_of(selected_host_images_.begin(), selected_host_images_.end(),
                                     [id](const auto& node) { return node.entry->first == id; })) {
                        selected_host_images_.push_back({it->second.host_image_address, &*it});
                    }
                }
                selected = selected_host_images_;
            }
        } catch (const std::bad_alloc&) {
            return ReplayResult::unsupported;
        }
        CommandSlot* slot = acquire();
        if (!slot) return ReplayResult::unsupported;
        slot->timestamp_queries = std::move(timestamp_guard.pools);
        timestamp_guard.pools.clear();
        struct TimestampSlotGuard {
            VulkanQueue* queue;
            CommandSlot* slot;
            ~TimestampSlotGuard() {
                // Successful submit() assigns a real queue ticket. On any earlier failure these queries were never
                // submitted; reset the buffer before destroying its recorded objects and make the slot reusable.
                if (slot->timestamp_queries.empty() || (slot->ticket && slot->ticket != UINT64_MAX)) return;
                if (slot->ticket == UINT64_MAX &&
                    queue->context_.vk.vkResetCommandBuffer(slot->buffer, 0) == VK_SUCCESS) slot->ticket = 0;
                queue->destroy_timestamp_queries(slot->timestamp_queries);
            }
        } timestamp_slot_guard{this, slot};
        slot->khr_instances = std::move(khr_guard.stages);  // freed when this slot is reused
        khr_guard.stages.clear();
        slot->khr_as_queries = std::move(khr_guard.as_queries);
        khr_guard.as_queries.clear();
        command_buffer = slot->buffer;
        // MoltenVK writes a timestamp at the end of a Metal encoder and resolves it with a blit that nothing orders
        // after that write, so a copy in the same or the next command buffer reads the old value (docs section 4.6).
        // An event set and waited for in this command buffer orders it after this submission's timestamps; a wait for
        // this queue's previous submission orders it after earlier ones still running.
        bool ordered_timestamps = false;
        const auto& events = context_.vk;
        if (context_.order_timestamp_copies() && events.vkCreateEvent && events.vkDestroyEvent && events.vkResetEvent &&
            events.vkCmdSetEvent && events.vkCmdWaitEvents)
            replay.order_timestamp_copy = [this, slot, &ordered_timestamps](VkCommandBuffer cb) {
                const VkEvent event = take_event();
                if (!event) return;
                slot->events.push_back(event);
                const auto& vk = context_.vk;
                vk.vkCmdSetEvent(cb, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                vk.vkCmdWaitEvents(cb, 1, &event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
                ordered_timestamps = true;
            };
        const auto discard = [&] {
            if (context_.vk.vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS) slot->ticket = 0;
        };
        // Validate/freeze CPU shadow sources before bindless_prepare_ can publish a new descriptor generation.
        // A source mutation must not strand a generation whose table upload was recorded but never submitted.
        // tracker_mutex_ also serializes scratch/snapshot access between queues on this device.
        if (!tracker_lock.owns_lock()) tracker_lock.lock();
        const auto profile_prepared = FrameProfile::now();
        HostShadowProfile::Samples shadow_samples{};
        const bool profile_host = shadow_profile_.enabled();
        const auto shadow_begin = profile_host ? HostShadowProfile::now() : HostShadowProfile::Time{};
        struct MutableUpload {
            ResourceId id;
            const ResourceBinding* image;
            std::shared_ptr<MutableHostShadow> owner;
        };
        std::vector<MutableUpload> mutable_uploads;
        std::vector<VkBufferMemoryBarrier> upload_sources;
        std::vector<VkImageMemoryBarrier> upload_before, upload_after;
        VkPipelineStageFlags upload_src_stages = VK_PIPELINE_STAGE_HOST_BIT;
        VkPipelineStageFlags upload_copy_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkPipelineStageFlags upload_dst_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkAccessFlags upload_write = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkAccessFlags upload_read = VK_ACCESS_MEMORY_READ_BIT;
        detail::restrict_to_queue(replay, upload_copy_stage, upload_write);
        detail::restrict_to_queue(replay, upload_dst_stages, upload_read);
        const auto shadow_refused = [](const char* reason, const ResourceBinding* image = nullptr,
                                        std::uint32_t layer = 0, std::uint32_t mip = 0,
                                        const VkBufferImageCopy* region = nullptr) {
            static const bool enabled = [] {
                const char* value = std::getenv("LEONIDA_HOST_SHADOW_DIAGNOSTICS");
                return value && std::strcmp(value, "1") == 0;
            }();
            static std::atomic<unsigned> reports{0};
            if (enabled && reports.fetch_add(1, std::memory_order_relaxed) < 32)
                std::fprintf(stderr, "[rt] VulkanQueue: host shadow refused reason=%s format=%d "
                    "size=%ux%ux%u layers=%u mips=%u volume=%u line=%u samples=%u layer_bytes=%llu "
                    "layer=%u mip=%u region=%ux%ux%u offset=%llu row_texels=%u\n", reason,
                    image ? int(image->format) : 0, image ? image->width : 0, image ? image->height : 0,
                    image ? image->depth : 0, image ? image->array_layers : 0, image ? image->mip_levels : 0,
                    image ? unsigned(image->volume) : 0, image ? unsigned(image->line) : 0,
                    image ? image->samples : 0,
                    static_cast<unsigned long long>(image ? image->host_shadow_layer_bytes : 0), layer, mip,
                    region ? region->imageExtent.width : 0, region ? region->imageExtent.height : 0,
                    region ? region->imageExtent.depth : 0,
                    static_cast<unsigned long long>(region ? region->bufferOffset : 0),
                    region ? region->bufferRowLength : 0);
            return ReplayResult::unsupported;
        };
        for (const auto& node : selected) {
            const auto& [id, image] = *node.entry;
            if (image.mutable_host_shadow) {
                const auto& shadow = image.mutable_host_shadow;
                if (!image.host_readonly_image || !image.image || !image.host_shadow_buffer ||
                    !image.host_shadow_layer_bytes || image.aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
                    (image.line && (image.volume || image.height != 1 || image.depth != 1 || image.array_layers != 1)) ||
                    image.samples != 1 || !image.array_layers ||
                    (image.volume && (image.array_layers != 1 || !image.depth)) ||
                    !image.width || !image.height || !image.mip_levels || image.mip_levels > 32 ||
                    !context_.vk.vkCmdCopyBufferToImage || !context_.vk.vkCmdPipelineBarrier ||
                    !shadow->capture || !shadow->stage || !shadow->commit)
                    return shadow_refused("mutable-binding", &image);
                if (image.array_layers > UINT32_MAX / image.mip_levels ||
                    image.host_shadow_layer_bytes > UINT64_MAX / image.array_layers ||
                    shadow->regions.size() != std::size_t(image.array_layers) * image.mip_levels)
                    return shadow_refused("mutable-regions", &image);
                const auto block = block_format(image.format, VK_IMAGE_ASPECT_COLOR_BIT);
                if ((block.block != 1 && block.block != 4) || (image.line && block.block != 1) ||
                    block.bytes > 16 || !std::has_single_bit(block.bytes))
                    return shadow_refused("mutable-block-format", &image);
                for (std::uint32_t layer = 0; layer < image.array_layers; ++layer) {
                    const VkDeviceSize layer_start = VkDeviceSize(layer) * image.host_shadow_layer_bytes;
                    for (std::uint32_t mip = 0; mip < image.mip_levels; ++mip) {
                        const auto& region = shadow->regions[std::size_t(layer) * image.mip_levels + mip];
                        const std::uint32_t width = std::max(1u, image.width >> mip);
                        const std::uint32_t height = std::max(1u, image.height >> mip);
                        const std::uint32_t depth = image.volume ? std::max(1u, image.depth >> mip) : 1;
                        if (region.imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                            region.imageSubresource.mipLevel != mip || region.imageSubresource.baseArrayLayer != layer ||
                            region.imageSubresource.layerCount != 1 || region.imageOffset.x || region.imageOffset.y ||
                            region.imageOffset.z || region.imageExtent.width != width || region.imageExtent.height != height ||
                            region.imageExtent.depth != depth || region.bufferOffset % std::max(4u, block.bytes) ||
                            (region.bufferRowLength &&
                             (region.bufferRowLength < width || region.bufferRowLength % block.block)) ||
                            region.bufferImageHeight || region.bufferOffset < layer_start ||
                            region.bufferOffset - layer_start > image.host_shadow_layer_bytes)
                            return shadow_refused("mutable-region-bounds", &image, layer, mip, &region);
                        const VkDeviceSize row_texels = region.bufferRowLength ? region.bufferRowLength : width;
                        const VkDeviceSize pitch = ((row_texels + block.block - 1) / block.block) * block.bytes;
                        const VkDeviceSize last_row = ((VkDeviceSize(width) + block.block - 1) / block.block) * block.bytes;
                        const VkDeviceSize rows = ((VkDeviceSize(height) + block.block - 1) / block.block) * depth;
                        const VkDeviceSize room = image.host_shadow_layer_bytes - (region.bufferOffset - layer_start);
                        if (last_row > room || (rows > 1 && pitch > (room - last_row) / (rows - 1)))
                            return shadow_refused("mutable-staging-bounds", &image, layer, mip, &region);
                    }
                }
                bool dirty = false;
                const auto capture_start = profile_host ? HostShadowProfile::now() : HostShadowProfile::Time{};
                if (!shadow->capture(dirty)) return shadow_refused("mutable-capture", &image);
                if (profile_host) {
                    const bool large_r8 = image.format == VK_FORMAT_R8_UNORM && image.width >= 1920 && image.height >= 1080;
                    const auto kind = large_r8 ? HostShadowProfile::large_r8 :
                                      block.block == 4 ? HostShadowProfile::bc : HostShadowProfile::other_mutable;
                    HostShadowProfile::capture(shadow_samples, kind, capture_start,
                                               image.host_shadow_layer_bytes * image.array_layers, dirty);
                }
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mip_levels, 0, image.array_layers};
                const auto pieces = detail::current(&context_.tracker_, id, range, VK_IMAGE_LAYOUT_UNDEFINED);
                for (const auto& piece : pieces)
                    if ((piece.layout != VK_IMAGE_LAYOUT_UNDEFINED && piece.layout != VK_IMAGE_LAYOUT_GENERAL) ||
                        (!dirty && piece.layout == VK_IMAGE_LAYOUT_UNDEFINED))
                        return shadow_refused("mutable-tracked-layout", &image);
                if (!dirty) continue;
                try {
                    mutable_uploads.push_back({id, &image, shadow});
                    VkBufferMemoryBarrier source{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                    source.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
                    source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    source.srcQueueFamilyIndex = source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    source.buffer = image.host_shadow_buffer;
                    source.size = VK_WHOLE_SIZE;
                    upload_sources.push_back(source);
                    for (const auto& piece : pieces) {
                        const bool first = piece.layout == VK_IMAGE_LAYOUT_UNDEFINED;
                        VkAccessFlags access = first ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                        VkPipelineStageFlags stages = first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT :
                                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                        detail::restrict_to_queue(replay, stages, access);
                        upload_src_stages |= stages;
                        upload_before.push_back(detail::image_barrier_info(image, piece.range, piece.layout,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, access, upload_write));
                    }
                    upload_after.push_back(detail::image_barrier_info(image, range,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, upload_write, upload_read));
                } catch (const std::bad_alloc&) { return shadow_refused("mutable-upload-allocation", &image); }
                continue;
            }
            if (!image.host_readonly_image || !image.host_shadow_prepare) continue;
            if (!image.host_shadow_buffer || !image.host_shadow_layer_bytes || !image.array_layers ||
                image.array_layers > 96 || !context_.vk.vkCmdCopyBufferToImage ||
                !context_.vk.vkCmdPipelineBarrier) return shadow_refused("immutable-binding", &image);
            const auto capture_start = profile_host ? HostShadowProfile::now() : HostShadowProfile::Time{};
            if (!image.host_shadow_prepare()) return shadow_refused("immutable-prepare", &image);
            if (profile_host)
                HostShadowProfile::capture(shadow_samples, HostShadowProfile::immutable, capture_start,
                                           image.host_shadow_layer_bytes * image.array_layers, false);
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.array_layers};
            for (const auto& piece : detail::current(&context_.tracker_, id, range, VK_IMAGE_LAYOUT_UNDEFINED))
                if (piece.layout != VK_IMAGE_LAYOUT_UNDEFINED && piece.layout != VK_IMAGE_LAYOUT_GENERAL)
                    return shadow_refused("immutable-tracked-layout", &image);
        }
        if (upload_sources.size() > UINT32_MAX || upload_before.size() > UINT32_MAX)
            return shadow_refused("mutable-upload-count");
        const auto wait_begin = profile_host ? HostShadowProfile::now() : HostShadowProfile::Time{};
        bool host_writes_safe = false;
        const auto retire_before_host_write = [&] {
            if (host_writes_safe) return true;
            const auto ticket = context_.submitted_retire_ticket();
            host_writes_safe = !ticket || context_.wait_retire_ticket(ticket) >= ticket;
            return host_writes_safe;
        };
        if (!refresh_descriptor_heaps(lists, bound_descriptor_heaps_, retire_before_host_write)) return shadow_refused("descriptor-retirement");
        if (!mutable_uploads.empty()) {
            // A persistent staging buffer cannot be changed while an older GPU copy reads it. tracker_mutex_
            // excludes new resource-using submissions, and QueueHolds never submits waits for unsent signals.
            // Join only already accepted native work; no pending/held operation is pumped while queue locks are held.
            if (!retire_before_host_write()) return shadow_refused("mutable-retirement");
        }
        const auto stage_begin = profile_host ? HostShadowProfile::now() : HostShadowProfile::Time{};
        for (const auto& upload : mutable_uploads) upload.owner->stage();
        if (profile_host)
            shadow_profile_.record(shadow_samples, present_sources.empty() ? image_reads.reasons : 0,
                                   image_reads.unbounded_names, selected.size(),
                                   shadow_begin, wait_begin, stage_begin, HostShadowProfile::now());
        const auto profile_shadows = FrameProfile::now();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (context_.vk.vkBeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
            discard();
            return ReplayResult::unsupported;
        }
        // D3D12 ends every ExecuteCommandLists with all its work done and its writes visible to the next one on the
        // queue (implicit state decay relies on it), and a list may then touch the same memory without a barrier:
        // two submissions copying into one buffer, say. Vulkan orders submissions on a queue only by what their
        // barriers say, so this one starts with a full memory barrier against everything submitted before it
        // (synchronization validation: WRITE_AFTER_WRITE between vk_queue_test's back-to-back copies).
        if (context_.vk.vkCmdPipelineBarrier) {  // a mocked context may leave it out
            VkMemoryBarrier full{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            full.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            full.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &full, 0, nullptr, 0, nullptr);
        }
        if (!tracker_lock.owns_lock()) tracker_lock.lock();
        const auto profile_before_bindless = FrameProfile::now();
        if (!transfer_only && context_.bindless_prepare_ && !context_.bindless_prepare_(command_buffer)) {
            const bool ended = context_.vk.vkEndCommandBuffer(command_buffer) == VK_SUCCESS;
            resource_lock.unlock();
            const bool submitted = ended && submit(command_buffer, VK_NULL_HANDLE, 0);
            if (!submitted) discard();
            if (depth24_guard.active) {
                context_.depth24_->retire(progress_, 0);
                depth24_guard.active = false;
            }
            if (scratch_guard.active) {
                context_.scratch_->retire(progress_, 0);
                scratch_guard.active = false;
            }
            if (bindless_commit) bindless_commit(submitted);
            tracker_lock.unlock();
            return ReplayResult::unsupported;
        }
        const auto profile_after_bindless = FrameProfile::now();
        replay.bindless_set = guest_queue_ && context_.bindless_current_ ? context_.bindless_current_() : VK_NULL_HANDLE;
        const bool bindless_uploaded = guest_queue_ && std::exchange(context_.bindless_upload_pending_, false);
        auto next_tracker = context_.tracker_.pending_changes();
        bool initialized_host_images = !mutable_uploads.empty();
        if (initialized_host_images) {
            // Sampled shadows own separate native images and non-overlapping staging ranges, even when they
            // snapshot the same guest pages. Group their transitions around the copies in this submission.
            context_.vk.vkCmdPipelineBarrier(command_buffer, upload_src_stages, upload_copy_stage,
                0, 0, nullptr, std::uint32_t(upload_sources.size()), upload_sources.data(),
                std::uint32_t(upload_before.size()), upload_before.data());
            for (const auto& upload : mutable_uploads) {
                const auto& image = *upload.image;
                context_.vk.vkCmdCopyBufferToImage(command_buffer, image.host_shadow_buffer, image.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(upload.owner->regions.size()), upload.owner->regions.data());
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mip_levels, 0, image.array_layers};
                next_tracker.set(upload.id, range, VK_IMAGE_LAYOUT_GENERAL);
            }
            context_.vk.vkCmdPipelineBarrier(command_buffer, upload_copy_stage, upload_dst_stages,
                0, 0, nullptr, 0, nullptr, std::uint32_t(upload_after.size()), upload_after.data());
        }
        // These images are the caller's coherent CPU bytes, including before their first use. A shader can
        // reach them without naming a resource in its command, so unknown reads include every live import.
        // GENERAL keeps subsequent CPU writes legal; the application fences previous GPU reads before writing.
        for (const auto& node : selected) {
            const auto& [id, image] = *node.entry;
            if (image.mutable_host_shadow) continue;
            if (!image.host_readonly_image) continue;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.array_layers};
            const auto pieces = detail::current(&next_tracker, id, range, VK_IMAGE_LAYOUT_UNDEFINED);
            if (image.host_shadow_prepare) {
                for (const auto& piece : pieces) {
                    if (piece.layout == VK_IMAGE_LAYOUT_GENERAL) continue;
                    VkBufferMemoryBarrier source{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                    source.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
                    source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    source.srcQueueFamilyIndex = source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    source.buffer = image.host_shadow_buffer;
                    source.size = VK_WHOLE_SIZE;
                    context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &source, 0, nullptr);
                    detail::image_barrier(replay, command_buffer, image, piece.range, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    // Separate regions retain four-byte offsets even for one- or two-byte color texels.
                    std::array<VkBufferImageCopy, 96> copies{};
                    for (std::uint32_t index = 0; index < piece.range.layerCount; ++index) {
                        auto& copy = copies[index];
                        const auto layer = piece.range.baseArrayLayer + index;
                        copy.bufferOffset = image.host_shadow_layer_bytes * layer;
                        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                        copy.imageExtent = {image.width, image.height, image.volume ? image.depth : 1};
                    }
                    context_.vk.vkCmdCopyBufferToImage(command_buffer, image.host_shadow_buffer, image.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, piece.range.layerCount, copies.data());
                    detail::image_barrier(replay, command_buffer, image, piece.range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    initialized_host_images = true;
                }
                next_tracker.set(id, range, VK_IMAGE_LAYOUT_GENERAL);
                continue;
            }
            for (const auto& piece : pieces) {
                const bool first = piece.layout == VK_IMAGE_LAYOUT_UNDEFINED;
                initialized_host_images |= first;
                detail::image_barrier(replay, command_buffer, image, piece.range, piece.layout,
                    VK_IMAGE_LAYOUT_GENERAL, first ? 0 : VK_ACCESS_HOST_WRITE_BIT,
                    first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_HOST_BIT,
                    VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
            next_tracker.set(id, range, VK_IMAGE_LAYOUT_GENERAL);
        }
        // Bindless images that may already be visible to shaders are initialized on a submission that runs shaders.
        // initialize() zeroes committed storage images and only transitions sparse images.
        std::vector<ResourceId> storage_images;
        std::vector<VulkanContext::PendingZero> zero_buffers;
        {
            std::lock_guard lock(context_.storage_mutex_);
            if (context_.storage_init_semaphore_ && context_.storage_init_semaphore_ != progress_ &&
                context_.storage_init_version_ != storage_init_seen_) {
                waits_.push_back(context_.storage_init_semaphore_);
                wait_values_.push_back(context_.storage_init_value_);
                storage_init_seen_ = context_.storage_init_version_;
            }
            // The set this submission binds may be a new generation whose table another queue's submission uploads:
            // wait for that one, as for the storage images (the table's buffers are used by one queue family only:
            // every D3D12 queue of the device is made in info.queue_family).
            if (context_.bindless_upload_semaphore_ && context_.bindless_upload_semaphore_ != progress_ &&
                context_.bindless_upload_version_ != bindless_upload_seen_) {
                waits_.push_back(context_.bindless_upload_semaphore_);
                wait_values_.push_back(context_.bindless_upload_value_);
            }
            bindless_upload_seen_ = context_.bindless_upload_version_;
            if (kind_ != QueueKind::copy && runs_shaders)
                storage_images.assign(context_.storage_pending_.begin(), context_.storage_pending_.end());
            if (!storage_images.empty()) context_.storage_pending_.clear();
            if (guest_queue_) zero_buffers.swap(context_.zero_pending_);  // any guest queue can fill a buffer
        }
        for (const auto& zero : zero_buffers)
            context_.vk.vkCmdFillBuffer(command_buffer, zero.buffer, 0, VK_WHOLE_SIZE, 0);
        if (!zero_buffers.empty()) {
            VkMemoryBarrier filled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            filled.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &filled, 0, nullptr, 0, nullptr);
        }
        for (const ResourceId id : storage_images) {
            const ResourceBinding* image = context_.resolve(id);
            if (!image || !image->image) continue;
            const VkImageSubresourceRange range{image->aspects, 0, image->mip_levels, 0, image->array_layers};
            auto pieces = detail::current(&next_tracker, id, range, VK_IMAGE_LAYOUT_UNDEFINED);
            detail::initialize(replay, command_buffer, *image, id, pieces, VK_IMAGE_LAYOUT_GENERAL, &next_tracker);
        }
        const auto profile_before_commands = FrameProfile::now();
        bool wrote_readback = false;
        VkPipelineStageFlags2 readback_stages = 0;
        std::uint32_t open_labels = 0;  // events may span the lists of one ExecuteCommandLists
        for (std::size_t i = 0; i < lists.size(); ++i) {
            const auto& list = prepared[i];
            emit(list, command_buffer, replay, &next_tracker, &open_labels);
            wrote_readback |= list.wrote_readback;
            readback_stages |= list.readback_src_stages;
        }
        close_labels(command_buffer, replay, open_labels);
        // Include shader/AS writes: descriptors and device addresses can reach CPU-visible memory without a copy
        // to a known readback resource. Fence completion alone does not make those writes visible to the host.
        if (wrote_readback) {
            const VkPipelineStageFlags2 allowed = queue_supported_stages(kind_);
            // ALL_COMMANDS is valid for every queue, but queue_supported_stages omits that aggregate bit.
            VkPipelineStageFlags2 src_stages =
                (readback_stages & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                                                      : readback_stages & allowed;
            if (!src_stages) src_stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            const bool copy_only =
                (src_stages & ~(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT)) == 0;
            const VkAccessFlags2 src_access =
                copy_only ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_MEMORY_WRITE_BIT;

            if (context_.vk.vkCmdPipelineBarrier2) {
                VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                host.srcStageMask = src_stages;
                host.srcAccessMask = src_access;
                host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &host;
                context_.vk.vkCmdPipelineBarrier2(command_buffer, &dep);
            } else {
                const VkPipelineStageFlags legacy_src =
                    copy_only ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                host.srcAccessMask = copy_only ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_MEMORY_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                context_.vk.vkCmdPipelineBarrier(command_buffer, legacy_src, VK_PIPELINE_STAGE_HOST_BIT,
                                                 0, 1, &host, 0, nullptr, 0, nullptr);
            }
        }
        const bool ended = context_.vk.vkEndCommandBuffer(command_buffer) == VK_SUCCESS;
        resource_lock.unlock();  // release resource_mutex_ before vkQueueSubmit while keeping tracker_lock held
        const bool wait_previous = ordered_timestamps && submitted_;
        if (wait_previous) {
            waits_.push_back(progress_);
            wait_values_.push_back(submitted_);
        }
        record_profile_.record("begin,bindless,uploads,commands", profile_shadows, profile_before_bindless,
                               profile_after_bindless, profile_before_commands);
        const auto profile_emit = FrameProfile::now();
        const bool submitted = ended && submit(command_buffer, VK_NULL_HANDLE, 0, replay.frame_draws, replay.attachment_writes);
        if (!submitted && (!storage_images.empty() || !zero_buffers.empty())) {
            // not initialized after all: try again with the next submission
            std::lock_guard lock(context_.storage_mutex_);
            context_.storage_pending_.insert(storage_images.begin(), storage_images.end());
            context_.zero_pending_.insert(context_.zero_pending_.end(), zero_buffers.begin(), zero_buffers.end());
        }
        if (submitted && (initialized_host_images || !storage_images.empty() || !zero_buffers.empty())) {
            std::lock_guard lock(context_.storage_mutex_);
            context_.storage_init_semaphore_ = progress_;
            context_.storage_init_value_ = submitted_;
            ++context_.storage_init_version_;
            storage_init_seen_ = context_.storage_init_version_;
        }
        if (submitted && bindless_uploaded) {
            std::lock_guard lock(context_.storage_mutex_);
            context_.bindless_upload_semaphore_ = progress_;
            context_.bindless_upload_value_ = submitted_;
            ++context_.bindless_upload_version_;
            bindless_upload_seen_ = context_.bindless_upload_version_;
        }
        if (wait_previous && !submitted && !waits_.empty()) {  // a failed submit keeps the waits for the next one
            waits_.pop_back();
            wait_values_.pop_back();
        }
        if (submitted) {
            context_.tracker_.commit_changes(std::move(next_tracker));
            for (const auto& upload : mutable_uploads) upload.owner->commit();
            slot->predicate = std::move(batch);
            slot->predicated_copy = std::move(copy_batch);
            slot->counted_dispatch = std::move(counted_batch);
            queries.commit();
            slot->segments = std::move(queries.retained);
            // Correlate recorded IR with this accepted native submission. These are command counts, not a claim
            // that predicated or indirect draws produced pixels.
            static std::atomic<unsigned> batch_traces{0};
            if (batch_traces.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::size_t total = 0, timestamps = 0, immediate = 0, host_acquires = 0;
                std::size_t clear_rt = 0, clear_ds = 0, clear_uav = 0, draws = 0, dispatches = 0, indirect = 0;
                for (const auto* list : lists) {
                    total += list->commands().size();
                    for (const auto& command : list->commands()) {
                        timestamps += std::holds_alternative<WriteTimestamp>(command);
                        immediate += std::holds_alternative<WriteImmediate>(command);
                        host_acquires += std::holds_alternative<HostMemoryAcquire>(command);
                        clear_rt += std::holds_alternative<ClearRenderTarget>(command);
                        clear_ds += std::holds_alternative<ClearDepthStencil>(command);
                        clear_uav += std::holds_alternative<ClearUnorderedAccess>(command);
                        draws += std::holds_alternative<Draw>(command);
                        dispatches += std::holds_alternative<Dispatch>(command);
                        indirect += std::holds_alternative<ExecuteIndirect>(command);
                    }
                }
                std::fprintf(stderr, "[rt] VulkanQueue: accepted batch IR family=%u progress=%p ticket=%llu "
                                     "lists=%zu total=%zu timestamp=%zu write_immediate=%zu host_acquire=%zu "
                                     "clear_rt=%zu clear_ds=%zu clear_uav=%zu draw=%zu dispatch=%zu indirect=%zu\n",
                    family_, reinterpret_cast<void*>(progress_), static_cast<unsigned long long>(submitted_),
                    lists.size(), total, timestamps, immediate, host_acquires, clear_rt, clear_ds, clear_uav,
                    draws, dispatches, indirect);
            }
        } else {
            discard();
        }
        if (depth24_guard.active) {
            context_.depth24_->retire(progress_, submitted ? submitted_ : 0);
            depth24_guard.active = false;
        }
        if (scratch_guard.active) {
            context_.scratch_->retire(progress_, submitted ? submitted_ : 0);
            scratch_guard.active = false;
        }
        // Under tracker_mutex_, so no other flush comes between this submit and the set's note of whether it reached
        // the queue (it keeps a single upload obligation).
        if (bindless_commit) bindless_commit(submitted);
        tracker_lock.unlock();
        submit_profile_.record("prepare,shadows,record,submit", profile_start, profile_prepared,
                               profile_shadows, profile_emit);
        return submitted ? ReplayResult::success : ReplayResult::unsupported;
    }

    void marker_now(DebugMarker::Kind kind, const char* label) {
        const DebugLabelFunctions& f = context_.labels_;
        if (!f.complete()) return;
        if (kind == DebugMarker::Kind::end && !open_labels_) return;
        VkDebugUtilsLabelEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        info.pLabelName = label ? label : "";
        std::lock_guard lock(context_.submit_mutex());
        if (kind == DebugMarker::Kind::set) {
            f.queue_insert(queue_, &info);
        } else if (kind == DebugMarker::Kind::begin) {
            f.queue_begin(queue_, &info);
            ++open_labels_;
        } else {
            f.queue_end(queue_);
            --open_labels_;
        }
    }

    // ---- Wait-before-Signal: operations held on the CPU (queue_holds.h) ----

    // One held operation, with the waits that were pending when it was asked for.
    struct Deferred {
        enum class Kind : std::uint8_t { batch, signal, marker };
        Kind kind = Kind::batch;
        std::uint64_t seq = 0;  // QueueHolds::next_seq(): the order operations were held in, across queues
        std::vector<VkSemaphore> waits;
        std::vector<std::uint64_t> wait_values;
        std::vector<CommandList> lists;  // batch: copies, so the application may Reset its lists after the call
        VkSemaphore fence = VK_NULL_HANDLE;  // signal
        std::uint64_t value = 0;
        DebugMarker::Kind marker_kind = DebugMarker::Kind::set;  // marker
        std::string label;
        // Alive until the operation has run: the fences it waits on and signals, the pipeline states and root
        // signatures of the lists (the application may release them right after the call).
        std::vector<std::shared_ptr<void>> pins;
        std::vector<std::shared_ptr<void>> wait_pins;  // the fences of `waits`
    };

    bool waits_pending(const std::vector<VkSemaphore>& semaphores, const std::vector<std::uint64_t>& values) const {
        for (std::size_t i = 0; i < semaphores.size(); ++i)
            if (context_.holds().pending(semaphores[i], values[i])) return true;
        return false;
    }

    // The next batch or Signal has to be held: something is held ahead of it (queue order), or a Wait it carries
    // has no submitted Signal yet. Called with state_mutex_ held.
    bool must_hold() const { return !held_.empty() || waits_pending(waits_, wait_values_); }

    // The waits are consumed: submitted as `ticket` (0: dropped on device loss). What the application may release
    // (the fences they named) is kept until that submission has completed on the GPU, as a D3D12 queue does:
    // destroying a semaphore a pending submission uses is a validation error. Retired pins are destroyed by the
    // caller after state_mutex_ is released (drain_pins): the last release of a fence waits for a running release,
    // which must not be waiting for this lock.
    void clear_waits(std::uint64_t ticket = 0) {
        waits_.clear();
        wait_values_.clear();
        retire_pins(wait_pins_, ticket);
    }

    void retire_pins(std::vector<std::shared_ptr<void>>& pins, std::uint64_t ticket) {
        for (auto& pin : pins) {
            if (ticket) pin_queue_.push_back({ticket, std::move(pin)});
            else retired_pins_.push_back(std::move(pin));
        }
        pins.clear();
    }

    // Moves the pins whose submission has completed (and the ones retired outright) to `out`. state_mutex_ held.
    void drain_pins(std::vector<std::shared_ptr<void>>& out) {
        out.swap(retired_pins_);
        if (pin_queue_.empty()) return;
        const std::uint64_t done = completed();
        while (!pin_queue_.empty() && pin_queue_.front().ticket <= done) {
            out.push_back(std::move(pin_queue_.front().pin));
            pin_queue_.pop_front();
        }
    }

    // Appends `op`, which takes the pending waits with it (a marker has none). Called with state_mutex_ held.
    void hold(Deferred&& op) {
        op.seq = context_.holds().next_seq();
        if (op.kind != Deferred::Kind::marker) {
            op.waits = std::move(waits_);
            op.wait_values = std::move(wait_values_);
            op.wait_pins = std::move(wait_pins_);
            waits_.clear();
            wait_values_.clear();
            wait_pins_.clear();
        }
        held_.push_back(std::move(op));
        context_.holds().held_changed(1);
    }

    // Runs one held operation as the call would have, with its own waits and none of the ones queued since.
    // A submission that fails leaves its waits for the next one, as when the call is not held (a transient
    // vkQueueSubmit failure is retried by the application's next call), ahead of the waits asked for since.
    void run(Deferred& op) {
        auto later_waits = std::move(waits_);
        auto later_values = std::move(wait_values_);
        auto later_pins = std::move(wait_pins_);
        waits_ = std::move(op.waits);
        wait_values_ = std::move(op.wait_values);
        wait_pins_ = std::move(op.wait_pins);  // the fences stay alive until the submission has consumed their waits
        switch (op.kind) {
        case Deferred::Kind::batch: {
            std::vector<const CommandList*> lists;
            for (const CommandList& list : op.lists) lists.push_back(&list);
            // The pipeline states and root signatures of the lists are in op.pins (see execute()): they live until
            // the batch has been recorded and then, with the fences, until its submission has completed (below).
            const ReplayResult result = execute_guest_batch(lists, op.pins);
            if (result != ReplayResult::success) {
                int none = static_cast<int>(ReplayResult::success);
                held_failure_.compare_exchange_strong(none, static_cast<int>(result));
                std::fprintf(stderr, "[rt] d3d12: a held ExecuteCommandLists failed when it was released (result %d)\n",
                             static_cast<int>(result));
            }
            break;
        }
        case Deferred::Kind::signal:
            if (!submit(VK_NULL_HANDLE, op.fence, op.value))
                std::fprintf(stderr, "[rt] d3d12: a held Signal failed when it was released\n");
            break;
        case Deferred::Kind::marker:
            marker_now(op.marker_kind, op.label.c_str());
            break;
        }
        retire_pins(op.pins, submitted_);  // the fence it signals, the pipeline states: until its submission is done
        waits_.insert(waits_.end(), later_waits.begin(), later_waits.end());
        wait_values_.insert(wait_values_.end(), later_values.begin(), later_values.end());
        for (auto& pin : later_pins) wait_pins_.push_back(std::move(pin));
    }

    bool ready(const Deferred& op) const { return !waits_pending(op.waits, op.wait_values); }

    // Holder: called by the thread whose Signal made a held operation ready (never with a queue lock held).
    bool release_ready() override {
        std::vector<std::shared_ptr<void>> retired;
        bool released = false;
        {
            std::lock_guard state(state_mutex_);
            while (!held_.empty() && (context_.is_device_lost() || ready(held_.front()))) {
                Deferred op = std::move(held_.front());
                held_.pop_front();
                context_.holds().held_changed(-1);
                if (!context_.is_device_lost()) run(op);
                released = true;
            }
            drain_pins(retired);
        }
        return released;
    }

    // Holder: the fence is released, so what waits on it can never run.
    void discard_waiting_on(VkSemaphore semaphore) override {
        std::vector<Deferred> dropped;
        {
            std::lock_guard state(state_mutex_);
            for (auto it = held_.begin(); it != held_.end();) {
                if (std::find(it->waits.begin(), it->waits.end(), semaphore) == it->waits.end() &&
                    !(it->kind == Deferred::Kind::signal && it->fence == semaphore)) {
                    ++it;
                    continue;
                }
                std::fprintf(stderr, "[rt] d3d12: a fence was released while a queue still held work that names it; "
                                     "that work is dropped\n");
                dropped.push_back(std::move(*it));
                it = held_.erase(it);
                context_.holds().held_changed(-1);
            }
        }
    }

    // Holder: the sequence number of the oldest held operation.
    std::uint64_t oldest_held_seq() override {
        std::lock_guard state(state_mutex_);
        return held_.empty() ? 0 : held_.front().seq;
    }

    // A queue destroyed with held work. Reported, not waited for: nothing was submitted for it, so the VkQueue is
    // idle and the destructor goes on. The fences of held Signals will never reach their values, so what is waiting
    // for them on the CPU (SetEventOnCompletion, as a released fence completes them) is woken, and the batches
    // that wait for them on other queues stay held (and are reported when those queues are destroyed).
    void report_held_at_teardown() {
        std::deque<Deferred> dropped;
        {
            std::lock_guard state(state_mutex_);
            if (held_.empty()) return;
            std::uint64_t value = 0;
            for (const Deferred& op : held_)
                for (std::size_t i = 0; i < op.waits.size() && !value; ++i)
                    if (context_.holds().pending(op.waits[i], op.wait_values[i])) value = op.wait_values[i];
            std::size_t signals = 0;
            for (const Deferred& op : held_) signals += op.kind == Deferred::Kind::signal;
            std::fprintf(stderr, "[rt] d3d12: queue destroyed with %zu held operation(s) never submitted: waits for a "
                                 "fence value (%llu) that no Signal reached; the fence was never signaled%s\n",
                         held_.size(), static_cast<unsigned long long>(value),
                         signals ? "; CPU waits on the fences of its held Signals are completed" : "");
            for (const Deferred& op : held_)
                if (op.kind == Deferred::Kind::signal) context_.cancel_fence_waits(op.fence);
            context_.holds().held_changed(-static_cast<std::int64_t>(held_.size()));
            dropped.swap(held_);
        }
        // The pins (fences, pipeline states) are released here, outside the lock.
    }

    struct CommandSlot {
        VkCommandBuffer buffer;
        std::uint64_t ticket;
        std::vector<std::shared_ptr<const OcclusionSegmentPool>> segments;
        std::vector<VkEvent> events;  // order_timestamp_copy's, reset and reused once the slot completes
        std::unique_ptr<PredicatedBufferCopier::Batch> predicated_copy;
        std::unique_ptr<IndirectDispatchCounter::Batch> counted_dispatch;
        std::unique_ptr<PredicateReducer::Batch> predicate; // retained until this slot completes
        std::vector<KhrInstanceStage> khr_instances;  // KHR ARRAY_OF_POINTERS staging, same lifetime
        std::vector<KhrAsQueryPool> khr_as_queries;   // KHR postbuild COMPACTED_SIZE query pools, same lifetime
        std::vector<VkQueryPool> timestamp_queries;  // fresh native marker queries, retained until this slot completes
    };

    struct PendingBindings {
        std::uint64_t ticket = 0;
        // Vulkan needs the resource until the bind completes and the previous allocation until it is unmapped.
        std::vector<std::shared_ptr<const TileHeapAllocation>> old_heaps;
        std::shared_ptr<SparseResource> resource;
    };

    static bool valid_tiles(const SparseResource& resource, TileRegion region) {
        return region.count && region.first < resource.tile_count && region.count <= resource.tile_count - region.first;
    }

    bool initialize_sparse_image_layout(ResourceId id) {
        std::lock_guard lock(context_.tracker_mutex_);
        if (context_.tracker_.layout(id, 0) != VK_IMAGE_LAYOUT_UNDEFINED) return true;
        auto* slot = acquire();
        if (!slot) return false;
        const VkCommandBuffer command_buffer = slot->buffer;
        auto& vk = context_.vk;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.vkBeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
            slot->ticket = 0;
            return false;
        }
        // Transition while the image is still unbound. Doing this after binding could discard another image's
        // contents in an aliased page. Subsequent uses keep the image in GENERAL.
        constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        detail::image_barrier(context_.replay_, command_buffer, context_.resources_.at(id), range,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        if (vk.vkEndCommandBuffer(command_buffer) != VK_SUCCESS || !submit(command_buffer, VK_NULL_HANDLE, 0)) {
            slot->ticket = 0;
            return false;
        }
        context_.tracker_.set(id, range, VK_IMAGE_LAYOUT_GENERAL);
        return true;
    }

    bool bind_tiles(ResourceId id, const std::shared_ptr<SparseResource>& resource,
                    std::map<std::uint64_t, TileMapping>& changes) {
        auto& vk = context_.vk;
        if (!vk.vkQueueBindSparse || context_.is_device_lost()) return false;
        PendingBindings pending{0, {}, resource};
        std::vector<VkSparseMemoryBind> binds;
        std::vector<VkSparseImageMemoryBind> image_binds;
        for (const auto& [index, page] : changes) {
            const auto old = resource->mapped.find(index);
            if (old != resource->mapped.end()) {
                if (old->second == page) continue;
                pending.old_heaps.push_back(old->second.heap);
            } else if (!page.heap) {
                continue;
            }
            if (page.heap && !(resource->memory_types & (1u << page.heap->memory_type))) return false;
            const VkDeviceMemory memory = page.heap ? page.heap->memory : VK_NULL_HANDLE;
            if (resource->image) {
                constexpr auto shape = element_tile_shape(false, 4);
                const auto extent = resource->image_extent;
                const auto columns = 1 + (extent.width - 1) / shape.width;
                const auto x = std::uint32_t(index % columns) * shape.width;
                const auto y = std::uint32_t(index / columns) * shape.height;
                image_binds.push_back({
                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}, {std::int32_t(x), std::int32_t(y), 0},
                    {std::min(shape.width, extent.width - x), std::min(shape.height, extent.height - y), 1},
                    memory, page.offset, 0});
                continue;
            }
            const VkDeviceSize resource_offset = index * tile_size_in_bytes;
            if (!binds.empty()) {
                auto& previous = binds.back();
                if (previous.resourceOffset + previous.size == resource_offset && previous.memory == memory &&
                    (!memory || previous.memoryOffset + previous.size == page.offset)) {
                    previous.size += tile_size_in_bytes;
                    continue;
                }
            }
            binds.push_back({resource_offset, tile_size_in_bytes, memory, page.offset, 0});
        }
        if (binds.empty() && image_binds.empty()) return true;
        if (!image_binds.empty() && !initialize_sparse_image_layout(id)) return false;
        const auto ticket = submitted_ + 1;
        pending.ticket = ticket;
        collect_bindings(completed());
        auto semaphores = waits_;
        auto values = wait_values_;
        // Sparse binds are not ordered against work on the same Vulkan queue by submission order alone.
        if (submitted_) {
            semaphores.push_back(progress_);
            values.push_back(submitted_);
        }
        VkTimelineSemaphoreSubmitInfo tickets{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tickets.waitSemaphoreValueCount = std::uint32_t(values.size());
        tickets.pWaitSemaphoreValues = values.data();
        tickets.signalSemaphoreValueCount = 1;
        tickets.pSignalSemaphoreValues = &ticket;
        const VkSparseBufferMemoryBindInfo buffer_bind{resource->buffer, std::uint32_t(binds.size()), binds.data()};
        const VkSparseImageMemoryBindInfo image_bind{
            resource->image, std::uint32_t(image_binds.size()), image_binds.data()};
        VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, &tickets};
        info.waitSemaphoreCount = std::uint32_t(semaphores.size());
        info.pWaitSemaphores = semaphores.data();
        if (!binds.empty()) {
            info.bufferBindCount = 1;
            info.pBufferBinds = &buffer_bind;
        }
        if (!image_binds.empty()) {
            info.imageBindCount = 1;
            info.pImageBinds = &image_bind;
        }
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &progress_;
        pending_bindings_.push_back(std::move(pending));
        {
            std::lock_guard lock(context_.submit_mutex());
            const auto result = vk.vkQueueBindSparse(queue_, 1, &info, VK_NULL_HANDLE);
            if (result != VK_SUCCESS) {
                if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
                pending_bindings_.pop_back();
                return false;
            }
            context_.record_submit_locked(progress_, ticket);
        }
        submitted_ = last_sparse_ = ticket;
        clear_waits(ticket);
        for (auto it = changes.begin(); it != changes.end();) {
            resource->mapped.erase(it->first);
            if (it->second.heap) ++it;
            else it = changes.erase(it);
        }
        resource->mapped.merge(changes);
        return true;
    }

    std::uint64_t completed() const {
        if (context_.is_device_lost()) return 0;
        std::uint64_t done = 0;
        const VkResult result = context_.vk.vkGetSemaphoreCounterValue(context_.device(), progress_, &done);
        if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
        if (result == VK_SUCCESS && done) {
            static std::atomic<unsigned> observations{0};
            if (observations.fetch_add(1, std::memory_order_relaxed) < 32)
                std::fprintf(stderr, "[rt] VulkanQueue: native completion observed family=%u queue=%p "
                                     "progress=%p done=%llu\n",
                    family_, reinterpret_cast<void*>(queue_), reinterpret_cast<void*>(progress_),
                    static_cast<unsigned long long>(done));
        }
        // A failed timeline query cannot be used as a resource-retirement or command-buffer reuse ticket.
        return result == VK_SUCCESS ? done : 0;
    }

    void collect_bindings(std::uint64_t done) {
        while (!pending_bindings_.empty() && pending_bindings_.front().ticket <= done)
            pending_bindings_.pop_front();
        if (last_sparse_ <= done) last_sparse_ = 0;
    }

    // Releases what every finished command buffer holds; the first of them, or null when none has finished.
    CommandSlot* recycle_finished() {
        const auto done = completed();
        if (context_.is_device_lost()) return nullptr;
        collect_bindings(done);
        CommandSlot* reuse = nullptr;
        for (auto& slot : buffers_) {
            if (slot.ticket > done) continue;
            slot.segments.clear();
            recycle_khr_stages(slot.khr_instances);
            recycle_khr_as_queries(slot.khr_as_queries);
            destroy_timestamp_queries(slot.timestamp_queries);
            for (VkEvent event : slot.events)
                if (context_.vk.vkResetEvent(context_.device(), event) == VK_SUCCESS) free_events_.push_back(event);
                else context_.vk.vkDestroyEvent(context_.device(), event, nullptr);
            slot.events.clear();
            if (!reuse) reuse = &slot;
        }
        return reuse;
    }

    // A queue at the cap with nothing finished waits here for its oldest submission. execute_now() calls it before it
    // takes resource_mutex_ or tracker_mutex_: the submission waited for may itself wait on another queue's work, and
    // that queue needs those locks to submit it. One execute runs at a time on a queue, so buffers_ is this caller's.
    // A timeout only means the GPU is slow: wait again rather than allocate past the cap. False when the device is lost
    // (now or while waiting), so the caller submits nothing.
    bool wait_for_room() {
        for (;;) {
            if (context_.is_device_lost()) return false;
            if (buffers_.size() < kMaxQueuedCommandBuffers) return true;
            const auto done = completed();
            for (const auto& slot : buffers_)
                if (slot.ticket <= done) return true;
            switch (wait_oldest_submitted()) {
            case VK_SUCCESS:
                return true;
            case VK_TIMEOUT:
                continue;
            case VK_ERROR_DEVICE_LOST:
                context_.mark_device_lost();
                return false;
            default:
                return true;  // nothing submitted to wait for (or a host error): no wait can make room, allocate
            }
        }
    }

    // Waits up to kQueuedWaitNs for the oldest submitted command buffer. VK_INCOMPLETE when none is submitted yet.
    VkResult wait_oldest_submitted() {
        std::uint64_t oldest = UINT64_MAX;
        for (const auto& slot : buffers_)
            if (slot.ticket < oldest) oldest = slot.ticket;
        if (oldest == UINT64_MAX) return VK_INCOMPLETE;  // all still being recorded
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &progress_;
        info.pValues = &oldest;
        return context_.vk.vkWaitSemaphores(context_.device(), &info, kQueuedWaitNs);
    }

    CommandSlot* acquire() {
        if (context_.is_device_lost()) return nullptr;
        context_.drain_deleted_resources(context_.completed_retire_ticket());
        CommandSlot* reuse = recycle_finished();
        if (reuse) {
            if (context_.vk.vkResetCommandBuffer(reuse->buffer, 0) != VK_SUCCESS) return nullptr;
            reuse->predicate.reset();
            reuse->predicated_copy.reset();
            reuse->counted_dispatch.reset();
            reuse->ticket = UINT64_MAX;  // in use until submitted
            return reuse;
        }
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = pool_;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        if (context_.vk.vkAllocateCommandBuffers(context_.device(), &info, &command_buffer) != VK_SUCCESS)
            return nullptr;
        buffers_.push_back({command_buffer, UINT64_MAX});
        return &buffers_.back();
    }

    // An unsignaled event for order_timestamp_copy, or null. MoltenVK decides when the command buffer is submitted
    // whether a set or wait does anything, from the event's state then, so each use gets an event of its own.
    VkEvent take_event() {
        if (!free_events_.empty()) {
            const VkEvent event = free_events_.back();
            free_events_.pop_back();
            return event;
        }
        VkEventCreateInfo info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VkEvent event = VK_NULL_HANDLE;
        if (context_.vk.vkCreateEvent(context_.device(), &info, nullptr, &event) != VK_SUCCESS) return VK_NULL_HANDLE;
        return event;
    }

    bool submit(VkCommandBuffer command_buffer, VkSemaphore fence, std::uint64_t value,
                const FrameDrawTargets* frame_draws = nullptr, const AttachmentWriteBatch* attachment_writes = nullptr) {
        if (context_.is_device_lost()) return false;
        const std::uint64_t ticket = submitted_ + 1;
        if (last_sparse_) {
            collect_bindings(completed());
            if (context_.is_device_lost()) return false;
        }
        std::span<const VkSemaphore> semaphores = waits_;
        std::span<const std::uint64_t> values = wait_values_;
        std::vector<VkSemaphore> sparse_waits;
        std::vector<std::uint64_t> sparse_values;
        if (last_sparse_) {
            sparse_waits = waits_;
            sparse_values = wait_values_;
            sparse_waits.push_back(progress_);
            sparse_values.push_back(last_sparse_);
            semaphores = sparse_waits;
            values = sparse_values;
        }
        const std::vector<VkPipelineStageFlags> stages(semaphores.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkResult res = VK_SUCCESS;
        {
            std::lock_guard lock(context_.submit_mutex());
            VkSemaphore signals[2]{progress_, VK_NULL_HANDLE};
            std::uint64_t signal_values[2]{ticket, 0};
            std::uint32_t signal_count = 1;
            // Decided under the submit lock, so the order of the checks is the order of the submissions. A Signal
            // that is not above what was already submitted for this fence (a held Signal(F, 2) released after an
            // unheld Signal(F, 3)) leaves the Vulkan signal out: the timeline cannot go down, and waits for 2 pass.
            // The value counts as submitted only after the submit succeeded (below).
            const bool signals_fence = fence && context_.holds().would_signal(fence, value);
            if (signals_fence) {
                signals[signal_count] = fence;
                signal_values[signal_count] = value;
                ++signal_count;
            }
            VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timeline.waitSemaphoreValueCount = std::uint32_t(values.size());
            timeline.pWaitSemaphoreValues = values.data();
            timeline.signalSemaphoreValueCount = signal_count;
            timeline.pSignalSemaphoreValues = signal_values;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
            submit.waitSemaphoreCount = std::uint32_t(semaphores.size());
            submit.pWaitSemaphores = semaphores.data();
            submit.pWaitDstStageMask = stages.data();
            submit.commandBufferCount = command_buffer ? 1 : 0;
            submit.pCommandBuffers = &command_buffer;
            submit.signalSemaphoreCount = signal_count;
            submit.pSignalSemaphores = signals;
            res = context_.vk.vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
            if (res == VK_SUCCESS) {
                if (frame_draws)
                    context_.frame_draw_activity().submit(reinterpret_cast<std::uintptr_t>(queue_), *frame_draws);
                static std::atomic<unsigned> submissions{0};
                if (submissions.fetch_add(1, std::memory_order_relaxed) < 32)
                    std::fprintf(stderr, "[rt] VulkanQueue: native submit accepted family=%u queue=%p "
                                         "progress=%p ticket=%llu command_buffer=%p command_buffers=%u\n",
                        family_, reinterpret_cast<void*>(queue_), reinterpret_cast<void*>(progress_),
                        static_cast<unsigned long long>(ticket), reinterpret_cast<void*>(command_buffer),
                        submit.commandBufferCount);
                context_.record_submit_locked(progress_, ticket);
                if (attachment_writes) attachment_writes->submit(context_.submitted_retire_ticket());
                if (signals_fence) context_.holds().signaled(fence, value);
            }
        }
        if (res == VK_ERROR_DEVICE_LOST) {
            context_.mark_device_lost();
            clear_waits();
            return false;
        }
        const bool ok = (res == VK_SUCCESS);
        if (ok) {
            submitted_ = ticket;
            clear_waits(ticket);
            for (auto& slot : buffers_)
                if (slot.buffer == command_buffer) slot.ticket = ticket;
        }
        return ok;
    }

    VulkanContext& context_;
    VkQueue queue_;
    QueueKind kind_ = QueueKind::direct;
    std::uint32_t family_;
    bool guest_queue_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSemaphore progress_ = VK_NULL_HANDLE;  // our own timeline: which command buffers are done
    std::uint64_t submitted_ = 0;
    std::vector<CommandSlot> buffers_;
    std::vector<VkEvent> free_events_;  // unsignaled, for order_timestamp_copy
    std::unique_ptr<OcclusionResolve> occlusion_resolve_;
    std::uint64_t storage_init_seen_ = 0;  // the storage_init_version_ this queue has waited for
    std::uint64_t bindless_upload_seen_ = 0;  // the bindless_upload_version_ this queue has waited for
    std::uint64_t last_sparse_ = 0;
    std::deque<PendingBindings> pending_bindings_;
    std::vector<VkSemaphore> waits_;
    std::vector<std::uint64_t> wait_values_;
    // execute's working storage. Per queue like buffers_: one execute at a time on a queue, as acquire() and
    // submit() already assume; other queues prepare concurrently with their own.
    std::vector<PreparedList> prepared_;
    std::uint32_t open_labels_ = 0;  // queue labels begun and not ended
    bool ok_ = false;
    // Wait-before-Signal. state_mutex_ is outermost (before resource_mutex_ -> tracker_mutex_ -> submit_mutex_) and
    // guards the members above that execute, signal, wait and marker touch.
    std::mutex state_mutex_;
    std::deque<Deferred> held_;
    std::vector<std::shared_ptr<void>> wait_pins_;     // keep the fences of waits_ alive
    std::vector<std::shared_ptr<void>> retired_pins_;  // released by the caller once state_mutex_ is dropped
    struct TicketedPin {
        std::uint64_t ticket;
        std::shared_ptr<void> pin;
    };
    std::deque<TicketedPin> pin_queue_;  // released once progress_ reaches the ticket
    std::atomic<int> held_failure_{0};                 // a ReplayResult: the first held batch that failed
};

} // namespace leonida
