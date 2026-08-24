/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file signal_probe.c
 * @brief Passive USB diagnostic probe and scanline decoder for P2000T RGBS.
 *
 * The firmware is intentionally not a VGA converter yet. It verifies that a
 * soldered adapter delivers plausible P2000T timing and color signals, then
 * reconstructs one 240-pixel source line over USB. GPIO0 through GPIO13 remain
 * high-impedance.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "p2000t_sample.pio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#if !defined(PICO_RP2040) || !PICO_RP2040
#error "The P2000T signal probe must be built for an RP2040 Pico 1"
#endif

enum {
    SYSTEM_CLOCK_KHZ = 126000,

    INPUT_PIN_BASE = 16,
    INPUT_PIN_COUNT = 4,

#if P2000T_PROTOTYPE_V1_MIRRORED_DIN
    /* Prototype v1 mirrors DIN contacts 1/5 and 2/4. */
    SYNC_CHANNEL = 1,
    RED_CHANNEL = 0,
    GREEN_CHANNEL = 3,
    BLUE_CHANNEL = 2,
#else
    /* Intended connector mapping for the corrected PCB revision. */
    SYNC_CHANNEL = 0,
    RED_CHANNEL = 1,
    GREEN_CHANNEL = 2,
    BLUE_CHANNEL = 3,
#endif

    SYNC_PIN = INPUT_PIN_BASE + SYNC_CHANNEL,
    RED_PIN = INPUT_PIN_BASE + RED_CHANNEL,
    GREEN_PIN = INPUT_PIN_BASE + GREEN_CHANNEL,
    BLUE_PIN = INPUT_PIN_BASE + BLUE_CHANNEL,

    VGA_PIN_FIRST = 0,
    VGA_PIN_LAST = 13,

    SLOW_SAMPLE_HZ = 1500000,
    SLOW_CAPTURE_SAMPLES = 72000,
    SLOW_CAPTURE_WORDS = SLOW_CAPTURE_SAMPLES / 8,

    FAST_SAMPLE_HZ = 18000000,
    FAST_CAPTURE_DURATION_US = 4096,
    FAST_CAPTURE_SAMPLES = 73728,
    FAST_CAPTURE_WORDS = FAST_CAPTURE_SAMPLES / 8,
    FAST_CAPTURE_MAX_ATTEMPTS = 6,

    SOURCE_DOT_CLOCK_HZ = 6000000,
    SOURCE_LINE_PIXELS = 240,
    FAST_SAMPLES_PER_DOT = FAST_SAMPLE_HZ / SOURCE_DOT_CLOCK_HZ,
    SOURCE_LINE_CAPTURE_SAMPLES = SOURCE_LINE_PIXELS * FAST_SAMPLES_PER_DOT,

    /* Normal horizontal sync is about 4.5 us and lines are about 64 us. */
    FAST_SYNC_PULSE_MIN_SAMPLES = FAST_SAMPLE_HZ * 2 / 1000000,
    FAST_SYNC_PULSE_MAX_SAMPLES = FAST_SAMPLE_HZ * 10 / 1000000,
    FAST_LINE_MIN_SAMPLES = FAST_SAMPLE_HZ * 60 / 1000000,
    FAST_LINE_MAX_SAMPLES = FAST_SAMPLE_HZ * 68 / 1000000,
    MAX_FAST_SYNC_PULSES = 128,

    /* The manual places display character 15 nine us after sync character 6. */
    ACTIVE_START_NOMINAL_SAMPLES = FAST_SAMPLE_HZ * 9 / 1000000,
    ACTIVE_START_MIN_SAMPLES = FAST_SAMPLE_HZ * 5 / 1000000,
    ACTIVE_START_MAX_SAMPLES = FAST_SAMPLE_HZ * 13 / 1000000,

    MAX_EDGE_COUNT = 2048,
    EXPECTED_LINE_SAMPLES = SLOW_SAMPLE_HZ * 64 / 1000000,
    MIN_LINE_SAMPLES = 80,
    MAX_LINE_SAMPLES = 112,
};

_Static_assert(SLOW_CAPTURE_SAMPLES % 8 == 0,
               "Slow capture must contain whole DMA words");
_Static_assert(FAST_CAPTURE_SAMPLES % 8 == 0,
               "Fast capture must contain whole DMA words");
_Static_assert(FAST_CAPTURE_SAMPLES ==
                   FAST_SAMPLE_HZ / 1000000 * FAST_CAPTURE_DURATION_US,
               "Fast capture duration and sample count must agree");
