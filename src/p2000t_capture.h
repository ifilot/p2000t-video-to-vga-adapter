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

#include "p2000t_reconstruction.h"

/** Capture geometry, adjustment limits, and conditioned input pin mapping. */
enum {
    /* Six-tap line reconstruction currently starves continuous VGA scanout.
       Keep it unavailable until reconstruction is moved out of the line-rate
       IRQ path. */
    P2000T_CAPTURE_WINDOW_REALTIME_SAFE = 0,
    P2000T_CAPTURE_WIDTH = 480,
    /**< Raw samples per line; two per nominal 6 MHz source dot. */
    P2000T_CAPTURE_HEIGHT = 240,
    /**< Complete source scanlines retained per captured frame. */
    P2000T_CAPTURE_WORDS_PER_LINE = P2000T_CAPTURE_WIDTH / 8,
    /**< Packed 32-bit words per line at eight RGBS nibbles per word. */
    P2000T_CAPTURE_WORDS_PER_FRAME =
        P2000T_CAPTURE_WORDS_PER_LINE * P2000T_CAPTURE_HEIGHT,
    /**< Packed 32-bit words in one complete captured frame. */

    P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE = 57,
    /**< Default source line at which visible capture begins. */
    P2000T_MIN_FIRST_VISIBLE_SCANLINE = 1,
    /**< Lowest accepted first-visible-line setting. */
    P2000T_MAX_FIRST_VISIBLE_SCANLINE = 73,
    /**< Highest accepted first-visible-line setting. */

    P2000T_DEFAULT_SAMPLE_PHASE = 0,
    /**< Default fine sampling phase in nominal 7.94 ns capture ticks. */
    P2000T_MIN_SAMPLE_PHASE = -10,
    /**< Earliest accepted fine sampling phase. */
    P2000T_MAX_SAMPLE_PHASE = 10,
    /**< Latest accepted fine sampling phase. */

    P2000T_DEFAULT_ODD_LINE_PHASE = 0,
    /**< Default extra phase applied to odd-numbered source lines. */
    P2000T_MIN_ODD_LINE_PHASE = -10,
    /**< Earliest accepted odd-line correction in capture-clock ticks. */
    P2000T_MAX_ODD_LINE_PHASE = 10,
    /**< Latest accepted odd-line correction in capture-clock ticks. */

    P2000T_DEFAULT_SAMPLE_RATE_TRIM = 0,
    /**< Exact 2.0 PIO divider and nominal 12 MHz sampling. */
    P2000T_MIN_SAMPLE_RATE_TRIM = -8,
    /**< Fastest capture rate, in signed 1/256-divider steps. */
    P2000T_MAX_SAMPLE_RATE_TRIM = 8,
    /**< Slowest capture rate, in signed 1/256-divider steps. */

    P2000T_DEFAULT_HORIZONTAL_OFFSET = 48,
    /**< Default coarse start in nominal 6 MHz source dots. */
    P2000T_MIN_HORIZONTAL_OFFSET = 0,
    /**< Earliest accepted coarse horizontal start. */
    P2000T_MAX_HORIZONTAL_OFFSET = 60,
    /**< Latest accepted coarse horizontal start. */
    P2000T_HORIZONTAL_OFFSET_STEP = 6,
    /**< Interactive coarse-adjustment step in source dots. */

