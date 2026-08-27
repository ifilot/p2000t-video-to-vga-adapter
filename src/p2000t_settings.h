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
    uint8_t no_signal_artwork;
    uint8_t reserved;
    uint16_t palette[P2000T_CONTROL_PALETTE_COLORS];
} p2000t_settings_t;

/** Populate a settings structure with firmware defaults. */
void p2000t_settings_defaults(p2000t_settings_t *settings);
/** Validate ranges, palette entries, and reserved fields. */
bool p2000t_settings_valid(const p2000t_settings_t *settings);
/** Load a CRC-protected settings record from the final flash sector. */
bool p2000t_settings_load(p2000t_settings_t *settings);
/** Persist a settings record using multicore-safe flash execution. */
bool p2000t_settings_save(const p2000t_settings_t *settings);

#endif