_Static_assert(EXPECTED_LINE_SAMPLES == 96,
               "The timing survey expects 96 samples per 64 us line");
_Static_assert(FAST_SAMPLES_PER_DOT == 3,
               "The fast capture must contain three samples per source dot");
_Static_assert(SOURCE_LINE_CAPTURE_SAMPLES == 720,
               "A source line must occupy 720 fast samples");

static uint32_t slow_capture[SLOW_CAPTURE_WORDS];
static uint32_t fast_capture[FAST_CAPTURE_WORDS];
static uint32_t rising_edges[MAX_EDGE_COUNT];
static uint32_t falling_edges[MAX_EDGE_COUNT];
static uint32_t fast_sync_pulse_starts[MAX_FAST_SYNC_PULSES];
static uint32_t fast_sync_pulse_widths[MAX_FAST_SYNC_PULSES];

typedef struct {
    bool valid;
    uint32_t sync_start;
    uint32_t sync_width;
    uint32_t active_start_offset;
    uint32_t foreground_pixels;
    uint32_t stable_pixels;
    uint32_t color_transitions;
    uint8_t pixels[SOURCE_LINE_PIXELS];
} decoded_line_t;

static decoded_line_t decoded_line;

static PIO capture_pio = pio0;
static uint capture_sm;
static uint capture_program_offset;
static int capture_dma_channel;

typedef struct {
    uint32_t high_count;
    uint32_t rising_count;
    uint32_t falling_count;
    uint32_t line_interval_count;
    uint64_t line_interval_sum;
    uint32_t line_interval_min;
    uint32_t line_interval_max;
    uint32_t active_pulse_count;
    uint64_t active_pulse_sum;
    uint32_t frame_marker_count;
    uint64_t frame_interval_sum;
    bool active_level;
} sync_statistics_t;

/** Return one chronological four-bit sample from left-shifted PIO words. */
static inline uint8_t capture_sample(const uint32_t *capture,
                                     uint32_t sample_index) {
    const uint32_t word = capture[sample_index / 8u];
    const uint32_t shift = 28u - 4u * (sample_index % 8u);
    return (uint8_t)((word >> shift) & 0x0fu);
}

/** Configure all VGA-connected pins as passive inputs during probing. */
static void configure_passive_gpio(void) {
    for (uint pin = VGA_PIN_FIRST; pin <= VGA_PIN_LAST; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
    }

    for (uint pin = INPUT_PIN_BASE;
         pin < INPUT_PIN_BASE + INPUT_PIN_COUNT;
         ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
        pio_gpio_init(capture_pio, pin);
    }
}

/** Capture four adjacent pins at a requested PIO sample frequency. */
static void capture_inputs(uint32_t *destination,
                           size_t word_count,
                           uint32_t sample_hz) {
    pio_sm_set_enabled(capture_pio, capture_sm, false);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);

    pio_sm_config config =
        p2000t_sample_program_get_default_config(capture_program_offset);
    sm_config_set_in_pins(&config, INPUT_PIN_BASE);
    sm_config_set_in_shift(&config, false, true, 32);
    sm_config_set_clkdiv(&config,
                         (float)clock_get_hz(clk_sys) / (float)sample_hz);
    pio_sm_init(capture_pio,
                capture_sm,
                capture_program_offset,
                &config);

    dma_channel_config dma_config =
        dma_channel_get_default_config(capture_dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_dreq(
        &dma_config,
        pio_get_dreq(capture_pio, capture_sm, false));

    dma_channel_configure(capture_dma_channel,
                          &dma_config,
                          destination,
                          &capture_pio->rxf[capture_sm],
                          word_count,
                          false);

    dma_start_channel_mask(1u << capture_dma_channel);
    pio_sm_set_enabled(capture_pio, capture_sm, true);
    dma_channel_wait_for_finish_blocking(capture_dma_channel);
    pio_sm_set_enabled(capture_pio, capture_sm, false);
}

static void add_line_intervals(const uint32_t *edges,
                               uint32_t edge_count,
                               sync_statistics_t *statistics) {
    for (uint32_t i = 1; i < edge_count; ++i) {
        const uint32_t interval = edges[i] - edges[i - 1u];
        if (interval < MIN_LINE_SAMPLES || interval > MAX_LINE_SAMPLES) {
            continue;
        }

        statistics->line_interval_sum += interval;
        ++statistics->line_interval_count;
        if (interval < statistics->line_interval_min) {
            statistics->line_interval_min = interval;
        }
        if (interval > statistics->line_interval_max) {
            statistics->line_interval_max = interval;
        }
    }
}

