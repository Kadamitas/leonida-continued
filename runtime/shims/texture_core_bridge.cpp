// Measured two-import XG boundary; the pinned backend owns the actual layout object and CPU methods.
#include <windows.h>
#include <array>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include "texture_core_bridge.h"
#include "leonida_texture_core.h"
#include "shims.h"
#include "startup_trace.h"

namespace {

bool copy_guest(void* destination, const void* source, size_t bytes) {
    if (!destination || !source) return false;
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void trace(const char* name, HRESULT result) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (!seen.insert(name).second && SUCCEEDED(result)) return;
    rt::startup_trace::event("texture_core_call", "\"import\":" + rt::startup_trace::quote(name) +
        ",\"result\":" + std::to_string(static_cast<int32_t>(result)));
}

void release_created(void* object) {
    if (!object) return;
    const auto* table = *static_cast<const uintptr_t* const*>(object);
    reinterpret_cast<uint32_t(WINAPI*)(void*)>(table[1])(object);
}

HRESULT WINAPI create_texture_computer(const void* descriptor, void** output) {
    std::array<uint8_t, rt::texture_core::descriptor_bytes> local{};
    void* object = nullptr;
    HRESULT result = E_POINTER;
    if (copy_guest(local.data(), descriptor, local.size()))
        result = lxg_create_texture_computer(local.data(), local.size(), &object);
    if (!copy_guest(output, &object, sizeof(object))) {
        release_created(object);
        result = E_POINTER;
    }
    trace("XGCreateTextureComputer", result);
    return result;
}

HRESULT WINAPI suggest_swizzle(const void* descriptor, uintptr_t flags, uint32_t* output) {
    std::array<uint8_t, rt::texture_core::descriptor_bytes> local{};
    if (!copy_guest(local.data(), descriptor, local.size())) {
        trace("XGSuggestSwizzleMode", E_POINTER);
        return E_POINTER;
    }
    uint32_t layout = 0;
    auto result = static_cast<HRESULT>(lxg_suggest_swizzle(local.data(), local.size(), flags, &layout));
    // Actual title calls alias output with descriptor+0x2c: copy the input before publishing the result.
    if (SUCCEEDED(result) && !copy_guest(output, &layout, sizeof(layout))) result = E_POINTER;
    trace("XGSuggestSwizzleMode", result);
    return result;
}
}  // namespace

SHIM_TABLE(texture_core,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&create_texture_computer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&suggest_swizzle)}
);
