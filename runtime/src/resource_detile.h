#pragma once

// Detiling and tiling for AMD GFX10 / RDNA2 console-tiled resources (docs/d3d12_vulkan.md section 4.2).
//
// Resources created through Xbox slot 76 carry layout 0x101 (kXboxTiledLayout).
// The data layout inside each 64 KiB tile is GFX10 SW_64KB_D (Mesa addrlib GFX10_SW_64K_D_RBPLUS_PATINFO):
// 64 KiB macro-blocks are laid out in row-major order across the surface, and intra-tile coordinates (x, y)
// are swizzled into byte offsets via bit-permutation equations matching RDNA2 / Xbox Series hardware.
//
// Independent X and Y coordinate bit mappings permit O(1) intra-tile offset resolution via compile-time
// lookup tables (lut_x[x % tile_w] | lut_y[y % tile_h]). No Windows or Vulkan dependency.

#include "resource_tiling.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>

namespace leonida {

// The Xbox tiled layout word (device slot 76, docs/d3d12_vulkan.md section 4.2).
inline constexpr std::uint32_t xbox_tiled_layout = 0x101;

constexpr bool is_xbox_tiled(std::uint32_t layout) {
    return layout == xbox_tiled_layout;
}

// Intra-tile bit permutation equations for 64 KiB tiles (Mesa addrlib GFX10_SW_64K_D_RBPLUS_PATINFO).
// For each bit 0..15:
//   s > 0: bit is ((x >> (s - 1)) & 1)
//   s < 0: bit is ((y >> (-s - 1)) & 1)
//   s == 0: intra-element byte bit (element_bytes > 1)
template <std::uint32_t ElementBytes>
struct SwizzleLut64K {
    static constexpr std::uint32_t bpe = ElementBytes;
    static constexpr TileShape shape = element_tile_shape(false, bpe);
    static constexpr std::uint32_t tile_w = shape.width;
    static constexpr std::uint32_t tile_h = shape.height;

    std::array<std::uint16_t, tile_w> lut_x{};
    std::array<std::uint16_t, tile_h> lut_y{};

