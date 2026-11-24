#pragma once

// AMD PAL describes RectList as three axis-aligned corners. Mesa's radeonsi blit, for example,
// submits (x1,y1), (x1,y2), (x2,y1). Input-space expansion requires a verified affine shader.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace leonida::rectangle {

struct Element {
    std::uint32_t offset = 0;
    std::uint32_t format = 0;  // DXGI_FORMAT
};

enum class Kind { f32, f16, unorm8, unorm16 };
struct Format { unsigned components = 0, bytes = 0; Kind kind = Kind::f32; };

inline Format format(std::uint32_t value) {
    switch (value) {
    case 2: return {4, 4, Kind::f32};
    case 6: return {3, 4, Kind::f32};
    case 16: return {2, 4, Kind::f32};
    case 41: return {1, 4, Kind::f32};
    case 10: return {4, 2, Kind::f16};
    case 34: return {2, 2, Kind::f16};
    case 54: return {1, 2, Kind::f16};
    case 11: return {4, 2, Kind::unorm16};
    case 35: return {2, 2, Kind::unorm16};
    case 28: case 29: case 87: case 91: return {4, 1, Kind::unorm8};
    default: return {};
    }
}

inline float from_half(std::uint16_t h) {
    const auto sign = std::uint32_t(h & 0x8000u) << 16;
    const auto exponent = (h >> 10) & 31u;
    const auto fraction = h & 1023u;
    if (!exponent) return std::copysign(std::ldexp(float(fraction), -24), sign ? -1.0f : 1.0f);
    return std::bit_cast<float>(sign | (exponent == 31 ? 0x7f800000u : (exponent + 112u) << 23) |
                                (std::uint32_t(fraction) << 13));
}

inline std::uint16_t to_half(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = std::uint16_t((bits >> 16) & 0x8000u);
    const auto exponent = int((bits >> 23) & 255u) - 127 + 15;
    auto fraction = bits & 0x7fffffu;
    if (exponent >= 31) return std::uint16_t(sign | 0x7c00u);
    if (exponent <= 0) {
        if (exponent < -10) return sign;
        fraction |= 0x800000u;
        const auto shift = unsigned(14 - exponent);
        const auto rounded = (fraction + ((1u << (shift - 1)) - 1) + ((fraction >> shift) & 1)) >> shift;
        return std::uint16_t(sign | rounded);
    }
    const auto rounded = fraction + 0xfffu + ((fraction >> 13) & 1);
    return std::uint16_t(sign | ((std::uint32_t(exponent) << 10) + (rounded >> 13)));
}

struct Position { float x, y; };

inline float component(const std::uint8_t* p, const Format& f) {
    if (f.kind == Kind::f32) { float value; std::memcpy(&value, p, 4); return value; }
    std::uint16_t value = 0;
    std::memcpy(&value, p, f.bytes);
    return f.kind == Kind::f16 ? from_half(value) : float(value);
}

// The caller verifies that the VS passes these two components to clip XY with constant positive W.
// The position stream uses f32 or f16; comparison is exact in its encoded precision.
inline std::uint8_t shared_corner(const std::uint8_t* first, std::size_t stride, Element xy,
                                  std::array<Position, 3>& positions) {
    const auto f = format(xy.format);
    if (f.components < 2 || xy.offset > stride || f.components * f.bytes > stride - xy.offset) return 3;
    for (unsigned i = 0; i < 3; ++i) {
        const auto* p = first + stride * i + xy.offset;
        positions[i] = {component(p, f), component(p + f.bytes, f)};
        if (!std::isfinite(positions[i].x) || !std::isfinite(positions[i].y)) return 3;
    }
    for (unsigned i = 0; i < 3; ++i) {
        const auto [x, y] = positions[i];
        const auto a = positions[(i + 1) % 3], b = positions[(i + 2) % 3];
        if ((a.x == x && a.y != y && b.y == y && b.x != x) ||
            (a.y == y && a.x != x && b.x == x && b.y != y)) return std::uint8_t(i);
    }
    return 3; // Collinear, coincident or non-axis-aligned: retain the supplied triangle.
}

inline bool expand(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination,
                   std::size_t stride, std::span<const Element> elements,
                   std::span<const std::uint8_t> corners = {}) {
    if (!stride || !source.size() || source.size() % stride || source.size() / stride % 3 ||
        source.size() > SIZE_MAX / 2 || destination.size() != source.size() * 2) return false;
    for (const auto& element : elements) {
        const auto f = format(element.format);
        if (!f.components || element.offset > stride || f.components * f.bytes > stride - element.offset) return false;
    }
    const auto vertices = source.size() / stride;
    if (!corners.empty() && corners.size() != vertices / 3) return false;
    for (std::size_t vertex = 0; vertex < vertices; vertex += 3) {
        const auto* first = source.data() + vertex * stride;
        auto* output = destination.data() + vertex * stride * 2;
        const unsigned corner = corners.empty() ? 1u : corners[vertex / 3];
        if (corner > 3) return false;
        // Each fan starts both triangles with v0 (flat inputs), retaining the submitted winding.
        // Index 3 is the synthetic vertex. The last row pads a single triangle with a degenerate one.
        constexpr unsigned order[4][6] = {{0,1,3,0,3,2}, {0,1,2,0,2,3}, {0,1,2,0,3,1}, {0,1,2,0,0,0}};
        constexpr unsigned generated[] = {2,5,4};
        for (unsigned i = 0; i < 6; ++i) {
            const auto src = order[corner][i];
            std::memcpy(output + i * stride, first + (src == 3 ? 0 : src) * stride, stride);
        }
        if (corner == 3) continue;
        const auto a = (corner + 1) % 3, b = (corner + 2) % 3;
        for (const auto& element : elements) {
            const auto f = format(element.format);
            for (unsigned c = 0; c < f.components; ++c) {
                const auto at = element.offset + c * f.bytes;
                auto* fourth = output + generated[corner] * stride + at;
                if (f.kind == Kind::unorm8 || f.kind == Kind::unorm16) {
                    const auto read = [&](unsigned i) {
                        std::uint16_t value = 0;
                        std::memcpy(&value, first + i * stride + at, f.bytes);
                        return int(value);
                    };
                    const int value = read(a) + read(b) - read(corner);
                    // Clamping would change the fourth corner. Refuse values the input format cannot represent.
                    if (value < 0 || value > (f.bytes == 1 ? 255 : 65535)) return false;
                    const auto encoded = std::uint16_t(value);
                    std::memcpy(fourth, &encoded, f.bytes);
                } else {
                    const auto read = [&](unsigned i) {
                        const auto* at_vertex = first + i * stride + at;
                        if (f.kind == Kind::f32) { float value; std::memcpy(&value, at_vertex, 4); return value; }
                        std::uint16_t value; std::memcpy(&value, at_vertex, 2); return from_half(value);
                    };
                    const float value = (read(a) + read(b)) - read(corner);
                    if (!std::isfinite(value)) return false;
                    if (f.kind == Kind::f32) std::memcpy(fourth, &value, 4);
                    else {
                        const auto encoded = to_half(value);
                        if (!std::isfinite(from_half(encoded))) return false;
                        std::memcpy(fourth, &encoded, 2);
                    }
                }
            }
        }
        if (corner == 0) std::memcpy(output + 4 * stride, output + 2 * stride, stride);
    }
    return true;
}

} // namespace leonida::rectangle
