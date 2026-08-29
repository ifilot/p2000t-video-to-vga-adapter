/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p2000t_usb_stream.c
 * @brief Non-blocking RGB111 framebuffer streaming for Pico 2.
 */

#include "p2000t_usb_stream.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "p2000t_capture.h"
#include "p2000t_control_protocol.h"
#include "p2000t_packbits.h"
#include "p2000t_reconstruction.h"
#include "p2000t_stream_protocol.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

#if !defined(PICO_RP2350) || !PICO_RP2350
#error "The framebuffer streamer is intentionally built only for Pico 2"
#endif

enum {
    STREAM_SEQUENCE_STEP = 2,
    STREAM_TX_SERVICE_BUDGET = 1024,
    STREAM_PACKBITS_STAGING_SIZE = 1024,
};

_Static_assert((unsigned)P2000T_STREAM_WIDTH == (unsigned)P2000T_CAPTURE_WIDTH,
               "Stream width must match capture width");
_Static_assert((unsigned)P2000T_STREAM_HEIGHT ==
                   (unsigned)P2000T_CAPTURE_HEIGHT,
               "Stream height must match capture height");
_Static_assert((unsigned)STREAM_PACKBITS_STAGING_SIZE >=
                   (unsigned)P2000T_PACKBITS_MAX_CHUNK,
               "PackBits staging must hold its largest chunk");

/** Stable RGB bitplanes, independent of capture and VGA buffer ownership. */
static uint8_t stream_frame[P2000T_STREAM_PAYLOAD_SIZE]
    __attribute__((aligned(4)));
static uint8_t stream_header[P2000T_STREAM_HEADER_SIZE];
static uint8_t packbits_staging[STREAM_PACKBITS_STAGING_SIZE];

static bool stream_active;
static bool packbits_allowed;
static unsigned reconstruction_mode;
static bool tx_active;
static bool tx_packbits;
static bool tx_configuration;
static bool configuration_pending;
static bool last_sequence_valid;
static bool last_display_state_valid;
static bool last_signal_present;
static bool tx_signal_present;
static uint32_t tx_sequence;
static uint64_t tx_capture_timestamp_us;
static uint32_t last_sequence;
static uint8_t last_no_signal_artwork;
static uint8_t tx_no_signal_artwork;
static uint8_t pending_configuration[P2000T_CONFIGURATION_STATE_SIZE];
static uint8_t tx_configuration_payload[P2000T_CONFIGURATION_STATE_SIZE];
static size_t tx_header_offset;
static size_t tx_payload_offset;
static size_t tx_payload_size;
static size_t packbits_input_offset;
static size_t packbits_staging_size;
static size_t packbits_staging_offset;
static uint32_t tx_encode_us;
static uint64_t tx_started_us;
static p2000t_usb_stream_stats_t stream_stats;
static p2000t_reconstruction_diagnostics_t tx_reconstruction_diagnostics;

static void store_u16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void store_u64(uint8_t *destination, uint64_t value) {
    store_u32(destination, (uint32_t)value);
    store_u32(destination + 4u, (uint32_t)(value >> 32u));
}

/** Standard reflected CRC-32 with a compact nibble lookup table. */
static uint32_t frame_crc32(const uint8_t *data, size_t length) {
    static const uint32_t nibble_table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        crc = (crc >> 4u) ^ nibble_table[crc & 0x0fu];
        crc = (crc >> 4u) ^ nibble_table[crc & 0x0fu];
    }
    return ~crc;
}

/** Convert active-low RGBS nibbles into R, G, and B one-bit planes. */
static void
count_changed_channels(uint8_t original, uint8_t output,
                       p2000t_reconstruction_diagnostics_t *diagnostics) {
    const uint8_t changed = original ^ output;
    if (changed == 0u) {
        return;
    }
    ++diagnostics->corrected_samples;
    diagnostics->red_corrections += (changed >> P2000T_RED_CHANNEL) & 1u;
    diagnostics->green_corrections += (changed >> P2000T_GREEN_CHANNEL) & 1u;
    diagnostics->blue_corrections += (changed >> P2000T_BLUE_CHANNEL) & 1u;
}

