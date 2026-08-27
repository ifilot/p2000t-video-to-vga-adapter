/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file firmware.c
 * @brief P2000T RGBS capture, VGA orchestration, and USB control interface.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "p2000t_capture.h"
#include "p2000t_video_renderer.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#if defined(PICO_RP2040) && PICO_RP2040
/** Human-readable processor name included in the USB banner. */
#define P2000T_PROCESSOR_NAME "RP2040 / Pico 1"
#elif defined(PICO_RP2350) && PICO_RP2350
/** Human-readable processor name included in the USB banner. */
#define P2000T_PROCESSOR_NAME "RP2350 / Pico 2"
#else
#error "The P2000T VGA firmware supports only Pico and Pico 2"
#endif

/** Firmware clock, voltage, and inter-core synchronization constants. */
enum {
    SYSTEM_CLOCK_KHZ = 252000,     /**< Overclocked system frequency. */
    SYSTEM_CORE_VOLTAGE_MV = 1300, /**< Core voltage used at 252 MHz. */
    VGA_READY_MAGIC = 0x56474154,  /**< Core-1 VGA-ready FIFO token. */
};

_Static_assert(P2000T_VGA_RENDER_WIDTH *P2000T_VGA_HORIZONTAL_SCALE ==
                   P2000T_VGA_TIMING_WIDTH,
               "Scanvideo must render all 640 VGA pixels directly");
_Static_assert(P2000T_VGA_RENDER_HEIGHT *P2000T_VGA_VERTICAL_SCALE ==
                   P2000T_VGA_TIMING_HEIGHT,
               "Each captured source line must produce two VGA lines");
_Static_assert((unsigned)P2000T_CAPTURE_HEIGHT ==
                   (unsigned)P2000T_VGA_RENDER_HEIGHT,
               "Each source line must map to one logical scanvideo line");

