#pragma once

#include <d3d12.h>
#include <bit>

namespace rt::d3d12 {

enum class TitleTextureUse { unsupported, color, storage, color_storage, depth };

// Xbox creation and allocation queries share PC COM methods. Preserve their origin for the duration of the call.
inline thread_local bool xbox_resource_call = false;
template<bool ResourceSlot> struct XboxResourceCall {};
template<> struct XboxResourceCall<true> {
    const bool previous = xbox_resource_call;
    XboxResourceCall() { xbox_resource_call = true; }
    ~XboxResourceCall() { xbox_resource_call = previous; }
};

inline constexpr bool title_srgb_rtv(const D3D12_RESOURCE_DESC& desc) {
    return desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET && desc.SampleDesc.Count == 1 &&
           desc.SampleDesc.Quality == 0 && desc.DepthOrArraySize == 1;
}

inline constexpr UINT kTitleTextureMetadata = 0x300000;
inline constexpr UINT kTitleByteTargetMetadata = 0xc00000;
inline constexpr UINT kTitleDepthTargetMetadata = 0x4800000;
inline constexpr UINT kTitleStorageLayout = 0x102;
inline constexpr UINT kTitleLinearLayout = 0x100;
inline constexpr DXGI_FORMAT kTitleDepth16Stencil8 = static_cast<DXGI_FORMAT>(118);

inline constexpr bool title_depth16_metadata(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == kTitleDepth16Stencil8 &&
        !desc.Alignment && desc.DepthOrArraySize == 1 &&
        desc.MipLevels == 1 && desc.SampleDesc.Count == 1 && !desc.SampleDesc.Quality &&
        desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
        ((desc.Width == 2560 && desc.Height == 1440 && UINT(desc.Flags) == 0x80300006u) ||
         (desc.Width == 1024 && desc.Height == 1024 &&
          (UINT(desc.Flags) == 0x800002u || UINT(desc.Flags) == 0x1800002u)));
}

inline constexpr UINT title_storage_layout(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) return kTitleLinearLayout;
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 0x105 : kTitleStorageLayout;
}