    P2000T_INPUT_PIN_BASE = 16,
/**< First GPIO in the four-bit conditioned RGBS input bus. */
#if P2000T_PROTOTYPE_V1_MIRRORED_DIN
    P2000T_SYNC_CHANNEL = 1,
    /**< CSYNC bit within the prototype's input nibble. */
    P2000T_RED_CHANNEL = 0,
    /**< Red bit within the prototype's input nibble. */
    P2000T_GREEN_CHANNEL = 3,
    /**< Green bit within the prototype's input nibble. */
    P2000T_BLUE_CHANNEL = 2,
/**< Blue bit within the prototype's input nibble. */
#else
    P2000T_SYNC_CHANNEL = 0,
    /**< CSYNC bit within the corrected PCB's input nibble. */
    P2000T_RED_CHANNEL = 1,
    /**< Red bit within the corrected PCB's input nibble. */
    P2000T_GREEN_CHANNEL = 2,
    /**< Green bit within the corrected PCB's input nibble. */
    P2000T_BLUE_CHANNEL = 3,
/**< Blue bit within the corrected PCB's input nibble. */
#endif
    P2000T_SYNC_PIN = P2000T_INPUT_PIN_BASE + P2000T_SYNC_CHANNEL,
    /**< GPIO carrying conditioned active-high composite sync. */
    P2000T_RED_PIN = P2000T_INPUT_PIN_BASE + P2000T_RED_CHANNEL,
    /**< GPIO carrying conditioned active-low red video. */
    P2000T_GREEN_PIN = P2000T_INPUT_PIN_BASE + P2000T_GREEN_CHANNEL,
    /**< GPIO carrying conditioned active-low green video. */
    P2000T_BLUE_PIN = P2000T_INPUT_PIN_BASE + P2000T_BLUE_CHANNEL,
    /**< GPIO carrying conditioned active-low blue video. */
};

_Static_assert(P2000T_CAPTURE_WIDTH % 8u == 0u,
               "Packed RGBS scanlines must contain complete DMA words");

/** Consistent snapshot of capture timing, buffering, and alignment state. */
typedef struct {
    uint32_t captured_frames;       /**< Complete DMA frames received. */
    uint32_t stale_frames_replaced; /**< Unconsumed ready frames overwritten. */
    uint32_t last_frame_period_us;  /**< Most recent source frame period. */
    uint32_t first_visible_scanline; /**< Active vertical capture alignment. */
    int32_t sample_phase;            /**< Fine sampling phase in PIO ticks. */
    int32_t odd_line_phase;   /**< Extra phase on odd-numbered source lines. */
    int32_t sample_rate_trim; /**< Signed 1/256 PIO-divider rate trim. */
    uint32_t horizontal_offset; /**< Coarse start in source dots. */
    bool signal_present;        /**< Whether source timing is credible. */
    uint32_t windowed_frames;   /**< Complete six-tap frames reconstructed. */
    uint32_t line_deadline_misses; /**< Late/coalesced window-line IRQs. */
    uint32_t
        last_corrected_samples; /**< Pixels changed in last window frame. */
    uint32_t last_ambiguous_samples; /**< Three-distinct-sample windows. */
    uint32_t last_red_corrections;   /**< Last-frame red channel changes. */
    uint32_t last_green_corrections; /**< Last-frame green channel changes. */
    uint32_t last_blue_corrections;  /**< Last-frame blue channel changes. */
    uint8_t reconstruction_mode;     /**< Requested live reconstruction. */
    uint8_t capture_engine;          /**< Active two-tap/windowed engine. */
    uint8_t window_samples;          /**< Raw samples per output pixel. */
    bool window_supported;      /**< Whether this target has six-tap PIO. */
    bool engine_switch_pending; /**< Requested engine awaits a boundary. */
} p2000t_capture_stats_t;

/**
 * @brief Initialize PIO1 and start continuous frame acquisition.
 *
 * This function claims one state machine, two DMA channels, and one spinlock.
 * It must be called once before any other capture API function.
 */
void p2000t_capture_start(void);

/** Stop PIO and DMA before a flash-safe operation pauses interrupt service. */
void p2000t_capture_pause_for_flash(void);

/** Rebuild window state and restart capture after a flash-safe operation. */
void p2000t_capture_resume_after_flash(void);

/**
 * @brief Determine whether credible complete source frames are arriving.
 *
 * @return true when recent frame timing lies inside the accepted range;
 *         otherwise false.
 */
bool p2000t_capture_signal_present(void);

