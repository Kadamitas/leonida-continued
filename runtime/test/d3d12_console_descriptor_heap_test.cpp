// Console Xbox CreateDescriptorHeap (issue #313 comment 7107265058) passes a 16-byte PC
// D3D12_DESCRIPTOR_HEAP_DESC followed by address at +0x10 and size at +0x18. The title asserts
// GetCPUDescriptorHandleForHeapStart() == address. This tests the explicit Leonida console-heap
// creation path used by the Xbox device proxy slot 14. It does not cover title-memory aliasing /
// section 3.3 backing.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include "../src/command_list.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace {

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** put() {
        if (p) {
            p->Release();
            p = nullptr;
        }
        return &p;
    }
    T* get() const { return p; }
    T* operator->() const { return p; }
};

bool check_console_addressed_heap(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 8;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS address = 0x112233440000ull;
    const std::uint64_t size = 8ull * 32ull;

    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = rt::d3d12::create_console_descriptor_heap(
        device, &desc, address, size, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "create_console_descriptor_heap failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr != static_cast<SIZE_T>(address)) {
        std::fprintf(stderr, "CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    if (gpu.ptr != address) {
        std::fprintf(stderr, "GPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(gpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC got = heap->GetDesc();
    if (got.Type != desc.Type || got.NumDescriptors != desc.NumDescriptors || got.Flags != desc.Flags ||
        got.NodeMask != 1) {
        std::fprintf(stderr, "GetDesc mismatch\n");
        return false;
    }
    return true;
}

bool check_xs_proxy_slot14(ID3D12Device* real_device) {
    Ref<ID3D12Device> proxy;
    real_device->AddRef();  // create_device_proxy takes ownership of one reference on success
    if (FAILED(rt::d3d12_xs::create_device_proxy(real_device, IID_PPV_ARGS(&proxy.p))) || !proxy.p) {
        real_device->Release();  // the caller keeps its reference on failure
        std::fprintf(stderr, "create_device_proxy failed\n");
        return false;
    }

    struct ConsoleDescriptorHeapDesc {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        std::uint64_t address = 0;
        std::uint64_t size = 0;
    } console{};
    console.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    console.desc.NumDescriptors = 4;
    console.desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    alignas(32) std::array<std::uint8_t, 4 * 32> title_bytes{};
    console.address = reinterpret_cast<std::uintptr_t>(title_bytes.data());
    console.size = 4ull * 32ull;

    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = proxy->CreateDescriptorHeap(&console.desc, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "proxy CreateDescriptorHeap(console) failed: 0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    const auto* slots = *reinterpret_cast<void***>(heap.get());
    const auto cpu = D3D12_CPU_DESCRIPTOR_HANDLE{reinterpret_cast<SIZE_T (*)(void*)>(slots[9])(heap.get())};
    if (cpu.ptr != static_cast<SIZE_T>(console.address)) {
        std::fprintf(stderr, "proxy CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu.ptr),
                     static_cast<unsigned long long>(console.address));
        return false;
    }
    return true;
}

bool check_normal_heap_unchanged(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 4;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "CreateDescriptorHeap(normal) failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr == 0) {
        std::fprintf(stderr, "normal heap CPU start is null\n");
        return false;
    }
    return true;
}

alignas(32) std::array<std::uint8_t, 96> hook_bytes{};
unsigned frees = 0;
void* allocate_hook_heap(std::size_t bytes, std::uint64_t) {
    return bytes == hook_bytes.size() ? hook_bytes.data() : nullptr;
}
void free_hook_heap(void* address, std::uint64_t) {
    if (address == hook_bytes.data()) ++frees;
}

bool check_hooked_visible_heap(ID3D12Device* device) {
    namespace xs = rt::d3d12_xs;
    hook_bytes.fill(0x32);
    xs::set_descriptor_heap_allocator(allocate_hook_heap, free_hook_heap);
    struct ClearHooks { ~ClearHooks() { xs::set_descriptor_heap_allocator(nullptr, nullptr); } } clear;
    Ref<ID3D12Device> proxy;
    device->AddRef();
    if (FAILED(xs::create_device_proxy(device, IID_PPV_ARGS(proxy.put()), true))) {
        device->Release();
        return false;
    }
    // The wrapper reads an optional pair of placement words. Zero both on this ordinary request.
    struct { D3D12_DESCRIPTOR_HEAP_DESC desc; std::uint64_t address, bytes; } request{};
    request.desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> wrapped;
    if (FAILED(proxy->CreateDescriptorHeap(&request.desc, IID_PPV_ARGS(wrapped.put())))) return false;
    auto* heap = xs::heap_proxy_real_heap(wrapped.get());
    const auto* mirror = rt::d3d12::descriptor_heap_mirror(heap);
    const auto* slots = *reinterpret_cast<void***>(wrapped.get());
    const auto cpu = reinterpret_cast<SIZE_T (*)(void*)>(slots[9])(wrapped.get());
    if (!mirror || mirror->bytes != hook_bytes.size() || cpu != reinterpret_cast<SIZE_T>(hook_bytes.data()) ||
        mirror->title != hook_bytes.data() || mirror->arena == mirror->title || !heap->GetGPUDescriptorHandleForHeapStart().ptr)
        return false;

    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put()))) ||
        FAILED(device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.put())))) return false;
    list->SetDescriptorHeaps(1, &heap);
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list.get()};
    const auto submit = [&] {
        queue->ExecuteCommandLists(1, lists);
        return std::memcmp(hook_bytes.data(), mirror->arena, hook_bytes.size()) == 0;
    };
    if (!submit() || hook_bytes[95] != 0x32) return false;
    D3D12_CONSTANT_BUFFER_VIEW_DESC view{0x10000, 256};
    proxy->CreateConstantBufferView(&view, {cpu});
    if (hook_bytes[0] == 0x32 || !submit()) return false;
    // Copy from ordinary CPU-only staging using device APIs, then mutate the shader-visible title bytes directly.
    D3D12_DESCRIPTOR_HEAP_DESC staging_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Ref<ID3D12DescriptorHeap> staging;
    if (FAILED(device->CreateDescriptorHeap(&staging_desc, IID_PPV_ARGS(staging.put())))) return false;
    auto from = staging->GetCPUDescriptorHandleForHeapStart();
    std::memset(reinterpret_cast<void*>(from.ptr), 0x56, 32);
    const D3D12_CPU_DESCRIPTOR_HANDLE to{cpu + 32};
    proxy->CopyDescriptors(1, &to, nullptr, 1, &from, nullptr, staging_desc.Type);
    if (hook_bytes[63] != 0x56 || !submit()) return false;
    proxy->CopyDescriptorsSimple(1, {cpu + 64}, from, staging_desc.Type);
    if (hook_bytes[95] != 0x56) return false;
    hook_bytes[63] = 0xa5;
    if (!submit()) return false;
    hook_bytes[63] = 0x56;
    // Releasing the public wrapper must keep the canonical CPU source alive for the closed list.
    wrapped.p->Release();
    wrapped.p = nullptr;
    return !frees && submit();
}

}  // namespace

int main() {
    vktest::Device vk;
    vk.request_buffer_device_address = true;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;

    Ref<ID3D12Device> device;
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    info.buffer_device_address = vk.buffer_device_address;
    if (!vk.buffer_device_address) return 77;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(device.put()))) || !device.get()) {
        std::fprintf(stderr, "create_vulkan_device failed\n");
        return 1;
    }

    if (!check_hooked_visible_heap(device.get()) || frees != 1) {
        std::fprintf(stderr, "hook-backed shader-visible descriptor heap failed (frees=%u)\n", frees);
        return 1;
    }
    if (!check_normal_heap_unchanged(device.get())) return 1;
    if (!check_console_addressed_heap(device.get())) return 1;
    if (!check_xs_proxy_slot14(device.get())) return 1;
    std::fprintf(stderr, "ok\n");
    return 0;
}
