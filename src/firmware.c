/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file firmware.c
 * @brief P2000T RGBS framebuffer capture and 640x480 VGA conversion.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "p2000t_capture.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#if !defined(PICO_RP2040) || !PICO_RP2040
#error "The P2000T VGA firmware must be built for an RP2040 Pico 1"
#endif

enum {
    SYSTEM_CLOCK_KHZ = 252000,
    SYSTEM_CORE_VOLTAGE_MV = 1300,
    VGA_TIMING_WIDTH = 640,
    VGA_TIMING_HEIGHT = 480,
    VGA_RENDER_WIDTH = 640,
    VGA_RENDER_HEIGHT = 240,
    VGA_HORIZONTAL_SCALE = 1,
    VGA_VERTICAL_SCALE = 2,
    VGA_LEFT_MARGIN = (VGA_RENDER_WIDTH - P2000T_CAPTURE_WIDTH) / 2,
    VGA_RIGHT_MARGIN =
        VGA_RENDER_WIDTH - VGA_LEFT_MARGIN - P2000T_CAPTURE_WIDTH,
    RAW_SCANLINE_TOKENS = (3 + VGA_RENDER_WIDTH - 1) + 2 + 2,
    RAW_SCANLINE_WORDS = RAW_SCANLINE_TOKENS / 2,
    VGA_READY_MAGIC = 0x56474154,
};

_Static_assert(VGA_RENDER_WIDTH * VGA_HORIZONTAL_SCALE == VGA_TIMING_WIDTH,
               "Scanvideo must render all 640 VGA pixels directly");
_Static_assert(VGA_RENDER_HEIGHT * VGA_VERTICAL_SCALE == VGA_TIMING_HEIGHT,
               "Each captured source line must produce two VGA lines");
_Static_assert(VGA_LEFT_MARGIN == 80 && VGA_RIGHT_MARGIN == 80,
               "The 480-sample source image must be centered in VGA");
_Static_assert((unsigned)P2000T_CAPTURE_HEIGHT ==
                   (unsigned)VGA_RENDER_HEIGHT,
               "Each source line must map to one logical scanvideo line");
_Static_assert(RAW_SCANLINE_WORDS == 323,
               "CMake scanvideo storage must match the raw line renderer");

/** Standard 640x480, nominal 60 Hz VGA at a 25.2 MHz pixel clock. */
static const scanvideo_timing_t vga_timing_640x480_60 = {
    .clock_freq = 25200000,
    .h_active = VGA_TIMING_WIDTH,
    .v_active = VGA_TIMING_HEIGHT,
    .h_front_porch = 16,
    .h_pulse = 96,
    .h_total = 800,
    .h_sync_polarity = 1,
    .v_front_porch = 10,
    .v_pulse = 2,
    .v_total = 525,
    .v_sync_polarity = 1,
    .enable_clock = 0,
    .clock_polarity = 0,
    .enable_den = 0,
};

static const scanvideo_mode_t firmware_vga_mode = {
    .default_timing = &vga_timing_640x480_60,
    .pio_program = &video_24mhz_composable,
    .width = VGA_RENDER_WIDTH,
    .height = VGA_RENDER_HEIGHT,
    .xscale = VGA_HORIZONTAL_SCALE,
    .yscale = VGA_VERTICAL_SCALE,
    .yscale_denominator = 1,
};

/** Full-scale RGB444 colors in P2000T bit order R=1, G=2, B=4. */
static const uint16_t source_colors[8] = {
    0x0000, 0x000f, 0x00f0, 0x00ff,
    0x0f00, 0x0f0f, 0x0ff0, 0x0fff,
};

/** Two decoded RGB444 pixels for every possible packed pair of raw nibbles. */
static uint32_t raw_byte_colors[256];
static int displayed_buffer = -1;
static const uint32_t *displayed_frame;
static volatile uint32_t displayed_sequence;
static bool displayed_signal_present;
static volatile uint32_t generated_vga_frames;
static volatile uint32_t source_frame_swaps;
static volatile uint32_t repeated_vga_frames;
static volatile uint32_t test_pattern_frames;
static volatile uint32_t vga_scanline_id_gaps;
static uint32_t previous_scanline_id;
static bool have_previous_scanline_id;

static void build_input_color_lookup(void) {
    uint16_t raw_nibble_colors[16];
    for (unsigned raw = 0; raw < 16u; ++raw) {
        const unsigned color =
            ((((raw >> P2000T_RED_CHANNEL) & 1u) == 0u) ? 1u : 0u) |
            ((((raw >> P2000T_GREEN_CHANNEL) & 1u) == 0u) ? 2u : 0u) |
            ((((raw >> P2000T_BLUE_CHANNEL) & 1u) == 0u) ? 4u : 0u);
        raw_nibble_colors[raw] = source_colors[color];
    }
    for (unsigned raw = 0; raw < 256u; ++raw) {
        raw_byte_colors[raw] =
            raw_nibble_colors[raw >> 4u] |
            ((uint32_t)raw_nibble_colors[raw & 0x0fu] << 16u);
    }
}

