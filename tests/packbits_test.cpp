/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "p2000t_packbits.h"

namespace {

bool roundTrip(const std::vector<std::uint8_t> &input) {
    const std::size_t expected =
        p2000t_packbits_encoded_size(input.data(), input.size());
    std::vector<std::uint8_t> encoded;
    encoded.reserve(expected);
    std::array<std::uint8_t, P2000T_PACKBITS_MAX_CHUNK> chunk = {};
    std::size_t offset = 0u;
    while (offset < input.size()) {
        const std::size_t size = p2000t_packbits_next_chunk(
            input.data(), input.size(), &offset, chunk.data());
        encoded.insert(encoded.end(), chunk.begin(),
                       chunk.begin() + static_cast<std::ptrdiff_t>(size));
    }
    std::vector<std::uint8_t> decoded(input.size());
    return encoded.size() == expected &&
           p2000t_packbits_decode(encoded.data(), encoded.size(),
                                  decoded.data(), decoded.size()) &&
           decoded == input;
}

}  // namespace

int main() {
    std::vector<std::vector<std::uint8_t>> cases = {
        {}, {0x00}, {0xff, 0xff}, std::vector<std::uint8_t>(128, 0xaa),
        std::vector<std::uint8_t>(129, 0xaa)};
    std::mt19937 generator(0x50325446u);
    std::uniform_int_distribution<unsigned> distribution(0u, 255u);
    for (unsigned iteration = 0; iteration < 200u; ++iteration) {
        const std::size_t length = iteration < 130u ? iteration : 43200u;
        std::vector<std::uint8_t> input(length);
        std::generate(input.begin(), input.end(), [&] {
            return static_cast<std::uint8_t>(distribution(generator));
        });
        cases.push_back(std::move(input));
    }
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (!roundTrip(cases[i])) {
            std::fprintf(stderr, "PackBits round trip failed for case %zu\n", i);
            return 1;
        }
    }

    const std::array<std::uint8_t, 2> invalid = {2u, 0x42u};
    std::array<std::uint8_t, 4> output = {};
    if (p2000t_packbits_decode(invalid.data(), invalid.size(), output.data(),
                               output.size())) {
        std::fputs("Truncated PackBits literal was accepted\n", stderr);
        return 1;
    }
    return 0;
}
