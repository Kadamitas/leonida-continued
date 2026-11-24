#pragma once
#include <cstddef>
#include <cstdint>

namespace rt::texture_core {
inline constexpr size_t descriptor_bytes = 56;
inline constexpr size_t resource_layout_bytes = 0x1b40;

struct PackedMipInfo {
    uint8_t standard_mips, packed_mips;
    uint8_t reserved[2];
    uint32_t packed_tiles, start_tile;
};
struct TileShape { uint32_t width, height, depth; };
struct SubresourceTiling {
    uint32_t width;
    uint16_t height, depth;
    uint32_t start_tile;
};
static_assert(sizeof(PackedMipInfo) == 12);
static_assert(sizeof(TileShape) == 12);
static_assert(sizeof(SubresourceTiling) == 12);
}  // namespace rt::texture_core
