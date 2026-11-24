// Production COM -> queue -> Vulkan indirect graphics, without external rendering or raw IR opt-ins.
// The valid workloads also run on native WARP with equivalent HLSL. All data is synthetic.
#include "vk_test_device.h"
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(LEONIDA_TEST_INDIRECT_DRAW_VS_SPV) || !defined(LEONIDA_TEST_INDIRECT_DRAW_PS_SPV)
#error "The two LEONIDA_TEST_INDIRECT_DRAW_*_SPV macros must name the synthetic shader modules"
#endif

using namespace d3d12test;
namespace {
constexpr UINT kSize = 64, kPitch = 256, kBytes = 4096;
constexpr UINT kDrawOffset = 512, kIndexedOffset = 640, kStride = 32;
constexpr UINT kTintA = 1024, kTintB = 1280, kTintPoison = 1536;
constexpr UINT kZero = 2048, kPoison = 2052, kCount = 2060, kBadVertices = 2304;
constexpr UINT kDrawBits = 0x1f8, kIndexedBits = 0x1e00, kAllBits = kDrawBits | kIndexedBits;
constexpr float kBlue[4] = {0, 0, 1, 1}, kBlack[4] = {0, 0, 0, 1}, kRed[4] = {1, 0, 0, 1};
using Pixel = std::array<std::uint8_t, 4>;
using Image = std::array<std::uint8_t, kSize * kPitch>;
struct Vertex { float x, y; UINT marker; };

const char native_vs[] = R"(
cbuffer Offset : register(b0) { float2 offset; }
RWByteAddressBuffer output_bits : register(u0);
struct Input { float2 position : POSITION; uint marker : MARKER; float4 color : COLOR; };
struct Output { float4 position : SV_Position; float4 color : COLOR; };
Output main(Input v) {
    uint ignored;
    output_bits.InterlockedOr(0, 1u << v.marker, ignored);
    Output o; o.position = float4(v.position + offset, 0.5, 1); o.color = v.color; return o;
})";
const char native_ps[] = R"(
cbuffer Tint : register(b1) { float4 tint; }
float4 main(float4 position : SV_Position, float4 color : COLOR) : SV_Target { return color * tint; }
)";

bool check(bool condition, const char* label) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", label);
    return condition;
}
#define REQUIRE(c) do { if (!check(bool(c), #c)) return false; } while (0)

struct Shaders { std::vector<std::uint8_t> vs, ps; };
struct Output { Image a{}, b{}; UINT bits = UINT_MAX; };

bool equal_image(const Image& image, const char* label, bool first, bool empty) {
    for (UINT y = 0; y < kSize; ++y) for (UINT x = 0; x < kSize; ++x) {
        const bool covered = !empty && (first ? x < 32 && y < 32 : x >= 32 && y >= 32);
        const Pixel expected = covered ? (first ? Pixel{255, 0, 0, 255} : Pixel{0, 255, 0, 255})
                                       : (first ? Pixel{0, 0, 255, 255} : Pixel{0, 0, 0, 255});
        const auto* got = image.data() + y * kPitch + x * 4;
        if (!std::equal(expected.begin(), expected.end(), got)) {
            std::fprintf(stderr, "%s (%u,%u): got %u,%u,%u,%u expected %u,%u,%u,%u\n", label, x, y,
                         got[0], got[1], got[2], got[3], expected[0], expected[1], expected[2], expected[3]);
            return false;
        }
    }
    return true;
}

struct Fixture {
    ID3D12Device* device;
    bool shim;
    Ref<ID3D12RootSignature> root;
    Ref<ID3D12PipelineState> normal, green, divisor;
    Ref<ID3D12CommandSignature> draw, indexed;
    Ref<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    Ref<ID3D12Resource> upload, args, bits, bits_read, a, b, a_read, b_read, depth_pass, depth_fail;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtv{}, dsv{};
    UINT64 ticket = 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    D3D12_INPUT_ELEMENT_DESC elements[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"MARKER", 0, DXGI_FORMAT_R32_UINT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};

