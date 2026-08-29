/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_SHARED_SCRATCH_H
#define P2000T_SHARED_SCRATCH_H

#include <stdint.h>

#include "p2000t_diagnostic_protocol.h"

/** SRAM shared by mutually exclusive Pico 2 high-resolution operations. */
extern uint32_t
    p2000t_high_resolution_scratch[P2000T_DIAGNOSTIC_TIMING_WORD_COUNT];

#endif
