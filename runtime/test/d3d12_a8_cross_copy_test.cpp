// Logical A8 copies across plain R8-backed and native-alpha RT images, through D3D12 COM/queue only.
// All texels, footprints and expected results are synthetic; no external Vulkan recording or game data.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace d3d12test;
namespace {
constexpr UINT kWidth = 32, kHeight = 16, kMips = 4, kLayers = 3, kSubresources = kMips * kLayers;
using Texels = std::array<std::vector<std::uint8_t>, kSubresources>;
bool check(bool condition, const char* expression, int line) {
    if (!condition) std::fprintf(stderr, "line %d: %s\n", line, expression);
    return condition;
}
#define REQUIRE(c) do { if (!check(bool(c), #c, __LINE__)) return false; } while (0)

UINT width(UINT sub) { return std::max(1u, kWidth >> (sub % kMips)); }
UINT height(UINT sub) { return std::max(1u, kHeight >> (sub % kMips)); }
UINT subresource(UINT mip, UINT layer) { return mip + layer * kMips; }
std::uint8_t value(UINT image, UINT sub, UINT x, UINT y) {
    // Nonuniform by row, column, mip, array slice and image; a wrong region/pitch/scratch offset changes bytes.
    return std::uint8_t(13 + image * 71 + sub * 19 + x * 7 + y * 23);
}
D3D12_RESOURCE_DESC texture_desc(bool rt) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = kWidth; d.Height = kHeight; d.DepthOrArraySize = kLayers; d.MipLevels = kMips;
    d.Format = DXGI_FORMAT_A8_UNORM; d.SampleDesc.Count = 1;
    d.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    return d;
}
D3D12_TEXTURE_COPY_LOCATION image_location(ID3D12Resource* image, UINT sub) {
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = image; location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex = sub;
    return location;
}

struct Fixture {
    ID3D12Device* device;
    bool native_rt;
    std::array<Ref<ID3D12Resource>, 3> images, readback;
    Ref<ID3D12Resource> upload;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kSubresources> footprints{};
    std::array<Texels, 3> expected;
    UINT64 total = 0, segment = 0, ticket = 0;