    constexpr SwizzleLut64K() {
        constexpr std::array<int8_t, 16> pat = []() {
            if constexpr (bpe == 1) {
                return std::array<int8_t, 16>{1, 2, 3, -2, -1, -3, 4, -4, -5, 5, -6, 6, -7, 7, -8, 8};
            } else if constexpr (bpe == 2) {
                return std::array<int8_t, 16>{0, 1, 2, 3, -1, -2, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8};
            } else if constexpr (bpe == 4) {
                // RDNA2/RbPlus SW_64KB_D: bits 6..7 swapped vs Navi 1x ((3, -3) instead of (-3, 3))
                return std::array<int8_t, 16>{0, 0, 1, 2, -1, -2, 3, -3, -4, 4, -5, 5, -6, 6, -7, 7};
            } else if constexpr (bpe == 8) {
                return std::array<int8_t, 16>{0, 0, 0, 1, -1, 2, 3, -2, -3, 4, -4, 5, -5, 6, -6, 7};
            } else if constexpr (bpe == 16) {
                return std::array<int8_t, 16>{0, 0, 0, 0, 1, -1, 2, -2, -3, 3, -4, 4, -5, 5, -6, 6};
            } else {
                return std::array<int8_t, 16>{};
            }
        }();

        for (std::uint32_t x = 0; x < tile_w; ++x) {
            std::uint16_t val = 0;
            for (std::uint32_t bit = 0; bit < 16; ++bit) {
                const int8_t s = pat[bit];
                if (s > 0) {
                    val |= static_cast<std::uint16_t>(((x >> (s - 1)) & 1) << bit);
                }
            }
            lut_x[x] = val;
        }

        for (std::uint32_t y = 0; y < tile_h; ++y) {
            std::uint16_t val = 0;
            for (std::uint32_t bit = 0; bit < 16; ++bit) {
                const int8_t s = pat[bit];
                if (s < 0) {
                    val |= static_cast<std::uint16_t>(((y >> (-s - 1)) & 1) << bit);
                }
            }
            lut_y[y] = val;
        }
    }
};

inline constexpr SwizzleLut64K<1> swizzle_lut_64k_1bpe{};
inline constexpr SwizzleLut64K<2> swizzle_lut_64k_2bpe{};
inline constexpr SwizzleLut64K<4> swizzle_lut_64k_4bpe{};
inline constexpr SwizzleLut64K<8> swizzle_lut_64k_8bpe{};
inline constexpr SwizzleLut64K<16> swizzle_lut_64k_16bpe{};

namespace detail {

constexpr bool checked_mul(std::size_t a, std::size_t b, std::size_t& out) {
    if (a != 0 && b > (std::numeric_limits<std::size_t>::max)() / a) return false;
    out = a * b;
    return true;
}

struct SurfaceLayout64K {
    std::uint32_t blocks_w = 0;
    std::uint32_t blocks_h = 0;
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    std::size_t linear_bytes = 0;
    std::size_t tiled_bytes = 0;
};

constexpr bool compute_surface_layout_64k(std::uint32_t width, std::uint32_t height,
                                          std::uint32_t element_bytes, std::uint32_t block_dim,
                                          SurfaceLayout64K& out) {
    if (!width || !height || (block_dim != 1 && block_dim != 4)) return false;
    const TileShape shape = element_tile_shape(false, element_bytes);
    if (!shape.width || !shape.height) return false;

    // Overflow-free ceiling division for positive uint32_t operands: 1 + (n - 1) / d.
    const std::uint32_t blocks_w = 1u + (width - 1u) / block_dim;
    const std::uint32_t blocks_h = 1u + (height - 1u) / block_dim;
    const std::uint32_t tiles_x = 1u + (blocks_w - 1u) / shape.width;
    const std::uint32_t tiles_y = 1u + (blocks_h - 1u) / shape.height;

    std::size_t block_count = 0;
    std::size_t linear_bytes = 0;
    std::size_t tile_count = 0;
    std::size_t tiled_bytes = 0;
    if (!checked_mul(blocks_w, blocks_h, block_count) ||
        !checked_mul(block_count, element_bytes, linear_bytes) ||
        !checked_mul(tiles_x, tiles_y, tile_count) ||
        !checked_mul(tile_count, tile_size_in_bytes, tiled_bytes)) {
        return false;
    }

    out = {blocks_w, blocks_h, tiles_x, tiles_y, linear_bytes, tiled_bytes};
    return true;
}

inline bool spans_overlap(const std::byte* a, std::size_t a_len,
                          const std::byte* b, std::size_t b_len) {
    if (!a_len || !b_len) return false;
    const std::less<const std::byte*> lt{};
    return lt(a, b + b_len) && lt(b, a + a_len);
}

struct VolumeTile4KS {
    std::uint32_t width, height, depth;
    std::array<std::uint16_t, 4> x, y, z;
};

// AMD PAL c5e800072a32f68b6ccc4422936d96167c6e0728, ComputeThickBlockDimension and
// gfx10SwizzlePattern.h GFX10_SW_4K_S3[_RBPLUS]_PATINFO. Indexed by log2(bytes per element).
inline constexpr std::array<VolumeTile4KS, 5> volume_tiles_4ks{{
    {16, 16, 16, {1, 2, 64, 512}, {8, 32, 256, 2048}, {4, 16, 128, 1024}},
    { 8, 16, 16, {2, 64, 512, 0}, {8, 32, 256, 2048}, {4, 16, 128, 1024}},
    { 8, 16,  8, {4, 64, 512, 0}, {8, 32, 256, 2048}, {16, 128, 1024, 0}},
    { 8,  8,  8, {8, 64, 512, 0}, {32, 256, 2048, 0}, {16, 128, 1024, 0}},
    { 4,  8,  8, {64, 512, 0, 0}, {32, 256, 2048, 0}, {16, 128, 1024, 0}},
}};

inline constexpr const VolumeTile4KS* volume_tile_4ks(std::uint32_t element_bytes) {
    if (element_bytes > 16 || !std::has_single_bit(element_bytes)) return nullptr;
    return &volume_tiles_4ks[std::countr_zero(element_bytes)];
}

inline constexpr std::uint32_t volume_axis_offset(std::uint32_t coordinate,
                                                   const std::array<std::uint16_t, 4>& masks) {
    return ((coordinate & 1u) ? masks[0] : 0u) | ((coordinate & 2u) ? masks[1] : 0u) |
           ((coordinate & 4u) ? masks[2] : 0u) | ((coordinate & 8u) ? masks[3] : 0u);
}

}  // namespace detail

// Single-mip, single-sample 3D SW_4KB_S. Each axis rounds up to whole 4 KiB blocks. Zero means invalid or overflow.
inline constexpr std::size_t volume_4ks_size(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                             std::uint32_t element_bytes) {
    const auto* tile = detail::volume_tile_4ks(element_bytes);
    if (!tile || !width || !height || !depth) return 0;
    const std::size_t tiles_x = 1u + (width - 1u) / tile->width;
    const std::size_t tiles_y = 1u + (height - 1u) / tile->height;
    const std::size_t tiles_z = 1u + (depth - 1u) / tile->depth;
    std::size_t tiles_xy = 0, tiles = 0, bytes = 0;
    return detail::checked_mul(tiles_x, tiles_y, tiles_xy) &&
           detail::checked_mul(tiles_xy, tiles_z, tiles) && detail::checked_mul(tiles, 4096, bytes) ? bytes : 0;
}

// Thick blocks run X, then Y, then Z. This equation is for uncompressed texels with no pipe/bank XOR.
inline bool detile_volume_4ks(std::span<const std::byte> tiled, std::span<std::byte> linear,
                             std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                             std::uint32_t element_bytes) {
    const auto* tile = detail::volume_tile_4ks(element_bytes);
    const auto tiled_bytes = volume_4ks_size(width, height, depth, element_bytes);
    std::size_t pixels_xy = 0, pixels = 0, linear_bytes = 0;
    if (!tiled_bytes || !detail::checked_mul(width, height, pixels_xy) ||
        !detail::checked_mul(pixels_xy, depth, pixels) || !detail::checked_mul(pixels, element_bytes, linear_bytes) ||
        tiled.size() < tiled_bytes || linear.size() < linear_bytes ||
        detail::spans_overlap(tiled.data(), tiled_bytes, linear.data(), linear_bytes)) return false;
    const std::size_t tiles_x = 1u + (width - 1u) / tile->width;
    const std::size_t tiles_y = 1u + (height - 1u) / tile->height;
    for (std::uint32_t z = 0; z < depth; ++z) {
        const auto slice = std::size_t(z / tile->depth) * tiles_y;
        const auto z_offset = detail::volume_axis_offset(z, tile->z);
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto row = (slice + y / tile->height) * tiles_x;
            const auto yz_offset = detail::volume_axis_offset(y, tile->y) | z_offset;
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto block = row + x / tile->width;
                const auto intra = detail::volume_axis_offset(x, tile->x) | yz_offset;
                const auto destination = ((std::size_t(z) * height + y) * width + x) * element_bytes;
                std::memcpy(linear.data() + destination, tiled.data() + block * 4096 + intra, element_bytes);
            }
        }
    }
    return true;
}

