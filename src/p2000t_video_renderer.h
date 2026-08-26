/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_VIDEO_RENDERER_H
#define P2000T_VIDEO_RENDERER_H

/**
 * @file p2000t_video_renderer.h
 * @brief Rendering of captured P2000T frames and the no-signal status screen.
 */

#include <stdint.h>

#include "pico/scanvideo.h"

/** Dimensions and storage requirements of the logical VGA renderer. */
enum {
    P2000T_VGA_TIMING_WIDTH = 640,   /**< Physical VGA width in pixels. */
    P2000T_VGA_TIMING_HEIGHT = 480,  /**< Physical VGA height in lines. */
    P2000T_VGA_RENDER_WIDTH = 640,   /**< Logical scanline width in pixels. */
    P2000T_VGA_RENDER_HEIGHT = 240,  /**< Logical frame height in lines. */
    P2000T_VGA_HORIZONTAL_SCALE = 1, /**< Horizontal scanvideo scale factor. */
    P2000T_VGA_VERTICAL_SCALE = 2,   /**< Vertical scanvideo scale factor. */
    P2000T_RAW_SCANLINE_TOKENS = (3 + P2000T_VGA_RENDER_WIDTH - 1) + 2 + 2,
    /**< Number of 16-bit composable tokens in one rendered scanline. */
    P2000T_RAW_SCANLINE_WORDS = P2000T_RAW_SCANLINE_TOKENS / 2,
    /**< Number of 32-bit scanvideo buffer words required per scanline. */
};

/**
 * @brief Initialize lookup tables used by the source-frame renderer.
 *
 * Call this once before rendering any captured scanlines.
 */
void p2000t_video_renderer_initialize(void);

/**
 * @brief Render one captured source line into a scanvideo buffer.
 *
 * @param scanline_buffer Writable scanvideo buffer with at least
 *        P2000T_RAW_SCANLINE_WORDS words of storage.
 * @param frame Immutable packed RGBS frame returned by the capture module.
 * @param source_y Zero-based source line in the captured frame.
 */
void p2000t_video_render_source_scanline(
    scanvideo_scanline_buffer_t *scanline_buffer, const uint32_t *frame,
    unsigned source_y);

/**
 * @brief Render one line of the no-signal status screen.
 *
 * @param scanline_buffer Writable scanvideo buffer with at least
 *        P2000T_RAW_SCANLINE_WORDS words of storage.
 * @param y Zero-based logical VGA line to render.
 */
void p2000t_video_render_no_signal_scanline(
    scanvideo_scanline_buffer_t *scanline_buffer, unsigned y);

#endif
