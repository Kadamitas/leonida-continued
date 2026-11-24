#pragma once

#include <cstdint>

namespace leonida {

enum class DescriptorHashKind { fnv1a, fnv1a_fmix32 };

// MurmurHash3's 32-bit finalizer. Keep the operations in step with rdna2spirv.Gen.desc_probe.
inline std::uint32_t finalize_descriptor_hash(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

} // namespace leonida
