/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "p2000t_settings.h"

#include "p2000t_palette.h"

#include <stddef.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/platform.h"

enum {
    SETTINGS_MAGIC = 0x53325450u, /* "P2TS" in little-endian order. */
    SETTINGS_VERSION = 2,
    SETTINGS_LEGACY_VERSION = 1,
    SETTINGS_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE,
};

extern char __flash_binary_end;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    p2000t_settings_t settings;
    uint8_t reconstruction;
    uint8_t reserved[3];
    uint32_t crc32;
} settings_record_t;

/** v0.3.x record retained for in-place migration on first v0.4.0 save. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    p2000t_settings_t settings;
    uint32_t crc32;
} legacy_settings_record_t;

_Static_assert(sizeof(settings_record_t) <= FLASH_PAGE_SIZE,
               "Persistent settings must fit in one flash page");
_Static_assert(sizeof(legacy_settings_record_t) == 36u,
               "Legacy settings layout must remain readable");

/** Return whether a reconstruction mode is supported by this processor. */
static bool reconstruction_valid(unsigned reconstruction) {
#if defined(PICO_RP2350) && PICO_RP2350
    return reconstruction < P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT;
#else
    return reconstruction < P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_FIRST;
#endif
}

static uint32_t settings_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? 0xedb88320u : 0u);
        }
    }
    return ~crc;
}

unsigned p2000t_settings_artwork(const p2000t_settings_t *settings) {
    return settings->capture_options & P2000T_CONFIGURATION_ARTWORK_MASK;
}

void p2000t_settings_set_artwork(p2000t_settings_t *settings,
                                 unsigned artwork) {
    settings->capture_options =
        (uint8_t)((settings->capture_options &
                   (P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK |
                    (P2000T_CONFIGURATION_RATE_TRIM_VALUE_MASK
                     << P2000T_CONFIGURATION_RATE_TRIM_SHIFT))) |
                  (artwork & P2000T_CONFIGURATION_ARTWORK_MASK));
}

unsigned
p2000t_settings_sample_reconstruction(const p2000t_settings_t *settings) {
    return (settings->capture_options &
            P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK) >>
           P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_SHIFT;
}

void p2000t_settings_set_sample_reconstruction(p2000t_settings_t *settings,
                                               unsigned reconstruction) {
    settings->capture_options =
        (uint8_t)((settings->capture_options &
                   (P2000T_CONFIGURATION_ARTWORK_MASK |
                    (P2000T_CONFIGURATION_RATE_TRIM_VALUE_MASK
                     << P2000T_CONFIGURATION_RATE_TRIM_SHIFT))) |
                  ((reconstruction
                    << P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_SHIFT) &
                   P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK));
}

int p2000t_settings_sample_rate_trim(const p2000t_settings_t *settings) {
    const unsigned encoded =
        settings->capture_options >> P2000T_CONFIGURATION_RATE_TRIM_SHIFT;
    return (encoded & 0x10u) != 0u ? (int)encoded - 32 : (int)encoded;
}

void p2000t_settings_set_sample_rate_trim(p2000t_settings_t *settings,
                                          int trim) {
    const uint8_t encoded =
        (uint8_t)(((unsigned)trim & P2000T_CONFIGURATION_RATE_TRIM_VALUE_MASK)
                  << P2000T_CONFIGURATION_RATE_TRIM_SHIFT);
    settings->capture_options =
        (uint8_t)((settings->capture_options &
                   (P2000T_CONFIGURATION_ARTWORK_MASK |
                    P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK)) |
                  encoded);
}

void p2000t_settings_defaults(p2000t_settings_t *settings,
                              unsigned *reconstruction) {
    static const uint16_t default_palette[P2000T_CONTROL_PALETTE_COLORS] = {
        0x0000, 0x000f, 0x00f0, 0x00ff, 0x0f00, 0x0f0f, 0x0ff0, 0x0fff,
    };
    hard_assert(settings != NULL);
    hard_assert(reconstruction != NULL);
    memset(settings, 0, sizeof(*settings));
    settings->first_visible_scanline = P2000T_CONTROL_DEFAULT_VERTICAL;
#if defined(PICO_RP2350) && PICO_RP2350
    settings->sample_phase = P2000T_CONTROL_PICO2_DEFAULT_PHASE;
    settings->odd_line_phase = P2000T_CONTROL_PICO2_DEFAULT_ODD_LINE_PHASE;
    *reconstruction = P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION;
#else
    settings->sample_phase = P2000T_CONTROL_DEFAULT_PHASE;
    settings->odd_line_phase = P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE;
    *reconstruction = P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION;
#endif
    settings->horizontal_offset = P2000T_CONTROL_DEFAULT_HORIZONTAL;
    p2000t_settings_set_artwork(settings, P2000T_CONTROL_DEFAULT_ARTWORK);
    p2000t_settings_set_sample_rate_trim(
        settings, P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM);
    p2000t_settings_set_sample_reconstruction(
        settings, P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION);
    memcpy(settings->palette, default_palette, sizeof(default_palette));
}