namespace detail {

struct SurfaceTile4KS {
    std::uint32_t width, height, micro_width, micro_height;
    std::array<std::uint16_t, 6> x, y;
};

// GFX10_SW_4K_S[_RBPLUS]_PATINFO: byte-offset masks for coordinate bits, by log2(BPE).
inline constexpr std::array<SurfaceTile4KS, 5> surface_tiles_4ks{{
    {64, 64, 16, 16, {1, 2, 4, 8, 512, 2048}, {16, 32, 64, 128, 256, 1024}},
    {64, 32, 16,  8, {2, 4, 8, 128, 512, 2048}, {16, 32, 64, 256, 1024, 0}},
    {32, 32,  8,  8, {4, 8, 128, 512, 2048, 0}, {16, 32, 64, 256, 1024, 0}},
    {32, 16,  8,  4, {8, 64, 128, 512, 2048, 0}, {16, 32, 256, 1024, 0, 0}},
    {16, 16,  4,  4, {64, 128, 512, 2048, 0, 0}, {16, 32, 256, 1024, 0, 0}},
}};

inline constexpr const SurfaceTile4KS* surface_tile_4ks(std::uint32_t element_bytes) {
    if (element_bytes > 16 || !std::has_single_bit(element_bytes)) return nullptr;
    return &surface_tiles_4ks[std::countr_zero(element_bytes)];
}

inline constexpr std::uint32_t surface_axis_offset(std::uint32_t coordinate,
                                                   const std::array<std::uint16_t, 6>& masks) {
    std::uint32_t offset = 0;
    for (std::uint32_t bit = 0; bit < masks.size(); ++bit)
        if (coordinate & (1u << bit)) offset |= masks[bit];
    return offset;
}

// Every standard 4-KiB element pattern stores 16 consecutive row bytes before its Y bits.
// Mip-tail origins are whole microtiles and therefore also start on a 16-byte run.
inline constexpr auto surface_columns_4ks = [] {
    std::array<std::array<std::uint16_t, 64>, 5> columns{};
    for (std::size_t i = 0; i < columns.size(); ++i)
        for (std::uint32_t x = 0; x < surface_tiles_4ks[i].width; ++x)
            columns[i][x] = static_cast<std::uint16_t>(surface_axis_offset(x, surface_tiles_4ks[i].x));
    return columns;
}();

inline void copy_surface_rows_4ks(const std::byte* tiled, std::byte* linear, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t padded_width, std::uint32_t origin_x,
                                  std::uint32_t origin_y, std::uint32_t element_bytes) {
    const auto index = std::countr_zero(element_bytes);
    const auto& tile = surface_tiles_4ks[index];
    const auto& columns = surface_columns_4ks[index];
    const std::size_t tiles_per_row = padded_width / tile.width;
    const auto elements_per_run = 16u / element_bytes;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto row_base = std::size_t(y / tile.height) * tiles_per_row * 4096 +
                              surface_axis_offset(y + origin_y, tile.y);
        auto* dst = linear + std::size_t(y) * width * element_bytes;
        const auto* row = tiled + row_base;
        for (std::uint32_t x = 0; x < width;) {
            const auto* src = row + std::size_t(x / tile.width) * 4096 + columns[(x + origin_x) % tile.width];
            if (width - x < elements_per_run) {
                std::memcpy(dst, src, (width - x) * element_bytes);
                break;
            }
            std::memcpy(dst, src, 16);
            dst += 16;
            x += elements_per_run;
        }
    }
}

}  // namespace detail

