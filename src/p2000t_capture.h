/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_CAPTURE_H
#define P2000T_CAPTURE_H

/**
 * @file p2000t_capture.h
 * @brief Continuous PIO/DMA capture of P2000T RGBS video frames.
 */

#include <stdbool.h>
#include <stdint.h>

enum {
    P2000T_CAPTURE_WIDTH = 240,
    P2000T_CAPTURE_HEIGHT = 240,
    P2000T_CAPTURE_WORDS_PER_LINE = P2000T_CAPTURE_WIDTH / 8,
    P2000T_CAPTURE_WORDS_PER_FRAME =
        P2000T_CAPTURE_WORDS_PER_LINE * P2000T_CAPTURE_HEIGHT,
    P2000T_CAPTURE_BYTES_PER_FRAME =
        P2000T_CAPTURE_WORDS_PER_FRAME * sizeof(uint32_t),
    P2000T_QUALITY_BIN_COUNT = 12,
    P2000T_RGB_CHANNEL_COUNT = 3,
    P2000T_TAP_WINDOW_COUNT = 3,
    P2000T_DEFAULT_TAP_WINDOW = 1,
    P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE = 57,
    P2000T_MIN_FIRST_VISIBLE_SCANLINE = 1,
    P2000T_MAX_FIRST_VISIBLE_SCANLINE = 73,
    P2000T_DEFAULT_SAMPLE_PHASE = 1,
    /* Twenty-one positions cover one complete 6 MHz dot at 126 MHz. */
    P2000T_MIN_SAMPLE_PHASE = -10,
    P2000T_MAX_SAMPLE_PHASE = 10,
    P2000T_DEFAULT_HORIZONTAL_OFFSET = 48,
    P2000T_MIN_HORIZONTAL_OFFSET = 0,
    P2000T_MAX_HORIZONTAL_OFFSET = 60,
    P2000T_HORIZONTAL_OFFSET_STEP = 6,

    P2000T_INPUT_PIN_BASE = 16,
#if P2000T_PROTOTYPE_V1_MIRRORED_DIN
    P2000T_SYNC_CHANNEL = 1,
    P2000T_RED_CHANNEL = 0,
    P2000T_GREEN_CHANNEL = 3,
    P2000T_BLUE_CHANNEL = 2,
#else
    P2000T_SYNC_CHANNEL = 0,
    P2000T_RED_CHANNEL = 1,
    P2000T_GREEN_CHANNEL = 2,
    P2000T_BLUE_CHANNEL = 3,
#endif
    P2000T_SYNC_PIN = P2000T_INPUT_PIN_BASE + P2000T_SYNC_CHANNEL,
    P2000T_RED_PIN = P2000T_INPUT_PIN_BASE + P2000T_RED_CHANNEL,
    P2000T_GREEN_PIN = P2000T_INPUT_PIN_BASE + P2000T_GREEN_CHANNEL,
    P2000T_BLUE_PIN = P2000T_INPUT_PIN_BASE + P2000T_BLUE_CHANNEL,
};

_Static_assert(P2000T_CAPTURE_WIDTH % 8u == 0u,
               "Packed RGBS scanlines must contain complete DMA words");

typedef struct {
    uint32_t captured_frames;
    uint32_t stale_frames_replaced;
    uint32_t last_frame_period_us;
    uint32_t first_visible_scanline;
    uint32_t maximum_line_decode_us;
    int32_t sample_phase;
    int32_t measured_phase;
    uint32_t horizontal_offset;
    uint32_t quality_frames;
    uint64_t quality_triplets;
    uint64_t quality_disagreements;
    uint64_t quality_early_centre_differences;
    uint64_t quality_centre_late_differences;
    uint64_t quality_centre_outliers;
    uint64_t quality_bin_disagreements[P2000T_QUALITY_BIN_COUNT];
    uint64_t quality_channel_window_disagreements
        [P2000T_RGB_CHANNEL_COUNT][P2000T_TAP_WINDOW_COUNT];
    uint8_t channel_tap_windows[P2000T_RGB_CHANNEL_COUNT];
    bool quality_measurement_enabled;
    bool signal_present;
} p2000t_capture_stats_t;

/** Initialize PIO1 and start continuous frame acquisition. */
void p2000t_capture_start(void);

/** Return whether credible complete source frames are arriving. */
bool p2000t_capture_signal_present(void);

/** Claim the newest complete frame, or return -1 when none is ready. */
int p2000t_capture_acquire_latest_frame(uint32_t *sequence);

/** Release a frame previously claimed by the VGA core. */
void p2000t_capture_release_frame(unsigned buffer_index);

/** Return immutable packed RGBS storage for a claimed frame. */
const uint32_t *p2000t_capture_buffer(unsigned buffer_index);

/** Copy a consistent snapshot of acquisition statistics. */
void p2000t_capture_get_stats(p2000t_capture_stats_t *stats);

/** Adjust the first captured field scanline for vertical alignment. */
bool p2000t_capture_set_first_visible_scanline(unsigned scanline);

/** Trim the dot-centre sampling phase in 7.94 ns capture ticks. */
bool p2000t_capture_set_sample_phase(int phase);

/** Select the early, centre, or late majority window for each RGB channel. */
bool p2000t_capture_set_channel_tap_windows(unsigned red,
                                            unsigned green,
                                            unsigned blue);

/** Enable or disable expensive sparse eye-quality measurements. */
void p2000t_capture_set_quality_measurement(bool enabled);

/** Select a later 240-dot window within the extended raw line capture. */
bool p2000t_capture_set_horizontal_offset(unsigned pixels);

/** Return one chronological raw GP19..GP16 nibble from a captured frame. */
static inline uint8_t p2000t_capture_raw_pixel(const uint32_t *frame,
                                               unsigned x,
                                               unsigned y) {
    const uint32_t word = frame[
        y * P2000T_CAPTURE_WORDS_PER_LINE + x / 8u];
    return (uint8_t)((word >> (28u - 4u * (x & 7u))) & 0x0fu);
}

#endif
