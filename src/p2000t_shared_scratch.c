/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "p2000t_shared_scratch.h"

uint32_t p2000t_high_resolution_scratch[P2000T_DIAGNOSTIC_TIMING_WORD_COUNT]
    __attribute__((aligned(4)));