static void finish_raw_scanline(scanvideo_scanline_buffer_t *scanline_buffer,
                                uint16_t *tokens) {
    tokens[RAW_SCANLINE_TOKENS - 4] = COMPOSABLE_RAW_1P;
    tokens[RAW_SCANLINE_TOKENS - 3] = 0x0000;
    tokens[RAW_SCANLINE_TOKENS - 2] = COMPOSABLE_EOL_SKIP_ALIGN;
    tokens[RAW_SCANLINE_TOKENS - 1] = 0;
    scanline_buffer->data_used = RAW_SCANLINE_WORDS;
    scanline_buffer->status = SCANLINE_OK;
}

/** Draw eight cheap color runs while waiting for credible source lock. */
static void render_test_pattern(
    scanvideo_scanline_buffer_t *scanline_buffer, unsigned y) {
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    unsigned token = 0;
    const bool border = y < 2u || y >= VGA_RENDER_HEIGHT - 2u;
    if (border) {
        tokens[token++] = COMPOSABLE_COLOR_RUN;
        tokens[token++] = source_colors[7];
        tokens[token++] = (VGA_RENDER_WIDTH - 1u) - 3u;
    } else {
        for (unsigned bar = 0; bar < 8u; ++bar) {
            tokens[token++] = COMPOSABLE_COLOR_RUN;
            tokens[token++] = source_colors[bar];
            /* Leave the last logical pixel black to prevent blanking bleed. */
            tokens[token++] = (bar == 7u ? 79u : 80u) - 3u;
        }
    }
    tokens[token++] = COMPOSABLE_RAW_1P;
    tokens[token++] = 0x0000;
    if ((token & 1u) == 0u) {
        tokens[token++] = COMPOSABLE_EOL_SKIP_ALIGN;
        tokens[token++] = 0;
    } else {
        tokens[token++] = COMPOSABLE_EOL_ALIGN;
    }
    scanline_buffer->data_used = (token + 1u) / 2u;
    scanline_buffer->status = SCANLINE_OK;
}

/** Draw one 480-sample source line centered in the 640-pixel VGA line. */
static void render_source_scanline(
    scanvideo_scanline_buffer_t *scanline_buffer, unsigned source_y) {
    uint16_t *tokens = (uint16_t *)scanline_buffer->data;
    tokens[0] = COMPOSABLE_RAW_RUN;
    tokens[1] = 0x0000;
    tokens[2] = VGA_RENDER_WIDTH - 3;
    uint16_t *destination = &tokens[3];

    /* Pixel zero is tokens[1]; destination begins at VGA pixel one. */
    for (unsigned x = 1; x < VGA_LEFT_MARGIN; ++x) {
        *destination++ = 0x0000;
    }
    const uint32_t *source = displayed_frame +
        source_y * P2000T_CAPTURE_WORDS_PER_LINE;
    hard_assert(((uintptr_t)destination & 3u) == 0u);
    uint32_t *packed_destination = (uint32_t *)destination;
    for (unsigned word_index = 0;
         word_index < P2000T_CAPTURE_WORDS_PER_LINE; ++word_index) {
        const uint32_t raw = source[word_index];
        *packed_destination++ = raw_byte_colors[(uint8_t)(raw >> 24u)];
        *packed_destination++ = raw_byte_colors[(uint8_t)(raw >> 16u)];
        *packed_destination++ = raw_byte_colors[(uint8_t)(raw >> 8u)];
        *packed_destination++ = raw_byte_colors[(uint8_t)raw];
    }
    destination = (uint16_t *)packed_destination;
    for (unsigned x = 0; x < VGA_RIGHT_MARGIN; ++x) {
        *destination++ = 0x0000;
    }

    hard_assert(destination == &tokens[VGA_RENDER_WIDTH + 2u]);
    finish_raw_scanline(scanline_buffer, tokens);
}

/** Atomically adopt the newest source frame at a VGA frame boundary. */
static void select_frame_for_next_vga_frame(void) {
    ++generated_vga_frames;
    displayed_signal_present = p2000t_capture_signal_present();

    uint32_t sequence;
    const int next = p2000t_capture_acquire_latest_frame(&sequence);
    if (next < 0) {
        if (displayed_buffer >= 0) {
            ++repeated_vga_frames;
        }
        if (!displayed_signal_present) {
            ++test_pattern_frames;
        }
        return;
    }

    const int previous = displayed_buffer;
    displayed_buffer = next;
    displayed_frame = p2000t_capture_buffer((unsigned)next);
    displayed_sequence = sequence;
    ++source_frame_swaps;
    if (previous >= 0) {
        p2000t_capture_release_frame((unsigned)previous);
    }
}

