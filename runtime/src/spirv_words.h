#pragma once

#include "descriptor_hash.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace leonida {

// The translator records a non-default descriptor lookup ABI in the actual module's debug section.
// A cache copied across builds cannot silently probe the other table. All decorations precede the
// first function in a valid SPIR-V module. Binding 2 is treated conservatively even outside set 0.
inline bool descriptor_hash_matches(std::span<const std::uint32_t> words, DescriptorHashKind expected,
                                    std::optional<DescriptorHashKind> declared = std::nullopt) {
    constexpr std::string_view prefix = "leonida.descriptor_hash=";
    constexpr std::string_view marker = "leonida.descriptor_hash=fnv1a_fmix32";
    bool marked = false, uses_table = false;
    if (words.size() < 5 || words.front() != 0x07230203) return false;
    for (std::size_t i = 5; i < words.size();) {
        const unsigned count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count || count > words.size() - i) return false;
        if (op == 54) break;  // OpFunction
        if (op == 330) {      // OpModuleProcessed
            const char* bytes = reinterpret_cast<const char*>(words.data() + i + 1);
            const auto size = (count - 1) * sizeof(std::uint32_t);
            const char* end = static_cast<const char*>(std::memchr(bytes, 0, size));
            if (!end) return false;
            const std::string_view tag(bytes, static_cast<std::size_t>(end - bytes));
            if (tag.starts_with(prefix)) {
                if (marked || tag != marker) return false;
                marked = true;
            }
        }
        if (op == 71 && count == 4 && words[i + 2] == 33 && words[i + 3] == 2) uses_table = true;
        i += count;
    }
    if (declared && (*declared == DescriptorHashKind::fnv1a_fmix32) != marked) return false;
    if (marked) return expected == DescriptorHashKind::fnv1a_fmix32;
    return !uses_table || expected == DescriptorHashKind::fnv1a;
}

// Checks the binary framing used by the translator and returns words aligned for Vulkan.
// The caller still relies on the driver or spirv-val for instruction and capability validation.
inline std::vector<std::uint32_t> read_spirv_words(std::istream& input, std::streamoff byte_count) {
    constexpr std::streamoff word_bytes = sizeof(std::uint32_t);
    constexpr std::streamoff header_bytes = 5 * word_bytes;
    if (byte_count < header_bytes || byte_count % word_bytes || !input.seekg(0)) return {};

    std::uint32_t magic = 0;
    if (!input.read(reinterpret_cast<char*>(&magic), word_bytes) || magic != 0x07230203) return {};

    std::vector<std::uint32_t> words(static_cast<std::size_t>(byte_count / word_bytes));
    words.front() = magic;
    if (!input.read(reinterpret_cast<char*>(words.data() + 1), byte_count - word_bytes)) return {};
    return words;
}

}  // namespace leonida
