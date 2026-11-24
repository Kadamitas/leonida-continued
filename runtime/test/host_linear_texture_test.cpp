// Synthetic CPU-owned linear image import: real GPU sampling/copy, later CPU stores, and a second CPU alias.
// No game data. The sampler fixture is original GLSL, embedded with its reproducible SPIR-V.
#include "vk_test_device.h"
#include "host_linear_sample_shader.h"
#include "../shims/d3d12_com.h"
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace {
unsigned failures = 0;
bool check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
    return condition;
}
#define REQUIRE(condition) do { if (!check(bool(condition), #condition)) return false; } while (0)

constexpr SIZE_T span = 65536;
constexpr UINT height = 3, pitch = 256;
struct Pages {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(span), nullptr);
    void* first = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, span) : nullptr;
    void* second = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, span) : nullptr;
    ~Pages() {
        if (first) UnmapViewOfFile(first);
        if (second) UnmapViewOfFile(second);
        if (section) CloseHandle(section);
    }
};

D3D12_RESOURCE_DESC texture_desc(UINT width) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width; d.Height = height; d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
std::uint32_t pixel(UINT x, UINT y, unsigned seed) {
    return ((x * 3 + seed) & 255) | (((y * 37 + seed * 5) & 255) << 8) |
           (((x + y * 9 + seed * 11) & 255) << 16) | 0xff000000u;
}
void fill(void* address, UINT width, unsigned seed) {
    std::memset(address, 0xa7, span);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) {
            const auto value = pixel(x, y, seed);
            std::memcpy(static_cast<std::uint8_t*>(address) + y * pitch + x * 4, &value, 4);
        }
}
bool pixels_match(const void* address, UINT width, UINT row_pitch, unsigned seed) {
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) {
            std::uint32_t got;
            std::memcpy(&got, static_cast<const std::uint8_t*>(address) + y * row_pitch + x * 4, 4);
            if (got != pixel(x, y, seed)) {
                std::printf("mismatch (%u,%u): %08x != %08x\n", x, y, got, pixel(x, y, seed));
                return false;
            }
        }
    return true;
}

// Uses the actual imported VkImage as a sampled image, with output written by the GPU into coherent memory.
struct SampleProbe {
    vktest::Device& d;
    UINT width;
    VkImageView view{}; VkSampler sampler{}; VkShaderModule module{};
    VkDescriptorSetLayout set_layout{}; VkDescriptorPool pool{}; VkDescriptorSet set{};
    VkPipelineLayout layout{}; VkPipeline pipeline{}; VkBuffer output{};
    void* mapped = nullptr;
    ~SampleProbe() {
        if (pipeline) d.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(d.device, pipeline, nullptr);
        if (layout) d.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(d.device, layout, nullptr);
        if (pool) d.fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(d.device, pool, nullptr);
        if (set_layout) d.fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(d.device, set_layout, nullptr);
        if (module) d.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(d.device, module, nullptr);
        if (sampler) d.fn<PFN_vkDestroySampler>("vkDestroySampler")(d.device, sampler, nullptr);
        if (view) d.fn<PFN_vkDestroyImageView>("vkDestroyImageView")(d.device, view, nullptr);
    }
    bool initialize(VkImage image) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        REQUIRE(d.fn<PFN_vkCreateImageView>("vkCreateImageView")(d.device, &vi, nullptr, &view) == VK_SUCCESS);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0;
        REQUIRE(d.fn<PFN_vkCreateSampler>("vkCreateSampler")(d.device, &si, nullptr, &sampler) == VK_SUCCESS);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof(host_linear_fixture::words); mi.pCode = host_linear_fixture::words;
        REQUIRE(d.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(d.device, &mi, nullptr, &module) == VK_SUCCESS);
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di.bindingCount = 2; di.pBindings = bindings;
        REQUIRE(d.fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(d.device, &di, nullptr, &set_layout) == VK_SUCCESS);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &set_layout; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &push;
        REQUIRE(d.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(d.device, &li, nullptr, &layout) == VK_SUCCESS);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main"; ci.layout = layout;
        REQUIRE(d.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(d.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) == VK_SUCCESS);
        VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
        REQUIRE(d.fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(d.device, &pi, nullptr, &pool) == VK_SUCCESS);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &set_layout;
        REQUIRE(d.fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(d.device, &ai, &set) == VK_SUCCESS);
        output = d.buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &mapped);
        REQUIRE(output && mapped);
        VkDescriptorImageInfo image_info{sampler, view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo buffer_info{output, 0, VkDeviceSize(width) * height * 4};
        VkWriteDescriptorSet writes[2]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image_info};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffer_info};
        d.fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(d.device, 2, writes, 0, nullptr);
        return true;
    }
    bool compare(unsigned seed) {
        std::memset(mapped, 0xcd, size_t(width) * height * 4);
        const bool completed = d.run([&](VkCommandBuffer cb) {
            d.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            d.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            const UINT dimensions[2]{width, height};
            d.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, dimensions);
            d.fn<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, (width * height + 63) / 64, 1, 1);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                                                 0, 1, &host, 0, nullptr, 0, nullptr);
            return true;
        });
        REQUIRE(completed);
        REQUIRE(pixels_match(mapped, width, width * 4, seed));
        return true;
    }
};

