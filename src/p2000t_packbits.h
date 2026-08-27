/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_PACKBITS_H
#define P2000T_PACKBITS_H

/**
 * @file p2000t_packbits.h
 * @brief Allocation-free PackBits codec shared by firmware and viewer tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    P2000T_PACKBITS_MAX_RUN = 128,
    P2000T_PACKBITS_MAX_CHUNK = 1 + P2000T_PACKBITS_MAX_RUN,
};

static inline size_t p2000t_packbits_repeat_length(const uint8_t *input,
                                                   size_t input_size,
                                                   size_t offset) {
    size_t limit = input_size - offset;
    if (limit > P2000T_PACKBITS_MAX_RUN) {
        limit = P2000T_PACKBITS_MAX_RUN;
    }
    size_t length = 1u;
    while (length < limit && input[offset + length] == input[offset]) {
        ++length;
    }
    return length;
}

static inline size_t p2000t_packbits_next_chunk(
    const uint8_t *input, size_t input_size, size_t *input_offset,
    uint8_t *output) {
    if (*input_offset >= input_size) {
        return 0u;
    }

    const size_t repeat = p2000t_packbits_repeat_length(
        input, input_size, *input_offset);
    if (repeat >= 3u) {
        if (output != NULL) {
            output[0] = (uint8_t)(257u - repeat);
            output[1] = input[*input_offset];
        }
        *input_offset += repeat;
        return 2u;
    }

    const size_t literal_start = *input_offset;
    size_t literal_length = 0u;
    while (*input_offset < input_size &&
           literal_length < P2000T_PACKBITS_MAX_RUN) {
        const size_t next_repeat = p2000t_packbits_repeat_length(
            input, input_size, *input_offset);
        if (next_repeat >= 3u) {
            break;
        }
        ++*input_offset;
        ++literal_length;
    }

    if (output != NULL) {
        output[0] = (uint8_t)(literal_length - 1u);
        memcpy(&output[1], &input[literal_start], literal_length);
    }
    return 1u + literal_length;
}

static inline size_t p2000t_packbits_encoded_size(const uint8_t *input,
                                                  size_t input_size) {
    size_t input_offset = 0u;
    size_t encoded_size = 0u;
    while (input_offset < input_size) {
        encoded_size += p2000t_packbits_next_chunk(
            input, input_size, &input_offset, NULL);
    }
    return encoded_size;
}

static inline bool p2000t_packbits_decode(const uint8_t *input,
                                          size_t input_size,
                                          uint8_t *output,
                                          size_t output_size) {
    size_t input_offset = 0u;
    size_t output_offset = 0u;
    while (input_offset < input_size) {
        const uint8_t control = input[input_offset++];
        if (control <= 127u) {
            const size_t count = (size_t)control + 1u;
            if (count > input_size - input_offset ||
                count > output_size - output_offset) {
                return false;
            }
            memcpy(&output[output_offset], &input[input_offset], count);
            input_offset += count;
            output_offset += count;
        } else if (control >= 129u) {
            const size_t count = 257u - (size_t)control;
            if (input_offset >= input_size ||
                count > output_size - output_offset) {
                return false;
            }
            memset(&output[output_offset], input[input_offset++], count);
            output_offset += count;
        }
    }
    return output_offset == output_size;
}

#endif
