/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_USB_STREAM_H
#define P2000T_USB_STREAM_H

/**
 * @file p2000t_usb_stream.h
 * @brief Pico 2 USB framebuffer streaming interface.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t frames_sent;
    uint32_t no_signal_records_sent;
    uint32_t raw_frames_sent;
    uint32_t packbits_frames_sent;
    uint32_t last_payload_size;
    uint32_t last_prepare_us;
    uint32_t maximum_prepare_us;
    uint32_t last_encode_us;
    uint32_t maximum_encode_us;
    uint32_t last_tx_us;
    uint32_t maximum_tx_us;
    uint32_t skipped_sequences;
    uint64_t bytes_sent;
} p2000t_usb_stream_stats_t;

/** Enter continuous binary screen mode. */
void p2000t_usb_stream_start(bool allow_packbits);
/** Abort the active record and return to the USB command interface. */
void p2000t_usb_stream_stop(void);
/** Return whether binary screen mode is active. */
bool p2000t_usb_stream_active(void);
/**
 * @brief Refill TinyUSB without ever waiting for host-side capacity.
 *
 * A header-only record is emitted when the P2000T signal disappears or the
 * selected no-connection artwork changes while no signal is present.
 *
 * @param signal_present Whether capture currently has a credible input.
 * @param no_signal_artwork Zero-based selected no-connection artwork.
 */
void p2000t_usb_stream_service(bool signal_present,
                               unsigned no_signal_artwork);
/** Copy a consistent snapshot of streaming diagnostics. */
void p2000t_usb_stream_get_stats(p2000t_usb_stream_stats_t *stats);

#endif