bool wait_queue(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    REQUIRE(SUCCEEDED(queue->Signal(fence, value)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(event);
    const HRESULT hr = fence->SetEventOnCompletion(value, event);
    const DWORD waited = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    REQUIRE(SUCCEEDED(hr) && waited == WAIT_OBJECT_0);
    return true;
}

bool check_texture(vktest::Device& vk, ID3D12Device* device, UINT width) {
    Pages pages;
    REQUIRE(pages.first && pages.second && pages.first != pages.second);
    fill(pages.first, width, 1);
    const std::vector<std::uint8_t> initial_span(static_cast<const std::uint8_t*>(pages.first),
                                               static_cast<const std::uint8_t*>(pages.first) + span);
    REQUIRE(pixels_match(pages.second, width, pitch, 1));
    auto desc = texture_desc(width);
    ComPtr<ID3D12Resource> texture;
    REQUIRE(SUCCEEDED(rt::d3d12::create_readonly_texture_at_host_address(device, pages.first, span, pitch, desc,
                                                                       IID_PPV_ARGS(texture.GetAddressOf()))));
    REQUIRE(texture && texture->GetGPUVirtualAddress() == 0);
    REQUIRE(std::memcmp(pages.first, initial_span.data(), initial_span.size()) == 0);
    constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    REQUIRE((rt::d3d12::vulkan_memory_flags(texture.Get()) & coherent) == coherent);
    REQUIRE(rt::d3d12::vulkan_image(texture.Get()) != VK_NULL_HANDLE);
    std::uint32_t unused = 0;
    REQUIRE(texture->WriteToSubresource(0, nullptr, &unused, 4, 4) == E_NOTIMPL);

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC qd{};
    REQUIRE(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.GetAddressOf()))));
    REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))));
    REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()))));
    REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))));
    // First use is sampling after an EMPTY backend execute. No copy initialized or zero-filled the image first.
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    REQUIRE(wait_queue(queue.Get(), fence.Get(), 1));
    SampleProbe sampler{vk, width};
    REQUIRE(sampler.initialize(rt::d3d12::vulkan_image(texture.Get())));
    REQUIRE(sampler.compare(1));

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = pitch * height;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    ComPtr<ID3D12Resource> readback;
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.GetAddressOf()))));
    REQUIRE(SUCCEEDED(allocator->Reset()));
    REQUIRE(SUCCEEDED(list->Reset(allocator.Get(), nullptr)));
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, pitch}};
    src.pResource = texture.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    REQUIRE(SUCCEEDED(list->Close()));
    // Mutate through the second CPU mapping AFTER recording. Creation/record-time snapshots cannot pass.
    for (unsigned seed : {2u, 3u}) {
        fill(seed == 2 ? pages.second : pages.first, width, seed);
        void* mapped = nullptr;
        REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)) && mapped);
        std::memset(mapped, 0xcd, pitch * height);
        readback->Unmap(0, nullptr);
        queue->ExecuteCommandLists(1, lists);
        REQUIRE(wait_queue(queue.Get(), fence.Get(), seed));
        REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)) && mapped);
        const bool same = pixels_match(mapped, width, pitch, seed);
        readback->Unmap(0, nullptr);
        REQUIRE(same);
        REQUIRE(sampler.compare(seed));  // copy use must keep the source in GENERAL for later CPU writes/sample.
    }
    std::printf("host-linear: width=%u: initial sample, two updated samples and two copy readbacks all match\n", width);
    return true;
}