/**
 * @brief Claim the newest complete frame for read-only display.
 *
 * @param sequence Output location for the claimed frame's sequence number.
 * @return Capture-buffer index, or -1 when no complete frame is ready.
 */
int p2000t_capture_acquire_latest_frame(uint32_t *sequence);

/**
 * @brief Add a short-lived USB hold to the newest complete frame.
 *
 * Unlike the VGA acquisition API, this does not change display ownership.
 * The returned frame may therefore be shared safely with the VGA core while
 * Pico 2 packs an independent USB snapshot.
 *
 * @param sequence Output location for the held frame's sequence number.
 * @return Capture-buffer index, or -1 when no complete frame is available.
 */
#if defined(PICO_RP2350) && PICO_RP2350
int p2000t_capture_acquire_latest_frame_for_usb(uint32_t *sequence);
#endif

/**
 * @brief Release a frame previously claimed by the VGA core.
 *
 * @param buffer_index Index returned by p2000t_capture_acquire_latest_frame().
 */
void p2000t_capture_release_frame(unsigned buffer_index);

/** Release a frame held by p2000t_capture_acquire_latest_frame_for_usb(). */
#if defined(PICO_RP2350) && PICO_RP2350
void p2000t_capture_release_frame_from_usb(unsigned buffer_index);
#endif

/**
 * @brief Return immutable packed RGBS storage for a claimed frame.
 *
 * @param buffer_index Index returned by p2000t_capture_acquire_latest_frame().
 * @return Pointer to P2000T_CAPTURE_WORDS_PER_FRAME packed words.
 */
const uint32_t *p2000t_capture_buffer(unsigned buffer_index);

/** Copy reconstruction evidence associated with one complete frame buffer. */
void p2000t_capture_get_frame_diagnostics(
    unsigned buffer_index, p2000t_reconstruction_diagnostics_t *diagnostics);

/**
 * @brief Copy a consistent snapshot of acquisition statistics.
 *
 * @param stats Output structure populated while holding the capture lock.
 */
void p2000t_capture_get_stats(p2000t_capture_stats_t *stats);

/**
 * @brief Adjust the first captured source scanline for vertical alignment.
 *
 * @param scanline Requested one-based source scanline.
 * @return true when accepted; false when outside the documented limits.
 */
bool p2000t_capture_set_first_visible_scanline(unsigned scanline);

/**
 * @brief Trim the sampling start in nominal 7.94 ns capture-clock ticks.
 *
 * @param phase Requested signed fine phase.
 * @return true when accepted; false when outside the documented limits.
 */
bool p2000t_capture_set_sample_phase(int phase);

/**
 * @brief Trim odd-numbered source lines relative to even-numbered lines.
 *
 * Source parity follows the configured one-based first visible scanline, so
 * moving the capture window vertically preserves the physical line parity.
 *
 * @param phase Signed correction in 7.94 ns capture-clock ticks.
 * @return true when accepted; false when outside the documented limits.
 */
bool p2000t_capture_set_odd_line_phase(int phase);

/**
 * @brief Trim the complete horizontal sampling interval.
 *
 * Positive values increase the PIO divider in 1/256 steps, sampling more
 * slowly and moving the right edge later while leaving the sync anchor fixed.
 * The new divider is adopted between complete source frames.
 *
 * @param trim Signed fractional-divider correction.
 * @return true when accepted; false when outside the documented limits.
 */
bool p2000t_capture_set_sample_rate_trim(int trim);

/**
 * @brief Select the coarse horizontal start in nominal 6 MHz source dots.
 *
 * @param pixels Requested unsigned source-dot offset.
 * @return true when accepted; false when outside the documented limits.
 */
bool p2000t_capture_set_horizontal_offset(unsigned pixels);

/** Select a live reconstruction mode, including Pico 2 windowed policies. */
bool p2000t_capture_set_reconstruction_mode(unsigned reconstruction);

#endif
