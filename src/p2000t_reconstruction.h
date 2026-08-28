/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_RECONSTRUCTION_H
#define P2000T_RECONSTRUCTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Three-sample window policies used by the Pico 2 capture engine. */
typedef enum {
    P2000T_WINDOW_POLICY_CENTER,
    P2000T_WINDOW_POLICY_CHANNEL_MAJORITY,
    P2000T_WINDOW_POLICY_COLOR_EARLY,
    P2000T_WINDOW_POLICY_COLOR_LATE,
} p2000t_window_policy_t;

/** Per-frame evidence produced while reconstructing a captured frame. */
typedef struct {
    uint32_t corrected_samples;
    uint32_t ambiguous_samples;
    uint32_t red_corrections;
    uint32_t green_corrections;
    uint32_t blue_corrections;
    uint32_t line_deadline_misses;
    uint8_t reconstruction_mode;
    uint8_t capture_engine;
    uint8_t samples_per_output;
} p2000t_reconstruction_diagnostics_t;

enum {
    P2000T_WINDOW_ENTRY_OUTPUT_MASK = 0x0000000fu,
    P2000T_WINDOW_ENTRY_DIAGNOSTICS_SHIFT = 8,
    P2000T_WINDOW_DIAGNOSTIC_UNCERTAIN_SHIFT = 20,
};

/**
 * @brief Reject a one-sample color transient from the selected second tap.
 *
 * A genuine transition leaves the second tap equal to either the first tap of
 * its own nominal source dot or the first tap of the following dot. A value
 * which matches neither is the one-tick intermediate RGB state observed in
 * the 126 MHz diagnostic traces. Falling back to the current dot's first tap
 * removes that state without using frame history or delaying real changes.
 */
static inline uint8_t p2000t_guarded_second_tap(uint8_t first, uint8_t second,
                                                uint8_t next_first) {
    return second != first && second != next_first ? first : second;
}

/**
 * @brief Reconstruct one three-tick 126 MHz sampling window.
 *
 * Atomic color policies deliberately avoid emitting the center value when all
 * three samples differ. Such A-B-C sequences are the measured signature of a
 * one-tick intermediate RGB state at a multi-channel edge. The early and late
 * variants retain the corresponding stable endpoint so their edge-placement
 * bias can be measured independently.
 */
static inline uint8_t p2000t_window_reconstruct(uint8_t first, uint8_t center,
                                                uint8_t last,
                                                p2000t_window_policy_t policy) {
    first &= 0x0fu;
    center &= 0x0fu;
    last &= 0x0fu;
    if (policy == P2000T_WINDOW_POLICY_CENTER) {
        return center;
    }
    if (policy == P2000T_WINDOW_POLICY_CHANNEL_MAJORITY) {
        return (uint8_t)((first & center) | (first & last) | (center & last));
    }
    if (first == center || first == last) {
        return first;
    }
    if (center == last) {
        return center;
    }
    return policy == P2000T_WINDOW_POLICY_COLOR_LATE ? last : first;
}

/** Build the compact lookup entry used by the scanline reconstruction IRQ. */
static inline uint32_t
p2000t_window_lookup_entry(uint8_t first, uint8_t center, uint8_t last,
                           p2000t_window_policy_t policy, unsigned red_channel,
                           unsigned green_channel, unsigned blue_channel) {
    first &= 0x0fu;
    center &= 0x0fu;
    last &= 0x0fu;
    const uint8_t output =
        p2000t_window_reconstruct(first, center, last, policy);
    const uint8_t changed = (uint8_t)(output ^ (center & 0x0fu));
    const bool ambiguous = first != center && first != last && center != last;
    const bool uncertain = first != center || center != last;
    const uint32_t contribution =
        (changed != 0u ? 1u : 0u) | (ambiguous ? 1u << 4u : 0u) |
        (((changed >> red_channel) & 1u) << 8u) |
        (((changed >> green_channel) & 1u) << 12u) |
        (((changed >> blue_channel) & 1u) << 16u) |
        (uncertain ? 1u << P2000T_WINDOW_DIAGNOSTIC_UNCERTAIN_SHIFT : 0u);
    return output | (contribution << P2000T_WINDOW_ENTRY_DIAGNOSTICS_SHIFT);
}

/**
 * @brief Reconstruct eight output samples from three packed input words.
 *
 * Eight chronological four-bit samples occupy each input word. Three input
 * words therefore contain exactly eight three-sample windows, avoiding all
 * division and cross-word branches in the line-rate interrupt handler.
 */