bool p2000t_settings_valid(const p2000t_settings_t *settings) {
    if (settings == NULL ||
        settings->first_visible_scanline < P2000T_CONTROL_MIN_VERTICAL ||
        settings->first_visible_scanline > P2000T_CONTROL_MAX_VERTICAL ||
        settings->sample_phase < P2000T_CONTROL_MIN_PHASE ||
        settings->sample_phase > P2000T_CONTROL_MAX_PHASE ||
        settings->odd_line_phase < P2000T_CONTROL_MIN_ODD_LINE_PHASE ||
        settings->odd_line_phase > P2000T_CONTROL_MAX_ODD_LINE_PHASE ||
        p2000t_settings_sample_rate_trim(settings) <
            P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM ||
        p2000t_settings_sample_rate_trim(settings) >
            P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM ||
        p2000t_settings_sample_reconstruction(settings) >=
            P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT ||
        settings->horizontal_offset > P2000T_CONTROL_MAX_HORIZONTAL ||
        settings->horizontal_offset % P2000T_CONTROL_HORIZONTAL_STEP != 0u ||
        p2000t_settings_artwork(settings) >= 3u) {
        return false;
    }
    for (unsigned index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        if (settings->palette[index] > P2000T_CONTROL_RGB444_MAX) {
            return false;
        }
    }
    /* A completely black source palette makes a valid captured image
       indistinguishable from failed VGA scanout. It can be produced by an
       incomplete host-side configuration transaction, but is never a useful
       persisted display configuration. Reject it so boot falls back to the
       firmware defaults. */
    return p2000t_palette_has_visible_source_color(settings->palette);
}

bool p2000t_settings_load(p2000t_settings_t *settings,
                          unsigned *reconstruction) {
    if (settings == NULL || reconstruction == NULL) {
        return false;
    }
    if ((uintptr_t)&__flash_binary_end - XIP_BASE > SETTINGS_FLASH_OFFSET) {
        return false;
    }
    const settings_record_t *record =
        (const settings_record_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
    if (record->magic != SETTINGS_MAGIC) {
        return false;
    }
    if (record->version == SETTINGS_VERSION &&
        record->size == sizeof(record->settings) +
                            sizeof(record->reconstruction) +
                            sizeof(record->reserved) &&
        settings_crc32((const uint8_t *)record,
                       offsetof(settings_record_t, crc32)) == record->crc32 &&
        p2000t_settings_valid(&record->settings) &&
        reconstruction_valid(record->reconstruction)) {
        *settings = record->settings;
        *reconstruction = record->reconstruction;
        return true;
    }

    const legacy_settings_record_t *legacy =
        (const legacy_settings_record_t *)record;
    if (legacy->version == SETTINGS_LEGACY_VERSION &&
        legacy->size == sizeof(legacy->settings) &&
        settings_crc32((const uint8_t *)legacy,
                       offsetof(legacy_settings_record_t, crc32)) ==
            legacy->crc32 &&
        p2000t_settings_valid(&legacy->settings)) {
        *settings = legacy->settings;
        *reconstruction =
            p2000t_settings_sample_reconstruction(&legacy->settings);
        return true;
    }
    return false;
}

typedef union {
    uint8_t page[FLASH_PAGE_SIZE];
    settings_record_t record;
} settings_write_t;

static void __not_in_flash_func(write_settings_page)(void *parameter) {
    const settings_write_t *write = (const settings_write_t *)parameter;
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, write->page, FLASH_PAGE_SIZE);
}

bool p2000t_settings_save(const p2000t_settings_t *settings,
                          unsigned reconstruction) {
    if (!p2000t_settings_valid(settings) ||
        !reconstruction_valid(reconstruction) ||
        (uintptr_t)&__flash_binary_end - XIP_BASE > SETTINGS_FLASH_OFFSET) {
        return false;
    }
    settings_write_t write;
    memset(&write, 0xff, sizeof(write));
    settings_record_t *record = &write.record;
    record->magic = SETTINGS_MAGIC;
    record->version = SETTINGS_VERSION;
    record->size = sizeof(record->settings) + sizeof(record->reconstruction) +
                   sizeof(record->reserved);
    record->settings = *settings;
    record->reconstruction = (uint8_t)reconstruction;
    memset(record->reserved, 0, sizeof(record->reserved));
    record->crc32 = settings_crc32((const uint8_t *)record,
                                   offsetof(settings_record_t, crc32));
    return flash_safe_execute(write_settings_page, &write, 1000u) == PICO_OK;
}