inline constexpr bool title_storage_layout_matches(const D3D12_RESOURCE_DESC& desc) {
    // Rust chooses LINEAR for a default-layout 1D resource. Keep the caller's descriptor intact while
    // admitting both that default and the explicitly encoded linear mode.
    return UINT(desc.Layout) == title_storage_layout(desc) ||
        (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
}

inline constexpr bool title_linear_color_layout(const D3D12_RESOURCE_DESC& desc) {
    const UINT usage = UINT(desc.Flags) & 7u;
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && UINT(desc.Layout) == kTitleLinearLayout &&
        desc.MipLevels == 1 && (usage == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ||
                              usage == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ||
                              usage == (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
}

inline constexpr bool title_byte_target(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == DXGI_FORMAT_R8_UINT &&
           desc.Width == 640 && desc.Height == 360 && desc.MipLevels == 1;
}

inline constexpr bool title_native_rt_target(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        !desc.Alignment && desc.Width && desc.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        desc.Height && desc.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && !desc.SampleDesc.Quality && desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
        (UINT(desc.Flags) & 7u) == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
}

inline constexpr bool title_packed_color_target(const D3D12_RESOURCE_DESC& desc) {
    return title_native_rt_target(desc) &&
        (desc.Format == DXGI_FORMAT_B4G4R4A4_UNORM || desc.Format == DXGI_FORMAT_B5G5R5A1_UNORM);
}

inline constexpr bool title_d16_shape(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == DXGI_FORMAT_R16_TYPELESS &&
        !desc.Alignment && desc.Width && desc.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        desc.Height && desc.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && desc.DepthOrArraySize &&
        desc.DepthOrArraySize <= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION &&
        desc.MipLevels == 1 && desc.SampleDesc.Count == 1 && !desc.SampleDesc.Quality &&
        desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN;
}

inline constexpr bool title_d16_target(const D3D12_RESOURCE_DESC& desc) {
    return title_d16_shape(desc) && (UINT(desc.Flags) & 7u) == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
}

inline constexpr bool title_d16_storage_metadata(const D3D12_RESOURCE_DESC& desc) {
    return title_d16_shape(desc) && UINT(desc.Flags) == 0x80300006u;
}

inline constexpr bool title_d32s8_shape(const D3D12_RESOURCE_DESC& desc) {
    auto d16 = desc;
    d16.Format = DXGI_FORMAT_R16_TYPELESS;
    return desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS && title_d16_shape(d16);
}

inline constexpr bool title_d32_target(const D3D12_RESOURCE_DESC& desc) {
    auto d16 = desc;
    d16.Format = DXGI_FORMAT_R16_TYPELESS;
    return desc.Format == DXGI_FORMAT_R32_TYPELESS && title_d16_shape(d16) &&
        desc.DepthOrArraySize == 1 && (UINT(desc.Flags) & 7u) == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
}

// Additional single-plane/combined depth declarations in the startup allocation census. Keep their
// metadata explicit: these flags select a depth allocation, not native unordered access or CPU bytes.
inline constexpr bool title_additional_depth_target(const D3D12_RESOURCE_DESC& desc) {
    auto d16 = desc;
    d16.Format = DXGI_FORMAT_R16_TYPELESS;
    if (!title_d16_shape(d16) || desc.DepthOrArraySize != 1 ||
        (desc.Format != DXGI_FORMAT_R16_TYPELESS && desc.Format != DXGI_FORMAT_R32_TYPELESS &&
         desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS)) return false;
    switch (UINT(desc.Flags)) {
    case 0x800002u: case 0x80300002u: case 0x1800002u: case 0x1000002u: return true;
    default: return false;
    }
}

inline constexpr bool title_d32s8_storage_metadata(const D3D12_RESOURCE_DESC& desc) {
    if (!title_d32s8_shape(desc)) return false;
    const UINT flags = UINT(desc.Flags);
    return flags == 0x800006u ||
        ((flags == 0x1800006u || flags == 0x80300006u) &&
         desc.Width == 2560 && desc.Height == 1440 && desc.DepthOrArraySize == 1);
}

inline constexpr bool title_combined_depth_metadata(const D3D12_RESOURCE_DESC& desc) {
    return title_d16_storage_metadata(desc) || title_d32s8_storage_metadata(desc);
}

inline constexpr UINT title_depth_metadata(const D3D12_RESOURCE_DESC& desc) {
    return title_d16_target(desc) ? 0x800000 : kTitleDepthTargetMetadata;
}

inline constexpr UINT title_color_metadata(const D3D12_RESOURCE_DESC& desc) {
    return title_byte_target(desc) || title_packed_color_target(desc) ? kTitleByteTargetMetadata : kTitleTextureMetadata;
}

inline constexpr bool title_sampled_2d_texture_shape(const D3D12_RESOURCE_DESC& desc, bool plain_flags = false) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (desc.Alignment && desc.Alignment != 4096 && desc.Alignment != 65536) ||
        !desc.Width || desc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !desc.Height || desc.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !desc.DepthOrArraySize || desc.DepthOrArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        !desc.MipLevels || desc.MipLevels > std::bit_width(desc.Width > desc.Height ? desc.Width : UINT64(desc.Height)) ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
        (UINT(desc.Flags) != kTitleTextureMetadata && !(plain_flags && desc.Flags == D3D12_RESOURCE_FLAG_NONE)))
        return false;
    return true;
}

inline constexpr bool title_sampled_4ks_texture_shape(const D3D12_RESOURCE_DESC& desc) {
    return UINT(desc.Layout) == 0x105 && title_sampled_2d_texture_shape(desc);
}

inline constexpr bool title_implicit_sampled_4ks_query(const D3D12_RESOURCE_DESC& desc) {
    return desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && title_sampled_2d_texture_shape(desc, true);
}

// The single-mip float16 RDS5 volumes use thick SW_4KB_S tiles, including padding on the depth axis.
inline constexpr bool title_sampled_volume_4ks_query(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && !desc.SampleDesc.Quality &&
        (desc.Format == DXGI_FORMAT_R16_FLOAT || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        (desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN || UINT(desc.Layout) == 0x105) &&
        (!desc.Alignment || desc.Alignment == 4096 || desc.Alignment == 65536) &&
        (desc.Flags == D3D12_RESOURCE_FLAG_NONE || UINT(desc.Flags) == kTitleTextureMetadata);
}

inline constexpr bool title_sampled_bc_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return true;
    default: return false;
    }
}