bool check_refusals(ID3D12Device* device) {
    Pages pages;
    REQUIRE(pages.first);
    const auto desc = texture_desc(64);
    const auto refused = [&](D3D12_RESOURCE_DESC d, void* address, SIZE_T bytes, UINT64 row_pitch) {
        void* out = reinterpret_cast<void*>(0x1234);
        const HRESULT hr = rt::d3d12::create_readonly_texture_at_host_address(device, address, bytes, row_pitch, d,
                                                                             __uuidof(ID3D12Resource), &out);
        return FAILED(hr) && out == nullptr;
    };
    REQUIRE(refused(desc, nullptr, span, pitch));
    REQUIRE(refused(desc, static_cast<std::uint8_t*>(pages.first) + 1, span - 1, pitch));
    REQUIRE(refused(desc, pages.first, 1, pitch));
    REQUIRE(refused(desc, pages.first, SIZE_MAX, pitch));  // address+span overflow, without an enormous allocation
    REQUIRE(refused(desc, pages.first, span, pitch + 1));
    REQUIRE(refused(desc, pages.first, span, 0));  // auto pitch is only unambiguous for height 1.
    auto d = desc; d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101); REQUIRE(refused(d, pages.first, span, pitch));
    d = desc; d.DepthOrArraySize = 2; REQUIRE(refused(d, pages.first, span, pitch));
    d = desc; d.MipLevels = 2; REQUIRE(refused(d, pages.first, span, pitch));
    d = desc; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; REQUIRE(refused(d, pages.first, span, pitch));
    d = desc; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; REQUIRE(refused(d, pages.first, span, pitch));
    d = desc; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; REQUIRE(refused(d, pages.first, span, pitch));
    REQUIRE(FAILED(rt::d3d12::create_readonly_texture_at_host_address(device, pages.first, span, pitch, desc,
                                                                   __uuidof(ID3D12Resource), nullptr)));
    void* reserved = VirtualAlloc(nullptr, span, MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(reserved);
    const bool reserve_refused = refused(desc, reserved, span, pitch);
    VirtualFree(reserved, 0, MEM_RELEASE);
    REQUIRE(reserve_refused);
    return true;
}

bool check_single_row_route(ID3D12Device* device, bool bindless) {
    Pages pages;
    REQUIRE(pages.first && pages.second);
    auto desc = texture_desc(4);
    ComPtr<ID3D12Resource> refused;
    REQUIRE(rt::d3d12::create_buffer_at_host_address(device, pages.first, desc,
                                                    IID_PPV_ARGS(refused.GetAddressOf())) == E_NOTIMPL);
    REQUIRE(!refused);  // The public route cannot infer a multirow console pitch.
    desc.Height = 1;
    fill(pages.second, 4, 7);
    ComPtr<ID3D12Resource> texture;
    REQUIRE(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(device, pages.first, desc,
                                                              IID_PPV_ARGS(texture.GetAddressOf()))));
    REQUIRE(texture && texture->GetGPUVirtualAddress() == 0);
    ComPtr<ID3D12DescriptorHeap> view_heap;
    std::uint32_t key[8]{};
    if (bindless) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(view_heap.GetAddressOf()))));
        const auto handle = view_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(texture.Get(), nullptr, handle);
        std::memcpy(key, reinterpret_cast<const void*>(handle.ptr), sizeof(key));
        REQUIRE(rt::d3d12::bindless_hash_lookup(device, key) != 0);
    }

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> readback;
    D3D12_COMMAND_QUEUE_DESC qd{};
    REQUIRE(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.GetAddressOf()))));
    REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))));
    REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(list.GetAddressOf()))));
    REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))));
    auto bd = desc;
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 4096; bd.Format = DXGI_FORMAT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.GetAddressOf()))));
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 256}};
    src.pResource = texture.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    REQUIRE(wait_queue(queue.Get(), fence.Get(), 1));
    void* mapped = nullptr;
    REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)) && mapped);
    const bool same = std::memcmp(mapped, pages.second, 16) == 0;
    readback->Unmap(0, nullptr);
    REQUIRE(same);

    // Retire recorded references, release the image and immediately invalidate both caller views.
    // A following unrelated submission must not contain a stale import barrier or bindless view.
    list.Reset();
    texture.Reset();
    if (bindless) REQUIRE(rt::d3d12::bindless_hash_lookup(device, key) == 0);
    REQUIRE(UnmapViewOfFile(pages.first)); pages.first = nullptr;
    REQUIRE(UnmapViewOfFile(pages.second)); pages.second = nullptr;
    REQUIRE(CloseHandle(pages.section)); pages.section = nullptr;
    REQUIRE(SUCCEEDED(allocator->Reset()));
    REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(list.GetAddressOf()))));
    REQUIRE(SUCCEEDED(list->Close()));
    lists[0] = list.Get();
    queue->ExecuteCommandLists(1, lists);
    REQUIRE(wait_queue(queue.Get(), fence.Get(), 2));
    std::printf("host-linear: public single-row route copied real bytes; released import survived immediate caller unmap and unrelated submit\n");
    return true;
}

