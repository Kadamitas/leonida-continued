#pragma once

#include "gfx10_descriptors.h"
#include "shader_library.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>

namespace leonida {

// Opt-in observation of distinct texture descriptors used by text/gradient draws. No texture contents are read.
class FontTableProfile {
public:
    void record(const char* vs, const char* ps, const ShaderRootTable& plan, std::span<const std::uint8_t> table) {
        if (!plan.image_offsets) return;
        auto signature = std::uint64_t{14695981039346656037ull};
        const auto mix = [&](std::uint32_t value) { signature = (signature ^ value) * 1099511628211ull; };
        for (const auto offset : *plan.image_offsets) {
            gfx10::ImageWords words{};
            if (offset > table.size() || table.size() - offset < sizeof(words)) return;
            std::memcpy(words.data(), table.data() + offset, sizeof(words));
            for (auto word : words) mix(word);
        }
        for (const auto& view : plan.constant_views) {
            gfx10::BufferWords words{};
            if (view.offset > table.size() || table.size() - view.offset < sizeof(words)) return;
            std::memcpy(words.data(), table.data() + view.offset, sizeof(words));
            // Constant storage may move every draw. Distinguish addressing modes and bounds, independently of VA.
            mix(words[1] >> 16); mix(words[2]); mix(words[3]);
        }
        static std::mutex output;
        std::lock_guard lock(output);
        if (count_ == seen_.size()) return;
        for (unsigned i = 0; i < count_; ++i) if (seen_[i] == signature) return;
        seen_[count_++] = signature;
        std::fprintf(stderr, "[rt] font table vs=%s ps=%s set=%u", vs, ps, count_);
        for (const auto offset : *plan.image_offsets) {
            gfx10::ImageWords words{};
            gfx10::Image view{};
            std::memcpy(words.data(), table.data() + offset, sizeof(words));
            if (gfx10::decode(words, view))
                std::fprintf(stderr, " image[%u]=%llx/%ux%u/f%u/m%u+%u/sw%u%u%u%u", offset,
                    static_cast<unsigned long long>(view.address), view.width, view.height, view.format,
                    view.first_mip, view.mip_count, unsigned(view.swizzle[0]), unsigned(view.swizzle[1]),
                    unsigned(view.swizzle[2]), unsigned(view.swizzle[3]));
            else std::fprintf(stderr, " image[%u]=invalid", offset);
        }
        for (const auto& entry : plan.constant_views) {
            gfx10::BufferWords words{};
            gfx10::Buffer view{};
            std::memcpy(words.data(), table.data() + entry.offset, sizeof(words));
            if (gfx10::decode(words, view))
                std::fprintf(stderr, " buffer[%u]=%llx/stride%u/count%u/oob%u", entry.offset,
                    static_cast<unsigned long long>(view.address), view.stride, view.num_records,
                    unsigned(view.oob_select));
            else std::fprintf(stderr, " buffer[%u]=invalid", entry.offset);
        }
        std::fputc('\n', stderr);
    }

private:
    std::array<std::uint64_t, 24> seen_{};
    unsigned count_ = 0;
};
}  // namespace leonida
