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
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/vreg.h"
#include "p2000t_capture.h"
#include "p2000t_control_protocol.h"
#include "p2000t_settings.h"
#include "p2000t_video_renderer.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#if defined(PICO_RP2350) && PICO_RP2350
#include "p2000t_diagnostic_protocol.h"
#include "p2000t_diagnostics.h"
#include "p2000t_stream_protocol.h"
#include "p2000t_usb_stream.h"
#endif

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
    SYSTEM_CLOCK_KHZ = 252000,        /**< Overclocked system frequency. */
    SYSTEM_CORE_VOLTAGE_MV = 1300,    /**< Core voltage used at 252 MHz. */
    VGA_READY_MAGIC = 0x56474154,     /**< Core-1 VGA-ready FIFO token. */
    STATUS_LED_MAX_LEVEL = 255,       /**< Maximum logical brightness. */
    STATUS_LED_PWM_WRAP = 65535,      /**< Full 16-bit PWM brightness range. */
    STATUS_LED_PWM_CLOCK_DIVIDER = 8, /**< About 480 Hz at 252 MHz. */
    STATUS_LED_SERVICE_INTERVAL_US = 5000,  /**< 200 Hz LED update rate. */
    STATUS_LED_ACTIVE_INTERVAL_US = 500000, /**< Connected on/off interval. */
    STATUS_LED_SEEK_CYCLE_US = 2500000, /**< Complete seeking breath cycle. */
    STATUS_LED_SEEK_HALF_CYCLE_US = STATUS_LED_SEEK_CYCLE_US / 2,
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
_Static_assert((int)P2000T_MIN_ODD_LINE_PHASE ==
                       (int)P2000T_CONTROL_MIN_ODD_LINE_PHASE &&
                   (int)P2000T_MAX_ODD_LINE_PHASE ==
                       (int)P2000T_CONTROL_MAX_ODD_LINE_PHASE &&
                   (int)P2000T_DEFAULT_ODD_LINE_PHASE ==
                       (int)P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE,
               "Capture and control odd-line phase ranges must match");
_Static_assert((int)P2000T_MIN_SAMPLE_RATE_TRIM ==
                       (int)P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM &&
                   (int)P2000T_MAX_SAMPLE_RATE_TRIM ==
                       (int)P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM &&
                   (int)P2000T_DEFAULT_SAMPLE_RATE_TRIM ==
                       (int)P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM,
               "Capture and control sample-rate trim ranges must match");
#if defined(PICO_RP2350) && PICO_RP2350
_Static_assert((unsigned)P2000T_NO_SIGNAL_GREEN_PHOSPHOR ==
                       (unsigned)P2000T_STREAM_ARTWORK_GREEN_PHOSPHOR &&
                   (unsigned)P2000T_NO_SIGNAL_SYNTHWAVE ==
                       (unsigned)P2000T_STREAM_ARTWORK_SYNTHWAVE &&
                   (unsigned)P2000T_NO_SIGNAL_AMBER_CIRCUIT ==
                       (unsigned)P2000T_STREAM_ARTWORK_AMBER_CIRCUIT &&
                   (unsigned)P2000T_NO_SIGNAL_ARTWORK_COUNT ==
                       (unsigned)P2000T_STREAM_ARTWORK_COUNT,
               "USB artwork identifiers must match the VGA renderer");
#endif

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
    int buffer_index;      /**< Currently displayed capture buffer. */
    const uint32_t *frame; /**< Packed frame currently being read. */
    bool signal_present;   /**< Signal decision fixed per VGA frame. */
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
    volatile uint32_t
        signal_present; /**< Signal state used by current VGA frame. */
    volatile uint32_t
        no_signal_artwork; /**< Artwork used by current VGA frame. */
} vga_statistics_t;

/** VGA-core-owned display state, initialized without a captured frame. */
static vga_display_state_t display_state = {
    .buffer_index = -1,
    .no_signal_artwork = P2000T_NO_SIGNAL_ARTWORK_DEFAULT,
};

/** Diagnostics shared atomically between the VGA and control cores. */
static vga_statistics_t vga_statistics = {
    .no_signal_artwork = P2000T_NO_SIGNAL_ARTWORK_DEFAULT,
};

/** USB-selected artwork adopted by the VGA core at its next frame boundary. */
static volatile uint32_t requested_no_signal_artwork =
    P2000T_NO_SIGNAL_ARTWORK_DEFAULT;

/** Active and last successfully persisted user configuration. */
static p2000t_settings_t current_settings;
static p2000t_settings_t stored_settings;
static unsigned stored_reconstruction_mode;
static bool stored_settings_valid;
static bool settings_save_failed;
/** Live reconstruction is stored separately from the legacy packed bit. */
static unsigned current_reconstruction_mode =
    P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION;

#if defined(PICO_RP2350) && PICO_RP2350
enum {
    FIRMWARE_DEFAULT_SAMPLE_PHASE = P2000T_CONTROL_PICO2_DEFAULT_PHASE,
    FIRMWARE_DEFAULT_ODD_LINE_PHASE =
        P2000T_CONTROL_PICO2_DEFAULT_ODD_LINE_PHASE,
};
#else
enum {
    FIRMWARE_DEFAULT_SAMPLE_PHASE = P2000T_CONTROL_DEFAULT_PHASE,
    FIRMWARE_DEFAULT_ODD_LINE_PHASE = P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE,
};
#endif