// GFX10 non-XOR SW_4KB_S, R8, whole 64x64 blocks. AMD AddrLib's standard micro tile maps the first eight
// address bits to x0..x3,y0..y3, then alternates y4,x4,y5,x5. These coordinate definitions are corroborated by
// Marvisak/VIctory's texture.rs at 35b054ec (GPL-3.0-or-later), based on AMD's MIT-licensed AddrLib tables.
inline bool detile_r8_4ks(std::span<const std::byte> source, std::span<std::byte> destination,
                         std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width % 64 || height % 64 ||
        std::uint64_t(width) * height != source.size() || source.size() != destination.size()) return false;
    if (!detail::spans_overlap(source.data(), source.size(), destination.data(), destination.size())) {
        detail::copy_surface_rows_4ks(source.data(), destination.data(), width, height, width, 0, 0, 1);
        return true;
    }
    // Preserve the bytewise order of the original atlas API for overlapping spans.
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t block = std::size_t(y / 64) * (width / 64) + x / 64;
            const std::size_t intra = (x & 15) | ((y & 15) << 4) | ((y & 16) << 4) |
                                      ((x & 16) << 5) | ((y & 32) << 5) | ((x & 32) << 6);
            destination[std::size_t(y) * width + x] = source[block * 4096 + intra];
        }
    return true;
}

struct Surface4ksMip {
    std::uint32_t width = 0, height = 0;
    std::uint32_t depth = 1;
    std::size_t packed_offset = 0, packed_bytes = 0;
    // For a tail mip this is its origin's byte offset in the shared first 4 KiB block of a depth slab.
    std::size_t tiled_offset = 0;
    std::uint32_t blocks_width = 0, blocks_height = 0;
    std::uint32_t padded_blocks_width = 0, padded_blocks_height = 0;
    std::uint32_t padded_depth = 1;
    std::uint32_t tail_x = 0, tail_y = 0;
    std::uint32_t tail_z = 0;
    bool in_tail = false;
};

struct Surface4ksLayout {
    std::size_t tiled_bytes = 0, packed_bytes = 0;
    std::uint32_t tile_bytes = 4096;
    // 3D only: the complete mip-chain stride for one thick tile's depth. Zero for a 2D surface.
    std::size_t volume_slab_bytes = 0;
    std::uint32_t mip_count = 0, first_tail = 0, element_bytes = 0, block_dim = 0;
    std::array<Surface4ksMip, 15> mips{};
};

// AMD PAL c5e800072a32f68b6ccc4422936d96167c6e0728: GetMipTailDim,
// GetMaxNumMipsInTail and ComputeSurfaceInfoMacroTiled, plus GFX10_SW_4K_S[_RBPLUS]_PATINFO.
// Independent implementation of the standard thin 4 KiB layout. One element is a texel (block_dim1),
// or a whole 4x4 BC block (block_dim4). No decoding, XOR, metadata plane, array or MSAA is inferred here.
inline constexpr std::optional<Surface4ksLayout> sampled_surface_layout(
    std::uint32_t width, std::uint32_t height, std::uint32_t mip_count, std::uint32_t element_bytes,
    std::uint32_t block_dim, bool display_64k) {
    const auto* tile = detail::surface_tile_4ks(element_bytes);
    if (!width || !height || width > 16384 || height > 16384 || !mip_count || mip_count > 15 ||
        mip_count > std::bit_width(std::max(width, height)) || !tile ||
        (block_dim != 1 && block_dim != 4) || (block_dim == 4 && element_bytes != 8 && element_bytes != 16))
        return std::nullopt;
    if (display_64k && block_dim != 4 && element_bytes != 4) return std::nullopt;
    const auto display_shape = element_tile_shape(false, element_bytes);
    const auto block_width = display_64k ? display_shape.width : tile->width;
    const auto block_height = display_64k ? display_shape.height : tile->height;
    const auto tail_levels = display_64k ? 12u : 8u;
    const auto micro_width = tile->micro_width, micro_height = tile->micro_height;
    const std::size_t upload_alignment = std::max(4u, element_bytes);
    constexpr auto size_limit = (std::numeric_limits<std::size_t>::max)();
    const std::uint32_t base_width = 1u + (width - 1u) / block_dim;
    const std::uint32_t base_height = 1u + (height - 1u) / block_dim;
    Surface4ksLayout out{};
    out.tile_bytes = display_64k ? 65536 : 4096;
    out.mip_count = mip_count;
    out.first_tail = mip_count;
    out.element_bytes = element_bytes;
    out.block_dim = block_dim;
    for (std::uint32_t i = 0; i < mip_count; ++i) {
        auto& mip = out.mips[i];
        mip.width = std::max(1u, width >> i);
        mip.height = std::max(1u, height >> i);
        mip.blocks_width = 1u + (mip.width - 1u) / block_dim;
        mip.blocks_height = 1u + (mip.height - 1u) / block_dim;
        std::size_t count = 0;
        if (!detail::checked_mul(mip.blocks_width, mip.blocks_height, count) ||
            !detail::checked_mul(count, element_bytes, mip.packed_bytes))
            return std::nullopt;
        const auto remainder = mip.packed_bytes % upload_alignment;
        const auto padding = remainder ? upload_alignment - remainder : 0;
        if (mip.packed_bytes > size_limit - padding ||
            mip.packed_bytes + padding > size_limit - out.packed_bytes) return std::nullopt;
        mip.packed_offset = out.packed_bytes;
        out.packed_bytes += mip.packed_bytes + padding;
        // AddrLib downsizes the base element dimensions with ceiling division, which may differ from
        // ceil(native pixel mip / block_dim) for an odd-sized texture. Preserve that source padding.
        const auto divisor = 1u << i;
        const auto source_width = 1u + (base_width - 1u) / divisor;
        const auto source_height = 1u + (base_height - 1u) / divisor;
        if (mip_count > 1 && out.first_tail == mip_count && mip_count - i <= tail_levels &&
            source_width <= block_width / 2 && source_height <= block_height)
            out.first_tail = i;
        if (i < out.first_tail) {
            mip.padded_blocks_width = (1u + (source_width - 1u) / block_width) * block_width;
            mip.padded_blocks_height = (1u + (source_height - 1u) / block_height) * block_height;
        }
    }
    // All tail levels share one block at offset zero. Larger mips follow it in increasing physical size.
    std::size_t offset = out.first_tail < mip_count ? out.tile_bytes : 0;
    for (std::uint32_t i = out.first_tail; i-- > 0;) {
        auto& mip = out.mips[i];
        std::size_t count = 0, bytes = 0;
        if (!detail::checked_mul(mip.padded_blocks_width, mip.padded_blocks_height, count) ||
            !detail::checked_mul(count, element_bytes, bytes) ||
            bytes > (std::numeric_limits<std::size_t>::max)() - offset) return std::nullopt;
        mip.tiled_offset = offset;
        offset += bytes;
    }
    out.tiled_bytes = offset;
    for (std::uint32_t i = out.first_tail; i < mip_count; ++i) {
        auto& mip = out.mips[i];
        const auto relative = i - out.first_tail;
        const auto m = tail_levels - 1u - relative;
        const auto tail_offset = m > 6 ? 16u << m : m << 8;
        mip.in_tail = true;
        mip.tiled_offset = tail_offset;
        mip.padded_blocks_width = std::max((block_width / 2) >> relative, micro_width);
        mip.padded_blocks_height = std::max(block_height >> relative, micro_height);
        // Address bits above the 256-byte micro block alternate Y,X for both even-log2 tile sizes.
        const auto axis = [tail_offset](unsigned first) {
            const auto bits = tail_offset >> first;
            return (bits & 1u) | ((bits >> 1) & 2u) | ((bits >> 2) & 4u) | ((bits >> 3) & 8u);
        };
        const auto micro_x = axis(9), micro_y = axis(8);
        mip.tail_x = micro_x * micro_width;
        mip.tail_y = micro_y * micro_height;
    }
    return out;
}

