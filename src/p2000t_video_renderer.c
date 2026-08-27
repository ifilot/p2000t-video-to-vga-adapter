/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p2000t_video_renderer.c
 * @brief Scanvideo rendering for captured frames and the no-signal screen.
 */

#include "p2000t_video_renderer.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "p2000t_capture.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/stdlib.h"

/** Layout and font constants used by the no-signal text overlay. */
enum {
    VGA_LEFT_MARGIN = (P2000T_VGA_RENDER_WIDTH - P2000T_CAPTURE_WIDTH) / 2,
    /**< Black pixels to the left of the centered source image. */
    VGA_RIGHT_MARGIN =
        P2000T_VGA_RENDER_WIDTH - VGA_LEFT_MARGIN - P2000T_CAPTURE_WIDTH,
    /**< Black pixels to the right of the centered source image. */
    NO_SIGNAL_GLYPH_WIDTH = 5,    /**< Width of one bitmap glyph. */
    NO_SIGNAL_GLYPH_HEIGHT = 7,   /**< Height of one bitmap glyph. */
    NO_SIGNAL_FONT_GLYPHS = 36,   /**< Stored A-Z and 0-9 glyph count. */
    NO_SIGNAL_TITLE_TOP = 3,   /**< Application title in logical VGA lines. */
    NO_SIGNAL_VERSION_TOP = 13, /**< Firmware version below the title. */
    NO_SIGNAL_ICON_TOP = 196,  /**< Disconnected-plug pictogram top edge. */
    NO_SIGNAL_ICON_HEIGHT = 14, /**< Pictogram height in logical VGA lines. */
    NO_SIGNAL_STATUS_TOP = 217, /**< Status label below the pictogram. */
    NO_SIGNAL_PALETTE_COLORS = 16,
    /**< Number of colors in the indexed artwork palette. */
    NO_SIGNAL_PALETTE_BYTES = NO_SIGNAL_PALETTE_COLORS * sizeof(uint16_t),
    /**< Byte offset from the asset start to its packed pixel data. */
    NO_SIGNAL_PACKED_BYTES_PER_LINE = P2000T_VGA_RENDER_WIDTH / 2,
    /**< Two four-bit palette indices are stored in each source byte. */
};

_Static_assert(VGA_LEFT_MARGIN == 80 && VGA_RIGHT_MARGIN == 80,
               "The 480-sample source image must be centered in VGA");
_Static_assert(P2000T_RAW_SCANLINE_WORDS == 323,
               "CMake scanvideo storage must match the raw line renderer");

/** Application identity and status displayed around the centered artwork. */
static const char no_signal_title[] = "P2000T VID2VGA";
static const char no_signal_version[] = "VERSION " P2000T_VID2VGA_VERSION;
static const char no_signal_status[] = "NO CONNECTION";

/** Compact indexed artwork embedded by CMake. */
extern const uint8_t no_connection_image_green_phosphor[];
extern const uint8_t no_connection_image_synthwave[];
extern const uint8_t no_connection_image_amber_circuit[];

/** Full-scale RGB444 colors in P2000T bit order R=1, G=2, B=4. */
static const uint16_t source_colors[8] = {
    0x0000, 0x000f, 0x00f0, 0x00ff, 0x0f00, 0x0f0f, 0x0ff0, 0x0fff,
};

/** Two decoded RGB444 pixels for every packed pair of raw input nibbles. */
static uint32_t raw_byte_colors[256];

/** Two RGB444 pixels for each packed pair in each artwork palette. */
static uint32_t
    no_signal_byte_colors[P2000T_NO_SIGNAL_ARTWORK_COUNT][256];

/**
 * @brief Resolve an artwork selection to its flash-resident asset.
 *
 * This is always inlined into the RAM-resident scanline renderer so selecting
 * an image does not introduce a call back into flash on the critical path.
 */
static inline __attribute__((always_inline)) const uint8_t *
no_signal_image(p2000t_no_signal_artwork_t artwork) {
    switch (artwork) {
    case P2000T_NO_SIGNAL_GREEN_PHOSPHOR:
        return no_connection_image_green_phosphor;
    case P2000T_NO_SIGNAL_SYNTHWAVE:
        return no_connection_image_synthwave;
    case P2000T_NO_SIGNAL_AMBER_CIRCUIT:
        return no_connection_image_amber_circuit;
    default:
        return no_connection_image_amber_circuit;
    }
}