static const char *sample_reconstruction_name(unsigned reconstruction);

/** On-board LED state, owned exclusively by the control core. */
static uint64_t status_led_state_started_us;
static uint64_t status_led_next_service_us;
static unsigned status_led_previous_level = STATUS_LED_MAX_LEVEL + 1u;
static bool status_led_previous_signal_present;
static bool status_led_state_initialized;

/** Incremental structured-control packet receiver. */
static uint8_t control_packet[P2000T_CONTROL_PACKET_SIZE];
static unsigned control_packet_offset;

_Static_assert(
    (int)P2000T_CONTROL_MIN_VERTICAL ==
            (int)P2000T_MIN_FIRST_VISIBLE_SCANLINE &&
        (int)P2000T_CONTROL_MAX_VERTICAL ==
            (int)P2000T_MAX_FIRST_VISIBLE_SCANLINE &&
        (int)P2000T_CONTROL_MIN_PHASE == (int)P2000T_MIN_SAMPLE_PHASE &&
        (int)P2000T_CONTROL_MAX_PHASE == (int)P2000T_MAX_SAMPLE_PHASE &&
        (int)P2000T_CONTROL_MIN_HORIZONTAL ==
            (int)P2000T_MIN_HORIZONTAL_OFFSET &&
        (int)P2000T_CONTROL_MAX_HORIZONTAL == (int)P2000T_MAX_HORIZONTAL_OFFSET,
    "Control protocol limits must match capture limits");

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

/** @brief Set the on-board LED PWM level, respecting active-low boards. */
static void status_led_set_level(unsigned level) {
    hard_assert(level <= STATUS_LED_MAX_LEVEL);
    if (level == status_led_previous_level) {
        return;
    }
    status_led_previous_level = level;
#if defined(PICO_DEFAULT_LED_PIN_INVERTED) && PICO_DEFAULT_LED_PIN_INVERTED
    level = STATUS_LED_MAX_LEVEL - level;
#endif
    pwm_set_gpio_level(
        PICO_DEFAULT_LED_PIN,
        (uint16_t)(level * (STATUS_LED_PWM_WRAP / STATUS_LED_MAX_LEVEL)));
}

/** @brief Configure low-frequency hardware PWM for the on-board LED. */
static void status_led_initialize(void) {
    gpio_set_function(PICO_DEFAULT_LED_PIN, GPIO_FUNC_PWM);
    const unsigned slice = pwm_gpio_to_slice_num(PICO_DEFAULT_LED_PIN);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&config, STATUS_LED_PWM_CLOCK_DIVIDER);
    pwm_config_set_wrap(&config, STATUS_LED_PWM_WRAP);
    pwm_init(slice, &config, true);
    status_led_set_level(0u);
    status_led_next_service_us = time_us_64();
}

/**
 * @brief Update the on-board LED for seeking or active capture.
 *
 * A credible P2000T signal produces a 0.5-second on/off activity blink. While
 * seeking, a gamma-shaped 2.5-second triangle wave controls a quiet 480 Hz
 * hardware PWM carrier. Brightness calculations remain capped at 200 Hz,
 * independently of the much faster USB streaming loop.
 *
 * @param signal_present Whether the VGA core has adopted a valid input signal.
 */
static void status_led_service(bool signal_present) {
    const uint64_t now_us = time_us_64();
    if (now_us < status_led_next_service_us) {
        return;
    }
    status_led_next_service_us = now_us + STATUS_LED_SERVICE_INTERVAL_US;

    if (!status_led_state_initialized ||
        signal_present != status_led_previous_signal_present) {
        status_led_state_initialized = true;
        status_led_previous_signal_present = signal_present;
        status_led_state_started_us = now_us;
    }

    const uint32_t elapsed_us =
        (uint32_t)(now_us - status_led_state_started_us);
    unsigned level;
    if (signal_present) {
        level = ((elapsed_us / STATUS_LED_ACTIVE_INTERVAL_US) & 1u) == 0u
                    ? STATUS_LED_MAX_LEVEL
                    : 0u;
    } else {
        const uint32_t phase_us = elapsed_us % STATUS_LED_SEEK_CYCLE_US;
        const uint32_t ramp_us = phase_us <= STATUS_LED_SEEK_HALF_CYCLE_US
                                     ? phase_us
                                     : STATUS_LED_SEEK_CYCLE_US - phase_us;
        const unsigned linear_level =
            (ramp_us * STATUS_LED_MAX_LEVEL) / STATUS_LED_SEEK_HALF_CYCLE_US;
        level = (linear_level * linear_level + STATUS_LED_MAX_LEVEL - 1u) /
                STATUS_LED_MAX_LEVEL;
    }

    status_led_set_level(level);
}

/**
 * @brief Adopt the newest complete source frame at a VGA frame boundary.
 *
 * The prior frame remains displayed when capture has not completed a newer
 * frame. A claimed buffer is released only after its replacement is active.
 */