static void render_scanline(scanvideo_scanline_buffer_t *scanline_buffer) {
    const unsigned y = scanvideo_scanline_number(scanline_buffer->scanline_id);
    if (have_previous_scanline_id) {
        const uint32_t previous_y = previous_scanline_id & 0xffffu;
        const uint32_t expected = previous_y + 1u < VGA_RENDER_HEIGHT
            ? previous_scanline_id + 1u
            : (previous_scanline_id & 0xffff0000u) + 0x10000u;
        if (scanline_buffer->scanline_id != expected) {
            ++vga_scanline_id_gaps;
        }
    }
    previous_scanline_id = scanline_buffer->scanline_id;
    have_previous_scanline_id = true;
    if (scanline_buffer->data_max < RAW_SCANLINE_WORDS) {
        scanline_buffer->data_used = 0;
        scanline_buffer->status = SCANLINE_ERROR;
        return;
    }

    if (y == 0u) {
        select_frame_for_next_vga_frame();
    }
    if (!displayed_signal_present || displayed_buffer < 0) {
        render_test_pattern(scanline_buffer, y);
        return;
    }
    render_source_scanline(scanline_buffer, y);
}

/** Core 1 is dedicated to the deadline-critical VGA scanline producer. */
static void __not_in_flash_func(vga_core_main)(void) {
    multicore_fifo_push_blocking(VGA_READY_MAGIC);
    while (true) {
        scanvideo_scanline_buffer_t *scanline_buffer =
            scanvideo_begin_scanline_generation(true);
        render_scanline(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void print_status(void) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const uint32_t vga_frames =
        __atomic_load_n(&generated_vga_frames, __ATOMIC_RELAXED);
    const uint32_t swaps =
        __atomic_load_n(&source_frame_swaps, __ATOMIC_RELAXED);
    const uint32_t repeats =
        __atomic_load_n(&repeated_vga_frames, __ATOMIC_RELAXED);
    const uint32_t tests =
        __atomic_load_n(&test_pattern_frames, __ATOMIC_RELAXED);
    const uint32_t id_gaps =
        __atomic_load_n(&vga_scanline_id_gaps, __ATOMIC_RELAXED);
    const uint32_t sequence =
        __atomic_load_n(&displayed_sequence, __ATOMIC_RELAXED);

    printf("VID2VGA captured=%" PRIu32 " locked=%s period=",
           capture.captured_frames,
           capture.signal_present ? "yes" : "no");
    if (capture.last_frame_period_us == 0u) {
        printf("unknown");
    } else {
        const uint32_t rate_millihz =
            1000000000u / capture.last_frame_period_us;
        printf("%" PRIu32 "us (%" PRIu32 ".%03" PRIu32 "Hz)",
               capture.last_frame_period_us,
               rate_millihz / 1000u,
               rate_millihz % 1000u);
    }
    printf(" first_line=%" PRIu32 " h_offset=%" PRIu32
           " phase=%" PRId32 " stale=%" PRIu32
           " vga=%" PRIu32 " swaps=%" PRIu32
           " repeats=%" PRIu32 " test=%" PRIu32
           " id_gaps=%" PRIu32 " displayed=%" PRIu32 "\n",
           capture.first_visible_scanline,
           capture.horizontal_offset,
           capture.sample_phase,
           capture.stale_frames_replaced,
           vga_frames, swaps, repeats, tests, id_gaps, sequence);
}

static void print_help(void) {
    printf("Commands: s=status, [=image up, ]=image down, "
           "0=reset line, ,=sample earlier, .=sample later, "
           "p=reset phase, <=start earlier, >=start later, "
           "x=reset start, h=help\n");
}

static void adjust_first_visible_line(int change) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = (int)capture.first_visible_scanline + change;
    if (!p2000t_capture_set_first_visible_scanline((unsigned)requested)) {
        printf("First visible line must remain between %u and %u.\n",
               P2000T_MIN_FIRST_VISIBLE_SCANLINE,
               P2000T_MAX_FIRST_VISIBLE_SCANLINE);
        return;
    }
    printf("First visible source scanline set to %d; applies on the next frame.\n",
           requested);
}

static void adjust_sample_phase(int change) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = capture.sample_phase + change;
    if (!p2000t_capture_set_sample_phase(requested)) {
        printf("Sample phase must remain between %d and %d.\n",
               P2000T_MIN_SAMPLE_PHASE, P2000T_MAX_SAMPLE_PHASE);
        return;
    }
    printf("Sample phase set to %+d (positive is later); "
           "one tick is 7.94 ns and it applies on the next source frame.\n",
           requested);
}