/** Analyze one input as a possible CSYNC signal, accepting either polarity. */
static sync_statistics_t analyze_input_timing(uint channel) {
    sync_statistics_t statistics = {
        .line_interval_min = UINT32_MAX,
    };

    bool previous =
        ((capture_sample(slow_capture, 0) >> channel) & 1u) != 0u;

    for (uint32_t i = 0; i < SLOW_CAPTURE_SAMPLES; ++i) {
        const bool current =
            ((capture_sample(slow_capture, i) >> channel) & 1u) != 0u;
        statistics.high_count += current ? 1u : 0u;

        if (i == 0 || current == previous) {
            previous = current;
            continue;
        }

        if (current) {
            if (statistics.rising_count < MAX_EDGE_COUNT) {
                rising_edges[statistics.rising_count++] = i;
            }
        } else if (statistics.falling_count < MAX_EDGE_COUNT) {
            falling_edges[statistics.falling_count++] = i;
        }
        previous = current;
    }

    statistics.active_level =
        statistics.high_count < SLOW_CAPTURE_SAMPLES / 2u;

    add_line_intervals(rising_edges,
                       statistics.rising_count,
                       &statistics);
    add_line_intervals(falling_edges,
                       statistics.falling_count,
                       &statistics);

    uint32_t estimated_line_samples = EXPECTED_LINE_SAMPLES;
    if (statistics.line_interval_count != 0u) {
        estimated_line_samples =
            (uint32_t)(statistics.line_interval_sum /
                       statistics.line_interval_count);
    }

    uint32_t run_start = 0;
    bool run_level =
        ((capture_sample(slow_capture, 0) >> channel) & 1u) != 0u;
    uint32_t last_frame_marker = 0;
    bool have_frame_marker = false;

    for (uint32_t i = 1; i <= SLOW_CAPTURE_SAMPLES; ++i) {
        const bool at_end = i == SLOW_CAPTURE_SAMPLES;
        const bool current = at_end
                                 ? !run_level
                                 : ((capture_sample(slow_capture, i) >> channel) &
                                    1u) != 0u;
        if (!at_end && current == run_level) {
            continue;
        }

        const uint32_t run_length = i - run_start;
        if (run_level == statistics.active_level) {
            if (run_length < estimated_line_samples / 4u) {
                statistics.active_pulse_sum += run_length;
                ++statistics.active_pulse_count;
            } else if (!have_frame_marker ||
                       run_start - last_frame_marker >
                           estimated_line_samples * 8u) {
                if (have_frame_marker) {
                    statistics.frame_interval_sum +=
                        run_start - last_frame_marker;
                }
                last_frame_marker = run_start;
                have_frame_marker = true;
                ++statistics.frame_marker_count;
            }
        }

        run_start = i;
        run_level = current;
    }

    return statistics;
}

static void print_duration(uint64_t sample_sum,
                           uint32_t interval_count,
                           uint32_t sample_hz) {
    if (interval_count == 0u) {
        printf("not determined");
        return;
    }

    const uint64_t milli_microseconds =
        sample_sum * UINT64_C(1000000000) /
        ((uint64_t)interval_count * sample_hz);
    printf("%llu.%03llu us",
           (unsigned long long)(milli_microseconds / 1000u),
           (unsigned long long)(milli_microseconds % 1000u));
}

static void print_level_percentage(const char *name,
                                   uint channel,
                                   const uint32_t *capture,
                                   uint32_t sample_count) {
    uint32_t high_count = 0;
    uint32_t transitions = 0;
    bool previous = ((capture_sample(capture, 0) >> channel) & 1u) != 0u;

    for (uint32_t i = 0; i < sample_count; ++i) {
        const bool current =
            ((capture_sample(capture, i) >> channel) & 1u) != 0u;
        high_count += current ? 1u : 0u;
        if (i != 0u && current != previous) {
            ++transitions;
        }
        previous = current;
    }

    const uint32_t permille =
        (uint32_t)((uint64_t)high_count * 1000u / sample_count);
    const uint64_t duration_us =
        (uint64_t)sample_count * 1000000u / FAST_SAMPLE_HZ;
    printf("  %-5s GP%u: high %3" PRIu32 ".%" PRIu32 "%%, %" PRIu32
           " transitions in %llu us\n",
           name,
           INPUT_PIN_BASE + channel,
           permille / 10u,
           permille % 10u,
           transitions,
           (unsigned long long)duration_us);
}