static void select_frame_for_next_vga_frame(void) {
    increment_counter(&vga_statistics.generated_frames);
    p2000t_video_renderer_begin_frame();
    display_state.no_signal_artwork =
        (p2000t_no_signal_artwork_t)load_statistic(
            &requested_no_signal_artwork);
    display_state.signal_present = p2000t_capture_signal_present();
    store_statistic(&vga_statistics.signal_present,
                    display_state.signal_present ? 1u : 0u);
    store_statistic(&vga_statistics.no_signal_artwork,
                    (uint32_t)display_state.no_signal_artwork);

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
        p2000t_video_render_no_signal_scanline(scanline_buffer, y,
                                               display_state.no_signal_artwork);
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
    hard_assert(flash_safe_execute_core_init());
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
    const uint32_t artwork = load_statistic(&vga_statistics.no_signal_artwork);

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
           " odd_phase=%" PRId32 " rate_trim=%" PRId32 " reconstruction=%s"
           " stale=%" PRIu32 " vga=%" PRIu32 " swaps=%" PRIu32
           " repeats=%" PRIu32 " lost=%" PRIu32 " id_gaps=%" PRIu32
           " displayed=%" PRIu32 " artwork=%" PRIu32 "\n",
           capture.first_visible_scanline, capture.horizontal_offset,
           capture.sample_phase, capture.odd_line_phase,
           capture.sample_rate_trim,
           sample_reconstruction_name(current_reconstruction_mode),
           capture.stale_frames_replaced, vga_frames, swaps, repeats, lost,
           id_gaps, sequence, artwork + 1u);
#if defined(PICO_RP2350) && PICO_RP2350
    printf(
        "Capture engine=%s pending=%s window_frames=%" PRIu32
        " corrected=%" PRIu32 " ambiguous=%" PRIu32 " rgb=%" PRIu32 "/%" PRIu32
        "/%" PRIu32 " line_deadline_misses=%" PRIu32 "\n",
        capture.capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED ? "windowed-3x"
                                                                 : "two-tap",
        capture.engine_switch_pending ? "yes" : "no", capture.windowed_frames,
        capture.last_corrected_samples, capture.last_ambiguous_samples,
        capture.last_red_corrections, capture.last_green_corrections,
        capture.last_blue_corrections, capture.line_deadline_misses);
#endif
#if defined(PICO_RP2350) && PICO_RP2350
    p2000t_usb_stream_stats_t stream;
    p2000t_usb_stream_get_stats(&stream);
    printf("USB screen frames=%" PRIu32 " no_signal=%" PRIu32 " bytes=%llu"
           " raw=%" PRIu32 " packbits=%" PRIu32 " payload=%" PRIu32
           " prepare=%" PRIu32 "/%" PRIu32 "us encode=%" PRIu32 "/%" PRIu32
           "us tx=%" PRIu32 "/%" PRIu32 "us skipped=%" PRIu32 "\n",
           stream.frames_sent, stream.no_signal_records_sent,
           (unsigned long long)stream.bytes_sent, stream.raw_frames_sent,
           stream.packbits_frames_sent, stream.last_payload_size,
           stream.last_prepare_us, stream.maximum_prepare_us,
           stream.last_encode_us, stream.maximum_encode_us, stream.last_tx_us,
           stream.maximum_tx_us, stream.skipped_sequences);
#endif
}

/** @brief Print the available single-character USB commands. */
static void print_help(void) {
    printf("Commands: s=status, [=image up, ]=image down, "
           "0=reset line, ,=sample earlier, .=sample later, "
           "p=reset phase, ;=odd lines earlier, '=odd lines later, "
           "o=reset odd-line phase, {=narrower, }=wider, w=reset width, "
           "d=toggle source-dot reconstruction, "
           "<=start earlier, >=start later, "
           "x=reset start, h=help\n");
    printf("No-connection artwork: 1=green phosphor, 2=synthwave, "
           "3=amber circuit\n");
#if defined(PICO_RP2350) && PICO_RP2350
    printf("Pico 2 screen capture: c=PackBits stream, r=raw stream, "
           "q=return to console\n");
    printf("Pico 2 high-resolution diagnostics are controlled by the "
           "desktop viewer.\n");
#endif
}

#if defined(PICO_RP2350) && PICO_RP2350
/** Enter the continuous binary RGB111 screen interface. */
static void enter_screen_mode(bool allow_packbits) {
    printf("SCREEN mode=binary version=%u width=%u height=%u fps=25 "
           "header=%u payload=%u format=planar-rgb111 encoding=%s "
           "flow=continuous exit=q\n",
           P2000T_STREAM_PROTOCOL_VERSION, P2000T_STREAM_WIDTH,
           P2000T_STREAM_HEIGHT, P2000T_STREAM_HEADER_SIZE,
           P2000T_STREAM_PAYLOAD_SIZE, allow_packbits ? "packbits+raw" : "raw");
    stdio_flush();
    p2000t_usb_stream_start(allow_packbits);
}
#endif

/**
 * @brief Request a no-connection artwork change at the next VGA frame.
 *
 * @param artwork Valid embedded artwork selection.
 * @param quiet Suppress console text while a binary stream is active.
 */