/** Five-bit rows for uppercase letters A-Z followed by digits 0-9. */
static const uint8_t
    no_signal_font[NO_SIGNAL_FONT_GLYPHS][NO_SIGNAL_GLYPH_HEIGHT] = {
        {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // A
        {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, // B
        {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, // C
        {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}, // D
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, // E
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, // F
        {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e}, // G
        {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // H
        {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f}, // I
        {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c}, // J
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, // L
        {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
        {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // O
        {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, // P
        {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, // Q
        {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, // R
        {0x0e, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, // S
        {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // U
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, // V
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11}, // W
        {0x11, 0x0a, 0x04, 0x04, 0x04, 0x0a, 0x11}, // X
        {0x11, 0x0a, 0x04, 0x04, 0x04, 0x04, 0x04}, // Y
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, // Z
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, // 0
        {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, // 1
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, // 2
        {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}, // 3
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, // 4
        {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}, // 5
        {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}, // 6
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, // 8
        {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}, // 9
};

/**
 * @brief Complete a composable raw scanline and mark it ready.
 *
 * @param scanline_buffer Scanvideo buffer being produced.
 * @param tokens Writable 16-bit view of the scanvideo data storage.
 */
static void finish_raw_scanline(scanvideo_scanline_buffer_t *scanline_buffer,
                                uint16_t *tokens) {
    tokens[P2000T_RAW_SCANLINE_TOKENS - 4] = COMPOSABLE_RAW_1P;
    tokens[P2000T_RAW_SCANLINE_TOKENS - 3] = 0x0000;
    tokens[P2000T_RAW_SCANLINE_TOKENS - 2] = COMPOSABLE_EOL_SKIP_ALIGN;
    tokens[P2000T_RAW_SCANLINE_TOKENS - 1] = 0;
    scanline_buffer->data_used = P2000T_RAW_SCANLINE_WORDS;
    scanline_buffer->status = SCANLINE_OK;
}

/**
 * @brief Return one bitmap-font row for a supported character.
 *
 * This lookup renders only the fixed no-signal status text; it does not
 * inspect or interpret captured source pixels. Unsupported characters are
 * blank, except for a period represented by one pixel on the final row.
 *
 * @param character Character to look up.
 * @param row Zero-based row within the seven-row glyph.
 * @return Five low-order bits containing the requested glyph row.
 */
static inline uint8_t no_signal_glyph_row(char character, unsigned row) {
    hard_assert(row < NO_SIGNAL_GLYPH_HEIGHT);
    if (character >= 'A' && character <= 'Z') {
        return no_signal_font[(unsigned)(character - 'A')][row];
    }
    if (character >= '0' && character <= '9') {
        return no_signal_font[26u + (unsigned)(character - '0')][row];
    }
    return character == '.' && row == 6u ? 0x04u : 0x00u;
}

/**
 * @brief Overlay a centered line of bitmap text on a raw scanline.
 *
 * @param tokens Writable 16-bit composable scanline tokens.
 * @param y Zero-based logical VGA line currently being rendered.
 * @param message Null-terminated text to draw.
 * @param top Top edge of the text in logical VGA lines.
 * @param x_scale Horizontal integer glyph scale.
 * @param y_scale Vertical integer glyph scale.
 * @param color RGB444 text color.
 */
static void __not_in_flash_func(render_no_signal_text)(
    uint16_t *tokens, unsigned y, const char *message, unsigned top,
    unsigned x_scale, unsigned y_scale, uint16_t color) {
    if (y < top || y >= top + NO_SIGNAL_GLYPH_HEIGHT * y_scale) {
        return;
    }

    const size_t length = strlen(message);
    const unsigned text_width =
        (unsigned)(((NO_SIGNAL_GLYPH_WIDTH + 1u) * length - 1u) * x_scale);
    const unsigned text_left = (P2000T_VGA_RENDER_WIDTH - text_width) / 2u;
    const unsigned font_row = (y - top) / y_scale;
    for (size_t index = 0; index < length; ++index) {
        const unsigned character_x =
            text_left +
            (unsigned)index * (NO_SIGNAL_GLYPH_WIDTH + 1u) * x_scale;
        const uint8_t row = no_signal_glyph_row(message[index], font_row);
        for (unsigned column = 0; column < NO_SIGNAL_GLYPH_WIDTH; ++column) {
            if ((row & (0x10u >> column)) == 0u) {
                continue;
            }
            const unsigned pixel_x = character_x + column * x_scale;
            for (unsigned offset = 0; offset < x_scale; ++offset) {
                tokens[pixel_x + offset + 2u] = color;
            }
        }
    }
}

/**
 * @brief Draw a compact disconnected plug-and-socket pictogram.
 *
 * The artwork below the computer is deliberately rendered in firmware so it
 * stays pure white after palette reduction and remains crisp on VGA output.
 *
 * @param tokens Writable 16-bit composable scanline tokens.
 * @param y Zero-based logical VGA line currently being rendered.
 * @param top Top edge of the pictogram in logical VGA lines.
 * @param color RGB444 pictogram color.
 */
static void __not_in_flash_func(render_no_signal_icon)(uint16_t *tokens,
                                                       unsigned y,
                                                       unsigned top,
                                                       uint16_t color) {
    if (y < top || y >= top + NO_SIGNAL_ICON_HEIGHT) {
        return;
    }

    const unsigned row = y - top;
    const unsigned center = P2000T_VGA_RENDER_WIDTH / 2u;

    /* Cable entering the solid plug from the left. */
    if (row >= 6u && row <= 7u) {
        for (unsigned x = center - 36u; x < center - 22u; ++x) {
            tokens[x + 2u] = color;
        }
    }

    /* Solid plug body with two separated prongs facing right. */
    if (row >= 3u && row <= 10u) {
        for (unsigned x = center - 22u; x < center - 10u; ++x) {
            tokens[x + 2u] = color;
        }
    }
    if ((row >= 4u && row <= 5u) || (row >= 8u && row <= 9u)) {
        for (unsigned x = center - 10u; x < center - 4u; ++x) {
            tokens[x + 2u] = color;
        }
    }

    /* Outlined socket and cable, clearly separated from the plug. */
    if (row == 3u || row == 10u) {
        for (unsigned x = center + 4u; x < center + 22u; ++x) {
            tokens[x + 2u] = color;
        }
    } else if (row > 3u && row < 10u) {
        tokens[center + 4u + 2u] = color;
        tokens[center + 5u + 2u] = color;
        tokens[center + 20u + 2u] = color;
        tokens[center + 21u + 2u] = color;
    }
    if (row >= 6u && row <= 7u) {
        for (unsigned x = center + 22u; x < center + 36u; ++x) {
            tokens[x + 2u] = color;
        }
    }

    /* Small separation rays make the lost connection legible at a glance. */
    if (row == 0u) {
        tokens[center + 2u] = color;
    } else if (row == 1u) {
        tokens[center - 2u + 2u] = color;
        tokens[center + 2u + 2u] = color;
    } else if (row == 12u) {
        tokens[center - 2u + 2u] = color;
        tokens[center + 2u + 2u] = color;
    } else if (row == 13u) {
        tokens[center + 2u] = color;
    }
}

void p2000t_video_renderer_initialize(void) {
    uint16_t raw_nibble_colors[16];
    for (unsigned raw = 0; raw < 16u; ++raw) {
        const unsigned color =
            ((((raw >> P2000T_RED_CHANNEL) & 1u) == 0u) ? 1u : 0u) |
            ((((raw >> P2000T_GREEN_CHANNEL) & 1u) == 0u) ? 2u : 0u) |
            ((((raw >> P2000T_BLUE_CHANNEL) & 1u) == 0u) ? 4u : 0u);
        raw_nibble_colors[raw] = source_colors[color];
    }
    for (unsigned raw = 0; raw < 256u; ++raw) {
        raw_byte_colors[raw] =
            raw_nibble_colors[raw >> 4u] |
            ((uint32_t)raw_nibble_colors[raw & 0x0fu] << 16u);
    }

    /* Expand all three tiny flash-resident palettes into SRAM. Selection can
     * then change at a frame boundary without rebuilding a table or adding
     * work to the scanline deadline. */
    for (unsigned artwork = 0; artwork < P2000T_NO_SIGNAL_ARTWORK_COUNT;
         ++artwork) {
        const uint16_t *palette = (const uint16_t *)no_signal_image(
            (p2000t_no_signal_artwork_t)artwork);
        for (unsigned packed = 0; packed < 256u; ++packed) {
            no_signal_byte_colors[artwork][packed] =
                palette[packed >> 4u] |
                ((uint32_t)palette[packed & 0x0fu] << 16u);
        }
    }
}

void __not_in_flash_func(p2000t_video_render_no_signal_scanline)(
    scanvideo_scanline_buffer_t *scanline_buffer, unsigned y,
    p2000t_no_signal_artwork_t artwork) {
    hard_assert(scanline_buffer != NULL);
    hard_assert(scanline_buffer->data_max >= P2000T_RAW_SCANLINE_WORDS);
    hard_assert(y < P2000T_VGA_RENDER_HEIGHT);
    hard_assert((unsigned)artwork < P2000T_NO_SIGNAL_ARTWORK_COUNT);

    const uint8_t *image = no_signal_image(artwork);
    const uint32_t *color_pairs = no_signal_byte_colors[artwork];
    const uint8_t *image_row =
        &image[NO_SIGNAL_PALETTE_BYTES +
               y * NO_SIGNAL_PACKED_BYTES_PER_LINE];
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[0] = COMPOSABLE_RAW_RUN;
    const uint32_t first_pair = color_pairs[image_row[0]];
    tokens[1] = (uint16_t)first_pair;
    tokens[2] = P2000T_VGA_RENDER_WIDTH - 3;
    tokens[3] = (uint16_t)(first_pair >> 16u);

    /* This is intentionally a separate compact pipeline from the source
     * renderer. It reads only 320 indexed bytes from XIP flash per line,
     * instead of copying 1,280 bytes of RGB444 data before the VGA deadline. */
    hard_assert(((uintptr_t)&tokens[4] & 3u) == 0u);
    uint32_t *destination = (uint32_t *)&tokens[4];
    for (unsigned index = 1; index < NO_SIGNAL_PACKED_BYTES_PER_LINE;
         ++index) {
        *destination++ = color_pairs[image_row[index]];
    }
    hard_assert(destination ==
                (uint32_t *)&tokens[P2000T_VGA_RENDER_WIDTH + 2u]);

    render_no_signal_text(tokens, y, no_signal_title, NO_SIGNAL_TITLE_TOP, 3, 1,
                          0x0fff);
    render_no_signal_text(tokens, y, no_signal_version, NO_SIGNAL_VERSION_TOP,
                          2, 1, 0x0fff);
    render_no_signal_icon(tokens, y, NO_SIGNAL_ICON_TOP, 0x0fff);
    render_no_signal_text(tokens, y, no_signal_status, NO_SIGNAL_STATUS_TOP, 2,
                          1, 0x0fff);
    finish_raw_scanline(scanline_buffer, tokens);
}

void __not_in_flash_func(p2000t_video_render_source_scanline)(
    scanvideo_scanline_buffer_t *scanline_buffer, const uint32_t *frame,
    unsigned source_y) {
    hard_assert(scanline_buffer != NULL);
    hard_assert(scanline_buffer->data_max >= P2000T_RAW_SCANLINE_WORDS);
    hard_assert(frame != NULL);
    hard_assert(source_y < P2000T_CAPTURE_HEIGHT);

    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[0] = COMPOSABLE_RAW_RUN;
    tokens[1] = 0x0000;
    tokens[2] = P2000T_VGA_RENDER_WIDTH - 3;
    uint16_t *destination = &tokens[3];

    /* Pixel zero is tokens[1]; destination begins at VGA pixel one. */
    for (unsigned x = 1; x < VGA_LEFT_MARGIN; ++x) {
        *destination++ = 0x0000;
    }
    const uint32_t *source = frame + source_y * P2000T_CAPTURE_WORDS_PER_LINE;
    hard_assert(((uintptr_t)destination & 3u) == 0u);
    uint32_t *packed_destination = (uint32_t *)destination;
    for (unsigned word_index = 0; word_index < P2000T_CAPTURE_WORDS_PER_LINE;
         ++word_index) {
        const uint32_t raw = source[word_index];
        *packed_destination++ = raw_byte_colors[(uint8_t)(raw >> 24u)];
        *packed_destination++ = raw_byte_colors[(uint8_t)(raw >> 16u)];
        *packed_destination++ = raw_byte_colors[(uint8_t)(raw >> 8u)];
        *packed_destination++ = raw_byte_colors[(uint8_t)raw];
    }
    destination = (uint16_t *)packed_destination;
    for (unsigned x = 0; x < VGA_RIGHT_MARGIN; ++x) {
        *destination++ = 0x0000;
    }

    hard_assert(destination == &tokens[P2000T_VGA_RENDER_WIDTH + 2u]);
    finish_raw_scanline(scanline_buffer, tokens);
}