inline constexpr std::optional<Surface4ksLayout> surface_4ks_layout(
    std::uint32_t width, std::uint32_t height, std::uint32_t mip_count, std::uint32_t element_bytes,
    std::uint32_t block_dim) {
    return sampled_surface_layout(width, height, mip_count, element_bytes, block_dim, false);
}

// SW_64KB_D uses the same thin tail placement as SW_4KB_S, with twelve tail levels and the
// display swizzle LUT at each element. The packed destination has one region per native mip.
inline bool detile_display_surface(std::span<const std::byte> tiled, std::span<std::byte> packed,
                                  const Surface4ksLayout& layout) {
    const bool rgba8 = layout.block_dim == 1 && layout.element_bytes == 4;
    const bool compressed = layout.block_dim == 4 && (layout.element_bytes == 8 || layout.element_bytes == 16);
    if (layout.tile_bytes != 65536 || (!rgba8 && !compressed) ||
        tiled.size() < layout.tiled_bytes || packed.size() < layout.packed_bytes ||
        detail::spans_overlap(tiled.data(), layout.tiled_bytes, packed.data(), layout.packed_bytes)) return false;
    const auto copy = [&](const auto& lut) {
        for (std::uint32_t i = 0; i < layout.mip_count; ++i) {
            const auto& mip = layout.mips[i];
            const auto base = mip.in_tail ? 0 : mip.tiled_offset;
            for (std::uint32_t y = 0; y < mip.blocks_height; ++y) {
                const auto sy = y + mip.tail_y;
                const auto row = std::size_t(sy / lut.tile_h) * (mip.padded_blocks_width / lut.tile_w);
                for (std::uint32_t x = 0; x < mip.blocks_width; ++x) {
                    const auto sx = x + mip.tail_x;
                    const auto address = base + (row + sx / lut.tile_w) * layout.tile_bytes +
                        (lut.lut_x[sx % lut.tile_w] | lut.lut_y[sy % lut.tile_h]);
                    const auto dest = mip.packed_offset + (std::size_t(y) * mip.blocks_width + x) * lut.bpe;
                    std::memcpy(packed.data() + dest, tiled.data() + address, lut.bpe);
                }
            }
        }
    };
    if (rgba8) copy(swizzle_lut_64k_4bpe);
    else if (layout.element_bytes == 8) copy(swizzle_lut_64k_8bpe);
    else copy(swizzle_lut_64k_16bpe);
    return true;
}

inline bool detile_display_bc(std::span<const std::byte> tiled, std::span<std::byte> packed,
                              const Surface4ksLayout& layout) {
    return layout.block_dim == 4 && detile_display_surface(tiled, packed, layout);
}