static void select_no_signal_artwork(p2000t_no_signal_artwork_t artwork,
                                     bool quiet) {
    hard_assert((unsigned)artwork < P2000T_NO_SIGNAL_ARTWORK_COUNT);
    store_statistic(&requested_no_signal_artwork, (uint32_t)artwork);
    p2000t_settings_set_artwork(&current_settings, (unsigned)artwork);
    static const char *const names[P2000T_NO_SIGNAL_ARTWORK_COUNT] = {
        "green phosphor", "synthwave", "amber circuit"};
    if (!quiet) {
        printf("No-connection artwork %u selected: %s. "
               "Applies at the next VGA frame.\n",
               (unsigned)artwork + 1u, names[artwork]);
    }
}

/** Return a stable machine-readable name for one reconstruction mode. */
static const char *sample_reconstruction_name(unsigned reconstruction) {
    static const char *const names[P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT] =
        {
            "raw",
            "guarded-duplicate",
            "sharp-guarded",
            "window-center",
            "window-channel",
            "window-color-early",
            "window-color-late",
        };
    return reconstruction < P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT
               ? names[reconstruction]
               : "invalid";
}

/** Apply one validated live reconstruction mode. */
static void select_sample_reconstruction(unsigned reconstruction, bool quiet) {
    hard_assert(reconstruction < P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT);
    if (!p2000t_capture_set_reconstruction_mode(reconstruction)) {
        if (!quiet) {
            printf("Reconstruction %s is unavailable on this processor.\n",
                   sample_reconstruction_name(reconstruction));
        }
        return;
    }
    current_reconstruction_mode = reconstruction;
    /* Retain the legacy packed bit for backward-compatible state payloads.
       The v2 flash record stores the complete live mode independently. */
    if (reconstruction <= P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP) {
        p2000t_settings_set_sample_reconstruction(&current_settings,
                                                  reconstruction);
    }
    p2000t_video_renderer_set_reconstruction(reconstruction);
#if defined(PICO_RP2350) && PICO_RP2350
    p2000t_usb_stream_set_reconstruction(reconstruction);
#endif
    if (!quiet) {
        printf("Source reconstruction set to %s; applies at the next source "
               "frame boundary.\n",
               sample_reconstruction_name(reconstruction));
    }
}

/**
 * @brief Adjust the first captured source line by a signed amount.
 *
 * @param change Signed number of source scanlines to add.
 * @param quiet Suppress console text while a binary stream is active.
 */
static void adjust_first_visible_line(int change, bool quiet) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = (int)capture.first_visible_scanline + change;
    if (!p2000t_capture_set_first_visible_scanline((unsigned)requested)) {
        if (!quiet) {
            printf("First visible line must remain between %u and %u.\n",
                   P2000T_MIN_FIRST_VISIBLE_SCANLINE,
                   P2000T_MAX_FIRST_VISIBLE_SCANLINE);
        }
        return;
    }
    current_settings.first_visible_scanline = (uint16_t)requested;
    if (!quiet) {
        printf("First visible source scanline set to %d; "
               "applies on the next frame.\n",
               requested);
    }
}

/**
 * @brief Adjust the fine capture phase by a signed number of PIO ticks.
 *
 * @param change Signed number of 7.94 ns capture ticks to add.
 * @param quiet Suppress console text while a binary stream is active.
 */
static void adjust_sample_phase(int change, bool quiet) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = capture.sample_phase + change;
    if (!p2000t_capture_set_sample_phase(requested)) {
        if (!quiet) {
            printf("Sample phase must remain between %d and %d.\n",
                   P2000T_MIN_SAMPLE_PHASE, P2000T_MAX_SAMPLE_PHASE);
        }
        return;
    }
    current_settings.sample_phase = (int16_t)requested;
    if (!quiet) {
        printf("Sample phase set to %+d (positive is later); "
               "one tick is nominally 7.94 ns and it applies on the next "
               "source frame.\n",
               requested);
    }
}

/**
 * @brief Adjust odd-numbered source lines relative to even-numbered lines.
 *
 * @param change Signed number of 7.94 ns capture ticks to add.
 * @param quiet Suppress console text while a binary stream is active.
 */
static void adjust_odd_line_phase(int change, bool quiet) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = capture.odd_line_phase + change;
    if (!p2000t_capture_set_odd_line_phase(requested)) {
        if (!quiet) {
            printf("Odd-line phase must remain between %d and %d.\n",
                   P2000T_MIN_ODD_LINE_PHASE, P2000T_MAX_ODD_LINE_PHASE);
        }
        return;
    }
    current_settings.odd_line_phase = (int8_t)requested;
    if (!quiet) {
        printf(
            "Odd-line phase set to %+d (positive is later); "
            "one tick is nominally 7.94 ns and it applies on the next source "
            "frame.\n",
            requested);
    }
}

/**
 * @brief Adjust the complete horizontal sampling interval.
 *
 * @param change Signed number of 1/256 PIO-divider steps to add.
 * @param quiet Suppress console text while a binary stream is active.
 */
