/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_SETTINGS_H
#define P2000T_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "p2000t_control_protocol.h"

typedef struct {
    uint16_t first_visible_scanline;
    int16_t sample_phase;
    uint16_t horizontal_offset;
    uint8_t capture_options;
    int8_t odd_line_phase;
    uint16_t palette[P2000T_CONTROL_PALETTE_COLORS];
} p2000t_settings_t;

/* capture_options retains artwork in bits 0..1, stores source-dot
   reconstruction in bit 2, and stores signed rate trim in bits 3..7. Old
   records therefore decode as raw reconstruction with trim zero.
   odd_line_phase occupies the former reserved byte from the original layout. */
_Static_assert(sizeof(p2000t_settings_t) == 24u,
               "Persistent settings layout must remain flash-compatible");

/** Extract the selected no-signal artwork from packed capture options. */
unsigned p2000t_settings_artwork(const p2000t_settings_t *settings);
/** Replace the selected artwork while preserving the sample-rate trim. */
void p2000t_settings_set_artwork(p2000t_settings_t *settings, unsigned artwork);
/** Return the selected raw/second-tap source-dot reconstruction mode. */
unsigned
p2000t_settings_sample_reconstruction(const p2000t_settings_t *settings);
/** Replace source-dot reconstruction while preserving artwork and rate. */
void p2000t_settings_set_sample_reconstruction(p2000t_settings_t *settings,
                                               unsigned reconstruction);
/** Extract the signed 1/256-divider capture-rate trim. */
int p2000t_settings_sample_rate_trim(const p2000t_settings_t *settings);
/** Replace the sample-rate trim while preserving the selected artwork. */
void p2000t_settings_set_sample_rate_trim(p2000t_settings_t *settings,
                                          int trim);

/** Populate a settings structure with firmware defaults. */
void p2000t_settings_defaults(p2000t_settings_t *settings);
/** Validate capture alignment, artwork, and palette ranges. */
bool p2000t_settings_valid(const p2000t_settings_t *settings);
/** Load a CRC-protected settings record from the final flash sector. */
bool p2000t_settings_load(p2000t_settings_t *settings);
/** Persist a settings record using multicore-safe flash execution. */
bool p2000t_settings_save(const p2000t_settings_t *settings);

#endif
