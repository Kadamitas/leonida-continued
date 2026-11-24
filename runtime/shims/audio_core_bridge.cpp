// Local CPU audio backend. docs/audio_core_bridge.md records the measured title ABI and source provenance.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <unordered_map>

#include "audio_core_bridge.h"
#include "leonida_audio_core.h"
#include "shims.h"
#include "startup_trace.h"

namespace {

void trace(const char* name, int32_t result = 0) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (!seen.insert(name).second && result >= 0) return;
    rt::startup_trace::event("audio_core_call", "\"import\":" + rt::startup_trace::quote(name) +
        ",\"result\":" + std::to_string(result));
}

uint64_t token(const void* pointer) { return reinterpret_cast<uint64_t>(pointer); }
void* handle(uint64_t value) { return reinterpret_cast<void*>(value); }

uint32_t read32(const uint8_t* pointer, size_t offset) {
    uint32_t value;
    std::memcpy(&value, pointer + offset, sizeof(value));
    return value;
}
uintptr_t read_pointer(const uint8_t* pointer, size_t offset) {
    uintptr_t value;
    std::memcpy(&value, pointer + offset, sizeof(value));
    return value;
}

struct Hal {
    const uintptr_t* vtable;
    uint64_t backend;
};
std::mutex hal_mutex;
std::unordered_map<Hal*, std::unique_ptr<Hal>> hals;

uint64_t hal_token(Hal* self) {
    std::lock_guard lock(hal_mutex);
    const auto it = hals.find(self);
    return it == hals.end() ? 0 : it->second->backend;
}

HRESULT WINAPI hal_connect(Hal* self, uint32_t commands, uint32_t messages) {
    const auto result = laudio_hal_connect(hal_token(self), commands, messages);
    trace("IAcpHal::Connect", result);
    return result;
}
HRESULT WINAPI hal_disconnect(Hal* self) {
    const auto result = laudio_hal_disconnect(hal_token(self));
    trace("IAcpHal::Disconnect", result);
    return result;
}
HRESULT WINAPI hal_submit(Hal* self, uint32_t kind, uint64_t id, uint32_t frame, const uint8_t* payload, uint32_t fence) {
    size_t length = 0;
    switch (kind) {
        case 1: case 0x10: case 0x12: length = 8; break;
        case 2: case 5: case 7: length = 4; break;
        case 6: case 8: length = 64; break;
        case 0xe: length = 12; break;
        case 0x13: length = 24; break;
        default: break;
    }
    const auto result = laudio_hal_submit(hal_token(self), kind, id, frame, payload, length, fence);
    trace("IAcpHal::SubmitCommands", result);
    return result;
}
uint32_t WINAPI hal_pop(Hal* self, uint8_t* output) {
    uint32_t present = 0;
    const auto result = laudio_hal_pop_message(hal_token(self), output, 28, &present);
    trace("IAcpHal::PopMessage", result);
    return result < 0 ? 0 : present;
}
uint32_t WINAPI hal_count(Hal* self) {
    uint32_t count = 0;
    const auto result = laudio_hal_message_count(hal_token(self), &count);
    trace("IAcpHal::GetNumMessages", result);
    return result < 0 ? 0 : count;
}
uint32_t WINAPI hal_release(Hal* self) {
    uint64_t backend = 0;
    {
        std::lock_guard lock(hal_mutex);
        const auto it = hals.find(self);
        if (it == hals.end()) return 0;
        backend = it->second->backend;
        hals.erase(it);
    }
    const auto result = laudio_hal_release(backend);
    trace("IAcpHal::Release", result);
    return 0;
}
[[noreturn]] uintptr_t WINAPI hal_unmeasured(Hal*) {
    trace("IAcpHal::unmeasured_vtable_slot", E_NOTIMPL);
    std::fputs("[rt] unmeasured IAcpHal vtable slot; refusing an unknown ABI\n", stderr);
    std::abort();
}
const uintptr_t* hal_vtable() {
    static const uintptr_t table[] = {
        reinterpret_cast<uintptr_t>(&hal_connect), reinterpret_cast<uintptr_t>(&hal_disconnect),
        reinterpret_cast<uintptr_t>(&hal_submit), reinterpret_cast<uintptr_t>(&hal_pop),
        reinterpret_cast<uintptr_t>(&hal_count), reinterpret_cast<uintptr_t>(&hal_release),
        reinterpret_cast<uintptr_t>(&hal_unmeasured), reinterpret_cast<uintptr_t>(&hal_unmeasured),
        reinterpret_cast<uintptr_t>(&hal_unmeasured), reinterpret_cast<uintptr_t>(&hal_unmeasured),
        reinterpret_cast<uintptr_t>(&hal_unmeasured), reinterpret_cast<uintptr_t>(&hal_unmeasured)
    };
    return table;
}