static inline uint32_t p2000t_reconstruct_window_group_with_evidence(
    uint32_t first_word, uint32_t second_word, uint32_t third_word,
    const uint32_t lookup[4096],
    p2000t_reconstruction_diagnostics_t *diagnostics, uint8_t *uncertain_mask) {
    const uint16_t triplets[8] = {
        (uint16_t)(first_word >> 20u),
        (uint16_t)((first_word >> 8u) & 0x0fffu),
        (uint16_t)(((first_word & 0x00ffu) << 4u) | (second_word >> 28u)),
        (uint16_t)((second_word >> 16u) & 0x0fffu),
        (uint16_t)((second_word >> 4u) & 0x0fffu),
        (uint16_t)(((second_word & 0x000fu) << 8u) | (third_word >> 24u)),
        (uint16_t)((third_word >> 12u) & 0x0fffu),
        (uint16_t)(third_word & 0x0fffu),
    };
    uint32_t output = 0u;
    uint32_t packed_diagnostics = 0u;
    uint8_t uncertainty = 0u;
    for (unsigned index = 0; index < 8u; ++index) {
        const uint32_t entry = lookup[triplets[index]];
        output = (output << 4u) | (entry & P2000T_WINDOW_ENTRY_OUTPUT_MASK);
        packed_diagnostics += entry >> P2000T_WINDOW_ENTRY_DIAGNOSTICS_SHIFT;
        uncertainty =
            (uint8_t)((uncertainty << 1u) |
                      ((entry >> (P2000T_WINDOW_ENTRY_DIAGNOSTICS_SHIFT +
                                  P2000T_WINDOW_DIAGNOSTIC_UNCERTAIN_SHIFT)) &
                       1u));
    }
    diagnostics->corrected_samples += packed_diagnostics & 0x0fu;
    diagnostics->ambiguous_samples += (packed_diagnostics >> 4u) & 0x0fu;
    diagnostics->red_corrections += (packed_diagnostics >> 8u) & 0x0fu;
    diagnostics->green_corrections += (packed_diagnostics >> 12u) & 0x0fu;
    diagnostics->blue_corrections += (packed_diagnostics >> 16u) & 0x0fu;
    if (uncertain_mask != NULL) {
        *uncertain_mask = uncertainty;
    }
    return output;
}

/** Reconstruct a group when per-output confidence is not required. */
static inline uint32_t p2000t_reconstruct_window_group(
    uint32_t first_word, uint32_t second_word, uint32_t third_word,
    const uint32_t lookup[4096],
    p2000t_reconstruction_diagnostics_t *diagnostics) {
    return p2000t_reconstruct_window_group_with_evidence(
        first_word, second_word, third_word, lookup, diagnostics, NULL);
}

/** Return one chronological four-bit sample from a packed capture word. */
static inline uint8_t p2000t_packed_sample(uint32_t word, unsigned sample) {
    return (uint8_t)(word >> (28u - sample * 4u)) & 0x0fu;
}

/**
 * @brief Correct one uncertain half-dot only from a stable paired half-dot.
 *
 * The two uncertainty bits correspond to the high and low nibbles. Equal
 * colors, two unanimous colors, and two uncertain colors are all preserved.
 * This means the guard cannot move an ordinary edge or impose a global
 * early/late bias.
 */
static inline uint8_t p2000t_guard_uncertain_pair(uint8_t pair,
                                                  uint8_t uncertainty) {
    const uint8_t first = (uint8_t)(pair >> 4u);
    const uint8_t second = pair & 0x0fu;
    uncertainty &= 0x03u;
    if (first == second || uncertainty == 0u || uncertainty == 3u) {
        return pair;
    }
    if (uncertainty == 2u) {
        return (uint8_t)((second << 4u) | second);
    }
    return (uint8_t)((first << 4u) | first);
}

/** Apply the conservative confidence guard to four adjacent source dots. */
static inline uint32_t p2000t_guard_uncertain_pairs(uint32_t reconstructed,
                                                    uint8_t uncertainty) {
    uint32_t output = 0u;
    for (unsigned pair = 0; pair < 4u; ++pair) {
        const unsigned shift = 24u - pair * 8u;
        const uint8_t samples = (uint8_t)(reconstructed >> shift);
        const uint8_t evidence = (uint8_t)(uncertainty >> (6u - pair * 2u));
        output |= (uint32_t)p2000t_guard_uncertain_pair(samples, evidence)
                  << shift;
    }
    return output;
}

/**
 * @brief Select the guarded second tap of one dot from packed capture words.
 *
 * @param word Capture word holding four complete two-tap source dots.
 * @param next_word Following capture word when @p has_next_word is true.
 * @param dot Zero-based source dot within @p word, in the range 0..3.
 * @param has_next_word Whether @p next_word belongs to the same source line.
 */
static inline uint8_t p2000t_guarded_tap_from_words(uint32_t word,
                                                    uint32_t next_word,
                                                    unsigned dot,
                                                    bool has_next_word) {
    const unsigned first_sample = dot * 2u;
    const uint8_t first = p2000t_packed_sample(word, first_sample);
    const uint8_t second = p2000t_packed_sample(word, first_sample + 1u);
    const uint8_t next_first =
        dot < 3u
            ? p2000t_packed_sample(word, first_sample + 2u)
            : (has_next_word ? p2000t_packed_sample(next_word, 0u) : second);
    return p2000t_guarded_second_tap(first, second, next_first);
}

#endif
