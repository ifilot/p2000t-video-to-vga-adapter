/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "p2000t_settings.h"

#include <stddef.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/platform.h"

enum {
    SETTINGS_MAGIC = 0x53325450u, /* "P2TS" in little-endian order. */
    SETTINGS_VERSION = 1,
    SETTINGS_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE,
};

extern char __flash_binary_end;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    p2000t_settings_t settings;
    uint32_t crc32;
} settings_record_t;

_Static_assert(sizeof(settings_record_t) <= FLASH_PAGE_SIZE,
               "Persistent settings must fit in one flash page");

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

void p2000t_settings_defaults(p2000t_settings_t *settings) {
    static const uint16_t default_palette[P2000T_CONTROL_PALETTE_COLORS] = {
        0x0000, 0x000f, 0x00f0, 0x00ff, 0x0f00, 0x0f0f, 0x0ff0, 0x0fff,
    };
    memset(settings, 0, sizeof(*settings));
    settings->first_visible_scanline = P2000T_CONTROL_DEFAULT_VERTICAL;
    settings->sample_phase = P2000T_CONTROL_DEFAULT_PHASE;
    settings->horizontal_offset = P2000T_CONTROL_DEFAULT_HORIZONTAL;
    settings->no_signal_artwork = P2000T_CONTROL_DEFAULT_ARTWORK;
    memcpy(settings->palette, default_palette, sizeof(default_palette));
}

bool p2000t_settings_valid(const p2000t_settings_t *settings) {
    if (settings == NULL ||
        settings->first_visible_scanline < P2000T_CONTROL_MIN_VERTICAL ||
        settings->first_visible_scanline > P2000T_CONTROL_MAX_VERTICAL ||
        settings->sample_phase < P2000T_CONTROL_MIN_PHASE ||
        settings->sample_phase > P2000T_CONTROL_MAX_PHASE ||
        settings->horizontal_offset > P2000T_CONTROL_MAX_HORIZONTAL ||
        settings->horizontal_offset % P2000T_CONTROL_HORIZONTAL_STEP != 0u ||
        settings->no_signal_artwork >= 3u || settings->reserved != 0u) {
        return false;
    }
    for (unsigned index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        if (settings->palette[index] > P2000T_CONTROL_RGB444_MAX) {
            return false;
        }
    }
    return true;
}

bool p2000t_settings_load(p2000t_settings_t *settings) {
    if ((uintptr_t)&__flash_binary_end - XIP_BASE > SETTINGS_FLASH_OFFSET) {
        return false;
    }
    const settings_record_t *record =
        (const settings_record_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
    if (record->magic != SETTINGS_MAGIC ||
        record->version != SETTINGS_VERSION ||
        record->size != sizeof(record->settings) ||
        settings_crc32((const uint8_t *)record,
                       offsetof(settings_record_t, crc32)) != record->crc32 ||
        !p2000t_settings_valid(&record->settings)) {
        return false;
    }
    *settings = record->settings;
    return true;
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

bool p2000t_settings_save(const p2000t_settings_t *settings) {
    if (!p2000t_settings_valid(settings) ||
        (uintptr_t)&__flash_binary_end - XIP_BASE > SETTINGS_FLASH_OFFSET) {
        return false;
    }
    settings_write_t write;
    memset(&write, 0xff, sizeof(write));
    settings_record_t *record = &write.record;
    record->magic = SETTINGS_MAGIC;
    record->version = SETTINGS_VERSION;
    record->size = sizeof(record->settings);
    record->settings = *settings;
    record->crc32 = settings_crc32((const uint8_t *)record,
                                   offsetof(settings_record_t, crc32));
    return flash_safe_execute(write_settings_page, &write, 1000u) == PICO_OK;
}
