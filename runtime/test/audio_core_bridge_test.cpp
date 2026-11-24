#include <windows.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "audio_core_bridge.h"
#include "leonida_audio_core.h"
#include "shims.h"

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", description); }
}
template<class Function> Function get(const char* dll, const char* name) {
    auto* address = rt::shims::find(dll, name);
    check(address != nullptr, name);
    return reinterpret_cast<Function>(address);
}
void put32(uint8_t* data, size_t offset, uint32_t value) { std::memcpy(data + offset, &value, 4); }
void put64(uint8_t* data, size_t offset, uint64_t value) { std::memcpy(data + offset, &value, 8); }
uint32_t read32(const uint8_t* data, size_t offset) { uint32_t result; std::memcpy(&result, data + offset, 4); return result; }

template<size_t Size> struct Guarded {
    std::array<uint8_t, 16> before;
    std::array<uint8_t, Size> data{};
    std::array<uint8_t, 16> after;
    Guarded() { before.fill(0xa5); after.fill(0x5a); }
    void verify() const {
        for (const auto value : before) check(value == 0xa5, "leading guard unchanged");
        for (const auto value : after) check(value == 0x5a, "trailing guard unchanged");
    }
};

void acphal() {
    auto create_heap = get<HRESULT(WINAPI*)(uint32_t,uint32_t)>("acphal.dll", "ApuCreateHeap");
    auto alloc = get<HRESULT(WINAPI*)(void**,uint32_t*,uint32_t,uint32_t,uint32_t)>("acphal.dll", "ApuAlloc");
    auto free = get<HRESULT(WINAPI*)(void*)>("acphal.dll", "ApuFree");
    auto to_apu = get<uint32_t(WINAPI*)(const void*)>("acphal.dll", "ApuMapVirtualAddress");
    auto to_cpu = get<void*(WINAPI*)(uint32_t)>("acphal.dll", "ApuMapApuAddress");
    auto valid = get<bool(WINAPI*)(const void*,uint32_t)>("acphal.dll", "ApuIsVirtualAddressValid");
    auto shape_alloc = get<HRESULT(WINAPI*)(rt::audio_core::ShapeContexts*)>("acphal.dll", "AcpHalAllocateShapeContexts");
    auto shape_free = get<HRESULT(WINAPI*)()>("acphal.dll", "AcpHalReleaseShapeContexts");
    auto create = get<HRESULT(WINAPI*)(void**)>("acphal.dll", "AcpHalCreate");
    check(create_heap(0x400000, 0x400000) == S_OK, "real APU heaps created");
    void* memory = nullptr;
    uint32_t address = 0;
    check(alloc(&memory, &address, 4096, 0x800, 2) == S_OK, "five-argument aligned APU allocation");
    check(memory && (reinterpret_cast<uintptr_t>(memory) & 0x7ff) == 0, "APU CPU address alignment");
    check(valid(memory, 0x800) && to_apu(memory) == address && to_cpu(address) == memory, "APU address roundtrip");
    check(to_apu(static_cast<uint8_t*>(memory)+4095) == address+4095, "interior address translation");
    check(free(memory) == S_OK && free(memory) == E_INVALIDARG, "APU free and double-free rejection");
    rt::audio_core::ShapeContexts contexts{};
    contexts.counts[4] = 350;
    check(shape_alloc(&contexts) == S_OK, "measured SHAPE context layout");
    check(contexts.virtual_addresses[4] && contexts.apu_addresses[4], "XMA context outputs written");
    void* object = nullptr;
    check(create(&object) == S_OK && object, "HAL object created");
    auto* table = *static_cast<uintptr_t**>(object);
    auto connect = reinterpret_cast<HRESULT(WINAPI*)(void*,uint32_t,uint32_t)>(table[0]);
    auto disconnect = reinterpret_cast<HRESULT(WINAPI*)(void*)>(table[1]);
    auto submit = reinterpret_cast<HRESULT(WINAPI*)(void*,uint32_t,uint64_t,uint32_t,const uint8_t*,uint32_t)>(table[2]);
    auto pop = reinterpret_cast<uint32_t(WINAPI*)(void*,uint8_t*)>(table[3]);
    auto count = reinterpret_cast<uint32_t(WINAPI*)(void*)>(table[4]);
    auto release = reinterpret_cast<uint32_t(WINAPI*)(void*)>(table[5]);
    check(connect(object, 0x1000, 0x1000) == S_OK, "HAL command/message rings created");
    const uint32_t config = 0xc0;
    check(submit(object, 2, 0xf0f0f0f0, UINT32_MAX, reinterpret_cast<const uint8_t*>(&config), 0) == S_OK, "real HAL config command");
    check(submit(object, 3, 1, 0, reinterpret_cast<const uint8_t*>(&config), 0) == E_NOTIMPL, "unknown HAL operation fails");
    check(count(object) > 0, "HAL command produced a real completion message");
    Guarded<28> message;
    check(pop(object, message.data.data()) == 1, "HAL completion dequeued");
    message.verify();
    check(disconnect(object) == S_OK, "HAL disconnected");
    check(release(object) == 0, "HAL object released");
    check(connect(object, 4, 4) == E_INVALIDARG, "stale HAL pointer rejected");
    check(shape_free() == S_OK, "SHAPE contexts released");
}

