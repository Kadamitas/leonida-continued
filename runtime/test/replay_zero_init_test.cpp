// CPU storage/coverage oracle for zero-initialization transfer scratch. No Vulkan loader, GPU or game data.
// The mock enforces Vulkan fill alignment/bounds, writes actual zero bytes, then verifies every image block is
// covered by buffer-to-image copies reading those bytes. Padding outside the allocation stays a canary.
#include "../src/vulkan_replay.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
using namespace leonida;
unsigned failures = 0, cases = 0;
void expect(bool ok, const char* label) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
constexpr std::size_t guard = 16;
struct Probe {
    ResourceBinding image{};
    VkDeviceSize offset = 0, allocated = 0;
    std::vector<unsigned char> bytes;
    std::vector<std::vector<bool>> covered;
    bool fail_all = false, fail_first = false, null_buffer = false, zero_page = false;
    unsigned allocations = 0, fills = 0, copies = 0, barriers = 0;
    BlockFormat format{};
    Probe(VkFormat vk_format, unsigned width, unsigned height, VkDeviceSize base,
          unsigned mips = 1, unsigned layers = 1) : offset(base) {
        image.image = reinterpret_cast<VkImage>(std::uintptr_t{1});
        image.format = vk_format; image.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        image.width = width; image.height = height; image.depth = 1;
        image.mip_levels = mips; image.array_layers = layers; image.samples = 1;
        format = block_format(vk_format, VK_IMAGE_ASPECT_COLOR_BIT);
        for (unsigned mip = 0; mip < mips; ++mip) {
            const auto w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
            const auto blocks = ((w + format.block - 1) / format.block) * ((h + format.block - 1) / format.block);
            for (unsigned layer = 0; layer < layers; ++layer) covered.emplace_back(blocks, false);
        }
    }
    bool allocate(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& base) {
        ++allocations;
        if (fail_all || (fail_first && allocations == 1)) return false;
        allocated = size;
        bytes.assign(std::size_t(size) + 2 * guard, 0xa5);
        base = offset;
        buffer = null_buffer ? VK_NULL_HANDLE : reinterpret_cast<VkBuffer>(std::uintptr_t{2});
        return true;
    }
    bool bounds(VkDeviceSize begin, VkDeviceSize count) const {
        return begin >= offset && begin - offset <= allocated && count <= allocated - (begin - offset);
    }
    void fill(VkBuffer buffer, VkDeviceSize begin, VkDeviceSize count, std::uint32_t value) {
        ++fills;
        expect(buffer != VK_NULL_HANDLE, "fill uses an actual scratch buffer");
        expect(begin % 4 == 0, "fill destination offset is a multiple of four");
        expect(count && count % 4 == 0 && count != VK_WHOLE_SIZE, "fill size is a positive finite multiple of four");
        expect(value == 0, "fill pattern is zero");
        expect(bounds(begin, count), "fill remains inside the exact allocation");
        if (bounds(begin, count)) std::fill_n(bytes.begin() + guard + std::size_t(begin - offset),
                                             std::size_t(count), static_cast<unsigned char>(0));
    }
    void copy(VkBuffer buffer, std::uint32_t count, const VkBufferImageCopy* regions) {
        ++copies;
        expect(buffer != VK_NULL_HANDLE, "copy uses an actual zero buffer");
        for (unsigned i = 0; i < count; ++i) {
            const auto& r = regions[i];
            expect(r.bufferOffset % format.bytes == 0 && r.bufferOffset % 4 == 0,
                   "copy source meets texel and transfer-queue alignment");
            const auto bx = (r.imageExtent.width + format.block - 1) / format.block;
            const auto by = (r.imageExtent.height + format.block - 1) / format.block;
            const VkDeviceSize span = VkDeviceSize(bx) * by * r.imageExtent.depth * r.imageSubresource.layerCount * format.bytes;
            expect(bounds(r.bufferOffset, span), "copy reads only allocated zero bytes");
            if (bounds(r.bufferOffset, span)) {
                const auto first = bytes.begin() + guard + std::size_t(r.bufferOffset - offset);
                expect(std::all_of(first, first + std::size_t(span), [](auto b) { return b == 0; }),
                       "every copied storage byte is actually zero");
            }
            const auto mip = r.imageSubresource.mipLevel;
            const auto mip_width = std::max(1u, image.width >> mip);
            const auto row_blocks = (mip_width + format.block - 1) / format.block;
            for (unsigned layer = 0; layer < r.imageSubresource.layerCount; ++layer)
                for (unsigned y = 0; y < by; ++y) for (unsigned x = 0; x < bx; ++x) {
                    const auto index = (unsigned(r.imageOffset.y) / format.block + y) * row_blocks +
                                       unsigned(r.imageOffset.x) / format.block + x;
                    auto& subresource = covered[mip * image.array_layers + r.imageSubresource.baseArrayLayer + layer];
                    expect(index < subresource.size(), "copy region remains inside destination blocks");
                    if (index < subresource.size()) subresource[index] = true;
                }
        }
    }
    void check_complete() const {
        expect(copies > 0, "image copies emitted");
        for (const auto& subresource : covered)
            expect(std::all_of(subresource.begin(), subresource.end(), [](bool b) { return b; }),
                   "every image block is covered, including mip/layer edges");
        expect(bytes.size() >= 2 * guard, "allocation guard storage exists");
        if (bytes.size() >= 2 * guard)
            expect(std::all_of(bytes.begin(), bytes.begin() + guard, [](auto b) { return b == 0xa5; }) &&
                   std::all_of(bytes.end() - guard, bytes.end(), [](auto b) { return b == 0xa5; }),
                   "allocation guard bytes remain unchanged");
    }
};
Probe* current = nullptr;
VKAPI_ATTR void VKAPI_CALL fill(VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, std::uint32_t value) {
    current->fill(buffer, offset, size, value);
}
VKAPI_ATTR void VKAPI_CALL copy(VkCommandBuffer, VkBuffer buffer, VkImage, VkImageLayout, std::uint32_t count,
                               const VkBufferImageCopy* regions) { current->copy(buffer, count, regions); }
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                  std::uint32_t, const VkMemoryBarrier*, std::uint32_t,
                                  const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {
    ++current->barriers;
}
bool run(Probe& p) {
    current = &p;
    ReplayFunctions vk{};
    vk.fill_buffer = fill; vk.copy_buffer_to_image = copy; vk.pipeline_barrier = barrier;
    vk.queue_stages = detail::queue_stages_copy;
    vk.copy_scratch = [&p](VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset) { return p.allocate(size, buffer, offset); };
    if (p.zero_page) {
        p.allocated = 256; p.offset = 16;
        p.bytes.assign(std::size_t(p.allocated) + 2 * guard, 0xa5);
        std::fill(p.bytes.begin() + guard, p.bytes.end() - guard, static_cast<unsigned char>(0));
        vk.zero_buffer = reinterpret_cast<VkBuffer>(std::uintptr_t{3});
        vk.zero_offset = p.offset; vk.zero_size = p.allocated;
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, p.image.mip_levels, 0, p.image.array_layers};
    ++cases;
    return detail::zero_compressed_image(vk, VK_NULL_HANDLE, p.image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}
void success(VkFormat format, unsigned w, unsigned h, VkDeviceSize offset, unsigned mips = 1, unsigned layers = 1) {
    Probe p(format, w, h, offset, mips, layers);
    expect(run(p), "scratch zero initialization succeeds"); p.check_complete();
    expect(p.fills == 1 && p.barriers == 1, "one fill and transfer write-to-read barrier precede copies");
}
} // namespace