static uint32_t count_transitions(const uint32_t *capture,
                                  uint32_t sample_count,
                                  uint channel) {
    uint32_t transitions = 0;
    bool previous =
        ((capture_sample(capture, 0) >> channel) & 1u) != 0u;

    for (uint32_t i = 1; i < sample_count; ++i) {
        const bool current =
            ((capture_sample(capture, i) >> channel) & 1u) != 0u;
        transitions += current != previous ? 1u : 0u;
        previous = current;
    }
    return transitions;
}

static uint32_t count_fast_rgb_transitions(void) {
    return count_transitions(
               fast_capture, FAST_CAPTURE_SAMPLES, RED_CHANNEL) +
           count_transitions(
               fast_capture, FAST_CAPTURE_SAMPLES, GREEN_CHANNEL) +
           count_transitions(
               fast_capture, FAST_CAPTURE_SAMPLES, BLUE_CHANNEL);
}

/** Retry consecutive windows so a vertical-blank capture is not selected. */
static uint capture_fast_rgb_detail(uint32_t *rgb_transitions) {
    for (uint attempt = 1; attempt <= FAST_CAPTURE_MAX_ATTEMPTS; ++attempt) {
        capture_inputs(fast_capture, FAST_CAPTURE_WORDS, FAST_SAMPLE_HZ);
        *rgb_transitions = count_fast_rgb_transitions();
        if (*rgb_transitions != 0u) {
            return attempt;
        }
    }
    return FAST_CAPTURE_MAX_ATTEMPTS;
}

static inline bool logical_color_is_on(uint8_t raw_sample, uint channel) {
    /* The 74LVC1G14 input conditioners invert the P2000T RGB signals. */
    return ((raw_sample >> channel) & 1u) == 0u;
}

static uint8_t decode_source_dot(uint32_t first_sample, bool *stable) {
    uint red_votes = 0;
    uint green_votes = 0;
    uint blue_votes = 0;
    uint8_t sample_colors[FAST_SAMPLES_PER_DOT];

    for (uint i = 0; i < FAST_SAMPLES_PER_DOT; ++i) {
        const uint8_t raw = capture_sample(fast_capture, first_sample + i);
        const bool red = logical_color_is_on(raw, RED_CHANNEL);
        const bool green = logical_color_is_on(raw, GREEN_CHANNEL);
        const bool blue = logical_color_is_on(raw, BLUE_CHANNEL);
        red_votes += red ? 1u : 0u;
        green_votes += green ? 1u : 0u;
        blue_votes += blue ? 1u : 0u;
        sample_colors[i] = (red ? 1u : 0u) |
                           (green ? 2u : 0u) |
                           (blue ? 4u : 0u);
    }

    *stable = sample_colors[0] == sample_colors[1] &&
              sample_colors[1] == sample_colors[2];
    return (red_votes >= 2u ? 1u : 0u) |
           (green_votes >= 2u ? 2u : 0u) |
           (blue_votes >= 2u ? 4u : 0u);
}

static void evaluate_source_line(uint32_t sync_start,
                                 uint32_t sync_width,
                                 uint32_t active_start_offset,
                                 decoded_line_t *candidate) {
    *candidate = (decoded_line_t) {
        .valid = true,
        .sync_start = sync_start,
        .sync_width = sync_width,
        .active_start_offset = active_start_offset,
    };

    const uint32_t active_start = sync_start + active_start_offset;
    uint8_t previous = 0;
    for (uint32_t pixel = 0; pixel < SOURCE_LINE_PIXELS; ++pixel) {
        bool stable = false;
        const uint8_t color = decode_source_dot(
            active_start + pixel * FAST_SAMPLES_PER_DOT, &stable);
        candidate->pixels[pixel] = color;
        candidate->foreground_pixels += color != 0u ? 1u : 0u;
        candidate->stable_pixels += stable ? 1u : 0u;
        if (pixel != 0u && color != previous) {
            ++candidate->color_transitions;
        }
        previous = color;
    }
}