HRESULT WINAPI apu_create_heap(uint32_t cached, uint32_t noncached) {
    const auto result = laudio_apu_create_heap(cached, noncached);
    trace("ApuCreateHeap", result);
    return result;
}
HRESULT WINAPI apu_alloc(void** output, uint32_t* apu, uint32_t size, uint32_t alignment, uint32_t heap) {
    const auto result = laudio_apu_alloc(output, apu, size, alignment, heap);
    trace("ApuAlloc", result);
    return result;
}
HRESULT WINAPI apu_free(void* pointer) {
    const auto result = laudio_apu_free(pointer);
    trace("ApuFree", result);
    return result;
}
uint32_t WINAPI apu_map_virtual(const void* pointer) {
    const auto result = laudio_apu_map_virtual(pointer);
    trace("ApuMapVirtualAddress");
    return result;
}
void* WINAPI apu_map_address(uint32_t apu) {
    trace("ApuMapApuAddress");
    return laudio_apu_map_address(apu);
}
bool WINAPI apu_is_valid(const void* pointer, uint32_t alignment) {
    trace("ApuIsVirtualAddressValid");
    return laudio_apu_is_valid(pointer, alignment) != 0;
}
HRESULT WINAPI shape_allocate(rt::audio_core::ShapeContexts* contexts) {
    const auto result = laudio_shape_allocate(reinterpret_cast<uint8_t*>(contexts), sizeof(*contexts));
    trace("AcpHalAllocateShapeContexts", result);
    return result;
}
HRESULT WINAPI shape_release() {
    const auto result = laudio_shape_release();
    trace("AcpHalReleaseShapeContexts", result);
    return result;
}
HRESULT WINAPI hal_create(void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    auto object = std::unique_ptr<Hal>(new (std::nothrow) Hal{hal_vtable(), 0});
    if (!object) return E_OUTOFMEMORY;
    const auto result = laudio_hal_create(&object->backend);
    trace("AcpHalCreate", result);
    if (result < 0) return result;
    *output = object.get();
    std::lock_guard lock(hal_mutex);
    hals.emplace(object.get(), std::move(object));
    return result;
}

