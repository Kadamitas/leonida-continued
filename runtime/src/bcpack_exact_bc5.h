// Included by bcpack.cpp alongside the other block reconstruction routines.
// BC5 stores two BC4 channels. Each block predicts its endpoints and indices independently,
// using up to 255 earlier blocks or an entry from the supplied format dictionary.
void exact_bc5_blocks(const Lanes& lanes, std::size_t blocks, const ExactTables& tables, std::uint8_t* out) {
    using Block = std::array<std::uint8_t, 36>;  // four endpoints followed by 16 indices per channel
    std::array<Block, 256> history{};
    constexpr Block zero{};
    constexpr std::array<unsigned, 8> bc4_codes{0, 2, 3, 4, 5, 6, 7, 1};
    for (std::size_t i = 0; i < blocks; ++i) {
        const auto at = 2 * i;
        const auto index_distance = static_cast<unsigned>(inter8(lanes[1][at], lanes[1][at + 1]));
        const auto delta = unzig(inter8(lanes[0][at], lanes[0][at + 1]));
        const unsigned endpoint_distance = (int(index_distance) + delta) & 255;
        auto reference = [&](unsigned distance) -> const std::uint8_t* {
            if (distance == 0) return zero.data();
            if (distance <= i) return history[(i - distance) % history.size()].data();
            return tables.bc5.data() + (distance - i - 1) * zero.size();
        };
        const auto* ep = reference(endpoint_distance);
        const auto* indices = reference(index_distance) + 4;
        auto& block = history[i % history.size()];
        for (unsigned channel = 0; channel < 2; ++channel) {
            const unsigned first = channel * 2;
            auto* dest = out + i * 16 + channel * 8;
            for (unsigned k = 0; k < 2; ++k) {
                block[first + k] = static_cast<std::uint8_t>(ep[first + k] + unzig(lanes[2 + channel][at + k]));
                dest[k] = block[first + k] ^ 0x80;
            }
            std::uint64_t packed = 0;
            for (unsigned pair = 0; pair < 8; ++pair) {
                const unsigned rank = lanes[4 + channel * 4 + pair / 2][at + pair % 2];
                const unsigned residual = tables.rank5[rank & 63];
                for (unsigned k = 0; k < 2; ++k) {
                    const unsigned pixel = pair * 2 + k;
                    const unsigned n = channel * 16 + pixel;
                    const auto value = static_cast<std::uint8_t>((indices[n] + (residual >> (4 * k))) & 7);
                    block[4 + n] = value;
                    packed |= std::uint64_t(bc4_codes[value]) << (3 * pixel);
                }
            }
            put_le(dest + 2, packed, 6);
        }
    }
}