static bool decoded_line_is_better(const decoded_line_t *candidate,
                                   const decoded_line_t *best) {
    if (!best->valid) {
        return true;
    }

    const uint32_t candidate_distance =
        candidate->active_start_offset > ACTIVE_START_NOMINAL_SAMPLES
            ? candidate->active_start_offset - ACTIVE_START_NOMINAL_SAMPLES
            : ACTIVE_START_NOMINAL_SAMPLES - candidate->active_start_offset;
    const uint32_t best_distance =
        best->active_start_offset > ACTIVE_START_NOMINAL_SAMPLES
            ? best->active_start_offset - ACTIVE_START_NOMINAL_SAMPLES
            : ACTIVE_START_NOMINAL_SAMPLES - best->active_start_offset;

    const uint64_t candidate_score =
        (uint64_t)candidate->foreground_pixels * 256u +
        (uint64_t)candidate->stable_pixels * 8u +
        (uint64_t)candidate->color_transitions * 16u +
        (ACTIVE_START_MAX_SAMPLES - ACTIVE_START_MIN_SAMPLES) -
        candidate_distance;
    const uint64_t best_score =
        (uint64_t)best->foreground_pixels * 256u +
        (uint64_t)best->stable_pixels * 8u +
        (uint64_t)best->color_transitions * 16u +
        (ACTIVE_START_MAX_SAMPLES - ACTIVE_START_MIN_SAMPLES) -
        best_distance;
    return candidate_score > best_score;
}

/** Locate normal horizontal sync pulses and decode the most informative line. */
static void decode_best_source_line(bool sync_active_level) {
    uint32_t pulse_count = 0;
    uint32_t run_start = 0;
    bool run_level =
        ((capture_sample(fast_capture, 0) >> SYNC_CHANNEL) & 1u) != 0u;

    decoded_line = (decoded_line_t) {0};

    for (uint32_t i = 1; i <= FAST_CAPTURE_SAMPLES; ++i) {
        const bool at_end = i == FAST_CAPTURE_SAMPLES;
        const bool current = at_end
                                 ? !run_level
                                 : ((capture_sample(fast_capture, i) >>
                                     SYNC_CHANNEL) & 1u) != 0u;
        if (!at_end && current == run_level) {
            continue;
        }

        const uint32_t run_length = i - run_start;
        if (run_level == sync_active_level &&
            run_length >= FAST_SYNC_PULSE_MIN_SAMPLES &&
            run_length <= FAST_SYNC_PULSE_MAX_SAMPLES &&
            pulse_count < MAX_FAST_SYNC_PULSES) {
            fast_sync_pulse_starts[pulse_count] = run_start;
            fast_sync_pulse_widths[pulse_count] = run_length;
            ++pulse_count;
        }
        run_start = i;
        run_level = current;
    }

    for (uint32_t pulse = 0; pulse < pulse_count; ++pulse) {
        const bool follows_line_period =
            pulse != 0u &&
            fast_sync_pulse_starts[pulse] -
                    fast_sync_pulse_starts[pulse - 1u] >=
                FAST_LINE_MIN_SAMPLES &&
            fast_sync_pulse_starts[pulse] -
                    fast_sync_pulse_starts[pulse - 1u] <=
                FAST_LINE_MAX_SAMPLES;
        const bool precedes_line_period =
            pulse + 1u < pulse_count &&
            fast_sync_pulse_starts[pulse + 1u] -
                    fast_sync_pulse_starts[pulse] >=
                FAST_LINE_MIN_SAMPLES &&
            fast_sync_pulse_starts[pulse + 1u] -
                    fast_sync_pulse_starts[pulse] <=
                FAST_LINE_MAX_SAMPLES;
        if (!follows_line_period && !precedes_line_period) {
            continue;
        }

        for (uint32_t offset = ACTIVE_START_MIN_SAMPLES;
             offset <= ACTIVE_START_MAX_SAMPLES;
             ++offset) {
            const uint32_t active_start =
                fast_sync_pulse_starts[pulse] + offset;
            if (active_start + SOURCE_LINE_CAPTURE_SAMPLES >
                FAST_CAPTURE_SAMPLES) {
                break;
            }

            decoded_line_t candidate;
            evaluate_source_line(fast_sync_pulse_starts[pulse],
                                 fast_sync_pulse_widths[pulse],
                                 offset,
                                 &candidate);
            if (decoded_line_is_better(&candidate, &decoded_line)) {
                decoded_line = candidate;
            }
        }
    }
}

