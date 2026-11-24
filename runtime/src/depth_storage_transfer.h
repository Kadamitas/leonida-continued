// GPU copies between the two representations of a single-plane use of a D32S8 resource. The caller owns all
// objects, queue ownership/synchronization, format capability checks, and lifetime through GPU retirement.
// Native image, shadow image and transfer buffer must have non-overlapping, fully bound native backing.
// This records outside a render pass/dynamic rendering instance. It does not update a resource's authority tracker.
//
// Primary rules: Vulkan Copy Commands, "Depth/Stencil Aspect Copy" (D32S8 depth is D32_SFLOAT, stencil is S8_UINT),
// https://docs.vulkan.org/spec/latest/chapters/copies.html; VkImageMemoryBarrier image-03319 requires enabled separate
// depth/stencil layouts for the one-aspect barriers below. BufferToImage pRegions-07931 restricts depth to [0,1]
// unless VK_EXT_depth_range_unrestricted is enabled. No other implementation's transfer code is used here.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <limits>

namespace leonida::depth_storage_transfer {

enum class Plane { depth, stencil };
enum class Direction { native_to_shadow, shadow_to_native };
enum class Result { unsupported, recorded };

struct Functions {
    PFN_vkCmdPipelineBarrier pipeline_barrier = nullptr;
    PFN_vkCmdCopyImageToBuffer copy_image_to_buffer = nullptr;
    PFN_vkCmdCopyBufferToImage copy_buffer_to_image = nullptr;
};

struct Capabilities {
    VkQueueFlags queue_flags = 0;
    bool separate_depth_stencil_layouts = false;
    bool depth_range_unrestricted = false;  // extension actually enabled on the recording device
};

// State is for the selected aspect only. An initialized source means real commands supplied its content;
// a declared initial D3D layout or an optimized clear value alone does not initialize it.
struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized = false;
};

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkExtent3D extent{};
    std::uint32_t mip_levels = 0;
    std::uint32_t array_layers = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    ImageState state;
};

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
};

struct Request {
    Plane plane = Plane::depth;
    Direction direction = Direction::native_to_shadow;
    Image native;
    Image shadow;
    Buffer transfer;
    VkDeviceSize transfer_offset = 0;
    // Only full mip zero/layer zero is implemented; explicit nonzero subresources refuse before recording.
    std::uint32_t mip = 0;
    std::uint32_t layer = 0;
    VkImageLayout native_after = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout shadow_after = VK_IMAGE_LAYOUT_UNDEFINED;
    bool outside_rendering = false;
    // A content invariant established by the caller, not an instruction to clamp/rewrite arbitrary UAV bits.
    bool depth_values_in_unit_range = false;
};

struct Outcome {
    Result result = Result::unsupported;
    const char* reason = nullptr;
    ImageState native;
    ImageState shadow;
    VkDeviceSize transfer_bytes = 0;
};

namespace detail {

inline bool exact_image(const Image& image) {
    constexpr VkImageCreateFlags sparse = VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
        VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    return image.handle && image.type == VK_IMAGE_TYPE_2D && image.extent.width && image.extent.height &&
        image.extent.depth == 1 && image.mip_levels == 1 && image.array_layers == 1 &&
        image.samples == VK_SAMPLE_COUNT_1_BIT && !(image.flags & (sparse | VK_IMAGE_CREATE_PROTECTED_BIT));
}

inline bool layout(const Image& image, VkImageLayout value, bool native, Plane plane, bool undefined) {
    switch (value) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return undefined;
    case VK_IMAGE_LAYOUT_GENERAL: return true;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return image.usage & VK_IMAGE_USAGE_SAMPLED_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        return native && (image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        return native && plane == Plane::depth && (image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
        return native && plane == Plane::stencil && (image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    default: return false;
    }
}

inline VkImageMemoryBarrier image_barrier(const Image& image, VkImageAspectFlags aspect,
                                         VkImageLayout before, VkImageLayout after,
                                         VkAccessFlags source, VkAccessFlags destination) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.handle;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    return barrier;
}

inline VkBufferMemoryBarrier buffer_barrier(const Request& request, VkDeviceSize bytes,
                                            VkAccessFlags source, VkAccessFlags destination) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = request.transfer.handle;
    barrier.offset = request.transfer_offset;
    barrier.size = bytes;
    return barrier;
}

}  // namespace detail