// Copy elements unchanged into ascending native mips with packed rows and aligned region offsets.
inline bool detile_surface_4ks(std::span<const std::byte> tiled, std::span<std::byte> linear,
                             std::uint32_t width, std::uint32_t height, std::uint32_t mip_count,
                             std::uint32_t element_bytes, std::uint32_t block_dim) {
    const auto layout = surface_4ks_layout(width, height, mip_count, element_bytes, block_dim);
    if (!layout || tiled.size() < layout->tiled_bytes || linear.size() < layout->packed_bytes ||
        detail::spans_overlap(tiled.data(), layout->tiled_bytes, linear.data(), layout->packed_bytes)) return false;
    for (std::uint32_t i = 0; i < mip_count; ++i) {
        const auto& mip = layout->mips[i];
        detail::copy_surface_rows_4ks(tiled.data() + (mip.in_tail ? 0 : mip.tiled_offset),
                                      linear.data() + mip.packed_offset, mip.blocks_width, mip.blocks_height,
                                      mip.padded_blocks_width, mip.tail_x, mip.tail_y, element_bytes);
    }
    return true;
}

// Thick SW_4KB_S stores the complete mip chain in each depth slab, rather than putting whole 3D mips
// consecutively. AMD PAL GetMipTailDim/GetMaxNumMipsInTail/ComputeSurfaceInfoMacroTiled define a
// full-width, half-height tail with five levels; the 256-byte 3D micro blocks determine its XY origins.
// Single-sample, non-XOR volumes within the D3D12 3D extent limit are represented. Elements are native
// texels (block_dim1) or unchanged 4x4 BC blocks (block_dim4); a compressed block is one texel deep.
inline constexpr std::optional<Surface4ksLayout> volume_mips_4ks_layout(
    std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t mip_count,
    std::uint32_t element_bytes, std::uint32_t block_dim) {
    const auto* tile = detail::volume_tile_4ks(element_bytes);
    if (!tile || !width || !height || !depth || width > 2048 || height > 2048 || depth > 2048 ||
        !mip_count || mip_count > 15 || mip_count > std::bit_width(std::max({width, height, depth})) ||
        (block_dim != 1 && block_dim != 4) || (block_dim == 4 && element_bytes != 8 && element_bytes != 16))
        return std::nullopt;
    // AMD Gfx10Lib::Block256_3d, indexed by log2(BPE).
    constexpr std::array<TileShape, 5> micro_tiles{{{8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}}};
    const auto micro = micro_tiles[std::countr_zero(element_bytes)];
    const std::size_t upload_alignment = std::max(4u, element_bytes);
    constexpr auto size_limit = (std::numeric_limits<std::size_t>::max)();
    Surface4ksLayout out{};
    out.mip_count = mip_count;
    out.first_tail = mip_count;
    out.element_bytes = element_bytes;
    out.block_dim = block_dim;
    const auto base_width = 1u + (width - 1u) / block_dim;
    const auto base_height = 1u + (height - 1u) / block_dim;
    for (std::uint32_t i = 0; i < mip_count; ++i) {
        auto& mip = out.mips[i];
        mip.width = std::max(1u, width >> i);
        mip.height = std::max(1u, height >> i);
        mip.depth = std::max(1u, depth >> i);
        mip.blocks_width = 1u + (mip.width - 1u) / block_dim;
        mip.blocks_height = 1u + (mip.height - 1u) / block_dim;
        std::size_t count = 0, voxels = 0;
        if (!detail::checked_mul(mip.blocks_width, mip.blocks_height, count) ||
            !detail::checked_mul(count, mip.depth, voxels) ||
            !detail::checked_mul(voxels, element_bytes, mip.packed_bytes)) return std::nullopt;
        const auto remainder = mip.packed_bytes % upload_alignment;
        const auto padding = remainder ? upload_alignment - remainder : 0;
        if (mip.packed_bytes > size_limit - padding ||
            mip.packed_bytes + padding > size_limit - out.packed_bytes) return std::nullopt;
        mip.packed_offset = out.packed_bytes;
        out.packed_bytes += mip.packed_bytes + padding;
        const auto divisor = 1u << i;
        const auto source_width = 1u + (base_width - 1u) / divisor;
        const auto source_height = 1u + (base_height - 1u) / divisor;
        const auto source_depth = 1u + (depth - 1u) / divisor;
        if (mip_count > 1 && out.first_tail == mip_count && mip_count - i <= 5 &&
            source_width <= tile->width && source_height <= tile->height / 2)
            out.first_tail = i;
        if (i < out.first_tail) {
            mip.padded_blocks_width = (1u + (source_width - 1u) / tile->width) * tile->width;
            mip.padded_blocks_height = (1u + (source_height - 1u) / tile->height) * tile->height;
            mip.padded_depth = (1u + (source_depth - 1u) / tile->depth) * tile->depth;
        } else {
            const auto relative = i - out.first_tail;
            mip.in_tail = true;
            mip.padded_blocks_width = std::max(tile->width >> relative, micro.width);
            mip.padded_blocks_height = std::max((tile->height / 2) >> relative, micro.height);
            mip.padded_depth = (1u + (source_depth - 1u) / micro.depth) * micro.depth;
            const auto tail_offset = (4u - relative) << 8;
            const auto micro_x = ((tail_offset >> 9) & 1u) | ((tail_offset >> 10) & 2u);
            const auto micro_y = ((tail_offset >> 8) & 1u) | ((tail_offset >> 9) & 2u);
            mip.tail_x = micro_x * micro.width;
            mip.tail_y = micro_y * micro.height;
            mip.tiled_offset = detail::volume_axis_offset(mip.tail_x, tile->x) |
                               detail::volume_axis_offset(mip.tail_y, tile->y);
        }
    }
    std::size_t slab = out.first_tail < mip_count ? 4096 : 0;
    for (std::uint32_t i = out.first_tail; i-- > 0;) {
        auto& mip = out.mips[i];
        std::size_t count = 0, voxels = 0, bytes = 0;
        if (!detail::checked_mul(mip.padded_blocks_width, mip.padded_blocks_height, count) ||
            !detail::checked_mul(count, tile->depth, voxels) ||
            !detail::checked_mul(voxels, element_bytes, bytes) || bytes > size_limit - slab)
            return std::nullopt;
        mip.tiled_offset = slab;
        slab += bytes;
    }
    out.volume_slab_bytes = slab;
    const std::size_t slabs = 1u + (depth - 1u) / tile->depth;
    if (!detail::checked_mul(slab, slabs, out.tiled_bytes)) return std::nullopt;
    return out;
}