static void
pack_rgb111_frame(const uint32_t *capture,
                  p2000t_reconstruction_diagnostics_t *diagnostics) {
    uint8_t *const red_plane = stream_frame;
    uint8_t *const green_plane = stream_frame + P2000T_STREAM_PLANE_SIZE;
    uint8_t *const blue_plane = stream_frame + 2u * P2000T_STREAM_PLANE_SIZE;

    for (unsigned y = 0; y < P2000T_CAPTURE_HEIGHT; ++y) {
        const uint32_t *source = capture + y * P2000T_CAPTURE_WORDS_PER_LINE;
        const unsigned row_offset = y * P2000T_STREAM_PLANE_STRIDE;
        for (unsigned word_index = 0;
             word_index < P2000T_CAPTURE_WORDS_PER_LINE; ++word_index) {
            const uint32_t raw_word = source[word_index];
            const bool has_next_word =
                word_index + 1u < P2000T_CAPTURE_WORDS_PER_LINE;
            const uint32_t next_word =
                has_next_word ? source[word_index + 1u] : 0u;
            uint8_t red = 0u;
            uint8_t green = 0u;
            uint8_t blue = 0u;
            for (unsigned pixel = 0; pixel < 8u; ++pixel) {
                const unsigned shift = 28u - pixel * 4u;
                const uint8_t original = (uint8_t)(raw_word >> shift) & 0x0fu;
                uint8_t raw = original;
                if (diagnostics->capture_engine ==
                    P2000T_CAPTURE_ENGINE_TWO_TAP) {
                    const unsigned dot = pixel / 2u;
                    const uint8_t selected = p2000t_guarded_tap_from_words(
                        raw_word, next_word, dot, has_next_word);
                    if ((pixel & 1u) == 0u) {
                        const uint8_t first =
                            p2000t_packed_sample(raw_word, dot * 2u);
                        const uint8_t second =
                            p2000t_packed_sample(raw_word, dot * 2u + 1u);
                        if (selected != second) {
                            ++diagnostics->ambiguous_samples;
                        }
                        if (reconstruction_mode ==
                            P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP) {
                            raw = selected;
                        } else {
                            raw = first;
                        }
                    } else if (reconstruction_mode !=
                               P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW) {
                        raw = selected;
                    }
                    if (reconstruction_mode !=
                        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW) {
                        count_changed_channels(original, raw, diagnostics);
                    }
                }
                red = (uint8_t)((red << 1u) |
                                ((((raw >> P2000T_RED_CHANNEL) & 1u) == 0u)
                                     ? 1u
                                     : 0u));
                green = (uint8_t)((green << 1u) |
                                  ((((raw >> P2000T_GREEN_CHANNEL) & 1u) == 0u)
                                       ? 1u
                                       : 0u));
                blue = (uint8_t)((blue << 1u) |
                                 ((((raw >> P2000T_BLUE_CHANNEL) & 1u) == 0u)
                                      ? 1u
                                      : 0u));
            }
            const unsigned output = row_offset + word_index;
            red_plane[output] = red;
            green_plane[output] = green;
            blue_plane[output] = blue;
        }
    }
}

