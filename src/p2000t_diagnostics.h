/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_DIAGNOSTICS_H
#define P2000T_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

/** Start one CSYNC trace followed by repeated raw RGBS line bursts. */
bool p2000t_diagnostics_start(unsigned start_line, unsigned line_count,
                              unsigned repetitions);
/** Request a clean, CRC-protected cancelled completion record. */
void p2000t_diagnostics_cancel(void);
/** Abandon acquisition immediately, for example after USB disconnect. */
void p2000t_diagnostics_stop(void);
/** Return whether acquisition or diagnostic USB transmission is active. */
bool p2000t_diagnostics_active(void);
/** Acknowledge or request retransmission of the retained data record. */
bool p2000t_diagnostics_acknowledge(uint32_t sequence, bool retry);
/** Progress capture and bounded non-blocking USB transmission. */
void p2000t_diagnostics_service(void);

#endif