static void adjust_sample_rate_trim(int change, bool quiet) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = capture.sample_rate_trim + change;
    if (!p2000t_capture_set_sample_rate_trim(requested)) {
        if (!quiet) {
            printf("Horizontal rate trim must remain between %d and %d.\n",
                   P2000T_MIN_SAMPLE_RATE_TRIM, P2000T_MAX_SAMPLE_RATE_TRIM);
        }
        return;
    }
    p2000t_settings_set_sample_rate_trim(&current_settings, requested);
    if (!quiet) {
        printf("Horizontal rate trim set to %+d (positive is wider); "
               "one step moves the right edge by about 0.94 sample and "
               "applies on the next source frame.\n",
               requested);
    }
}

/**
 * @brief Adjust the coarse horizontal capture start.
 *
 * @param change Signed number of nominal source dots to add.
 * @param quiet Suppress console text while a binary stream is active.
 */
static void adjust_horizontal_offset(int change, bool quiet) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    const int requested = (int)capture.horizontal_offset + change;
    if (requested < P2000T_MIN_HORIZONTAL_OFFSET ||
        requested > P2000T_MAX_HORIZONTAL_OFFSET ||
        !p2000t_capture_set_horizontal_offset((unsigned)requested)) {
        if (!quiet) {
            printf("Horizontal start must remain between %u and %u "
                   "source dots.\n",
                   P2000T_MIN_HORIZONTAL_OFFSET, P2000T_MAX_HORIZONTAL_OFFSET);
        }
        return;
    }
    current_settings.horizontal_offset = (uint16_t)requested;
    if (!quiet) {
        printf("Horizontal start set to %d source dots (%d characters); "
               "applies on the next frame.\n",
               requested, requested / P2000T_HORIZONTAL_OFFSET_STEP);
    }
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
#if defined(PICO_RP2350) && PICO_RP2350
    const bool quiet =
        p2000t_usb_stream_active() || p2000t_diagnostics_active();
#else
    const bool quiet = false;