static void print_decoded_source_line(void) {
    static const char color_char[8] = {
        '.', 'R', 'G', 'Y', 'B', 'M', 'C', 'W'
    };

    printf("\nDecoded 240-pixel P2000T source line\n");
    if (!decoded_line.valid) {
        printf("  No normal horizontal-sync-delimited line was found.\n");
        return;
    }

    const uint32_t offset_ns =
        (uint32_t)((uint64_t)decoded_line.active_start_offset * 1000000000u /
                   FAST_SAMPLE_HZ);
    const uint32_t pulse_ns =
        (uint32_t)((uint64_t)decoded_line.sync_width * 1000000000u /
                   FAST_SAMPLE_HZ);
    printf("  sync sample: %" PRIu32 ", pulse: %" PRIu32 ".%03" PRIu32
           " us\n",
           decoded_line.sync_start,
           pulse_ns / 1000u,
           pulse_ns % 1000u);
    printf("  active start: %" PRIu32 ".%03" PRIu32
           " us after sync leading edge\n",
           offset_ns / 1000u,
           offset_ns % 1000u);
    printf("  non-black: %" PRIu32 "/%u, stable 3-sample dots: %" PRIu32
           "/%u, color transitions: %" PRIu32 "\n",
           decoded_line.foreground_pixels,
           SOURCE_LINE_PIXELS,
           decoded_line.stable_pixels,
           SOURCE_LINE_PIXELS,
           decoded_line.color_transitions);

    for (uint32_t row = 0; row < SOURCE_LINE_PIXELS; row += 80u) {
        printf("  %03" PRIu32 ": ", row);
        for (uint32_t pixel = row; pixel < row + 80u; ++pixel) {
            putchar(color_char[decoded_line.pixels[pixel] & 7u]);
        }
        putchar('\n');
    }
    printf("  legend: .=black R=red G=green Y=yellow B=blue M=magenta C=cyan W=white\n");
    stdio_flush();
}

static void print_dot_grid_score(const char *name, uint channel) {
    uint32_t matching_runs = 0;
    uint32_t tested_runs = 0;
    uint32_t run_start = 0;
    bool run_level =
        ((capture_sample(fast_capture, 0) >> channel) & 1u) != 0u;

    for (uint32_t i = 1; i <= FAST_CAPTURE_SAMPLES; ++i) {
        const bool at_end = i == FAST_CAPTURE_SAMPLES;
        const bool current = at_end
                                 ? !run_level
                                 : ((capture_sample(fast_capture, i) >> channel) &
                                    1u) != 0u;
        if (!at_end && current == run_level) {
            continue;
        }

        const uint32_t run_length = i - run_start;
        if (run_length >= 2u && run_length <= 180u) {
            const uint32_t remainder = run_length % 3u;
            if (remainder == 0u) {
                ++matching_runs;
            }
            ++tested_runs;
        }

        run_start = i;
        run_level = current;
    }

    if (tested_runs == 0u) {
        printf("  %-5s: no transitions available for dot-grid check\n", name);
        return;
    }

    const uint32_t percentage = matching_runs * 100u / tested_runs;
    printf("  %-5s: %" PRIu32 "/%" PRIu32
           " runs exactly align to the 3x dot grid (%" PRIu32 "%%)\n",
           name,
           matching_runs,
           tested_runs,
           percentage);
}

static bool sync_line_is_plausible(const sync_statistics_t *statistics) {
    if (statistics->line_interval_count == 0u) {
        return false;
    }

    const uint64_t average = statistics->line_interval_sum /
                             statistics->line_interval_count;
    return average >= 94u && average <= 98u;
}

static bool sync_frame_is_plausible(const sync_statistics_t *statistics) {
    if (statistics->frame_marker_count < 2u) {
        return false;
    }

    const uint32_t interval_count = statistics->frame_marker_count - 1u;
    const uint64_t average = statistics->frame_interval_sum / interval_count;
    return average >= 28500u && average <= 31500u;
}

static bool sync_pulse_is_plausible(const sync_statistics_t *statistics) {
    if (statistics->active_pulse_count == 0u) {
        return false;
    }

    const uint64_t average = statistics->active_pulse_sum /
                             statistics->active_pulse_count;
    return average >= 4u && average <= 18u;
}

static const char *input_name(uint channel) {
#if P2000T_PROTOTYPE_V1_MIRRORED_DIN
    static const char *const names[INPUT_PIN_COUNT] = {
        "logical RED",
        "logical CSYNC",
        "logical BLUE",
        "logical GREEN",
    };
#else
    static const char *const names[INPUT_PIN_COUNT] = {
        "logical CSYNC",
        "logical RED",
        "logical GREEN",
        "logical BLUE",
    };
#endif
    return names[channel];
}

