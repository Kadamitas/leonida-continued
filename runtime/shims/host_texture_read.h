#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>

namespace rt {

enum class HostTextureRead { failed, captured, unchanged };

// The caller owns stable, separate scratch and previously uploaded bytes. All reads of ordinary title
// memory use ReadProcessMemory. A failed/unchanged observation can leave arbitrary bytes in scratch.
inline HostTextureRead read_host_texture(const void* source, std::span<std::byte> scratch,
                                         std::span<const std::byte> previous, SIZE_T& read) noexcept {
    constexpr std::size_t chunk = 1024 * 1024;
    if (previous.size() == scratch.size() && scratch.size() >= 2 * chunk) {
        const auto* bytes = static_cast<const std::byte*>(source);
        std::size_t offset = 0;
        while (offset < scratch.size()) {
            const auto count = (std::min)(chunk, scratch.size() - offset);
            SIZE_T copied = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), bytes + offset, scratch.data(), count, &copied) || copied != count) {
                read = offset + copied;
                return HostTextureRead::failed;
            }
            if (std::memcmp(scratch.data(), previous.data() + offset, count) != 0) break;
            offset += count;
        }
        if (offset == scratch.size()) {
            read = offset;
            return HostTextureRead::unchanged;
        }
    }
    // First use, small textures and detected edits all get a fresh, complete snapshot.
    read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), source, scratch.data(), scratch.size(), &read) || read != scratch.size())
        return HostTextureRead::failed;
    return HostTextureRead::captured;
}

} // namespace rt