static void build_header(uint32_t sequence, uint32_t checksum) {
    memset(stream_header, 0, sizeof(stream_header));
    memcpy(stream_header, P2000T_STREAM_MAGIC, 4u);
    stream_header[4] = P2000T_STREAM_PROTOCOL_VERSION;
    stream_header[5] = tx_configuration
                           ? P2000T_STREAM_ENCODING_CONFIGURATION
                           : (tx_packbits ? P2000T_STREAM_ENCODING_PACKBITS
                                          : P2000T_STREAM_ENCODING_RAW);
    if (tx_configuration) {
        store_u16(&stream_header[6],
                  P2000T_STREAM_FLAG_CONFIGURATION_STATE |
                      P2000T_STREAM_FLAG_CAPTURE_TIMESTAMP_US);
        store_u16(&stream_header[22], P2000T_STREAM_HEADER_SIZE);
        store_u32(&stream_header[24], (uint32_t)tx_payload_size);
        store_u32(&stream_header[28], checksum);
        store_u32(&stream_header[32], (uint32_t)tx_payload_size);
        stream_header[P2000T_STREAM_ARTWORK_OFFSET] =
            tx_configuration_payload
                [P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] &
            P2000T_CONFIGURATION_ARTWORK_MASK;
        store_u64(&stream_header[P2000T_STREAM_CAPTURE_TIMESTAMP_US_OFFSET],
                  tx_capture_timestamp_us);
        return;
    }
    uint16_t flags = P2000T_STREAM_FLAG_PLANAR_RGB111 |
                     P2000T_STREAM_FLAG_PIXELS_MSB_FIRST |
                     P2000T_STREAM_FLAG_TIMING_DIAGNOSTICS |
                     P2000T_STREAM_FLAG_CAPTURE_TIMESTAMP_US;
    if (tx_signal_present) {
        flags |= P2000T_STREAM_FLAG_SIGNAL_PRESENT;
    }
    if (tx_reconstruction_diagnostics.line_deadline_misses != 0u) {
        flags |= P2000T_STREAM_FLAG_CAPTURE_DEADLINE_MISS;
    }
    store_u16(&stream_header[6], flags);
    store_u32(&stream_header[8], sequence);
    const uint32_t timing = (stream_stats.last_prepare_us > UINT16_MAX
                                 ? UINT16_MAX
                                 : stream_stats.last_prepare_us) |
                            ((stream_stats.last_encode_us > UINT16_MAX
                                  ? UINT16_MAX
                                  : stream_stats.last_encode_us)
                             << 16u);
    store_u32(&stream_header[12], timing);
    store_u16(&stream_header[16], P2000T_STREAM_WIDTH);
    store_u16(&stream_header[18], P2000T_STREAM_HEIGHT);
    store_u16(&stream_header[20], P2000T_STREAM_PLANE_STRIDE);
    store_u16(&stream_header[22], P2000T_STREAM_HEADER_SIZE);
    store_u32(&stream_header[24], (uint32_t)tx_payload_size);
    store_u32(&stream_header[28], checksum);
    store_u32(&stream_header[32],
              tx_signal_present ? P2000T_STREAM_PAYLOAD_SIZE : 0u);
    store_u32(&stream_header[36], 25000u);
    stream_header[P2000T_STREAM_ARTWORK_OFFSET] = tx_no_signal_artwork;
    stream_header[P2000T_STREAM_RECONSTRUCTION_OFFSET] =
        tx_reconstruction_diagnostics.reconstruction_mode;
    stream_header[P2000T_STREAM_CAPTURE_ENGINE_OFFSET] =
        tx_reconstruction_diagnostics.capture_engine;
    stream_header[P2000T_STREAM_SAMPLES_PER_OUTPUT_OFFSET] =
        tx_reconstruction_diagnostics.samples_per_output;
    store_u32(&stream_header[P2000T_STREAM_CORRECTED_SAMPLES_OFFSET],
              tx_reconstruction_diagnostics.corrected_samples);
    store_u32(&stream_header[P2000T_STREAM_AMBIGUOUS_SAMPLES_OFFSET],
              tx_reconstruction_diagnostics.ambiguous_samples);
    store_u32(&stream_header[P2000T_STREAM_RED_CORRECTIONS_OFFSET],
              tx_reconstruction_diagnostics.red_corrections);
    store_u32(&stream_header[P2000T_STREAM_GREEN_CORRECTIONS_OFFSET],
              tx_reconstruction_diagnostics.green_corrections);
    store_u32(&stream_header[P2000T_STREAM_BLUE_CORRECTIONS_OFFSET],
              tx_reconstruction_diagnostics.blue_corrections);
    store_u64(&stream_header[P2000T_STREAM_CAPTURE_TIMESTAMP_US_OFFSET],
              tx_capture_timestamp_us);
}