inline constexpr bool title_default_bc_texture_shape(const D3D12_RESOURCE_DESC& desc) {
    return desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && title_sampled_2d_texture_shape(desc) &&
        title_sampled_bc_format(desc.Format);
}

inline constexpr UINT kTitleDisplay64k = 0x10a;
inline constexpr bool title_display_bc_texture_shape(const D3D12_RESOURCE_DESC& desc) {
    return UINT(desc.Layout) == kTitleDisplay64k && (!desc.Alignment || desc.Alignment == 65536) &&
        title_sampled_2d_texture_shape(desc) && title_sampled_bc_format(desc.Format);
}

inline constexpr bool title_display_rgba8_array_shape(const D3D12_RESOURCE_DESC& desc) {
    return UINT(desc.Layout) == kTitleDisplay64k && (!desc.Alignment || desc.Alignment == 65536) &&
        desc.DepthOrArraySize > 1 && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM && title_sampled_2d_texture_shape(desc);
}

// VIctory's host_texture translates resource usage and keeps the actual dimensions. Use that model for
// the native GPU texture path. CPU-tiled textures, aliased formats and combined depth/storage need
// separate backing rules. Native capability and placement checks still run after classification.
inline TitleTextureUse title_texture_use(const D3D12_RESOURCE_DESC& desc) {
    if ((title_d16_target(desc) || title_d32_target(desc) || title_d32s8_shape(desc)) &&
        desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        return TitleTextureUse::depth;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        if ((desc.Alignment && desc.Alignment != 4096) || !desc.Width ||
            desc.Width > D3D12_REQ_TEXTURE1D_U_DIMENSION || desc.Height != 1 || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
            desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || desc.Flags != D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
            return TitleTextureUse::unsupported;
        return TitleTextureUse::storage;
    }
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        const UINT64 largest = desc.Width > desc.Height ? desc.Width : UINT64(desc.Height);
        const UINT64 largest_3d = largest > desc.DepthOrArraySize ? largest : UINT64(desc.DepthOrArraySize);
        if (desc.Alignment || !desc.Width || desc.Width > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
            !desc.Height || desc.Height > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
            !desc.DepthOrArraySize || desc.DepthOrArraySize > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
            desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
            desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || !desc.MipLevels ||
            desc.MipLevels > std::bit_width(largest_3d) ||
            (desc.Flags != D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS &&
             desc.Flags != (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            return TitleTextureUse::unsupported;
        return desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ? TitleTextureUse::storage :
            TitleTextureUse::color_storage;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Alignment ||
        !desc.Width || desc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !desc.Height || desc.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !desc.DepthOrArraySize || desc.DepthOrArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        (desc.DepthOrArraySize != 1 && desc.Flags != D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
        desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || !desc.MipLevels ||
        desc.MipLevels > std::bit_width(desc.Width > desc.Height ? desc.Width : UINT64(desc.Height)))
        return TitleTextureUse::unsupported;

    constexpr auto color = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    constexpr auto storage = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    switch (UINT(desc.Flags)) {
    case color: return TitleTextureUse::color;
    case storage: return TitleTextureUse::storage;
    case UINT(color) | UINT(storage): return TitleTextureUse::color_storage;
    case D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL:
        if (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS || desc.Format == kTitleDepth16Stencil8 ||
            title_d16_target(desc))
            return TitleTextureUse::depth;
        break;
    default: break;
    }
    return TitleTextureUse::unsupported;
}

inline bool title_storage_array_profile(const D3D12_RESOURCE_DESC& source) {
    // Natural and explicit 4 KiB alignment share the same SW_256_D planner for single slices and arrays.
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (source.Alignment && source.Alignment != 4096) || UINT(source.Flags) != 0x300004u ||
        UINT(source.Layout) != kTitleStorageLayout) return false;
    auto native = source;
    native.Alignment = 0;
    native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    native.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return title_texture_use(native) == TitleTextureUse::storage;
}

inline bool title_default_storage_profile(const D3D12_RESOURCE_DESC& source) {
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (source.Alignment && source.Alignment != 4096) || UINT(source.Flags) != 0x300004u ||
        source.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN) return false;
    auto native = source;
    native.Alignment = 0;
    native.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return title_texture_use(native) == TitleTextureUse::storage;
}

inline bool title_linear_volume_storage(const D3D12_RESOURCE_DESC& source) {
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
        (source.Alignment && source.Alignment != 4096) || UINT(source.Flags) != 0x300004u ||
        UINT(source.Layout) != kTitleLinearLayout) return false;
    auto native = source;
    native.Alignment = 0;
    native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    native.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return title_texture_use(native) == TitleTextureUse::storage;
}

inline bool title_rt_storage_profile(const D3D12_RESOURCE_DESC& source) {
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    const UINT flags = UINT(source.Flags);
    // Retain other established single-mip 0x300005 sizes; packed16 and mip chains use explicit guest layout.
    const bool packed16 = source.Format == DXGI_FORMAT_B5G5R5A1_UNORM;
    if ((flags != 0xc00005u && (flags != 0x300005u || (source.MipLevels <= 1 && !packed16))) ||
        title_byte_target(source)) return false;
    auto native = source;
    native.Flags = D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return title_texture_use(native) == TitleTextureUse::color_storage;
}

inline bool title_volume_rt_storage_profile(const D3D12_RESOURCE_DESC& source) {
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || source.Alignment ||
        UINT(source.Flags) != 0x300005u || UINT(source.Layout) != 0x105u) return false;
    auto native = source;
    native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    native.Flags = D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return title_texture_use(native) == TitleTextureUse::color_storage;
}

