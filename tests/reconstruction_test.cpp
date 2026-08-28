/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdint>
#include <cstdio>

#include "p2000t_reconstruction.h"

int main() {
    for (std::uint8_t first = 0; first < 16u; ++first) {
        for (std::uint8_t second = 0; second < 16u; ++second) {
            for (std::uint8_t next = 0; next < 16u; ++next) {
                const std::uint8_t selected =
                    p2000t_guarded_second_tap(first, second, next);
                const std::uint8_t expected =
                    second != first && second != next ? first : second;
                if (selected != expected ||
                    (selected != first && selected != second)) {
                    std::fprintf(
                        stderr, "Guard failed for first=%u second=%u next=%u\n",
                        first, second, next);
                    return 1;
                }
            }
        }
    }

    /* White -> cyan transient -> blue rejects the third color. */
    if (p2000t_guarded_second_tap(7u, 3u, 1u) != 7u) {
        std::fputs("White/cyan/blue transient was not rejected\n", stderr);
        return 1;
    }
    /* A genuine transition represented by the next dot remains intact. */
    if (p2000t_guarded_second_tap(7u, 1u, 1u) != 1u) {
        std::fputs("Genuine transition was rejected\n", stderr);
        return 1;
    }
    /* A settled dot is unchanged. */
    if (p2000t_guarded_second_tap(1u, 1u, 7u) != 1u) {
        std::fputs("Settled dot changed\n", stderr);
        return 1;
    }

    const std::uint32_t word = 0x73112225u;
    const std::uint32_t nextWord = 0x45566778u;
    if (p2000t_packed_sample(word, 0u) != 7u ||
        p2000t_packed_sample(word, 7u) != 5u) {
        std::fputs("Packed sample extraction failed\n", stderr);
        return 1;
    }
    /* Dot zero sees 7 -> 3 -> 1 and rejects the transient 3. */
    if (p2000t_guarded_tap_from_words(word, nextWord, 0u, true) != 7u) {
        std::fputs("Packed transient rejection failed\n", stderr);
        return 1;
    }
    /* Dot three crosses the packed-word boundary to nextWord's first tap. */
    if (p2000t_guarded_tap_from_words(word, nextWord, 3u, true) != 2u) {
        std::fputs("Packed word-boundary guard failed\n", stderr);
        return 1;
    }
    /* Without another word, the final second tap is retained. */
    if (p2000t_guarded_tap_from_words(word, 0u, 3u, false) != 5u) {
        std::fputs("Final-dot handling failed\n", stderr);
        return 1;
    }

    if (p2000t_window_reconstruct(1u, 1u, 7u,
                                  P2000T_WINDOW_POLICY_COLOR_EARLY) != 1u ||
        p2000t_window_reconstruct(1u, 7u, 7u,
                                  P2000T_WINDOW_POLICY_COLOR_EARLY) != 7u ||
        p2000t_window_reconstruct(1u, 7u, 1u,
                                  P2000T_WINDOW_POLICY_COLOR_LATE) != 1u) {
        std::fputs("Atomic window majority failed\n", stderr);
        return 1;
    }
    if (p2000t_window_reconstruct(1u, 3u, 7u, P2000T_WINDOW_POLICY_CENTER) !=
            3u ||
        p2000t_window_reconstruct(
            1u, 3u, 7u, P2000T_WINDOW_POLICY_CHANNEL_MAJORITY) != 3u ||
        p2000t_window_reconstruct(1u, 3u, 7u,
                                  P2000T_WINDOW_POLICY_COLOR_EARLY) != 1u ||
        p2000t_window_reconstruct(1u, 3u, 7u,
                                  P2000T_WINDOW_POLICY_COLOR_LATE) != 7u) {
        std::fputs("All-distinct window policy failed\n", stderr);
        return 1;
    }

    std::uint32_t lookup[4096] = {};
    for (unsigned packed = 0; packed < 4096u; ++packed) {
        lookup[packed] = p2000t_window_lookup_entry(
            static_cast<std::uint8_t>(packed >> 8u),
            static_cast<std::uint8_t>(packed >> 4u),
            static_cast<std::uint8_t>(packed), P2000T_WINDOW_POLICY_COLOR_EARLY,
            0u, 1u, 2u);
    }
    const std::uint8_t samples[24] = {
        1, 1, 7, 1, 7, 7, 1, 7, 1, 1, 3, 7, 2, 2, 2, 4, 5, 4, 6, 6, 0, 7, 3, 1,
    };
    std::uint32_t packedWords[3] = {};
    for (unsigned sample = 0; sample < 24u; ++sample) {
        packedWords[sample / 8u] |= static_cast<std::uint32_t>(samples[sample])
                                    << (28u - (sample % 8u) * 4u);
    }
    p2000t_reconstruction_diagnostics_t diagnostics = {};
    std::uint8_t uncertainty = 0u;
    const std::uint32_t reconstructed =
        p2000t_reconstruct_window_group_with_evidence(
            packedWords[0], packedWords[1], packedWords[2], lookup,
            &diagnostics, &uncertainty);
    if (reconstructed != 0x17112467u || diagnostics.ambiguous_samples != 2u ||
        diagnostics.corrected_samples != 4u || uncertainty != 0xf7u) {
        std::fprintf(stderr,
                     "Packed window reconstruction failed: %08x, %u/%u/%02x\n",
                     reconstructed, diagnostics.corrected_samples,
                     diagnostics.ambiguous_samples, uncertainty);
        return 1;
    }

    if (p2000t_guard_uncertain_pair(0x17u, 0u) != 0x17u ||
        p2000t_guard_uncertain_pair(0x17u, 1u) != 0x11u ||
        p2000t_guard_uncertain_pair(0x17u, 2u) != 0x77u ||
        p2000t_guard_uncertain_pair(0x17u, 3u) != 0x17u ||
        p2000t_guard_uncertain_pair(0x77u, 2u) != 0x77u) {
        std::fputs("Confidence guard changed unsupported pair evidence\n",
                   stderr);
        return 1;
    }
    if (p2000t_guard_uncertain_pairs(0x12345678u, 0x93u) != 0x22335678u) {
        std::fputs("Packed confidence guard failed\n", stderr);
        return 1;
    }
    return 0;
}