static void begin_no_signal_record(unsigned no_signal_artwork) {
    tx_signal_present = false;
    tx_no_signal_artwork = (uint8_t)no_signal_artwork;
    tx_packbits = false;
    tx_configuration = false;
    tx_payload_size = 0u;
    tx_sequence = last_sequence_valid ? last_sequence : 0u;
    tx_capture_timestamp_us = time_us_64();
    tx_header_offset = 0u;
    tx_payload_offset = 0u;
    packbits_input_offset = 0u;
    packbits_staging_size = 0u;
    packbits_staging_offset = 0u;
    tx_encode_us = 0u;
    stream_stats.last_prepare_us = 0u;
    tx_reconstruction_diagnostics = (p2000t_reconstruction_diagnostics_t){
        .reconstruction_mode = (uint8_t)reconstruction_mode,
        .capture_engine = P2000T_CAPTURE_ENGINE_TWO_TAP,
        .samples_per_output = 1u,
    };
    build_header(tx_sequence, 0u);
    tx_started_us = time_us_64();
    tx_active = true;
}

static void begin_configuration_record(void) {
    memcpy(tx_configuration_payload, pending_configuration,
           sizeof(tx_configuration_payload));
    configuration_pending = false;
    tx_configuration = true;
    tx_signal_present = false;
    tx_packbits = false;
    tx_payload_size = sizeof(tx_configuration_payload);
    tx_header_offset = 0u;
    tx_payload_offset = 0u;
    packbits_input_offset = 0u;
    packbits_staging_size = 0u;
    packbits_staging_offset = 0u;
    tx_encode_us = 0u;
    tx_capture_timestamp_us = time_us_64();
    build_header(last_sequence_valid ? last_sequence : 0u,
                 frame_crc32(tx_configuration_payload,
                             sizeof(tx_configuration_payload)));
    tx_started_us = time_us_64();
    tx_active = true;
}