// All validation precedes the first Vulkan command. On refusal, no commands or state updates are made.
// On recorded, returned layouts describe the commands just recorded, not GPU completion. The other native
// aspect is neither copied nor transitioned. The transfer buffer ends behind a transfer-read/write -> all-memory
// dependency; its owner must still serialize subsequent users, including users on other queues.
inline Outcome record(const Functions& vk, const Capabilities& capabilities, VkCommandBuffer command_buffer,
                      const Request& request) {
    Outcome out{Result::unsupported, nullptr, request.native.state, request.shadow.state, 0};
    const auto refuse = [&](const char* reason) { out.reason = reason; return out; };
    if (!command_buffer || !vk.pipeline_barrier || !vk.copy_image_to_buffer || !vk.copy_buffer_to_image)
        return refuse("transfer functions unavailable");
    if (!request.outside_rendering || !(capabilities.queue_flags & VK_QUEUE_GRAPHICS_BIT) ||
        !capabilities.separate_depth_stencil_layouts)
        return refuse("graphics queue, separate depth/stencil layouts and outside-rendering recording required");
    if ((request.plane != Plane::depth && request.plane != Plane::stencil) ||
        (request.direction != Direction::native_to_shadow && request.direction != Direction::shadow_to_native))
        return refuse("unknown plane/direction");
    const bool depth = request.plane == Plane::depth;
    const bool to_shadow = request.direction == Direction::native_to_shadow;
    if (!detail::exact_image(request.native) || !detail::exact_image(request.shadow) || request.mip || request.layer ||
        request.native.handle == request.shadow.handle || request.native.format != VK_FORMAT_D32_SFLOAT_S8_UINT ||
        request.native.extent.width != request.shadow.extent.width ||
        request.native.extent.height != request.shadow.extent.height ||
        (depth ? request.shadow.format != VK_FORMAT_R32_UINT && request.shadow.format != VK_FORMAT_R32_SFLOAT :
                 request.shadow.format != VK_FORMAT_R8_UINT))
        return refuse("only matching single-mip/layer/sample 2D D32S8 and R32/R8 planes are supported");
    constexpr VkImageUsageFlags image_copies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkBufferUsageFlags buffer_copies = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((request.native.usage & (image_copies | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) !=
            (image_copies | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        (request.shadow.usage & (image_copies | VK_IMAGE_USAGE_STORAGE_BIT)) !=
            (image_copies | VK_IMAGE_USAGE_STORAGE_BIT) || !request.transfer.handle ||
        (request.transfer.usage & buffer_copies) != buffer_copies || request.transfer_offset % 4)
        return refuse("required image/buffer usages or transfer alignment missing");
    const Image& source = to_shadow ? request.native : request.shadow;
    if (!source.state.initialized || source.state.layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return refuse("source content is uninitialized");
    if (!detail::layout(request.native, request.native.state.layout, true, request.plane, !to_shadow) ||
        !detail::layout(request.shadow, request.shadow.state.layout, false, request.plane, to_shadow) ||
        !detail::layout(request.native, request.native_after, true, request.plane, false) ||
        !detail::layout(request.shadow, request.shadow_after, false, request.plane, false))
        return refuse("unsupported or unspecified image layout");
    if (depth && !to_shadow && !capabilities.depth_range_unrestricted && !request.depth_values_in_unit_range)
        return refuse("depth copy requires unrestricted range or a proven [0,1] content invariant");
    const VkDeviceSize pixels = VkDeviceSize(request.native.extent.width) * request.native.extent.height;
    const VkDeviceSize element_bytes = depth ? 4 : 1;
    if (pixels > std::numeric_limits<VkDeviceSize>::max() / element_bytes)
        return refuse("plane extent overflow");
    const VkDeviceSize bytes = pixels * element_bytes;
    if (request.transfer_offset > request.transfer.size || bytes > request.transfer.size - request.transfer_offset)
        return refuse("transfer buffer is too small");
    constexpr VkAccessFlags any_memory = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const VkImageAspectFlags native_aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkImageLayout native_copy = to_shadow ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageLayout shadow_copy = to_shadow ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const std::array before{
        detail::image_barrier(request.native, native_aspect, request.native.state.layout, native_copy, any_memory,
            to_shadow ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT),
        detail::image_barrier(request.shadow, VK_IMAGE_ASPECT_COLOR_BIT, request.shadow.state.layout, shadow_copy, any_memory,
            to_shadow ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT)};
    auto transfer_barrier = detail::buffer_barrier(request, bytes, any_memory, VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 1, &transfer_barrier, std::uint32_t(before.size()), before.data());
    VkBufferImageCopy region{};
    region.bufferOffset = request.transfer_offset;
    region.imageSubresource = {to_shadow ? native_aspect : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = request.native.extent;
    vk.copy_image_to_buffer(command_buffer, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        request.transfer.handle, 1, &region);
    transfer_barrier = detail::buffer_barrier(request, bytes, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vk.pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 1, &transfer_barrier, 0, nullptr);
    region.imageSubresource.aspectMask = to_shadow ? VK_IMAGE_ASPECT_COLOR_BIT : native_aspect;
    vk.copy_buffer_to_image(command_buffer, request.transfer.handle,
        to_shadow ? request.shadow.handle : request.native.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    const std::array after{
        detail::image_barrier(request.native, native_aspect, native_copy, request.native_after,
            to_shadow ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, any_memory),
        detail::image_barrier(request.shadow, VK_IMAGE_ASPECT_COLOR_BIT, shadow_copy, request.shadow_after,
            to_shadow ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT, any_memory)};
    transfer_barrier = detail::buffer_barrier(request, bytes,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, any_memory);
    vk.pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
        0, nullptr, 1, &transfer_barrier, std::uint32_t(after.size()), after.data());
    out.result = Result::recorded;
    out.native = {request.native_after, request.native.state.initialized || !to_shadow};
    out.shadow = {request.shadow_after, request.shadow.state.initialized || to_shadow};
    out.transfer_bytes = bytes;
    return out;
}

}  // namespace leonida::depth_storage_transfer