void xapu() {
    auto connect = get<HRESULT(WINAPI*)(const rt::audio_core::XApuConfig*,rt::audio_core::XApuRegion*,void**)>("xapu.dll", "XApuConnect");
    auto disconnect = get<HRESULT(WINAPI*)(void*)>("xapu.dll", "XApuDisconnect");
    auto enqueue = get<HRESULT(WINAPI*)(void*,const uint8_t*,uint32_t*)>("xapu.dll", "XApuEnqueueCommand");
    auto dequeue = get<HRESULT(WINAPI*)(void*,uint8_t*)>("xapu.dll", "XApuDequeueResult");
    auto count = get<uint32_t(WINAPI*)(void*,uint32_t*)>("xapu.dll", "XApuGetResultCount");
    const rt::audio_core::XApuConfig measured{1,256,2,0x104000,0,1};
    auto original = measured;
    rt::audio_core::XApuRegion region{};
    void* client = nullptr;
    check(connect(&measured, &region, &client) == S_OK, "measured six-DWORD input configuration accepted");
    check(std::memcmp(&measured, &original, sizeof(measured)) == 0, "connect input configuration preserved");
    check(region.memory && !region.result_event, "base/event output roles");
    check(disconnect(client) == S_OK, "measured configuration torn down");
    const rt::audio_core::XApuConfig pcm{3,2,2,0x10000,480,8};
    check(connect(&pcm, &region, &client) == S_OK && region.result_event, "real PCM converter and event created");
    std::array<uint8_t, 0x38> start{};
    put32(start.data(), 0, 1); put32(start.data(), 0x18, 1);
    put32(start.data(), 0x28, 0x3f800000); put32(start.data(), 0x2c, 480*4+2*960*4);
    uint32_t sequence = 0;
    check(enqueue(client, start.data(), &sequence) == S_OK && sequence == 1, "XAPU start command enqueued");
    std::array<uint8_t, 0x18> reset{};
    put32(reset.data(), 0, 5);
    put32(reset.data(), 0xc, 1);
    put64(reset.data(), 0x10, 0x1122334455667788ull);
    check(enqueue(client, reset.data(), nullptr) == S_OK, "null sequence output accepted");
    check(enqueue(client, reset.data(), nullptr) == static_cast<HRESULT>(0x8ac81000u), "queue depth genuinely enforced");
    check(laudio_xapu_wait_idle(reinterpret_cast<uint64_t>(client)) == 0, "host workers completed");
    uint32_t previous = 99;
    check(count(client, &previous) == 2 && previous == 0, "result count and optional dequeued output");
    Guarded<0x50> result;
    check(dequeue(client, result.data.data()) == S_OK && read32(result.data.data(), 0) == 1, "first ordered result");
    check(dequeue(client, result.data.data()) == S_OK && read32(result.data.data(), 0) == 5, "second ordered result");
    check(std::memcmp(result.data.data(), reset.data(), reset.size()) == 0, "reset echoes the full command ID");
    result.verify();
    std::vector<float> input(960), output(482, 9876.0f);
    for (size_t i = 0; i < input.size(); ++i) input[i] = std::sin(static_cast<float>(i)*0.013f)*0.5f;
    std::array<uint8_t, 0x40> command{};
    put32(command.data(),0,3); put32(command.data(),0x1c,0x3f800000); put32(command.data(),0x24,960);
    put32(command.data(),0x28,static_cast<uint32_t>(input.size()*4)); put64(command.data(),0x2c,reinterpret_cast<uint64_t>(input.data()));
    put32(command.data(),0x34,480*4); put64(command.data(),0x38,reinterpret_cast<uint64_t>(output.data()+1));
    check(enqueue(client, command.data(), nullptr) == S_OK, "packed PCM command accepted");
    check(laudio_xapu_wait_idle(reinterpret_cast<uint64_t>(client)) == 0, "PCM worker completed");
    check(dequeue(client, result.data.data()) == S_OK && read32(result.data.data(),0x2c) == 0, "PCM command completed successfully");
    check(read32(result.data.data(),0x30) == 480, "actual PCM frames produced");
    for (size_t i = 0; i < 480; ++i) check(output[i+1] == (i < 5 ? 0.0f : input[i-5]), "PCM identity with five-sample filter delay");
    check(output.front() == 9876.0f && output.back() == 9876.0f, "PCM output guards");
    command[0] = 7;
    check(enqueue(client, command.data(), nullptr) == E_INVALIDARG, "unknown command rejected");
    auto stop = reset;
    put32(stop.data(), 0, 2);
    check(enqueue(client, stop.data(), nullptr) == S_OK, "stop command accepted");
    check(laudio_xapu_wait_idle(reinterpret_cast<uint64_t>(client)) == 0, "stop command completed");
    check(dequeue(client, result.data.data()) == S_OK && std::memcmp(result.data.data(), stop.data(), stop.size()) == 0,
          "stop echoes the full command ID");
    check(disconnect(client) == S_OK && disconnect(client) == E_INVALIDARG, "XAPU teardown retires handle");
}