// Copy unchanged texels or BC elements into ascending mip regions, with native depth and aligned offsets.
inline bool detile_volume_mips_4ks(std::span<const std::byte> tiled, std::span<std::byte> linear,
                                 std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                 std::uint32_t mip_count, std::uint32_t element_bytes, std::uint32_t block_dim) {
    const auto layout = volume_mips_4ks_layout(width, height, depth, mip_count, element_bytes, block_dim);
    if (!layout || tiled.size() < layout->tiled_bytes || linear.size() < layout->packed_bytes ||
        detail::spans_overlap(tiled.data(), layout->tiled_bytes, linear.data(), layout->packed_bytes)) return false;
    const auto* tile = detail::volume_tile_4ks(element_bytes);
    for (std::uint32_t i = 0; i < mip_count; ++i) {
        const auto& mip = layout->mips[i];
        for (std::uint32_t z = 0; z < mip.depth; ++z) {
            const auto slab = std::size_t(z / tile->depth) * layout->volume_slab_bytes;
            const auto z_offset = detail::volume_axis_offset(z + mip.tail_z, tile->z);
            for (std::uint32_t y = 0; y < mip.blocks_height; ++y) {
                const auto yz_offset = detail::volume_axis_offset(y + mip.tail_y, tile->y) | z_offset;
                for (std::uint32_t x = 0; x < mip.blocks_width; ++x) {
                    const std::size_t block = mip.in_tail ? 0 :
                        std::size_t(y / tile->height) * (mip.padded_blocks_width / tile->width) + x / tile->width;
                    const auto intra = detail::volume_axis_offset(x + mip.tail_x, tile->x) | yz_offset;
                    const auto source = slab + (mip.in_tail ? 0 : mip.tiled_offset) + block * 4096 + intra;
                    const auto destination = mip.packed_offset +
                        ((std::size_t(z) * mip.blocks_height + y) * mip.blocks_width + x) * element_bytes;
                    std::memcpy(linear.data() + destination, tiled.data() + source, element_bytes);
                }
            }
        }
    }
    return true;
}

// Compute required tiled byte size for a 2D surface of the given dimensions (0 on invalid input or overflow).
constexpr std::size_t tiled_surface_size_64k(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t element_bytes, std::uint32_t block_dim = 1) {
    detail::SurfaceLayout64K layout{};
    return detail::compute_surface_layout_64k(width, height, element_bytes, block_dim, layout)
               ? layout.tiled_bytes
               : 0;
}

// Compute required linear byte size for a 2D surface of the given dimensions (0 on invalid input or overflow).
constexpr std::size_t linear_surface_size(std::uint32_t width, std::uint32_t height,
                                         std::uint32_t element_bytes, std::uint32_t block_dim = 1) {
    detail::SurfaceLayout64K layout{};
    return detail::compute_surface_layout_64k(width, height, element_bytes, block_dim, layout)
               ? layout.linear_bytes
               : 0;
}