static void adjust_horizontal_offset(int change) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = (int)capture.horizontal_offset + change;
    if (requested < P2000T_MIN_HORIZONTAL_OFFSET ||
        requested > P2000T_MAX_HORIZONTAL_OFFSET ||
        !p2000t_capture_set_horizontal_offset((unsigned)requested)) {
        printf("Horizontal start must remain between %u and %u source dots.\n",
               P2000T_MIN_HORIZONTAL_OFFSET,
               P2000T_MAX_HORIZONTAL_OFFSET);
        return;
    }
    printf("Horizontal start set to %d source dots (%d characters); "
           "applies on the next frame.\n",
           requested, requested / P2000T_HORIZONTAL_OFFSET_STEP);
}

static void poll_usb_commands(void) {
    int command;
    while ((command = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (command == 's' || command == 'S') {
            print_status();
        } else if (command == '[' || command == '-') {
            adjust_first_visible_line(-1);
        } else if (command == ']' || command == '+') {
            adjust_first_visible_line(1);
        } else if (command == '0') {
            if (p2000t_capture_set_first_visible_scanline(
                    P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE)) {
                printf("First visible source scanline reset to %u.\n",
                       P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE);
            }
        } else if (command == ',') {
            adjust_sample_phase(-1);
        } else if (command == '.') {
            adjust_sample_phase(1);
        } else if (command == 'p' || command == 'P') {
            if (p2000t_capture_set_sample_phase(
                    P2000T_DEFAULT_SAMPLE_PHASE)) {
                printf("Sample phase reset to %d.\n",
                       P2000T_DEFAULT_SAMPLE_PHASE);
            }
        } else if (command == '<') {
            adjust_horizontal_offset(-P2000T_HORIZONTAL_OFFSET_STEP);
        } else if (command == '>') {
            adjust_horizontal_offset(P2000T_HORIZONTAL_OFFSET_STEP);
        } else if (command == 'x' || command == 'X') {
            if (p2000t_capture_set_horizontal_offset(
                    P2000T_DEFAULT_HORIZONTAL_OFFSET)) {
                printf("Horizontal start reset to %u source dots.\n",
                       P2000T_DEFAULT_HORIZONTAL_OFFSET);
            }
        } else if (command == 'h' || command == 'H' || command == '?') {
            print_help();
        }
    }
}

int main(void) {
    /* Experimental Pico 1 overclock. The 252 MHz clock is a common multiple
       of the 6 MHz P2000T dot clock and the 25.2 MHz VGA pixel clock. */
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_us(1000u);
    if (!set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true)) {
        panic("Unable to set the experimental 252 MHz Pico 1 system clock");
    }
    stdio_init_all();
    build_input_color_lookup();

    /* scanvideo owns PIO0 and its fixed DMA channel; capture then claims PIO1
       and two otherwise-unused DMA channels. */
    if (!scanvideo_setup(&firmware_vga_mode)) {
        panic("Unable to initialize VGA scanvideo");
    }
    p2000t_capture_start();
    multicore_launch_core1(vga_core_main);
    if (multicore_fifo_pop_blocking() != VGA_READY_MAGIC) {
        panic("Unable to start VGA rendering core");
    }
    scanvideo_timing_enable(true);

    bool announced = false;
    while (true) {
        if (!stdio_usb_connected()) {
            announced = false;
            sleep_ms(10);
            continue;
        }
        if (!announced) {
            sleep_ms(100);
            printf("\nP2000T VID2VGA firmware v%s -- RP2040 / Pico 1\n",
                   P2000T_VID2VGA_VERSION);
            printf("Input: CSYNC=GP%u R=GP%u G=GP%u B=GP%u; "
                   "VGA: RGB=GP0-GP11 HSYNC=GP12 VSYNC=GP13\n",
                   P2000T_SYNC_PIN, P2000T_RED_PIN,
                   P2000T_GREEN_PIN, P2000T_BLUE_PIN);
#if P2000T_PROTOTYPE_V1_MIRRORED_DIN
            printf("Input profile: prototype v1 mirrored DIN contacts 1/5 and 2/4\n");
#else
            printf("Input profile: corrected PCB v2 DIN mapping\n");
#endif
            printf("Display: 480x240 raw RGBS capture, vertically doubled "
                   "to centered 480x480 VGA\n");
            printf("EXPERIMENTAL clock=%uMHz core_voltage=%u.%03uV; "
                   "capture=12MHz raw VGA=25.2MHz\n",
                   SYSTEM_CLOCK_KHZ / 1000u,
                   SYSTEM_CORE_VOLTAGE_MV / 1000u,
                   SYSTEM_CORE_VOLTAGE_MV % 1000u);
            print_status();
            print_help();
            announced = true;
        }
        poll_usb_commands();
        sleep_ms(10);
    }
}