#endif
    switch (command) {
    case '1':
        select_no_signal_artwork(P2000T_NO_SIGNAL_GREEN_PHOSPHOR, quiet);
        break;
    case '2':
        select_no_signal_artwork(P2000T_NO_SIGNAL_SYNTHWAVE, quiet);
        break;
    case '3':
        select_no_signal_artwork(P2000T_NO_SIGNAL_AMBER_CIRCUIT, quiet);
        break;
#if defined(PICO_RP2350) && PICO_RP2350
    case 'c':
    case 'C':
        enter_screen_mode(true);
        break;
    case 'r':
    case 'R':
        enter_screen_mode(false);
        break;
    case 'q':
    case 'Q':
        if (p2000t_diagnostics_active()) {
            p2000t_diagnostics_cancel();
        } else if (p2000t_usb_stream_active()) {
            p2000t_usb_stream_stop();
            printf("\nUSB screen stream stopped.\n");
        }
        break;
#endif
    case 's':
    case 'S':
        print_status();
        break;
    case '[':
    case '-':
        adjust_first_visible_line(-1, quiet);
        break;
    case ']':
    case '+':
        adjust_first_visible_line(1, quiet);
        break;
    case '0':
        if (p2000t_capture_set_first_visible_scanline(
                P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE)) {
            current_settings.first_visible_scanline =
                P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE;
            if (!quiet) {
                printf("First visible source scanline reset to %u.\n",
                       P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE);
            }
        }
        break;
    case ',':
        adjust_sample_phase(-1, quiet);
        break;
    case '.':
        adjust_sample_phase(1, quiet);
        break;
    case 'p':
    case 'P':
        if (p2000t_capture_set_sample_phase(FIRMWARE_DEFAULT_SAMPLE_PHASE)) {
            current_settings.sample_phase = FIRMWARE_DEFAULT_SAMPLE_PHASE;
            if (!quiet) {
                printf("Sample phase reset to %d.\n",
                       FIRMWARE_DEFAULT_SAMPLE_PHASE);
            }
        }
        break;
    case ';':
        adjust_odd_line_phase(-1, quiet);
        break;
    case '\'':
        adjust_odd_line_phase(1, quiet);
        break;
    case 'o':
    case 'O':
        if (p2000t_capture_set_odd_line_phase(
                FIRMWARE_DEFAULT_ODD_LINE_PHASE)) {
            current_settings.odd_line_phase = FIRMWARE_DEFAULT_ODD_LINE_PHASE;
            if (!quiet) {
                printf("Odd-line phase reset to %d.\n",
                       FIRMWARE_DEFAULT_ODD_LINE_PHASE);
            }
        }
        break;
    case '{':
        adjust_sample_rate_trim(-1, quiet);
        break;
    case '}':
        adjust_sample_rate_trim(1, quiet);
        break;
    case 'w':
    case 'W':
        if (p2000t_capture_set_sample_rate_trim(
                P2000T_DEFAULT_SAMPLE_RATE_TRIM)) {
            p2000t_settings_set_sample_rate_trim(
                &current_settings, P2000T_DEFAULT_SAMPLE_RATE_TRIM);
            if (!quiet) {
                printf("Horizontal rate trim reset to %d.\n",
                       P2000T_DEFAULT_SAMPLE_RATE_TRIM);
            }
        }
        break;
    case 'd':
    case 'D':
        select_sample_reconstruction(
            current_reconstruction_mode ==
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW
                ? P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP
                : P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW,
            quiet);
        break;
    case '<':
        adjust_horizontal_offset(-P2000T_HORIZONTAL_OFFSET_STEP, quiet);
        break;
    case '>':
        adjust_horizontal_offset(P2000T_HORIZONTAL_OFFSET_STEP, quiet);
        break;
    case 'x':
    case 'X':
        if (p2000t_capture_set_horizontal_offset(
                P2000T_DEFAULT_HORIZONTAL_OFFSET)) {
            current_settings.horizontal_offset =
                P2000T_DEFAULT_HORIZONTAL_OFFSET;
            if (!quiet) {
                printf("Horizontal start reset to %u source dots.\n",
                       P2000T_DEFAULT_HORIZONTAL_OFFSET);
            }
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

static uint16_t control_crc16(const uint8_t *data, unsigned length) {
    uint16_t crc = 0xffffu;
    for (unsigned index = 0; index < length; ++index) {
        crc ^= (uint16_t)data[index] << 8u;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (uint16_t)((crc << 1u) ^
                             ((crc & 0x8000u) != 0u ? 0x1021u : 0u));
        }
    }
    return crc;
}

static uint16_t control_load_u16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t control_load_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

#if defined(PICO_RP2350) && PICO_RP2350
static void control_store_u16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void control_store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void queue_configuration_state(void) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    uint8_t payload[P2000T_CONFIGURATION_STATE_SIZE] = {0};
    memcpy(payload, P2000T_CONFIGURATION_STATE_MAGIC, 4u);
    payload[4] = P2000T_CONFIGURATION_STATE_VERSION;
    if (stored_settings_valid) {
        payload[5] |= P2000T_CONFIGURATION_FLAG_STORED_AVAILABLE;
        if (memcmp(&current_settings, &stored_settings,
                   sizeof(current_settings)) == 0 &&
            current_reconstruction_mode == stored_reconstruction_mode) {
            payload[5] |= P2000T_CONFIGURATION_FLAG_MATCHES_STORED;
        }
    }
    if (settings_save_failed) {
        payload[5] |= P2000T_CONFIGURATION_FLAG_SAVE_FAILED;
    }
    control_store_u16(&payload[6], sizeof(payload));
    control_store_u16(&payload[8], current_settings.first_visible_scanline);
    control_store_u16(&payload[10], (uint16_t)current_settings.sample_phase);
    control_store_u16(&payload[12], current_settings.horizontal_offset);
    payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] =
        current_settings.capture_options;
    payload[P2000T_CONFIGURATION_ODD_LINE_PHASE_OFFSET] =
        (uint8_t)current_settings.odd_line_phase;
    for (unsigned index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        control_store_u16(
            &payload[P2000T_CONFIGURATION_PALETTE_OFFSET + index * 2u],
            current_settings.palette[index]);
    }
    payload[P2000T_CONFIGURATION_RUNTIME_RECONSTRUCTION_OFFSET] =
        (uint8_t)current_reconstruction_mode;
    payload[P2000T_CONFIGURATION_CAPTURE_ENGINE_OFFSET] =
        capture.capture_engine;
    payload[P2000T_CONFIGURATION_WINDOW_SAMPLES_OFFSET] =
        capture.window_samples;
    if (capture.window_supported) {
        payload[P2000T_CONFIGURATION_CAPTURE_FLAGS_OFFSET] |=
            P2000T_CONFIGURATION_CAPTURE_FLAG_WINDOW_SUPPORTED;
    }
    if (capture.engine_switch_pending) {
        payload[P2000T_CONFIGURATION_CAPTURE_FLAGS_OFFSET] |=
            P2000T_CONFIGURATION_CAPTURE_FLAG_ENGINE_SWITCH_PENDING;
    }
    control_store_u32(&payload[P2000T_CONFIGURATION_WINDOW_FRAMES_OFFSET],
                      capture.windowed_frames);
    control_store_u32(&payload[P2000T_CONFIGURATION_LAST_CORRECTED_OFFSET],
                      capture.last_corrected_samples);
    control_store_u32(&payload[P2000T_CONFIGURATION_LAST_AMBIGUOUS_OFFSET],
                      capture.last_ambiguous_samples);
    control_store_u32(&payload[P2000T_CONFIGURATION_LAST_RED_CORRECTED_OFFSET],
                      capture.last_red_corrections);
    control_store_u32(
        &payload[P2000T_CONFIGURATION_LAST_GREEN_CORRECTED_OFFSET],
        capture.last_green_corrections);
    control_store_u32(&payload[P2000T_CONFIGURATION_LAST_BLUE_CORRECTED_OFFSET],
                      capture.last_blue_corrections);
    control_store_u32(
        &payload[P2000T_CONFIGURATION_LINE_DEADLINE_MISSES_OFFSET],
        capture.line_deadline_misses);
    p2000t_usb_stream_queue_configuration(payload, sizeof(payload));
}
#endif