static void print_input_timing(uint channel,
                               const sync_statistics_t *statistics) {
    const uint32_t permille =
        (uint32_t)((uint64_t)statistics->high_count * 1000u /
                   SLOW_CAPTURE_SAMPLES);

    printf("  GP%u %-14s high %3" PRIu32 ".%" PRIu32
           "%%, edges %" PRIu32 "/%" PRIu32 "\n",
           INPUT_PIN_BASE + channel,
           input_name(channel),
           permille / 10u,
           permille % 10u,
           statistics->rising_count,
           statistics->falling_count);

    printf("    line:  ");
    print_duration(statistics->line_interval_sum,
                   statistics->line_interval_count,
                   SLOW_SAMPLE_HZ);
    if (statistics->line_interval_count != 0u) {
        const uint64_t line_hz =
            (uint64_t)SLOW_SAMPLE_HZ * statistics->line_interval_count /
            statistics->line_interval_sum;
        printf(" (%llu Hz, %s)",
               (unsigned long long)line_hz,
               sync_line_is_plausible(statistics) ? "plausible" : "unexpected");
    }
    printf("\n");

    printf("    frame: ");
    if (statistics->frame_marker_count >= 2u) {
        const uint32_t frame_intervals = statistics->frame_marker_count - 1u;
        print_duration(statistics->frame_interval_sum,
                       frame_intervals,
                       SLOW_SAMPLE_HZ);
        const uint64_t milli_hz =
            (uint64_t)SLOW_SAMPLE_HZ * frame_intervals * 1000u /
            statistics->frame_interval_sum;
        printf(" (%llu.%03llu Hz, %s)",
               (unsigned long long)(milli_hz / 1000u),
               (unsigned long long)(milli_hz % 1000u),
               sync_frame_is_plausible(statistics)
                   ? "plausible"
                   : "unexpected");
    } else {
        printf("not detected");
    }
    printf("\n");
}

static void print_report(void) {
    sync_statistics_t timing[INPUT_PIN_COUNT];
    int sync_candidate = -1;
    uint32_t best_score = 0;

    for (uint channel = 0; channel < INPUT_PIN_COUNT; ++channel) {
        timing[channel] = analyze_input_timing(channel);

        uint32_t score = 0;
        score += sync_line_is_plausible(&timing[channel]) ? 4u : 0u;
        score += sync_frame_is_plausible(&timing[channel]) ? 8u : 0u;
        score += sync_pulse_is_plausible(&timing[channel]) ? 2u : 0u;
        if (score > best_score) {
            best_score = score;
            sync_candidate = (int)channel;
        }
    }

    printf("\nP2000T signal probe v%s -- RP2040 / Pico 1\n",
           P2000T_SIGNAL_PROBE_VERSION);
    printf("System clock: %" PRIu32 " Hz\n", clock_get_hz(clk_sys));
    printf("Pins: CSYNC=GP%u R=GP%u G=GP%u B=GP%u\n",
           SYNC_PIN,
           RED_PIN,
           GREEN_PIN,
           BLUE_PIN);
#if P2000T_PROTOTYPE_V1_MIRRORED_DIN
    printf("Input profile: prototype v1 mirrored DIN contacts 1/5 and 2/4\n");
#else
    printf("Input profile: corrected PCB v2 DIN mapping\n");
#endif
    printf("VGA pins GP%u-GP%u: passive inputs\n",
           VGA_PIN_FIRST,
           VGA_PIN_LAST);

    printf("\nInput timing survey (48 ms)\n");
    for (uint channel = 0; channel < INPUT_PIN_COUNT; ++channel) {
        print_input_timing(channel, &timing[channel]);
    }

    if (sync_candidate >= 0) {
        const sync_statistics_t *sync = &timing[sync_candidate];
        printf("\nBest sync candidate: GP%u (%s, active %s, pulse ",
               INPUT_PIN_BASE + (uint)sync_candidate,
               input_name((uint)sync_candidate),
               sync->active_level ? "high" : "low");
        print_duration(sync->active_pulse_sum,
                       sync->active_pulse_count,
                       SLOW_SAMPLE_HZ);
        printf(")\n");
    } else {
        printf("\nBest sync candidate: none\n");
    }

    printf("\nFast RGBS capture at 18 MHz (three samples per 6 MHz dot)\n");
    print_level_percentage(
        "CSYNC", SYNC_CHANNEL, fast_capture, FAST_CAPTURE_SAMPLES);
    print_level_percentage(
        "RED", RED_CHANNEL, fast_capture, FAST_CAPTURE_SAMPLES);
    print_level_percentage(
        "GREEN", GREEN_CHANNEL, fast_capture, FAST_CAPTURE_SAMPLES);
    print_level_percentage(
        "BLUE", BLUE_CHANNEL, fast_capture, FAST_CAPTURE_SAMPLES);

    printf("\nRGB transition alignment\n");
    print_dot_grid_score("RED", RED_CHANNEL);
    print_dot_grid_score("GREEN", GREEN_CHANNEL);
    print_dot_grid_score("BLUE", BLUE_CHANNEL);

    decode_best_source_line(timing[SYNC_CHANNEL].active_level);
    print_decoded_source_line();

    printf("\nRESULT: ");
    if (sync_candidate == SYNC_CHANNEL &&
        sync_frame_is_plausible(&timing[sync_candidate])) {
        printf("P2000T synchronization is present on mapped GP%u.\n",
               SYNC_PIN);
    } else if (sync_candidate == SYNC_CHANNEL) {
        printf("mapped GP%u has plausible line timing; frame timing needs inspection.\n",
               SYNC_PIN);
    } else if (sync_candidate >= 0) {
        printf("sync-like timing is on GP%u, but this profile maps it to GP%u.\n",
               INPUT_PIN_BASE + (uint)sync_candidate,
               SYNC_PIN);
    } else {
        printf("no P2000T synchronization candidate was recognized.\n");
    }
    printf("Commands: r = recapture, l = reprint decoded line, "
           "d = dump 4096 us as hexadecimal RGBS nibbles\n");
    stdio_flush();
}