static void begin_available_frame(bool signal_present,
                                  unsigned no_signal_artwork) {
    if (tx_active) {
        return;
    }

    if (configuration_pending) {
        begin_configuration_record();
        return;
    }

    if (!signal_present) {
        const bool state_changed = !last_display_state_valid ||
                                   last_signal_present ||
                                   last_no_signal_artwork != no_signal_artwork;
        if (state_changed) {
            begin_no_signal_record(no_signal_artwork);
        }
        return;
    }

    uint32_t sequence = 0u;
    uint64_t capture_timestamp_us = 0u;
    const int buffer_index =
        p2000t_capture_acquire_latest_frame_for_usb(&sequence,
                                                    &capture_timestamp_us);
    if (buffer_index < 0) {
        return;
    }
    if (last_sequence_valid && last_display_state_valid &&
        last_signal_present &&
        (int32_t)(sequence - last_sequence) < STREAM_SEQUENCE_STEP) {
        p2000t_capture_release_frame_from_usb((unsigned)buffer_index);
        return;
    }

    const uint64_t prepare_started = time_us_64();
    p2000t_reconstruction_diagnostics_t diagnostics;
    p2000t_capture_get_frame_diagnostics((unsigned)buffer_index, &diagnostics);
    if (diagnostics.capture_engine == P2000T_CAPTURE_ENGINE_TWO_TAP) {
        diagnostics = (p2000t_reconstruction_diagnostics_t){
            .reconstruction_mode = (uint8_t)reconstruction_mode,
            .capture_engine = P2000T_CAPTURE_ENGINE_TWO_TAP,
            .samples_per_output = 1u,
        };
    }
    pack_rgb111_frame(p2000t_capture_buffer((unsigned)buffer_index),
                      &diagnostics);
    p2000t_capture_release_frame_from_usb((unsigned)buffer_index);
    tx_reconstruction_diagnostics = diagnostics;

    const size_t encoded_size =
        packbits_allowed ? p2000t_packbits_encoded_size(
                               stream_frame, P2000T_STREAM_PAYLOAD_SIZE)
                         : P2000T_STREAM_PAYLOAD_SIZE;
    tx_packbits = encoded_size < P2000T_STREAM_PAYLOAD_SIZE;
    tx_configuration = false;
    tx_payload_size = tx_packbits ? encoded_size : P2000T_STREAM_PAYLOAD_SIZE;
    const uint32_t checksum =
        frame_crc32(stream_frame, P2000T_STREAM_PAYLOAD_SIZE);
    stream_stats.last_prepare_us = (uint32_t)(time_us_64() - prepare_started);
    if (stream_stats.last_prepare_us > stream_stats.maximum_prepare_us) {
        stream_stats.maximum_prepare_us = stream_stats.last_prepare_us;
    }

    tx_sequence = sequence;
    tx_capture_timestamp_us = capture_timestamp_us;
    tx_signal_present = true;
    tx_no_signal_artwork = (uint8_t)no_signal_artwork;
    tx_header_offset = 0u;
    tx_payload_offset = 0u;
    packbits_input_offset = 0u;
    packbits_staging_size = 0u;
    packbits_staging_offset = 0u;
    tx_encode_us = 0u;
    build_header(sequence, checksum);
    tx_started_us = time_us_64();
    tx_active = true;
}

static void fill_packbits_staging(void) {
    const uint64_t started = time_us_64();
    packbits_staging_size = 0u;
    packbits_staging_offset = 0u;
    while (packbits_input_offset < P2000T_STREAM_PAYLOAD_SIZE &&
           STREAM_PACKBITS_STAGING_SIZE - packbits_staging_size >=
               P2000T_PACKBITS_MAX_CHUNK) {
        const size_t chunk = p2000t_packbits_next_chunk(
            stream_frame, P2000T_STREAM_PAYLOAD_SIZE, &packbits_input_offset,
            &packbits_staging[packbits_staging_size]);
        hard_assert(chunk != 0u);
        packbits_staging_size += chunk;
    }
    tx_encode_us += (uint32_t)(time_us_64() - started);
}

void p2000t_usb_stream_start(bool allow_packbits) {
    p2000t_usb_stream_stop();
    packbits_allowed = allow_packbits;
    last_sequence_valid = false;
    last_display_state_valid = false;
    stream_active = true;
}

void p2000t_usb_stream_stop(void) {
    stream_active = false;
    tx_active = false;
    configuration_pending = false;
    tx_header_offset = 0u;
    tx_payload_offset = 0u;
    tx_payload_size = 0u;
    packbits_input_offset = 0u;
    packbits_staging_size = 0u;
    packbits_staging_offset = 0u;
}

void p2000t_usb_stream_queue_configuration(const uint8_t *payload,
                                           uint32_t payload_size) {
    hard_assert(payload != NULL);
    hard_assert(payload_size == sizeof(pending_configuration));
    memcpy(pending_configuration, payload, sizeof(pending_configuration));
    configuration_pending = true;
}

bool p2000t_usb_stream_active(void) {
    return stream_active;
}

void p2000t_usb_stream_set_reconstruction(unsigned reconstruction) {
    hard_assert(reconstruction < P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT);
    reconstruction_mode = reconstruction;
}