    Fixture(ID3D12Device* dev, bool layer) : device(dev), shim(layer) {}

    D3D12_CPU_DESCRIPTOR_HANDLE heap_start(ID3D12DescriptorHeap* heap) const {
        if (!shim) return heap->GetCPUDescriptorHandleForHeapStart();
        // The proxy's console heap slot returns its handle in RAX, including with PC command lists.
        using Fn = UINT64(WINAPI*)(void*);
        return {reinterpret_cast<Fn>(slot(heap, 9))(heap)};
    }

    bool buffer(Ref<ID3D12Resource>& out, UINT64 size, D3D12_HEAP_TYPE type,
                D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        const D3D12_HEAP_PROPERTIES heap{type};
        const auto desc = buffer_desc(size, flags);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                         nullptr, IID_PPV_ARGS(&out.p)));
    }

    bool initialize(const Shaders& shaders, bool make_divisor) {
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = {1, 0};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor = {0, 0};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto blob = serialize_root_signature(root_desc);
        REQUIRE(!blob.empty());
        REQUIRE(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p))));
        description.pRootSignature = root.p;
        description.VS = {shaders.vs.data(), shaders.vs.size()};
        description.PS = {shaders.ps.data(), shaders.ps.size()};
        description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        description.SampleMask = UINT_MAX;
        description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.RasterizerState.DepthClipEnable = TRUE;
        description.DepthStencilState.DepthEnable = TRUE;
        description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        description.InputLayout = {elements, 3};
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        description.SampleDesc.Count = 1;
        REQUIRE(SUCCEEDED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&normal.p))));
        auto green_desc = description;
        green_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_ALPHA;
        REQUIRE(SUCCEEDED(device->CreateGraphicsPipelineState(&green_desc, IID_PPV_ARGS(&green.p))));
        if (make_divisor) {
            D3D12_INPUT_ELEMENT_DESC step2[3] = {elements[0], elements[1], elements[2]};
            step2[2].InstanceDataStepRate = 2;
            auto desc = description;
            desc.InputLayout = {step2, 3};
            REQUIRE(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&divisor.p))));
        }
        for (auto kind : {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED}) {
            const D3D12_INDIRECT_ARGUMENT_DESC arg{kind};
            const D3D12_COMMAND_SIGNATURE_DESC signature{kStride, 1, &arg, 0};
            auto& out = kind == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ? draw : indexed;
            REQUIRE(SUCCEEDED(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&out.p))));
        }
        REQUIRE(buffer(upload, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ));
        REQUIRE(buffer(args, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST));
        REQUIRE(buffer(bits, 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
        REQUIRE(buffer(bits_read, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
        REQUIRE(buffer(a_read, kSize * kPitch, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
        REQUIRE(buffer(b_read, kSize * kPitch, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
        std::array<std::uint8_t, kBytes> data{};
        std::array<Vertex, 16> vertices{};
        for (UINT i = 0; i < vertices.size(); ++i) vertices[i] = {10, 10, i};
        const float quad[6][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (UINT i = 0; i < 6; ++i) vertices[3 + i] = {quad[i][0], quad[i][1], 3 + i};
        const float corners[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
        for (UINT i = 0; i < 4; ++i) vertices[9 + i] = {corners[i][0], corners[i][1], 9 + i};
        std::memcpy(data.data(), vertices.data(), sizeof(vertices));
        const float colors[4][4] = {{0, 0, 1, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {0, 0, 1, 1}};
        std::memcpy(data.data() + 256, colors, sizeof(colors));
        const std::uint16_t indices[] = {15, 15, 15, 0, 1, 2, 2, 1, 3};
        std::memcpy(data.data() + 384, indices, sizeof(indices));
        const std::uint16_t wrong_indices[] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(data.data() + 448, wrong_indices, sizeof(wrong_indices));
        const UINT draw_records[16] = {6, 1, 3, 1, 0xa5a5a5a5, 0x5a5a5a5a, 0xa5a5a5a5, 0x5a5a5a5a,
                                       6, 0, 3, 1, 0xa5a5a5a5, 0x5a5a5a5a, 0xa5a5a5a5, 0x5a5a5a5a};
        const UINT indexed_records[16] = {6, 1, 3, 9, 2, 0xa5a5a5a5, 0x5a5a5a5a, 0xa5a5a5a5,
                                          6, 0, 3, 9, 2, 0xa5a5a5a5, 0x5a5a5a5a, 0xa5a5a5a5};
        std::memcpy(data.data() + kDrawOffset, draw_records, sizeof(draw_records));
        std::memcpy(data.data() + kIndexedOffset, indexed_records, sizeof(indexed_records));
        const float tints[3][4] = {{1, 1, 1, 1}, {0.5f, 1, 0, 1}, {0, 0, 1, 1}};
        std::memcpy(data.data() + kTintA, tints[0], sizeof(tints[0]));
        std::memcpy(data.data() + kTintB, tints[1], sizeof(tints[1]));
        std::memcpy(data.data() + kTintPoison, tints[2], sizeof(tints[2]));
        const UINT poison = 0xdeadbeef, count = UINT_MAX;
        std::memcpy(data.data() + kPoison, &poison, 4);
        std::memcpy(data.data() + kCount, &count, 4);
        for (auto& v : vertices) v = {10, 10, 0};
        std::memcpy(data.data() + kBadVertices, vertices.data(), sizeof(vertices));
        void* mapped = nullptr;
        REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &mapped)) && mapped);
        std::memcpy(mapped, data.data(), data.size());
        upload->Unmap(0, nullptr);

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = texture.Height = kSize;
        texture.DepthOrArraySize = texture.MipLevels = 1;
        texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (auto* target : {&a, &b})
            REQUIRE(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target->p))));
        texture.Format = DXGI_FORMAT_D32_FLOAT;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        for (auto* target : {&depth_pass, &depth_fail})
            REQUIRE(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&target->p))));
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
        REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap.p))));
        REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap.p))));
        rtv[0] = heap_start(rtv_heap.p); rtv[1] = rtv[0];
        rtv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dsv[0] = heap_start(dsv_heap.p); dsv[1] = dsv[0];
        dsv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        device->CreateRenderTargetView(a.p, nullptr, rtv[0]);
        device->CreateRenderTargetView(b.p, nullptr, rtv[1]);
        device->CreateDepthStencilView(depth_pass.p, nullptr, dsv[0]);
        device->CreateDepthStencilView(depth_fail.p, nullptr, dsv[1]);
        const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        REQUIRE(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        list->CopyBufferRegion(args.p, 0, upload.p, 0, kBytes);
        list->CopyBufferRegion(bits.p, 0, upload.p, kZero, 4);
        list->ClearRenderTargetView(rtv[0], kBlue, 0, nullptr);
        list->ClearRenderTargetView(rtv[1], kBlack, 0, nullptr);
        list->ClearDepthStencilView(dsv[0], D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list->ClearDepthStencilView(dsv[1], D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
        const D3D12_RESOURCE_BARRIER ready[] = {
            transition(args.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            transition(bits.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(a.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(b.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(4, ready);
        REQUIRE(submit(leonida::ReplayResult::success));
        return true;
    }

    bool begin(ID3D12PipelineState* initial = nullptr) {
        REQUIRE(SUCCEEDED(allocator->Reset()));
        REQUIRE(SUCCEEDED(list->Reset(allocator.p, initial)));
        return true;
    }

    bool submit(leonida::ReplayResult expected) {
        REQUIRE(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        if (shim) REQUIRE(rt::d3d12::last_execute_result(queue.p) == expected);
        REQUIRE(SUCCEEDED(queue->Signal(fence.p, ++ticket)));
        const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        REQUIRE(done);
        const bool finished = SUCCEEDED(fence->SetEventOnCompletion(ticket, done)) &&
                              WaitForSingleObject(done, 20000) == WAIT_OBJECT_0;
        CloseHandle(done);
        REQUIRE(finished);
        return true;
    }

    void bind(bool second, UINT empty_dimension = 0, bool poison = false) {
        const auto va = upload->GetGPUVirtualAddress();
        list->SetGraphicsRootSignature(root.p);
        const float offset[2] = {poison ? 20.0f : second ? 1.0f : -1.0f, 0};
        list->SetGraphicsRoot32BitConstants(0, 2, offset, 0);
        list->SetGraphicsRootConstantBufferView(1, va + (poison ? kTintPoison : second ? kTintB : kTintA));
        list->SetGraphicsRootUnorderedAccessView(2, bits->GetGPUVirtualAddress());
        const D3D12_VERTEX_BUFFER_VIEW views[] = {
            {va + (poison ? kBadVertices : 0), sizeof(Vertex) * 16, sizeof(Vertex)},
            {va + 256, sizeof(float) * 16, sizeof(float) * 4}};
        list->IASetVertexBuffers(0, 2, views);
        const D3D12_INDEX_BUFFER_VIEW indices{va + (poison ? 448 : 384), 18, DXGI_FORMAT_R16_UINT};
        list->IASetIndexBuffer(&indices);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
        if (empty_dimension == 1) viewport.Width = 0;
        if (empty_dimension == 2) viewport.Height = 0;
        list->RSSetViewports(1, &viewport);
        const D3D12_RECT scissor{0, second ? 32 : 0, 64, second ? 64 : 32};
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv[second ? 1 : 0], FALSE, &dsv[poison ? 1 : 0]);
        list->SetPipelineState(second ? green.p : normal.p);
    }

    void copy_image(ID3D12Resource* texture, ID3D12Resource* buffer) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = buffer; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kPitch};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    bool read(Output& output) {
        REQUIRE(begin());
        copy_image(a.p, a_read.p); copy_image(b.p, b_read.p);
        list->CopyBufferRegion(bits_read.p, 0, bits.p, 0, 4);
        REQUIRE(submit(leonida::ReplayResult::success));
        for (auto pair : {std::pair{a_read.p, output.a.data()}, std::pair{b_read.p, output.b.data()}}) {
            void* mapped = nullptr;
            REQUIRE(SUCCEEDED(pair.first->Map(0, nullptr, &mapped)) && mapped);
            std::memcpy(pair.second, mapped, kSize * kPitch);
            pair.first->Unmap(0, nullptr);
        }
        void* mapped = nullptr;
        REQUIRE(SUCCEEDED(bits_read->Map(0, nullptr, &mapped)) && mapped);
        std::memcpy(&output.bits, mapped, 4);
        bits_read->Unmap(0, nullptr);
        return true;
    }

    bool valid(UINT empty_dimension, Output& output) {
        REQUIRE(begin());
        const D3D12_RESOURCE_BARRIER ready[] = {
            transition(a.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(b.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(bits.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(3, ready);
        bind(false, empty_dimension);
        list->ExecuteIndirect(draw.p, 2, args.p, kDrawOffset, nullptr, 0);
        if (!empty_dimension) {
            bind(true);
            list->ExecuteIndirect(indexed.p, 2, args.p, kIndexedOffset, nullptr, 0);
        }
        // Mutate every relevant state block after recording. Prior indirect draws must keep their own snapshot.
        bind(false, 0, true);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        list->SetGraphicsRootSignature(nullptr);
        // Windows accepts an aligned out-of-range offset for a zero-command no-op, without graphics state.
        list->ExecuteIndirect(draw.p, 0, args.p, kBytes + 4, nullptr, 0);
        const D3D12_RESOURCE_BARRIER done[] = {
            transition(a.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(b.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(bits.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(3, done);
        REQUIRE(submit(leonida::ReplayResult::success));
        REQUIRE(read(output));
        REQUIRE(equal_image(output.a, "target A", true, empty_dimension != 0));
        REQUIRE(equal_image(output.b, "target B", false, empty_dimension != 0));
        REQUIRE(output.bits == (empty_dimension ? kDrawBits : kAllBits));
        return true;
    }

    // The first submission deliberately cannot execute. A subsequent ordinary copy must observe the baseline,
    // including image layouts from before the rejected prefix. Native D3D12 is not asked to execute invalid state.
    bool refusal(const char* name, unsigned missing, bool counted, bool with_divisor,
                 leonida::ReplayResult expected) {
        REQUIRE(begin());
        auto rt = transition(a.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto writable = transition(bits.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &rt); list->ResourceBarrier(1, &writable);
        list->ClearRenderTargetView(rtv[0], kRed, 0, nullptr);
        list->CopyBufferRegion(bits.p, 0, upload.p, kPoison, 4);
        auto uav = transition(bits.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &uav);
        if (!missing) bind(false);
        else {
            // Each absence is isolated: 1 no PSO, 2 no root, 3 no topology.
            if (missing != 1) list->SetPipelineState(normal.p);
            if (missing != 2) list->SetGraphicsRootSignature(root.p);
            if (missing != 3) list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
            const D3D12_RECT scissor{0, 0, 64, 64};
            list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv[0]);
        }
        if (with_divisor) list->SetPipelineState(divisor.p);
        list->ExecuteIndirect(draw.p, 1, args.p, kDrawOffset, counted ? args.p : nullptr, counted ? kCount : 0);
        REQUIRE(submit(expected));
        Output result;
        REQUIRE(read(result));
        REQUIRE(result.bits == 0);
        REQUIRE(equal_image(result.a, name, true, true));
        REQUIRE(equal_image(result.b, name, false, true));
        std::printf("refused before prefix effects: %s\n", name);
        return true;
    }
};

bool native_shaders(Shaders& out) {
    const HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(module, "D3DCompile"));
    bool ok = compile != nullptr;
    for (unsigned stage = 0; ok && stage != 2; ++stage) {
        const char* source = stage ? native_ps : native_vs;
        Ref<ID3DBlob> blob, errors;
        ok = SUCCEEDED(compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main",
                               stage ? "ps_5_1" : "vs_5_1", D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob.p, &errors.p));
        if (!ok && errors.p) std::fprintf(stderr, "%.*s\n", int(errors->GetBufferSize()),
                                        static_cast<const char*>(errors->GetBufferPointer()));
        if (ok && blob.p) {
            const auto* start = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
            (stage ? out.ps : out.vs).assign(start, start + blob->GetBufferSize());
        } else ok = false;
    }
    FreeLibrary(module);
    return ok;
}

bool fixture_library(const std::filesystem::path& path, Shaders& out) {
    const auto vs = fssm_program({0x1d5a0001, 0xbf810000});
    const auto ps = fssm_program({0x1d5a0002, 0xbf810000});
    out.vs = shader_container(vs); out.ps = shader_container(ps);
    REQUIRE(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_INDIRECT_DRAW_VS_SPV), path / "vs.spv"));
    REQUIRE(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_INDIRECT_DRAW_PS_SPV), path / "ps.spv"));
    std::ofstream index(path / "index.json", std::ios::binary);
    index << "{\"vs\":{\"stage\":\"vs\",\"fssm_sha256\":\"" << leonida::sha256_hex(vs)
          << "\",\"bindings\":[],\"push_constant_bytes\":144,\"input_signature\":["
             "{\"semantic\":\"POSITION\",\"semantic_index\":0,\"register\":0,\"mask\":3,\"rw_mask\":3,\"system_value\":0,\"component_type\":\"float\"},"
             "{\"semantic\":\"MARKER\",\"semantic_index\":0,\"register\":1,\"mask\":1,\"rw_mask\":1,\"system_value\":0,\"component_type\":\"uint\"},"
             "{\"semantic\":\"COLOR\",\"semantic_index\":0,\"register\":2,\"mask\":15,\"rw_mask\":15,\"system_value\":0,\"component_type\":\"float\"}]},"
             "\"ps\":{\"stage\":\"ps\",\"fssm_sha256\":\"" << leonida::sha256_hex(ps)
          << "\",\"bindings\":[],\"push_constant_bytes\":144,\"outputs\":[0]}}";
    REQUIRE(index.good());
    return true;
}

bool make_layer(vktest::Device& d, const std::filesystem::path& library, bool first_instance, Ref<ID3D12Device>& out) {
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc; info.instance = d.instance; info.physical = d.physical;
    info.device = d.device; info.queue = d.queue; info.queue_family = d.family;
    info.buffer_device_address = true; info.subgroup = d.subgroup; info.shader_dir = library.string();
    info.features.drawIndirectFirstInstance = first_instance ? d.enabled.drawIndirectFirstInstance : VK_FALSE;
    info.features.vertexPipelineStoresAndAtomics = d.enabled.vertexPipelineStoresAndAtomics;
    info.vertex_attribute_divisor = d.vertex_attribute_divisor;
    ID3D12Device* inner = nullptr;
    REQUIRE(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
    const HRESULT hr = rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                       reinterpret_cast<void**>(&out.p), false);
    if (FAILED(hr)) { inner->Release(); return false; }
    rt::d3d12::set_device_outer(inner, out.p);
    return true;
}
} // namespace

int main() {
    vktest::Device d;
    d.request_buffer_device_address = true;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (!check(status == vktest::DeviceStatus::ready, "Vulkan initialization")) return 1;
    if (!d.enabled.drawIndirectFirstInstance || !d.enabled.vertexPipelineStoresAndAtomics) {
        std::fprintf(stderr, "SKIP: indirect firstInstance and vertex-stage atomics are required\n");
        return 77;
    }
    const testfiles::ScratchDir scratch("leonida_indirect_draw_state");
    Shaders layer, native;
    if (!fixture_library(scratch.path(), layer) || !native_shaders(native)) return 1;
    Ref<IDXGIFactory4> factory;
    Ref<IDXGIAdapter> warp;
    Ref<ID3D12Device> host;
    if (!check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory.p))) &&
               SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp.p))) &&
               SUCCEEDED(D3D12CreateDevice(warp.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.p))),
               "native WARP reference")) return 1;
    {
        Ref<ID3D12Device> device;
        if (!make_layer(d, scratch.path(), true, device)) return 1;
        for (UINT empty = 0; empty != 3; ++empty) {
            Output actual, reference;
            Fixture vk(device.p, true), pc(host.p, false);
            if (!vk.initialize(layer, false) || !pc.initialize(native, false) ||
                !pc.valid(empty, reference) || !vk.valid(empty, actual)) return 1;
            if (!check(actual.a == reference.a && actual.b == reference.b && actual.bits == reference.bits,
                       "WARP/Vulkan exact image and vertex-marker parity")) return 1;
        }
        const struct { const char* name; unsigned missing; bool count, divisor; } cases[] = {
            {"GPU count UINT_MAX / MaxCommandCount 1", 0, true, false},
            {"missing PSO", 1, false, false}, {"missing root", 2, false, false},
            {"missing topology", 3, false, false}, {"divisor > 1", 0, false, true}};
        for (const auto& c : cases) {
            if (c.divisor && !d.vertex_attribute_divisor) {
                std::printf("divisor extension unavailable: its existing PSO refusal covers this device\n");
                continue;
            }
            Fixture vk(device.p, true);
            const auto expected = c.missing ? leonida::ReplayResult::unresolved_resource : leonida::ReplayResult::unsupported;
            if (!vk.initialize(layer, c.divisor) || !vk.refusal(c.name, c.missing, c.count, c.divisor, expected)) return 1;
        }
    }
    {
        Ref<ID3D12Device> feature_off;
        if (!make_layer(d, scratch.path(), false, feature_off)) return 1;
        Fixture vk(feature_off.p, true);
        if (!vk.initialize(layer, false) || !vk.refusal("firstInstance feature disabled", 0, false, false,
                                                       leonida::ReplayResult::unsupported)) return 1;
    }
    if (!check(d.validation_errors() == 0, "zero Vulkan validation errors")) return 1;
    std::printf("indirect DRAW/indexed state: 3 exact WARP/Vulkan workloads; root/PSO/RTV/DSV/IA snapshots, "
                "padded GPU records, firstInstance/baseVertex, empty-viewport VS writes and pre-emit refusals pass\n");
    return 0;
}