static void acquire_and_report(void) {
    printf("\nCapturing 48 ms timing survey...\n");
    stdio_flush();
    capture_inputs(slow_capture, SLOW_CAPTURE_WORDS, SLOW_SAMPLE_HZ);

    printf("Capturing 4096 us RGBS detail; retrying if blank...\n");
    stdio_flush();
    uint32_t rgb_transitions = 0;
    const uint attempt = capture_fast_rgb_detail(&rgb_transitions);
    printf("Selected fast window %u/%u with %" PRIu32
           " RGB transitions.\n",
           attempt,
           FAST_CAPTURE_MAX_ATTEMPTS,
           rgb_transitions);
    print_report();
}

static void dump_fast_capture(void) {
    printf("\n# P2000T RGBS raw capture\n");
    printf("# sample_hz=%u samples=%u duration_us=%u raw_bits=GP19..GP16\n",
           (unsigned int)FAST_SAMPLE_HZ,
           (unsigned int)FAST_CAPTURE_SAMPLES,
           (unsigned int)FAST_CAPTURE_DURATION_US);
    printf("# logical_pins=CSYNC:GP%u,R:GP%u,G:GP%u,B:GP%u\n",
           SYNC_PIN,
           RED_PIN,
           GREEN_PIN,
           BLUE_PIN);
    for (uint32_t i = 0; i < FAST_CAPTURE_SAMPLES; ++i) {
        printf("%x", capture_sample(fast_capture, i));
        if ((i + 1u) % 96u == 0u) {
            printf("\n");
        }
    }
    if (FAST_CAPTURE_SAMPLES % 96u != 0u) {
        printf("\n");
    }
    printf("# end\n");
    stdio_flush();
}

int main(void) {
    if (!set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true)) {
        while (true) {
            tight_loop_contents();
        }
    }

    stdio_init_all();

    capture_sm = pio_claim_unused_sm(capture_pio, true);
    capture_program_offset =
        pio_add_program(capture_pio, &p2000t_sample_program);
    capture_dma_channel = dma_claim_unused_channel(true);
    configure_passive_gpio();

    while (!stdio_usb_connected()) {
        sleep_ms(10);
    }
    sleep_ms(100);

    acquire_and_report();

    while (true) {
        const int command = getchar_timeout_us(0);
        if (command == 'r' || command == 'R') {
            acquire_and_report();
        } else if (command == 'l' || command == 'L') {
            print_decoded_source_line();
        } else if (command == 'd' || command == 'D') {
            dump_fast_capture();
        }
        sleep_ms(10);
    }
}