/** Standard 640x480, nominal 60 Hz VGA at a 25.2 MHz pixel clock. */
static const scanvideo_timing_t vga_timing_640x480_60 = {
    .clock_freq = 25200000,
    .h_active = P2000T_VGA_TIMING_WIDTH,
    .v_active = P2000T_VGA_TIMING_HEIGHT,
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

/** Logical scanvideo mode that vertically doubles the 240-line source. */
static const scanvideo_mode_t firmware_vga_mode = {
    .default_timing = &vga_timing_640x480_60,
    .pio_program = &video_24mhz_composable,
    .width = P2000T_VGA_RENDER_WIDTH,
    .height = P2000T_VGA_RENDER_HEIGHT,
    .xscale = P2000T_VGA_HORIZONTAL_SCALE,
    .yscale = P2000T_VGA_VERTICAL_SCALE,
    .yscale_denominator = 1,
};

/** Frame and scanline state owned exclusively by the VGA core. */
typedef struct {
    int buffer_index;               /**< Currently displayed capture buffer. */
    const uint32_t *frame;          /**< Packed frame currently being read. */
    bool signal_present;            /**< Signal decision fixed per VGA frame. */
    p2000t_no_signal_artwork_t
        no_signal_artwork;          /**< Artwork fixed per VGA frame. */
    uint32_t previous_scanline_id;  /**< Last scanvideo identifier observed. */
    bool have_previous_scanline_id; /**< Whether the previous ID is valid. */
} vga_display_state_t;

/** Cross-core diagnostic counters updated by the VGA core. */
typedef struct {
    volatile uint32_t displayed_sequence; /**< Current source-frame sequence. */
    volatile uint32_t generated_frames;   /**< VGA frames started. */
    volatile uint32_t source_frame_swaps; /**< New source frames adopted. */
    volatile uint32_t repeated_frames; /**< VGA frames reusing source data. */
    volatile uint32_t
        signal_lost_frames;             /**< VGA frames with no valid input. */
    volatile uint32_t scanline_id_gaps; /**< Non-contiguous scanline IDs. */
} vga_statistics_t;

/** VGA-core-owned display state, initialized without a captured frame. */
static vga_display_state_t display_state = {
    .buffer_index = -1,
    .no_signal_artwork = P2000T_NO_SIGNAL_ARTWORK_DEFAULT,
};

/** Diagnostics shared atomically between the VGA and control cores. */
static vga_statistics_t vga_statistics;

/** USB-selected artwork adopted by the VGA core at its next frame boundary. */
static volatile uint32_t requested_no_signal_artwork =
    P2000T_NO_SIGNAL_ARTWORK_DEFAULT;

/**
 * @brief Increment a diagnostic counter owned exclusively by the VGA core.
 *
 * The control core performs aligned atomic loads, while the VGA core remains
 * the sole writer. Avoiding a read-modify-write atomic here also avoids the
 * RP2040's out-of-line atomic helper in a deadline-sensitive path.
 *
 * @param counter VGA-core-owned counter to increment.
 */
static inline void increment_counter(volatile uint32_t *counter) {
    ++*counter;
}

/**
 * @brief Atomically load a cross-core diagnostic value.
 *
 * @param value Value to read using relaxed memory ordering.
 * @return Consistent 32-bit snapshot of the value.
 */
static inline uint32_t load_statistic(const volatile uint32_t *value) {
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

/**
 * @brief Atomically store a cross-core diagnostic value.
 *
 * @param destination Value to update using relaxed memory ordering.
 * @param value New value to store.
 */
static inline void store_statistic(volatile uint32_t *destination,
                                   uint32_t value) {
    __atomic_store_n(destination, value, __ATOMIC_RELAXED);
}

/**
 * @brief Adopt the newest complete source frame at a VGA frame boundary.
 *
 * The prior frame remains displayed when capture has not completed a newer
 * frame. A claimed buffer is released only after its replacement is active.
 */
static void select_frame_for_next_vga_frame(void) {
    increment_counter(&vga_statistics.generated_frames);
    display_state.no_signal_artwork = (p2000t_no_signal_artwork_t)
        load_statistic(&requested_no_signal_artwork);
    display_state.signal_present = p2000t_capture_signal_present();

    uint32_t sequence;
    const int next_buffer = p2000t_capture_acquire_latest_frame(&sequence);
    if (next_buffer < 0) {
        if (display_state.buffer_index >= 0) {
            increment_counter(&vga_statistics.repeated_frames);
        }
        if (!display_state.signal_present) {
            increment_counter(&vga_statistics.signal_lost_frames);
        }
        return;
    }

    const int previous_buffer = display_state.buffer_index;
    display_state.buffer_index = next_buffer;
    display_state.frame = p2000t_capture_buffer((unsigned)next_buffer);
    store_statistic(&vga_statistics.displayed_sequence, sequence);
    increment_counter(&vga_statistics.source_frame_swaps);
    if (previous_buffer >= 0) {
        p2000t_capture_release_frame((unsigned)previous_buffer);
    }
}

/**
 * @brief Record scanvideo identifier discontinuities for diagnostics.
 *
 * @param scanline_id Identifier assigned to the current logical scanline.
 */
static void track_scanline_id(uint32_t scanline_id) {
    if (display_state.have_previous_scanline_id) {
        const uint32_t previous_y =
            display_state.previous_scanline_id & 0xffffu;
        const uint32_t expected =
            previous_y + 1u < P2000T_VGA_RENDER_HEIGHT
                ? display_state.previous_scanline_id + 1u
                : (display_state.previous_scanline_id & 0xffff0000u) + 0x10000u;
        if (scanline_id != expected) {
            increment_counter(&vga_statistics.scanline_id_gaps);
        }
    }
    display_state.previous_scanline_id = scanline_id;
    display_state.have_previous_scanline_id = true;
}

/**
 * @brief Render one logical VGA scanline from the selected display state.
 *
 * @param scanline_buffer Scanvideo buffer supplied for generation.
 */
static void render_scanline(scanvideo_scanline_buffer_t *scanline_buffer) {
    const unsigned y = scanvideo_scanline_number(scanline_buffer->scanline_id);
    track_scanline_id(scanline_buffer->scanline_id);

    if (scanline_buffer->data_max < P2000T_RAW_SCANLINE_WORDS) {
        scanline_buffer->data_used = 0;
        scanline_buffer->status = SCANLINE_ERROR;
        return;
    }
    if (y == 0u) {
        select_frame_for_next_vga_frame();
    }
    if (!display_state.signal_present || display_state.buffer_index < 0) {
        p2000t_video_render_no_signal_scanline(
            scanline_buffer, y, display_state.no_signal_artwork);
        return;
    }
    p2000t_video_render_source_scanline(scanline_buffer, display_state.frame,
                                        y);
}

/**
 * @brief Produce deadline-critical VGA scanlines continuously on core 1.
 *
 * This function signals core 0 once it is running and then never returns.
 */
static void __not_in_flash_func(vga_core_main)(void) {
    multicore_fifo_push_blocking(VGA_READY_MAGIC);
    while (true) {
        scanvideo_scanline_buffer_t *scanline_buffer =
            scanvideo_begin_scanline_generation(true);
        render_scanline(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

/**
 * @brief Print a consistent snapshot of capture and VGA diagnostics over USB.
 */
static void print_status(void) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const uint32_t vga_frames =
        load_statistic(&vga_statistics.generated_frames);
    const uint32_t swaps = load_statistic(&vga_statistics.source_frame_swaps);
    const uint32_t repeats = load_statistic(&vga_statistics.repeated_frames);
    const uint32_t lost = load_statistic(&vga_statistics.signal_lost_frames);
    const uint32_t id_gaps = load_statistic(&vga_statistics.scanline_id_gaps);
    const uint32_t sequence =
        load_statistic(&vga_statistics.displayed_sequence);
    const uint32_t artwork = load_statistic(&requested_no_signal_artwork);

    printf("VID2VGA captured=%" PRIu32 " locked=%s period=",
           capture.captured_frames, capture.signal_present ? "yes" : "no");
    if (capture.last_frame_period_us == 0u) {
        printf("unknown");
    } else {
        const uint32_t rate_millihz =
            1000000000u / capture.last_frame_period_us;
        printf("%" PRIu32 "us (%" PRIu32 ".%03" PRIu32 "Hz)",
               capture.last_frame_period_us, rate_millihz / 1000u,
               rate_millihz % 1000u);
    }
    printf(" first_line=%" PRIu32 " h_offset=%" PRIu32 " phase=%" PRId32
           " stale=%" PRIu32 " vga=%" PRIu32 " swaps=%" PRIu32
           " repeats=%" PRIu32 " lost=%" PRIu32 " id_gaps=%" PRIu32
           " displayed=%" PRIu32 " artwork=%" PRIu32 "\n",
           capture.first_visible_scanline, capture.horizontal_offset,
           capture.sample_phase, capture.stale_frames_replaced, vga_frames,
           swaps, repeats, lost, id_gaps, sequence, artwork + 1u);
}

/** @brief Print the available single-character USB commands. */
static void print_help(void) {
    printf("Commands: s=status, [=image up, ]=image down, "
           "0=reset line, ,=sample earlier, .=sample later, "
           "p=reset phase, <=start earlier, >=start later, "
           "x=reset start, h=help\n");
    printf("No-connection artwork: 1=green phosphor, 2=synthwave, "
           "3=amber circuit\n");
}

/**
 * @brief Request a no-connection artwork change at the next VGA frame.
 *
 * @param artwork Valid embedded artwork selection.
 */
static void select_no_signal_artwork(p2000t_no_signal_artwork_t artwork) {
    hard_assert((unsigned)artwork < P2000T_NO_SIGNAL_ARTWORK_COUNT);
    store_statistic(&requested_no_signal_artwork, (uint32_t)artwork);
    static const char *const names[P2000T_NO_SIGNAL_ARTWORK_COUNT] = {
        "green phosphor", "synthwave", "amber circuit"};
    printf("No-connection artwork %u selected: %s. "
           "Applies at the next VGA frame.\n",
           (unsigned)artwork + 1u, names[artwork]);
}

/**
 * @brief Adjust the first captured source line by a signed amount.
 *
 * @param change Signed number of source scanlines to add.
 */
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
    printf("First visible source scanline set to %d; "
           "applies on the next frame.\n",
           requested);
}

/**
 * @brief Adjust the fine capture phase by a signed number of PIO ticks.
 *
 * @param change Signed number of 7.94 ns capture ticks to add.
 */
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

/**
 * @brief Adjust the coarse horizontal capture start.
 *
 * @param change Signed number of nominal source dots to add.
 */
static void adjust_horizontal_offset(int change) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = (int)capture.horizontal_offset + change;
    if (requested < P2000T_MIN_HORIZONTAL_OFFSET ||
        requested > P2000T_MAX_HORIZONTAL_OFFSET ||
        !p2000t_capture_set_horizontal_offset((unsigned)requested)) {
        printf("Horizontal start must remain between %u and %u source dots.\n",
               P2000T_MIN_HORIZONTAL_OFFSET, P2000T_MAX_HORIZONTAL_OFFSET);
        return;
    }
    printf("Horizontal start set to %d source dots (%d characters); "
           "applies on the next frame.\n",
           requested, requested / P2000T_HORIZONTAL_OFFSET_STEP);
}

/**
 * @brief Execute one character received from the USB command interface.
 *
 * Unknown characters are ignored deliberately so terminal line endings do
 * not produce noise.
 *
 * @param command Character code returned by the Pico stdio layer.
 */
static void handle_usb_command(int command) {
    switch (command) {
    case '1':
        select_no_signal_artwork(P2000T_NO_SIGNAL_GREEN_PHOSPHOR);
        break;
    case '2':
        select_no_signal_artwork(P2000T_NO_SIGNAL_SYNTHWAVE);
        break;
    case '3':
        select_no_signal_artwork(P2000T_NO_SIGNAL_AMBER_CIRCUIT);
        break;
    case 's':
    case 'S':
        print_status();
        break;
    case '[':
    case '-':
        adjust_first_visible_line(-1);
        break;
    case ']':
    case '+':
        adjust_first_visible_line(1);
        break;
    case '0':
        if (p2000t_capture_set_first_visible_scanline(
                P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE)) {
            printf("First visible source scanline reset to %u.\n",
                   P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE);
        }
        break;
    case ',':
        adjust_sample_phase(-1);
        break;
    case '.':
        adjust_sample_phase(1);
        break;
    case 'p':
    case 'P':
        if (p2000t_capture_set_sample_phase(P2000T_DEFAULT_SAMPLE_PHASE)) {
            printf("Sample phase reset to %d.\n", P2000T_DEFAULT_SAMPLE_PHASE);
        }
        break;
    case '<':
        adjust_horizontal_offset(-P2000T_HORIZONTAL_OFFSET_STEP);
        break;
    case '>':
        adjust_horizontal_offset(P2000T_HORIZONTAL_OFFSET_STEP);
        break;
    case 'x':
    case 'X':
        if (p2000t_capture_set_horizontal_offset(
                P2000T_DEFAULT_HORIZONTAL_OFFSET)) {
            printf("Horizontal start reset to %u source dots.\n",
                   P2000T_DEFAULT_HORIZONTAL_OFFSET);
        }
        break;
    case 'h':
    case 'H':
    case '?':
        print_help();
        break;
    default:
        break;
    }
}

/** @brief Drain and execute all currently buffered USB commands. */
static void poll_usb_commands(void) {
    int command;
    while ((command = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        handle_usb_command(command);
    }
}

/** @brief Print the connection banner, hardware mapping, status, and help. */
static void announce_usb_connection(void) {
    printf("\nP2000T VID2VGA firmware v%s -- %s\n", P2000T_VID2VGA_VERSION,
           P2000T_PROCESSOR_NAME);
    printf("Input: CSYNC=GP%u R=GP%u G=GP%u B=GP%u; "
           "VGA: RGB=GP0-GP11 HSYNC=GP12 VSYNC=GP13\n",
           P2000T_SYNC_PIN, P2000T_RED_PIN, P2000T_GREEN_PIN, P2000T_BLUE_PIN);
#if P2000T_PROTOTYPE_V1_MIRRORED_DIN
    printf("Input profile: prototype v1 mirrored DIN contacts 1/5 and 2/4\n");
#else
    printf("Input profile: corrected PCB v2 DIN mapping\n");
#endif
    printf("Display: 480x240 raw RGBS capture, vertically doubled "
           "to centered 480x480 VGA\n");
    printf("EXPERIMENTAL clock=%uMHz core_voltage=%u.%03uV; "
           "capture=12MHz raw VGA=25.2MHz\n",
           SYSTEM_CLOCK_KHZ / 1000u, SYSTEM_CORE_VOLTAGE_MV / 1000u,
           SYSTEM_CORE_VOLTAGE_MV % 1000u);
    print_status();
    print_help();
}

/** @brief Configure the voltage and exact 252 MHz system clock. */
static void configure_system_clock(void) {
    /* The clock is a common multiple of the 6 MHz source and the 25.2 MHz
       VGA pixel clock. Both supported boards currently use this overclock. */
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_us(1000u);
    if (!set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true)) {
        panic("Unable to set the experimental 252 MHz system clock");
    }
}

/**
 * @brief Initialize capture and VGA, then service the USB control interface.
 *
 * @return This firmware entry point does not return during normal operation.
 */
int main(void) {
    configure_system_clock();
    stdio_init_all();
    p2000t_video_renderer_initialize();

    /* Scanvideo owns PIO0 and its fixed DMA channel. Capture then claims PIO1
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
            announce_usb_connection();
            announced = true;
        }
        poll_usb_commands();
        sleep_ms(10);
    }
}