inline bool title_explicit_4k_storage(const D3D12_RESOURCE_DESC& source) {
    if (title_volume_rt_storage_profile(source)) return true;
    if (source.Alignment != 4096 || UINT(source.Flags) != 0x300004u ||
        (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) ||
        UINT(source.Layout) != title_storage_layout(source)) return false;
    auto native = source;
    native.Alignment = 0;
    native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    native.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return title_texture_use(native) == TitleTextureUse::storage;
}

inline bool translate_title_texture_desc(const D3D12_RESOURCE_DESC& source, D3D12_RESOURCE_DESC& result) {
    if (title_explicit_4k_storage(source) || title_storage_array_profile(source) ||
        title_default_storage_profile(source) || title_linear_volume_storage(source)) {
        result = source;
        result.Alignment = 0;
        result.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        result.Flags = D3D12_RESOURCE_FLAGS(UINT(source.Flags) & 7u);
        return true;
    }
    if (title_depth16_metadata(source) || title_combined_depth_metadata(source) ||
        title_additional_depth_target(source)) {
        result = source;
        // As VIctory's host_texture: native depth attachments cannot also be storage images. The original
        // guest flag remains visible in the allocation query; this does not implement a depth UAV view.
        result.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        return true;
    }
    const UINT raw_flags = UINT(source.Flags);
    const UINT raw_layout = UINT(source.Layout);
    auto candidate = source;
    candidate.Flags = D3D12_RESOURCE_FLAGS(raw_flags & 7u);
    candidate.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const auto use = title_texture_use(candidate);
    if (use == TitleTextureUse::unsupported) return false;
    if (source.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && use == TitleTextureUse::color_storage)
        return false;  // the explicit SW_4KB_S profile above supplies the volume's verified guest layout
    // R32 depth is admitted by the explicit metadata allowlist above, not by another family's default bits.
    if (use == TitleTextureUse::depth && source.Format == DXGI_FORMAT_R32_TYPELESS) return false;

    const UINT expected_layout = title_linear_color_layout(source) ? kTitleLinearLayout :
        use == TitleTextureUse::storage ? title_storage_layout(source) : 0;
    const UINT expected_metadata = use == TitleTextureUse::depth ? title_depth_metadata(source) :
        title_color_metadata(source);
    const bool color_metadata =
        (use == TitleTextureUse::color && title_native_rt_target(source) &&
         (raw_flags == (kTitleTextureMetadata | UINT(candidate.Flags)) ||
          raw_flags == (kTitleByteTargetMetadata | UINT(candidate.Flags)))) ||
        (use == TitleTextureUse::color_storage && title_rt_storage_profile(source));
    const bool layout_matches = raw_layout == expected_layout ||
        (use == TitleTextureUse::storage && source.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
         title_storage_layout_matches(source));
    if (!layout_matches ||
        (raw_flags != (expected_metadata | UINT(candidate.Flags)) && !color_metadata)) return false;
    result = candidate;
    return true;
}

}  // namespace rt::d3d12