int main() {
    success(VK_FORMAT_R8_UNORM, 3, 1, 0);
    success(VK_FORMAT_R8_UNORM, 5, 1, 1);
    success(VK_FORMAT_R8G8_UNORM, 3, 1, 0);
    success(VK_FORMAT_R8G8_UNORM, 2, 1, 2);
    success(VK_FORMAT_R8G8B8A8_UNORM, 3, 1, 2);
    success(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 7, 5, 5);
    success(VK_FORMAT_BC7_UNORM_BLOCK, 7, 5, 17);
    success(VK_FORMAT_R8_UNORM, 17, 9, 3, 4, 3);
    success(VK_FORMAT_R8_UNORM, 65539, 3, 1);
    success(VK_FORMAT_BC7_UNORM_BLOCK, 32769, 5, 1);
    {
        Probe p(VK_FORMAT_R8_UNORM, 3, 1, 0); p.fail_first = true;
        expect(run(p), "aligned smaller-allocation fallback succeeds"); p.check_complete();
        expect(p.allocations == 2, "padding request failure retries the smaller aligned request");
    }
    {
        Probe p(VK_FORMAT_R8_UNORM, 3, 1, 0); p.fail_all = true;
        expect(!run(p) && !p.fills && !p.copies, "allocation failure emits no fill/copy");
    }
    {
        Probe p(VK_FORMAT_R8_UNORM, 3, 1, 0); p.null_buffer = true;
        expect(!run(p) && !p.fills && !p.copies, "null successful allocation emits no fill/copy");
    }
    {
        Probe p(VK_FORMAT_R8_UNORM, 3, 1, UINT64_MAX - 1);
        expect(!run(p) && !p.fills && !p.copies, "overflowing allocation span emits no fill/copy");
    }
    {
        Probe p(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 7, 5, 0); p.zero_page = true;
        expect(run(p), "existing aligned zero-page path succeeds"); p.check_complete();
        expect(!p.allocations && !p.fills && !p.barriers, "zero page needs no scratch/fill barrier");
    }
    if (failures) { std::fprintf(stderr, "replay_zero_init: %u failures in %u cases\n", failures, cases); return 1; }
    std::printf("replay_zero_init: %u CPU storage/coverage/alignment/control cases pass\n", cases);
    return 0;
}