void dsp() {
    auto connect = get<HRESULT(WINAPI*)(void*,uint32_t,uint32_t,uint32_t,uint32_t,void**)>("chad.dll", "XDspConnectWithMaximumStreamLimit");
    auto activate = get<HRESULT(WINAPI*)(void*,const uint8_t*,uint32_t**,void**)>("chad.dll", "XDspActivate");
    auto submit = get<HRESULT(WINAPI*)(void*,const uint8_t*,uint32_t*)>("chad.dll", "XDspSubmitCommand");
    auto deactivate = get<HRESULT(WINAPI*)(void*)>("chad.dll", "XDspDeactivate");
    auto disconnect = get<HRESULT(WINAPI*)(void*)>("chad.dll", "XDspDisconnect");
    std::vector<float> memory(0x8000), source(512);
    for (size_t i = 0; i < source.size(); ++i) memory[i] = source[i] = std::sin(static_cast<float>(i)*0.13f);
    void* client = nullptr;
    check(connect(memory.data(), static_cast<uint32_t>(memory.size()*4), 2, 0x80, 1, &client) == S_OK, "six-argument DSP client connection");
    std::array<uint8_t, 32> descriptor{};
    put32(descriptor.data(),0,2); put32(descriptor.data(),8,512); put32(descriptor.data(),12,1);
    void *fft = nullptr, *ifft = nullptr;
    uint32_t *status = nullptr, *inverse_status = nullptr;
    check(activate(client,descriptor.data(),&status,&fft) == S_OK && status && fft, "four-argument activation output roles");
    put32(descriptor.data(),0,3);
    check(activate(client,descriptor.data(),&inverse_status,&ifft) == S_OK, "inverse FFT activated");
    std::array<uint8_t,32> command{};
    put32(command.data(),0,0x3f800000); put32(command.data(),4,0x3f800000);
    put64(command.data(),8,reinterpret_cast<uint64_t>(memory.data())); put64(command.data(),16,reinterpret_cast<uint64_t>(memory.data()+0x1000));
    uint32_t sequence = 0;
    check(submit(fft,command.data(),&sequence) == S_OK && sequence == 1, "third DSP argument is sequence output");
    check(status[0] == sequence && status[1] == sequence, "borrowed status reflects real processing");
    put64(command.data(),8,reinterpret_cast<uint64_t>(memory.data()+0x1000)); put64(command.data(),16,reinterpret_cast<uint64_t>(memory.data()+0x2000));
    check(submit(ifft,command.data(),&sequence) == S_OK, "inverse FFT processed");
    for (size_t i = 0; i < source.size(); ++i) check(std::fabs(memory[0x2000+i]-source[i]) < 1e-5f, "FFT roundtrip numerical output");
    check(disconnect(client) == static_cast<HRESULT>(0x8ac810a4u), "live contexts prevent client destruction");
    check(deactivate(fft) == S_OK && deactivate(ifft) == S_OK, "DSP contexts released");
    std::vector<float> ir(2*512*9, 0.0f);
    ir[0] = ir[1] = 0.5f;
    for (size_t k = 1; k < 512; ++k) ir[2*k] = 0.5f;
    put32(descriptor.data(),0,1); put32(descriptor.data(),16,static_cast<uint32_t>(ir.size()));
    put64(descriptor.data(),20,reinterpret_cast<uint64_t>(ir.data()));
    void* convolution = nullptr;
    check(activate(client,descriptor.data(),&status,&convolution) == S_OK, "external impulse-response input copied at activation");
    put64(command.data(),8,reinterpret_cast<uint64_t>(memory.data())); put64(command.data(),16,reinterpret_cast<uint64_t>(memory.data()+0x1000));
    check(submit(convolution,command.data(),&sequence) == S_OK, "convolution command processed");
    for (size_t i = 0; i < source.size(); ++i) check(std::fabs(memory[0x1000+i]-0.5f*source[i]) < 1e-5f, "convolution matches scaled impulse");
    check(deactivate(convolution) == S_OK && disconnect(client) == S_OK, "DSP lifecycle complete");
    check(deactivate(convolution) == E_INVALIDARG, "stale DSP context rejected");
}
}  // namespace

int main() {
    check(laudio_abi_version() == LAUDIO_ABI_VERSION, "backend ABI version");
    acphal();
    xapu();
    dsp();
    std::printf("audio_core_bridge_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
