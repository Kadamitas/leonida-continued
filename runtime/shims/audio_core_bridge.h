#pragma once
#include <cstddef>
#include <cstdint>

namespace rt::audio_core {

struct ShapeContexts {
    uint32_t counts[6];
    void* virtual_addresses[6];
    uint32_t apu_addresses[6];
};
static_assert(sizeof(ShapeContexts) == 0x60);
static_assert(offsetof(ShapeContexts, virtual_addresses) == 0x18);
static_assert(offsetof(ShapeContexts, apu_addresses) == 0x48);

struct XApuConfig {
    uint32_t codec, streams, depth, memory_bytes, frame, options;
};
static_assert(sizeof(XApuConfig) == 24);

struct XApuRegion {
    void* memory;
    void* result_event;
};
static_assert(sizeof(XApuRegion) == 16);

}  // namespace rt::audio_core