HRESULT WINAPI xapu_connect(const rt::audio_core::XApuConfig* params, rt::audio_core::XApuRegion* region, void** output) {
    if (!output || !region) return E_POINTER;
    *output = nullptr;
    uint64_t backend = 0;
    uint64_t words[2]{};
    const auto result = laudio_xapu_connect(reinterpret_cast<const uint32_t*>(params), 6, words, 2, &backend);
    trace("XApuConnect", result);
    region->memory = handle(words[0]);
    region->result_event = handle(words[1]);
    if (result >= 0) *output = handle(backend);
    return result;
}
HRESULT WINAPI xapu_disconnect(void* value) {
    const auto result = laudio_xapu_disconnect(token(value));
    trace("XApuDisconnect", result);
    return result;
}
HRESULT WINAPI xapu_enqueue(void* value, const uint8_t* command, uint32_t* sequence) {
    if (!command) return E_POINTER;
    std::array<uint8_t, LAUDIO_XAPU_COMMAND_BYTES> local{};
    const auto kind = read32(command, 0);
    size_t size = 0;
    switch (kind) {
        case 1: size = 0x38; break;
        case 3: size = 0x40; break;
        // Stop and reset carry the complete common command ID. The result echoes its 64-bit caller context.
        case 2:
        case 5: size = 0x18; break;
        default: return E_INVALIDARG;
    }
    std::memcpy(local.data(), command, size);
    const auto* input = kind == 3 ? reinterpret_cast<const uint8_t*>(read_pointer(local.data(), 0x2c)) : nullptr;
    auto* output = kind == 3 ? reinterpret_cast<uint8_t*>(read_pointer(local.data(), 0x38)) : nullptr;
    const auto input_bytes = kind == 3 ? read32(local.data(), 0x28) : 0;
    const auto output_bytes = kind == 3 ? read32(local.data(), 0x34) : 0;
    const auto result = laudio_xapu_enqueue(token(value), local.data(), local.size(), input, input_bytes, output, output_bytes, sequence);
    trace("XApuEnqueueCommand", result);
    return result;
}
HRESULT WINAPI xapu_dequeue(void* value, uint8_t* output) {
    const auto result = laudio_xapu_dequeue(token(value), output, LAUDIO_XAPU_RESULT_BYTES);
    trace("XApuDequeueResult", result);
    return result;
}
uint32_t WINAPI xapu_count(void* value, uint32_t* dequeued) {
    uint32_t count = 0, previous = 0;
    const auto result = laudio_xapu_result_count(token(value), &count, &previous);
    if (dequeued) *dequeued = previous;
    trace("XApuGetResultCount", result);
    return result < 0 ? 0 : count;
}

HRESULT WINAPI dsp_connect(void* memory, uint32_t bytes, uint32_t activations, uint32_t streams, uint32_t priority, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    uint64_t backend = 0;
    const auto result = laudio_dsp_connect(memory, bytes, activations, streams, priority, &backend);
    if (result >= 0) *output = handle(backend);
    trace("XDspConnectWithMaximumStreamLimit", result);
    return result;
}
HRESULT WINAPI dsp_activate(void* client, const uint8_t* descriptor, uint32_t** status, void** output) {
    if (!status || !output) return E_POINTER;
    *status = nullptr;
    *output = nullptr;
    uint64_t backend = 0;
    auto result = laudio_dsp_activate(token(client), descriptor, 0x20, &backend);
    if (result >= 0) {
        result = laudio_dsp_status_pointer(backend, status);
        if (result >= 0) *output = handle(backend);
        else laudio_dsp_deactivate(backend);
    }
    trace("XDspActivate", result);
    return result;
}
HRESULT WINAPI dsp_submit(void* context, const uint8_t* command, uint32_t* sequence) {
    const auto result = laudio_dsp_submit(token(context), command, 0x20, sequence);
    trace("XDspSubmitCommand", result);
    return result;
}
HRESULT WINAPI dsp_deactivate(void* context) {
    const auto result = laudio_dsp_deactivate(token(context));
    trace("XDspDeactivate", result);
    return result;
}
HRESULT WINAPI dsp_disconnect(void* client) {
    const auto result = laudio_dsp_disconnect(token(client));
    trace("XDspDisconnect", result);
    return result;
}

}  // namespace

SHIM_TABLE(audio_core,
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&apu_create_heap)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&apu_alloc)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&apu_free)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&apu_map_virtual)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&apu_map_address)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&apu_is_valid)},
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&shape_allocate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&shape_release)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&hal_create)},
    {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&xapu_connect)},
    {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&xapu_disconnect)},
    {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&xapu_enqueue)},
    {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&xapu_dequeue)},
    {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&xapu_count)},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", reinterpret_cast<void*>(&dsp_connect)},
    {"chad.dll", "XDspActivate", reinterpret_cast<void*>(&dsp_activate)},
    {"chad.dll", "XDspSubmitCommand", reinterpret_cast<void*>(&dsp_submit)},
    {"chad.dll", "XDspDeactivate", reinterpret_cast<void*>(&dsp_deactivate)},
    {"chad.dll", "XDspDisconnect", reinterpret_cast<void*>(&dsp_disconnect)}
);
