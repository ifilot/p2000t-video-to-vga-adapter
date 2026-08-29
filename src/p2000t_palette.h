/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_PALETTE_H
#define P2000T_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

#include "p2000t_control_protocol.h"

/** Return whether a source palette can display any non-background color. */
static inline bool p2000t_palette_has_visible_source_color(
    const uint16_t colors[P2000T_CONTROL_PALETTE_COLORS]) {
    for (unsigned index = 1u; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        if (colors[index] != 0u) {
            return true;
        }
    }
    return false;
}

#endif