static void apply_settings(const p2000t_settings_t *settings,
                           unsigned reconstruction) {
    hard_assert(p2000t_settings_valid(settings));
    hard_assert(reconstruction < P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT);
    current_settings = *settings;
    p2000t_capture_set_first_visible_scanline(
        current_settings.first_visible_scanline);
    p2000t_capture_set_sample_phase(current_settings.sample_phase);
    p2000t_capture_set_odd_line_phase(current_settings.odd_line_phase);
    p2000t_capture_set_sample_rate_trim(
        p2000t_settings_sample_rate_trim(&current_settings));
    p2000t_capture_set_horizontal_offset(current_settings.horizontal_offset);
    select_sample_reconstruction(reconstruction, true);
    store_statistic(&requested_no_signal_artwork,
                    p2000t_settings_artwork(&current_settings));
    p2000t_video_renderer_set_source_palette(current_settings.palette);
}

static void handle_control_packet(void) {
    const uint8_t opcode = control_packet[3];
    const uint8_t argument = control_packet[4];
    const uint32_t value = control_load_u32(&control_packet[6]);
    switch (opcode) {
    case P2000T_CONTROL_GET_SETTINGS:
        break;
    case P2000T_CONTROL_SET_VERTICAL:
        if (value >= P2000T_CONTROL_MIN_VERTICAL &&
            value <= P2000T_CONTROL_MAX_VERTICAL &&
            p2000t_capture_set_first_visible_scanline((unsigned)value)) {
            current_settings.first_visible_scanline = (uint16_t)value;
        }
        break;
    case P2000T_CONTROL_SET_PHASE: {
        const int32_t phase = (int32_t)value;
        if (phase >= P2000T_CONTROL_MIN_PHASE &&
            phase <= P2000T_CONTROL_MAX_PHASE &&
            p2000t_capture_set_sample_phase((int)phase)) {
            current_settings.sample_phase = (int16_t)phase;
        }
        break;
    }
    case P2000T_CONTROL_SET_ODD_LINE_PHASE: {
        const int32_t phase = (int32_t)value;
        if (phase >= P2000T_CONTROL_MIN_ODD_LINE_PHASE &&
            phase <= P2000T_CONTROL_MAX_ODD_LINE_PHASE &&
            p2000t_capture_set_odd_line_phase((int)phase)) {
            current_settings.odd_line_phase = (int8_t)phase;
        }
        break;
    }
    case P2000T_CONTROL_SET_SAMPLE_RATE_TRIM: {
        const int32_t trim = (int32_t)value;
        if (trim >= P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM &&
            trim <= P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM &&
            p2000t_capture_set_sample_rate_trim((int)trim)) {
            p2000t_settings_set_sample_rate_trim(&current_settings, (int)trim);
        }
        break;
    }
    case P2000T_CONTROL_SET_SAMPLE_RECONSTRUCTION:
        if (value < P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT) {
            select_sample_reconstruction((unsigned)value, true);
        }
        break;
    case P2000T_CONTROL_SET_HORIZONTAL:
        if (value <= P2000T_CONTROL_MAX_HORIZONTAL &&
            value % P2000T_CONTROL_HORIZONTAL_STEP == 0u &&
            p2000t_capture_set_horizontal_offset((unsigned)value)) {
            current_settings.horizontal_offset = (uint16_t)value;
        }
        break;
    case P2000T_CONTROL_SET_ARTWORK:
        if (value < P2000T_NO_SIGNAL_ARTWORK_COUNT) {
            p2000t_settings_set_artwork(&current_settings, (unsigned)value);
            store_statistic(&requested_no_signal_artwork, value);
        }
        break;
    case P2000T_CONTROL_SET_PALETTE:
        if (argument < P2000T_CONTROL_PALETTE_COLORS &&
            value <= P2000T_CONTROL_RGB444_MAX) {
            current_settings.palette[argument] = (uint16_t)value;
            p2000t_video_renderer_set_source_palette(current_settings.palette);
        }
        break;
    case P2000T_CONTROL_SAVE_SETTINGS:
        settings_save_failed = !p2000t_settings_save(
            &current_settings, current_reconstruction_mode);
        p2000t_capture_resume_after_flash();
        if (!settings_save_failed) {
            stored_settings = current_settings;
            stored_reconstruction_mode = current_reconstruction_mode;
            stored_settings_valid = true;
        }
        break;
    case P2000T_CONTROL_LOAD_SETTINGS:
        if (stored_settings_valid) {
            apply_settings(&stored_settings, stored_reconstruction_mode);
        }
        break;
    case P2000T_CONTROL_FACTORY_DEFAULTS: {
        p2000t_settings_t defaults;
        unsigned default_reconstruction;
        p2000t_settings_defaults(&defaults, &default_reconstruction);
        apply_settings(&defaults, default_reconstruction);
        break;
    }
#if defined(PICO_RP2350) && PICO_RP2350
    case P2000T_CONTROL_START_DIAGNOSTICS: {
        const unsigned start_line = value & 0xffffu;
        const unsigned repetitions = value >> 16u;
        const unsigned line_count = argument;
        if (p2000t_diagnostics_active()) {
            break;
        }
        p2000t_usb_stream_stop();
        p2000t_diagnostics_start(start_line, line_count, repetitions);
        break;
    }
    case P2000T_CONTROL_CANCEL_DIAGNOSTICS:
        p2000t_diagnostics_cancel();
        break;
#endif
    default:
        return;
    }
#if defined(PICO_RP2350) && PICO_RP2350
    if (p2000t_usb_stream_active()) {
        queue_configuration_state();
    }
#endif
}

