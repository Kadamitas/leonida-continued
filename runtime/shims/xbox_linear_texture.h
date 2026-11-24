#pragma once

#include <d3d12.h>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>

namespace rt::d3d12 {

struct TitleLinearTextureMip {
    std::uint64_t offset = 0;
    std::uint64_t row_pitch = 0;
    std::uint64_t source_bytes = 0;
    UINT width = 0;   // Native mip extent: floor(base / 2^mip), clamped to one.
    UINT height = 0;
    UINT rows = 0;    // CPU allocation extent: ceil(base height / 2^mip).
};

struct TitleLinearTextureLayout {
    std::uint64_t size = 0;  // Exact CPU source footprint, before resource-placement rounding.
    UINT mip_count = 0;
    UINT texel_bytes = 0;
    std::array<TitleLinearTextureMip, 15> mips{};  // Indexed by native mip number, not storage order.
};

// This validates the CPU layout's shape, not its format. The backend must prove a byte-identical native typed
// color format before supplying texel_bytes; depth/stencil, BC, typeless and repacked formats need other rules.
inline bool title_linear_texture_shape(const D3D12_RESOURCE_DESC& desc) {
    const bool line = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    if ((!line && desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) ||
        !desc.Width || desc.Width > (line ? D3D12_REQ_TEXTURE1D_U_DIMENSION : D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
        !desc.Height || (line ? desc.Height != 1 : desc.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
        desc.DepthOrArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
        UINT(desc.Layout) != 0x100 || UINT(desc.Flags) != 0x300000 ||
        (desc.Alignment != 0 && desc.Alignment != 256 &&
         desc.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
         desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)) return false;
    const auto extent = desc.Width > desc.Height ? desc.Width : UINT64(desc.Height);
    return desc.MipLevels && desc.MipLevels <= std::bit_width(extent) && desc.MipLevels <= 15;
}

// The GPL-3.0-or-later xg-xs reference engine's layout.rs mode-0 path packs the smallest mip first.
// Each row (including a 1D mip's single row) is 256-byte aligned using ceil-shifted base dimensions;
// the native image still uses floor-shifted mip extents. This describes CPU bytes only: native image requirements
// and placement remain separate checks.
inline std::optional<TitleLinearTextureLayout> title_linear_texture_layout(const D3D12_RESOURCE_DESC& desc,
                                                                        UINT texel_bytes) {
    if (!title_linear_texture_shape(desc) || !texel_bytes || texel_bytes > 16 || !std::has_single_bit(texel_bytes))
        return std::nullopt;

    TitleLinearTextureLayout result{};
    result.mip_count = desc.MipLevels;
    result.texel_bytes = texel_bytes;
    constexpr auto max = (std::numeric_limits<std::uint64_t>::max)();
    for (UINT index = result.mip_count; index-- > 0;) {
        const std::uint64_t divisor = std::uint64_t{1} << index;
        // The validated dimensions and mip count bound the ceil additions and shifts.
        const auto columns = (desc.Width + divisor - 1) / divisor;
        const auto rows = (std::uint64_t(desc.Height) + divisor - 1) / divisor;
        if (columns > (max - 255) / result.texel_bytes) return std::nullopt;
        const auto pitch = (columns * result.texel_bytes + 255) & ~std::uint64_t{255};
        if (rows > max / pitch) return std::nullopt;
        const auto bytes = pitch * rows;
        if (result.size > max - bytes) return std::nullopt;
        const auto width = desc.Width >> index;
        const auto height = desc.Height >> index;
        result.mips[index] = {result.size, pitch, bytes, UINT(width ? width : 1), height ? height : 1, UINT(rows)};
        result.size += bytes;
    }
    return result;
}

}  // namespace rt::d3d12