void p2000t_usb_stream_service(bool signal_present,
                               unsigned no_signal_artwork) {
    if (!stream_active) {
        return;
    }
    hard_assert(no_signal_artwork < P2000T_STREAM_ARTWORK_COUNT);
    if (!stdio_usb_connected()) {
        p2000t_usb_stream_stop();
        return;
    }

    begin_available_frame(signal_present, no_signal_artwork);
    if (!tx_active) {
        return;
    }

    size_t budget = STREAM_TX_SERVICE_BUDGET;
    while (budget != 0u) {
        const uint32_t available = tud_cdc_write_available();
        if (available == 0u) {
            break;
        }

        const uint8_t *source;
        size_t remaining;
        if (tx_header_offset < P2000T_STREAM_HEADER_SIZE) {
            source = &stream_header[tx_header_offset];
            remaining = P2000T_STREAM_HEADER_SIZE - tx_header_offset;
        } else if (tx_packbits) {
            if (packbits_staging_offset == packbits_staging_size) {
                fill_packbits_staging();
            }
            source = &packbits_staging[packbits_staging_offset];
            remaining = packbits_staging_size - packbits_staging_offset;
        } else {
            source = tx_configuration
                         ? &tx_configuration_payload[tx_payload_offset]
                         : &stream_frame[tx_payload_offset];
            remaining = tx_payload_size - tx_payload_offset;
        }

        size_t count = remaining;
        if (count > available) {
            count = available;
        }
        if (count > budget) {
            count = budget;
        }
        const uint32_t written = tud_cdc_write(source, (uint32_t)count);
        if (written == 0u) {
            break;
        }
        budget -= written;
        if (tx_header_offset < P2000T_STREAM_HEADER_SIZE) {
            tx_header_offset += written;
        } else {
            tx_payload_offset += written;
            if (tx_packbits) {
                packbits_staging_offset += written;
            }
        }

        if (tx_header_offset == P2000T_STREAM_HEADER_SIZE &&
            tx_payload_offset == tx_payload_size) {
            if (!tx_configuration && tx_signal_present && last_sequence_valid &&
                last_display_state_valid && last_signal_present &&
                (uint32_t)(tx_sequence - last_sequence) >
                    STREAM_SEQUENCE_STEP) {
                stream_stats.skipped_sequences +=
                    (uint32_t)(tx_sequence - last_sequence) -
                    STREAM_SEQUENCE_STEP;
            }
            if (!tx_configuration && tx_signal_present) {
                last_sequence = tx_sequence;
                last_sequence_valid = true;
            }
            if (!tx_configuration) {
                last_signal_present = tx_signal_present;
                last_no_signal_artwork = tx_no_signal_artwork;
                last_display_state_valid = true;
                if (!tx_signal_present) {
                    ++stream_stats.no_signal_records_sent;
                } else {
                    ++stream_stats.frames_sent;
                    if (tx_packbits) {
                        ++stream_stats.packbits_frames_sent;
                    } else {
                        ++stream_stats.raw_frames_sent;
                    }
                }
                stream_stats.last_payload_size = (uint32_t)tx_payload_size;
            }
            stream_stats.bytes_sent +=
                P2000T_STREAM_HEADER_SIZE + tx_payload_size;
            stream_stats.last_encode_us = tx_encode_us;
            if (stream_stats.last_encode_us > stream_stats.maximum_encode_us) {
                stream_stats.maximum_encode_us = stream_stats.last_encode_us;
            }
            stream_stats.last_tx_us = (uint32_t)(time_us_64() - tx_started_us);
            if (stream_stats.last_tx_us > stream_stats.maximum_tx_us) {
                stream_stats.maximum_tx_us = stream_stats.last_tx_us;
            }
            tx_active = false;
            if (budget == 0u) {
                break;
            }
            begin_available_frame(signal_present, no_signal_artwork);
            if (!tx_active) {
                break;
            }
        }
    }
    tud_cdc_write_flush();
}

void p2000t_usb_stream_get_stats(p2000t_usb_stream_stats_t *stats) {
    hard_assert(stats != NULL);
    *stats = stream_stats;
}