static bool consume_control_byte(uint8_t byte) {
    if (control_packet_offset == 0u) {
        if (byte != P2000T_CONTROL_MAGIC_0) {
            return false;
        }
        control_packet[control_packet_offset++] = byte;
        return true;
    }
    if (control_packet_offset == 1u && byte != P2000T_CONTROL_MAGIC_1) {
        control_packet_offset = byte == P2000T_CONTROL_MAGIC_0 ? 1u : 0u;
        return true;
    }
    control_packet[control_packet_offset++] = byte;
    if (control_packet_offset != P2000T_CONTROL_PACKET_SIZE) {
        return true;
    }
    control_packet_offset = 0u;
    if (control_packet[2] == P2000T_CONTROL_VERSION &&
        control_load_u16(&control_packet[P2000T_CONTROL_CRC_OFFSET]) ==
            control_crc16(control_packet, P2000T_CONTROL_CRC_OFFSET)) {
        handle_control_packet();
    }
    return true;
}

/** @brief Drain and execute all currently buffered USB commands. */
static void poll_usb_commands(void) {
    int command;
    while ((command = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (consume_control_byte((uint8_t)command)) {
            continue;
        }
#if defined(PICO_RP2350) && PICO_RP2350
        if (p2000t_diagnostics_active()) {
            if (command == 'q' || command == 'Q' || command == 0x1b) {
                p2000t_diagnostics_cancel();
            }
            continue;
        }
        if (p2000t_usb_stream_active()) {
            const bool stream_command =
                command == 'q' || command == 'Q' || command == 0x1b;
            const bool configuration_command =
                (command >= '1' && command <= '3') || command == '[' ||
                command == '-' || command == ']' || command == '+' ||
                command == '0' || command == ',' || command == '.' ||
                command == 'p' || command == 'P' || command == ';' ||
                command == '\'' || command == 'o' || command == 'O' ||
                command == '{' || command == '}' || command == 'w' ||
                command == 'W' || command == 'd' || command == 'D' ||
                command == '<' || command == '>' || command == 'x' ||
                command == 'X';
            if (!stream_command && !configuration_command) {
                continue;
            }
        }
        if (command == 0x1b) {
            command = 'q';
        }
#endif
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
#if defined(PICO_RP2350) && PICO_RP2350
    printf("USB capture: 480x240 planar RGB111, raw or lossless PackBits, "
           "25 FPS target\n");
    printf("Diagnostics: PIO2 63MHz CSYNC trace and 126MHz raw RGBS bursts\n");
#endif
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
    p2000t_settings_defaults(&current_settings, &current_reconstruction_mode);
    stored_settings_valid =
        p2000t_settings_load(&stored_settings, &stored_reconstruction_mode);
    if (stored_settings_valid) {
        current_settings = stored_settings;
        current_reconstruction_mode = stored_reconstruction_mode;
    }
    p2000t_video_renderer_initialize();
    p2000t_video_renderer_set_source_palette(current_settings.palette);
    select_sample_reconstruction(current_reconstruction_mode, true);

    /* Scanvideo owns PIO0 and its fixed DMA channel. Capture then claims PIO1
       and two otherwise-unused DMA channels. */
    if (!scanvideo_setup(&firmware_vga_mode)) {
        panic("Unable to initialize VGA scanvideo");
    }
    p2000t_capture_start();
    p2000t_capture_set_first_visible_scanline(
        current_settings.first_visible_scanline);
    p2000t_capture_set_sample_phase(current_settings.sample_phase);
    p2000t_capture_set_odd_line_phase(current_settings.odd_line_phase);
    p2000t_capture_set_sample_rate_trim(
        p2000t_settings_sample_rate_trim(&current_settings));
    p2000t_capture_set_horizontal_offset(current_settings.horizontal_offset);
    store_statistic(&requested_no_signal_artwork,
                    p2000t_settings_artwork(&current_settings));
    multicore_launch_core1(vga_core_main);
    if (multicore_fifo_pop_blocking() != VGA_READY_MAGIC) {
        panic("Unable to start VGA rendering core");
    }
    scanvideo_timing_enable(true);
    status_led_initialize();

    bool announced = false;
    while (true) {
        const bool signal_present =
            load_statistic(&vga_statistics.signal_present) != 0u;
        status_led_service(signal_present);
        if (!stdio_usb_connected()) {
            announced = false;
            sleep_us(STATUS_LED_SERVICE_INTERVAL_US);
            continue;
        }
        if (!announced) {
            sleep_ms(100);
            announce_usb_connection();
            announced = true;
        }
        poll_usb_commands();
#if defined(PICO_RP2350) && PICO_RP2350
        if (p2000t_diagnostics_active()) {
            p2000t_diagnostics_service();
            sleep_us(100u);
            continue;
        }
        if (p2000t_usb_stream_active()) {
            p2000t_usb_stream_service(
                signal_present,
                load_statistic(&vga_statistics.no_signal_artwork));
            sleep_us(100u);
            continue;
        }
#endif
        sleep_us(STATUS_LED_SERVICE_INTERVAL_US);
    }
}