namespace detail {

template <std::uint32_t Bpe>
void detile_typed(std::span<const std::byte> tiled, std::span<std::byte> linear,
                  std::uint32_t blocks_w, std::uint32_t blocks_h, std::uint32_t tiles_x) {
    constexpr const auto& lut = []() constexpr -> const auto& {
        if constexpr (Bpe == 1) return swizzle_lut_64k_1bpe;
        else if constexpr (Bpe == 2) return swizzle_lut_64k_2bpe;
        else if constexpr (Bpe == 4) return swizzle_lut_64k_4bpe;
        else if constexpr (Bpe == 8) return swizzle_lut_64k_8bpe;
        else if constexpr (Bpe == 16) return swizzle_lut_64k_16bpe;
    }();

    constexpr std::uint32_t tile_w = lut.tile_w;
    constexpr std::uint32_t tile_h = lut.tile_h;

    for (std::uint32_t y = 0; y < blocks_h; ++y) {
        const std::uint32_t ty = y / tile_h;
        const std::uint16_t intra_y = lut.lut_y[y % tile_h];
        const std::size_t macro_y_base = static_cast<std::size_t>(ty) * tiles_x * tile_size_in_bytes;
        const std::size_t linear_row = static_cast<std::size_t>(y) * blocks_w * Bpe;

        for (std::uint32_t x = 0; x < blocks_w; ++x) {
            const std::uint32_t tx = x / tile_w;
            const std::uint16_t intra_x = lut.lut_x[x % tile_w];
            const std::size_t tiled_off = macro_y_base + static_cast<std::size_t>(tx) * tile_size_in_bytes + (intra_x | intra_y);
            const std::size_t linear_off = linear_row + static_cast<std::size_t>(x) * Bpe;

            std::memcpy(&linear[linear_off], &tiled[tiled_off], Bpe);
        }
    }
}

template <std::uint32_t Bpe>
void tile_typed(std::span<const std::byte> linear, std::span<std::byte> tiled,
                std::uint32_t blocks_w, std::uint32_t blocks_h, std::uint32_t tiles_x) {
    constexpr const auto& lut = []() constexpr -> const auto& {
        if constexpr (Bpe == 1) return swizzle_lut_64k_1bpe;
        else if constexpr (Bpe == 2) return swizzle_lut_64k_2bpe;
        else if constexpr (Bpe == 4) return swizzle_lut_64k_4bpe;
        else if constexpr (Bpe == 8) return swizzle_lut_64k_8bpe;
        else if constexpr (Bpe == 16) return swizzle_lut_64k_16bpe;
    }();

    constexpr std::uint32_t tile_w = lut.tile_w;
    constexpr std::uint32_t tile_h = lut.tile_h;

    for (std::uint32_t y = 0; y < blocks_h; ++y) {
        const std::uint32_t ty = y / tile_h;
        const std::uint16_t intra_y = lut.lut_y[y % tile_h];
        const std::size_t macro_y_base = static_cast<std::size_t>(ty) * tiles_x * tile_size_in_bytes;
        const std::size_t linear_row = static_cast<std::size_t>(y) * blocks_w * Bpe;

        for (std::uint32_t x = 0; x < blocks_w; ++x) {
            const std::uint32_t tx = x / tile_w;
            const std::uint16_t intra_x = lut.lut_x[x % tile_w];
            const std::size_t tiled_off = macro_y_base + static_cast<std::size_t>(tx) * tile_size_in_bytes + (intra_x | intra_y);
            const std::size_t linear_off = linear_row + static_cast<std::size_t>(x) * Bpe;

            std::memcpy(&tiled[tiled_off], &linear[linear_off], Bpe);
        }
    }
}

}  // namespace detail

// Detile a 2D surface from console-tiled 64 KiB layout into linear row-major order.
inline bool detile_surface_64k(std::span<const std::byte> tiled, std::span<std::byte> linear,
                               std::uint32_t width, std::uint32_t height,
                               std::uint32_t element_bytes, std::uint32_t block_dim = 1) {
    detail::SurfaceLayout64K layout{};
    if (!detail::compute_surface_layout_64k(width, height, element_bytes, block_dim, layout)) return false;
    if (linear.size() < layout.linear_bytes || tiled.size() < layout.tiled_bytes) return false;
    if (detail::spans_overlap(tiled.data(), layout.tiled_bytes, linear.data(), layout.linear_bytes)) return false;

    switch (element_bytes) {
    case 1:
        detail::detile_typed<1>(tiled, linear, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 2:
        detail::detile_typed<2>(tiled, linear, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 4:
        detail::detile_typed<4>(tiled, linear, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 8:
        detail::detile_typed<8>(tiled, linear, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 16:
        detail::detile_typed<16>(tiled, linear, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    default:
        return false;
    }
}

// Tile a 2D surface from linear row-major order into console-tiled 64 KiB layout.
inline bool tile_surface_64k(std::span<const std::byte> linear, std::span<std::byte> tiled,
                             std::uint32_t width, std::uint32_t height,
                             std::uint32_t element_bytes, std::uint32_t block_dim = 1) {
    detail::SurfaceLayout64K layout{};
    if (!detail::compute_surface_layout_64k(width, height, element_bytes, block_dim, layout)) return false;
    if (linear.size() < layout.linear_bytes || tiled.size() < layout.tiled_bytes) return false;
    if (detail::spans_overlap(linear.data(), layout.linear_bytes, tiled.data(), layout.tiled_bytes)) return false;

    switch (element_bytes) {
    case 1:
        detail::tile_typed<1>(linear, tiled, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 2:
        detail::tile_typed<2>(linear, tiled, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 4:
        detail::tile_typed<4>(linear, tiled, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 8:
        detail::tile_typed<8>(linear, tiled, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    case 16:
        detail::tile_typed<16>(linear, tiled, layout.blocks_w, layout.blocks_h, layout.tiles_x);
        return true;
    default:
        return false;
    }
}

}  // namespace leonida
