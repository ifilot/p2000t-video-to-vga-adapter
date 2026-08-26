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

/** Layout and font constants used by the no-signal status card. */
enum {
    VGA_LEFT_MARGIN = (P2000T_VGA_RENDER_WIDTH - P2000T_CAPTURE_WIDTH) / 2,
    /**< Black pixels to the left of the centered source image. */
    VGA_RIGHT_MARGIN =
        P2000T_VGA_RENDER_WIDTH - VGA_LEFT_MARGIN - P2000T_CAPTURE_WIDTH,
    /**< Black pixels to the right of the centered source image. */
    NO_SIGNAL_GLYPH_WIDTH = 5,    /**< Width of one bitmap glyph. */
    NO_SIGNAL_GLYPH_HEIGHT = 7,   /**< Height of one bitmap glyph. */
    NO_SIGNAL_FONT_GLYPHS = 36,   /**< Stored A-Z and 0-9 glyph count. */
    NO_SIGNAL_PANEL_LEFT = 90,    /**< Left edge of the status panel. */
    NO_SIGNAL_PANEL_RIGHT = 550,  /**< Exclusive right panel edge. */
    NO_SIGNAL_PANEL_TOP = 70,     /**< Top edge of the status panel. */
    NO_SIGNAL_PANEL_BOTTOM = 170, /**< Exclusive bottom panel edge. */
    NO_SIGNAL_PANEL_BORDER_X = 3, /**< Vertical border thickness. */
    NO_SIGNAL_PANEL_BORDER_Y = 2, /**< Horizontal border thickness. */
    NO_SIGNAL_PRODUCT_TOP = 82,   /**< Product-name text baseline area. */
    NO_SIGNAL_MESSAGE_TOP = 100,  /**< Alert text top edge. */
    NO_SIGNAL_FIRMWARE_TOP = 127, /**< Firmware-version text top edge. */
    NO_SIGNAL_WAITING_TOP = 147,  /**< Waiting-message text top edge. */
};

_Static_assert(VGA_LEFT_MARGIN == 80 && VGA_RIGHT_MARGIN == 80,
               "The 480-sample source image must be centered in VGA");
_Static_assert(P2000T_RAW_SCANLINE_WORDS == 323,
               "CMake scanvideo storage must match the raw line renderer");

/** Product name displayed on the no-signal status card. */
static const char no_signal_product[] = "P2000T VID2VGA";

/** Main alert displayed on the no-signal status card. */
static const char no_signal_message[] = "SIGNAL LOST";

/** Compiled firmware version displayed on the no-signal status card. */
static const char no_signal_firmware[] = "FIRMWARE V" P2000T_VID2VGA_VERSION;

/** Capture state displayed on the no-signal status card. */
static const char no_signal_waiting[] = "WAITING FOR CSYNC";

/** Full-scale RGB444 colors in P2000T bit order R=1, G=2, B=4. */
static const uint16_t source_colors[8] = {
    0x0000, 0x000f, 0x00f0, 0x00ff, 0x0f00, 0x0f0f, 0x0ff0, 0x0fff,
};

/** Two decoded RGB444 pixels for every packed pair of raw input nibbles. */
static uint32_t raw_byte_colors[256];

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
static uint8_t no_signal_glyph_row(char character, unsigned row) {
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
static void render_no_signal_text(uint16_t *tokens, unsigned y,
                                  const char *message, unsigned top,
                                  unsigned x_scale, unsigned y_scale,
                                  uint16_t color) {
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
}

void __not_in_flash_func(p2000t_video_render_no_signal_scanline)(
    scanvideo_scanline_buffer_t *scanline_buffer, unsigned y) {
    hard_assert(scanline_buffer != NULL);
    hard_assert(scanline_buffer->data_max >= P2000T_RAW_SCANLINE_WORDS);
    hard_assert(y < P2000T_VGA_RENDER_HEIGHT);

    const uint16_t canvas_color = 0x0000;
    const uint16_t panel_color = 0x0221;
    const uint16_t alert_color = 0x033e;
    const uint16_t text_color = 0x0fff;
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[0] = COMPOSABLE_RAW_RUN;
    tokens[1] = canvas_color;
    tokens[2] = P2000T_VGA_RENDER_WIDTH - 3;
    for (unsigned x = 1; x < P2000T_VGA_RENDER_WIDTH; ++x) {
        tokens[x + 2u] = canvas_color;
    }

    const bool panel_line =
        y >= NO_SIGNAL_PANEL_TOP && y < NO_SIGNAL_PANEL_BOTTOM;
    const bool horizontal_border =
        panel_line && (y < NO_SIGNAL_PANEL_TOP + NO_SIGNAL_PANEL_BORDER_Y ||
                       y >= NO_SIGNAL_PANEL_BOTTOM - NO_SIGNAL_PANEL_BORDER_Y);
    if (panel_line) {
        const uint16_t fill_color =
            horizontal_border ? alert_color : panel_color;
        for (unsigned x = NO_SIGNAL_PANEL_LEFT; x < NO_SIGNAL_PANEL_RIGHT;
             ++x) {
            tokens[x + 2u] = fill_color;
        }
        if (!horizontal_border) {
            for (unsigned offset = 0; offset < NO_SIGNAL_PANEL_BORDER_X;
                 ++offset) {
                tokens[NO_SIGNAL_PANEL_LEFT + offset + 2u] = alert_color;
                tokens[NO_SIGNAL_PANEL_RIGHT - offset + 1u] = alert_color;
            }
        }
    }

    render_no_signal_text(tokens, y, no_signal_product, NO_SIGNAL_PRODUCT_TOP,
                          2, 1, text_color);
    render_no_signal_text(tokens, y, no_signal_message, NO_SIGNAL_MESSAGE_TOP,
                          4, 2, text_color);
    render_no_signal_text(tokens, y, no_signal_firmware, NO_SIGNAL_FIRMWARE_TOP,
                          2, 1, text_color);
    render_no_signal_text(tokens, y, no_signal_waiting, NO_SIGNAL_WAITING_TOP,
                          2, 1, text_color);
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