bool check_replay_guards(vktest::Device& d) {
    using namespace leonida;
    ResourceBinding a{};
    a.image = reinterpret_cast<VkImage>(std::uintptr_t(0x1000)); a.width = a.height = 4;
    a.format = VK_FORMAT_R8G8B8A8_UNORM; a.host_readonly_image = true;
    ResourceBinding b = a; b.image = reinterpret_cast<VkImage>(std::uintptr_t(0x2000)); b.host_readonly_image = false;
    const auto resolve = [&](ResourceId id) -> const ResourceBinding* { return id == 1 ? &a : id == 2 ? &b : nullptr; };
    ReplayFunctions functions{};
    functions.pipeline_barrier = d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    functions.pipeline_barrier2 = d.fn<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2");
    functions.clear_color_image = d.fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
    functions.copy_buffer = d.fn<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    functions.copy_buffer_to_image = d.fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    functions.copy_image_to_buffer = d.fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    functions.copy_image = d.fn<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    const auto refused = [&](auto record) {
        CommandList list;
        if (!record(list) || !list.close()) return false;
        PreparedList prepared;
        a.host_readonly_image = true; functions.has_host_readonly_images = true;
        const auto rejected = prepare(list, resolve, functions, prepared);
        // A positive control proves refusal is the new readonly policy, not absent Vulkan functions or bad IR.
        a.host_readonly_image = false; functions.has_host_readonly_images = false;
        const auto ordinary = prepare(list, resolve, functions, prepared);
        return rejected == ReplayResult::unsupported && ordinary == ReplayResult::success;
    };
    REQUIRE(refused([](CommandList& l) { return l.clear_render_target(TargetView{1, 0, 0, 1}, {1, 0, 0, 1}); }));
    REQUIRE(refused([](CommandList& l) { return l.copy_resource({1, 2}); }));
    REQUIRE(refused([](CommandList& l) { return l.discard({1, 0, 0, false, true}); }));
    REQUIRE(refused([](CommandList& l) { return l.aliasing_barrier({1, 2}); }));
    REQUIRE(refused([](CommandList& l) { return l.aliasing_barrier({0, 0}); }));
    REQUIRE(refused([](CommandList& l) {
        TextureBarrier barrier{};
        barrier.resource = 1; barrier.range.index_or_first_mip = all_subresources;
        barrier.layout_before = d3d12_layout::common; barrier.layout_after = d3d12_layout::common;
        barrier.flags = texture_barrier_discard;
        return l.barrier(barrier);
    }));
    REQUIRE(refused([](CommandList& l) {
        TextureBarrier barrier{};
        barrier.resource = 1; barrier.range.index_or_first_mip = all_subresources;
        barrier.layout_before = d3d12_layout::undefined; barrier.layout_after = d3d12_layout::common;
        return l.barrier(barrier);
    }));
    return true;
}
} // namespace

int main() {
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status != vktest::DeviceStatus::ready) return status == vktest::DeviceStatus::unavailable ? 77 : 1;
    if (!vk.host_pointer_alignment) { std::printf("SKIP: VK_EXT_external_memory_host unavailable\n"); return 77; }
    ComPtr<ID3D12Device> device;
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc; info.instance = vk.instance; info.physical = vk.physical;
    info.device = vk.device; info.queue = vk.queue; info.queue_family = vk.family;
    info.host_pointer_alignment = vk.host_pointer_alignment; info.features = vk.enabled;
    info.bindless = vk.bindless; info.bindless_partially_bound = vk.partially_bound;
    if (!check(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(device.GetAddressOf()))), "Vulkan backend device creates")) return 1;
    if (!check_refusals(device.Get()) || !check_replay_guards(vk) ||
        !check_texture(vk, device.Get(), 63) || !check_texture(vk, device.Get(), 64) ||
        !check_single_row_route(device.Get(), vk.bindless)) return 1;
    if (!check(vk.validation_errors() == 0, "no counted validation errors (only meaningful when validation enabled)")) return 1;
    std::printf("host_linear_texture_test: PASS\n");
    return failures ? 1 : 0;
}