    Fixture(ID3D12Device* dev, bool native) : device(dev), native_rt(native) {}
    bool buffer(Ref<ID3D12Resource>& out, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        const D3D12_HEAP_PROPERTIES heap{type};
        const auto desc = buffer_desc(bytes);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    }
    bool initialize() {
        const auto plain = texture_desc(false);
        std::array<UINT, kSubresources> rows{};
        std::array<UINT64, kSubresources> row_bytes{};
        device->GetCopyableFootprints(&plain, 0, kSubresources, 0, footprints.data(), rows.data(), row_bytes.data(), &total);
        REQUIRE(total && total != UINT64_MAX && total <= 1024 * 1024);
        for (UINT sub = 0; sub < kSubresources; ++sub) {
            const auto& f = footprints[sub];
            REQUIRE(f.Footprint.Format == DXGI_FORMAT_A8_UNORM && f.Footprint.Width == width(sub) &&
                    f.Footprint.Height == height(sub) && f.Footprint.Depth == 1);
            REQUIRE(rows[sub] == height(sub) && row_bytes[sub] == width(sub));
            REQUIRE(f.Offset % 512 == 0 && f.Footprint.RowPitch % 256 == 0 && f.Footprint.RowPitch >= width(sub));
            REQUIRE(f.Offset + UINT64(height(sub) - 1) * f.Footprint.RowPitch + width(sub) <= total);
        }
        segment = (total + 511) / 512 * 512;
        REQUIRE(buffer(upload, 3 * segment, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ));
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        for (UINT i = 0; i < images.size(); ++i) {
            const auto desc = texture_desc(native_rt && i == 1);
            const auto size = device->GetResourceAllocationInfo(0, 1, &desc);
            REQUIRE(size.SizeInBytes != UINT64_MAX);
            REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&images[i].p))));
            REQUIRE(images[i]->GetDesc().Format == DXGI_FORMAT_A8_UNORM && images[i]->GetDesc().Flags == desc.Flags);
            REQUIRE(buffer(readback[i], total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
        }
        void* mapped = nullptr;
        REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &mapped)) && mapped);
        std::memset(mapped, 0xa5, std::size_t(3 * segment));
        for (UINT i = 0; i < images.size(); ++i) for (UINT sub = 0; sub < kSubresources; ++sub) {
            const auto& fp = footprints[sub];
            for (UINT y = 0; y < height(sub); ++y) for (UINT x = 0; x < width(sub); ++x)
                static_cast<std::uint8_t*>(mapped)[i * segment + fp.Offset + UINT64(y) * fp.Footprint.RowPitch + x] =
                    value(i, sub, x, y);
        }
        upload->Unmap(0, nullptr);
        const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        REQUIRE(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        upload_patterns(false);
        REQUIRE(submit(leonida::ReplayResult::success));
        REQUIRE(verify());
        return true;
    }
    bool begin() {
        REQUIRE(SUCCEEDED(allocator->Reset()));
        REQUIRE(SUCCEEDED(list->Reset(allocator.p, nullptr)));
        return true;
    }
    bool submit(leonida::ReplayResult want) {
        REQUIRE(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        REQUIRE(rt::d3d12::last_execute_result(queue.p) == want);
        REQUIRE(SUCCEEDED(queue->Signal(fence.p, ++ticket)));
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        REQUIRE(event);
        const bool done = SUCCEEDED(fence->SetEventOnCompletion(ticket, event)) &&
                          WaitForSingleObject(event, 20000) == WAIT_OBJECT_0;
        CloseHandle(event);
        REQUIRE(done);
        return true;
    }
    void state(UINT image, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        const auto barrier = transition(images[image].p, before, after);
        list->ResourceBarrier(1, &barrier);
    }
    void upload_subresource(UINT image, UINT sub) {
        auto dst = image_location(images[image].p, sub);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.p; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[sub]; src.PlacedFootprint.Offset += image * segment;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    void upload_patterns(bool previously_source) {
        for (UINT i = 0; i < images.size(); ++i) {
            if (previously_source) state(i, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT sub = 0; sub < kSubresources; ++sub) {
                upload_subresource(i, sub);
                auto& bytes = expected[i][sub]; bytes.resize(width(sub) * height(sub));
                for (UINT y = 0; y < height(sub); ++y) for (UINT x = 0; x < width(sub); ++x)
                    bytes[y * width(sub) + x] = value(i, sub, x, y);
            }
            state(i, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
    }
    bool verify() {
        REQUIRE(begin());
        for (UINT i = 0; i < images.size(); ++i) for (UINT sub = 0; sub < kSubresources; ++sub) {
            auto src = image_location(images[i].p, sub);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback[i].p; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprints[sub];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        REQUIRE(submit(leonida::ReplayResult::success));
        for (UINT i = 0; i < images.size(); ++i) {
            void* mapped = nullptr;
            REQUIRE(SUCCEEDED(readback[i]->Map(0, nullptr, &mapped)) && mapped);
            bool matches = true;
            for (UINT sub = 0; sub < kSubresources && matches; ++sub) {
                const auto& fp = footprints[sub];
                for (UINT y = 0; y < height(sub) && matches; ++y) for (UINT x = 0; x < width(sub); ++x) {
                    const auto got = static_cast<const std::uint8_t*>(mapped)[fp.Offset + UINT64(y) * fp.Footprint.RowPitch + x];
                    const auto want = expected[i][sub][y * width(sub) + x];
                    if (got != want) {
                        std::fprintf(stderr, "image%u layer%u mip%u (%u,%u): got%u expected%u\n", i,
                                     sub / kMips, sub % kMips, x, y, got, want);
                        matches = false; break;
                    }
                }
            }
            readback[i]->Unmap(0, nullptr);
            REQUIRE(matches);
        }
        return true;
    }
    bool whole() {
        REQUIRE(begin());
        state(1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(images[1].p, images[0].p);
        state(1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        state(2, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(images[2].p, images[1].p);
        state(2, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        REQUIRE(submit(leonida::ReplayResult::success));
        expected[1] = expected[0]; expected[2] = expected[0];
        REQUIRE(verify());
        return true;
    }
    void region(UINT src_image, UINT src_sub, D3D12_BOX box, UINT dst_image, UINT dst_sub, UINT x, UINT y) {
        auto src = image_location(images[src_image].p, src_sub);
        auto dst = image_location(images[dst_image].p, dst_sub);
        list->CopyTextureRegion(&dst, x, y, 0, &src, &box);
        const UINT w = box.right - box.left, h = box.bottom - box.top;
        for (UINT row = 0; row < h; ++row) for (UINT col = 0; col < w; ++col)
            expected[dst_image][dst_sub][(y + row) * width(dst_sub) + x + col] =
                expected[src_image][src_sub][(box.top + row) * width(src_sub) + box.left + col];
    }
    bool partial() {
        REQUIRE(begin()); upload_patterns(true); REQUIRE(submit(leonida::ReplayResult::success));
        REQUIRE(begin());
        state(1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        region(0, subresource(1, 2), {3, 1, 0, 11, 6, 1}, 1, subresource(0, 1), 13, 8);
        state(1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        state(2, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        // Includes the first copy and untouched bytes around it, so the native-A8 read must see the preceding write.
        region(1, subresource(0, 1), {11, 7, 0, 23, 14, 1}, 2, subresource(0, 0), 4, 3);
        state(2, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        state(1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        region(2, subresource(1, 1), {4, 2, 0, 9, 5, 1}, 1, subresource(2, 0), 1, 1);
        state(1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        REQUIRE(submit(leonida::ReplayResult::success));
        REQUIRE(verify());
        return true;
    }
    bool invalid_preserves_prefix() {
        REQUIRE(begin());
        state(2, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        // This would erase the preceding partial-copy result; rejection must leave it unchanged.
        upload_subresource(2, subresource(0, 0));
        state(2, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        state(1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto src = image_location(images[0].p, 0), dst = image_location(images[1].p, 0);
        const D3D12_BOX invalid{0, 0, 0, kWidth + 1, 1, 1};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &invalid);
        REQUIRE(submit(leonida::ReplayResult::invalid_copy));
        // Original COPY_SOURCE layouts and every byte must survive the failed batch.
        REQUIRE(verify());
        return true;
    }
};

bool make_device(vktest::Device& d, bool native, Ref<ID3D12Device>& out) {
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc; info.instance = d.instance; info.physical = d.physical;
    info.device = d.device; info.queue = d.queue; info.queue_family = d.family;
    info.a8_render_target = native && d.a8_render_target;
    REQUIRE(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&out.p))));
    return true;
}
bool feature_off(vktest::Device& d) {
    Ref<ID3D12Device> device;
    REQUIRE(make_device(d, false, device));
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_A8_UNORM};
    REQUIRE(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))));
    REQUIRE(!(support.Support1 & (D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE)));
    const auto rt = texture_desc(true);
    REQUIRE(device->GetResourceAllocationInfo(0, 1, &rt).SizeInBytes == UINT64_MAX);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    void* rejected = reinterpret_cast<void*>(std::uintptr_t(1));
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rt,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), &rejected);
    REQUIRE(FAILED(hr) && !rejected);
    // Disabling native alpha cannot break existing plain logical-A8 resources/copies.
    Fixture plain(device.p, false);
    REQUIRE(plain.initialize() && plain.whole());
    return true;
}
} // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (!check(status == vktest::DeviceStatus::ready, "Vulkan initialization", __LINE__)) return 1;
    if (!feature_off(d)) return 1;
    if (!d.a8_render_target) {
        if (d.validation_errors()) return 1;
        std::printf("SKIP: native A8 RT unavailable; feature-off refusal and plain-A8 copy control passed\n");
        return 77;
    }
    {
        Ref<ID3D12Device> device;
        if (!make_device(d, true, device)) return 1;
        Fixture copy(device.p, true);
        if (!copy.initialize() || !copy.whole() || !copy.partial() || !copy.invalid_preserves_prefix()) return 1;
    }
    if (!check(d.validation_errors() == 0, "zero Vulkan validation errors", __LINE__)) return 1;
    std::printf("A8 R8/native-alpha cross copies: both directions; CopyResource 4mips x3layers, "
                "3 offset boxes, all source/untouched bytes, invalid-prefix preservation and feature-off controls pass\n");
    return 0;
}
