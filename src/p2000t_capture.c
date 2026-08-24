/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p2000t_capture.c
 * @brief PIO1/DMA acquisition of complete packed P2000T RGBS frames.
 */

#include "p2000t_capture.h"

#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "p2000t_capture.pio.h"
#include "pico/binary_info.h"
#include "pico/stdlib.h"

enum {
    REQUIRED_SYSTEM_CLOCK_HZ = 252000000,
    CAPTURE_PIO_CLOCK_HZ = 126000000,
    CAPTURE_CLOCK_DIVIDER = 2,
    CAPTURE_BUFFER_COUNT = 3,
    RAW_LINE_BUFFER_COUNT = 2,
    RAW_CAPTURE_WIDTH =
        P2000T_CAPTURE_WIDTH + P2000T_MAX_HORIZONTAL_OFFSET,
    RAW_PIXELS_PER_WORD = 1,
    RAW_LINE_WORD_COUNT = RAW_CAPTURE_WIDTH / RAW_PIXELS_PER_WORD,
    CAPTURE_TX_COMMAND_COUNT = 2 + 2 * P2000T_CAPTURE_HEIGHT,
    FRAME_LINE_COUNT = P2000T_CAPTURE_HEIGHT - 1,
    LINE_PIXEL_COUNT = RAW_CAPTURE_WIDTH - 1,
    NOMINAL_PHASE_DELAY_COUNT = 111,
    MINIMUM_FRAME_PERIOD_US = 19000,
    MAXIMUM_FRAME_PERIOD_US = 21200,
    SIGNAL_LOSS_TIMEOUT_US = 100000,
    QUALITY_X_STRIDE = 4,
    QUALITY_Y_STRIDE = 4,
};

typedef enum {
    BUFFER_FREE,
    BUFFER_FILLING,
    BUFFER_READY,
    BUFFER_IN_USE,
} buffer_state_t;

_Static_assert(REQUIRED_SYSTEM_CLOCK_HZ / CAPTURE_CLOCK_DIVIDER ==
                   CAPTURE_PIO_CLOCK_HZ,
               "Capture PIO must use an exact integer clock divider");
_Static_assert(P2000T_CAPTURE_WORDS_PER_LINE == 30,
               "Each packed scanline must contain thirty words");
_Static_assert(RAW_CAPTURE_WIDTH % RAW_PIXELS_PER_WORD == 0,
               "Every raw word must hold complete source pixels");
_Static_assert(P2000T_MAX_HORIZONTAL_OFFSET % RAW_PIXELS_PER_WORD == 0,
               "Every selectable window must start at a raw-word boundary");
_Static_assert((P2000T_CAPTURE_WIDTH / P2000T_QUALITY_BIN_COUNT) %
                   QUALITY_X_STRIDE == 0,
               "Each horizontal quality bin needs equal sparse samples");

static PIO capture_pio = pio1;
static unsigned capture_sm;
static unsigned capture_program_offset;
static int capture_rx_dma;
static int capture_tx_dma;
static spin_lock_t *buffer_lock;

static uint32_t capture_buffers[CAPTURE_BUFFER_COUNT]
                               [P2000T_CAPTURE_WORDS_PER_FRAME];
static uint32_t raw_line_buffers[RAW_LINE_BUFFER_COUNT][RAW_LINE_WORD_COUNT];
static uint32_t tx_commands[CAPTURE_TX_COMMAND_COUNT];
static uint8_t majority_triplet[1u << 12u];
static uint16_t signal_quality_triplet[1u << 12u];
static uint8_t rgb_bit_count[1u << 4u];
static buffer_state_t buffer_states[CAPTURE_BUFFER_COUNT];
static uint32_t buffer_sequences[CAPTURE_BUFFER_COUNT];
static unsigned capture_fill_index;
static unsigned raw_fill_index;
static unsigned capture_line_index;
static uint32_t captured_frames;
static uint32_t stale_frames_replaced;
static uint32_t last_frame_period_us;
static uint64_t last_frame_time_us;
static uint32_t maximum_line_decode_us;
static unsigned first_visible_scanline =
    P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE;
static int sample_phase = P2000T_DEFAULT_SAMPLE_PHASE;
static int measured_phase = P2000T_DEFAULT_SAMPLE_PHASE;
static unsigned horizontal_offset = P2000T_DEFAULT_HORIZONTAL_OFFSET;
static unsigned applied_horizontal_offset =
    P2000T_DEFAULT_HORIZONTAL_OFFSET;
static uint32_t quality_frames;
static uint64_t quality_triplets;
static uint64_t quality_disagreements;
static uint64_t quality_early_centre_differences;
static uint64_t quality_centre_late_differences;
static uint64_t quality_centre_outliers;
static uint64_t quality_bin_disagreements[P2000T_QUALITY_BIN_COUNT];
static uint64_t quality_channel_window_disagreements
    [P2000T_RGB_CHANNEL_COUNT][P2000T_TAP_WINDOW_COUNT];
static uint8_t channel_tap_windows[P2000T_RGB_CHANNEL_COUNT] = {
    P2000T_DEFAULT_TAP_WINDOW,
    P2000T_DEFAULT_TAP_WINDOW,
    P2000T_DEFAULT_TAP_WINDOW,
};
static uint8_t applied_channel_tap_windows[P2000T_RGB_CHANNEL_COUNT] = {
    P2000T_DEFAULT_TAP_WINDOW,
    P2000T_DEFAULT_TAP_WINDOW,
    P2000T_DEFAULT_TAP_WINDOW,
};
static bool quality_measurement_enabled;

static bool timing_is_locked(uint64_t now, uint64_t last_frame_time,
                             uint32_t frame_period) {
    return last_frame_time != 0u &&
        frame_period >= MINIMUM_FRAME_PERIOD_US &&
        frame_period <= MAXIMUM_FRAME_PERIOD_US &&
        now - last_frame_time <= SIGNAL_LOSS_TIMEOUT_US;
}

static unsigned count_rgb_bits(unsigned bits) {
    const unsigned rgb_mask =
        (1u << P2000T_RED_CHANNEL) |
        (1u << P2000T_GREEN_CHANNEL) |
        (1u << P2000T_BLUE_CHANNEL);
    bits &= rgb_mask;
    unsigned count = 0;
    for (unsigned channel = 0; channel < 4u; ++channel) {
        count += (bits >> channel) & 1u;
    }
    return count;
}

/** Build majority and signal-quality results for every sample triplet. */
static void initialize_majority_lookup(void) {
    for (unsigned bits = 0; bits < (1u << 4u); ++bits) {
        rgb_bit_count[bits] = (uint8_t)count_rgb_bits(bits);
    }
    for (unsigned samples = 0; samples < (1u << 12u); ++samples) {
        const unsigned early = samples >> 8u;
        const unsigned centre = (samples >> 4u) & 0x0fu;
        const unsigned late = samples & 0x0fu;
        majority_triplet[samples] = (uint8_t)(
            (early & centre) | (early & late) | (centre & late));
        const unsigned early_centre = early ^ centre;
        const unsigned centre_late = centre ^ late;
        signal_quality_triplet[samples] = (uint16_t)(
            (early_centre | centre_late) |
            (early_centre << 4u) |
            (centre_late << 8u) |
            ((early_centre & centre_late) << 12u));
    }
}

static inline unsigned select_window(unsigned early, unsigned centre,
                                     unsigned late, unsigned window) {
    return window == 0u ? early : (window == 1u ? centre : late);
}

/** Decode five-tap samples and optionally gather sparse eye statistics. */
static void decode_raw_line(const uint32_t *raw, uint32_t *destination,
                            bool measure_quality) {
    const bool need_early = measure_quality ||
        applied_channel_tap_windows[0] == 0u ||
        applied_channel_tap_windows[1] == 0u ||
        applied_channel_tap_windows[2] == 0u;
    const bool need_late = measure_quality ||
        applied_channel_tap_windows[0] == 2u ||
        applied_channel_tap_windows[1] == 2u ||
        applied_channel_tap_windows[2] == 2u;
    uint32_t disagreements = 0;
    uint32_t early_centre_differences = 0;
    uint32_t centre_late_differences = 0;
    uint32_t centre_outliers = 0;
    uint32_t bin_disagreements = 0;
    uint32_t red_early = 0;
    uint32_t red_centre = 0;
    uint32_t red_late = 0;
    uint32_t green_early = 0;
    uint32_t green_centre = 0;
    uint32_t green_late = 0;
    uint32_t blue_early = 0;
    uint32_t blue_centre = 0;
    uint32_t blue_late = 0;
    unsigned bin = 0;
    unsigned pixels_in_bin = 0;
    raw += applied_horizontal_offset;
    for (unsigned output_word = 0;
         output_word < P2000T_CAPTURE_WORDS_PER_LINE; ++output_word) {
        uint32_t packed = 0;
        for (unsigned pixel = 0; pixel < 8u; ++pixel) {
            const uint32_t samples = raw[output_word * 8u + pixel];
            const unsigned centre_triplet =
                (samples >> 4u) & 0x0fffu;
            const unsigned centre_majority =
                majority_triplet[centre_triplet];
            const unsigned early_triplet = need_early
                ? (samples >> 8u) & 0x0fffu
                : 0u;
            const unsigned late_triplet = need_late
                ? samples & 0x0fffu
                : 0u;
            const unsigned early_majority = need_early
                ? majority_triplet[early_triplet]
                : 0u;
            const unsigned late_majority = need_late
                ? majority_triplet[late_triplet]
                : 0u;

            unsigned decoded;
            if (!need_early && !need_late) {
                decoded = centre_majority;
            } else {
                decoded =
                    centre_majority & (1u << P2000T_SYNC_CHANNEL);
                decoded |= select_window(
                    early_majority, centre_majority, late_majority,
                    applied_channel_tap_windows[0]) &
                        (1u << P2000T_RED_CHANNEL);
                decoded |= select_window(
                    early_majority, centre_majority, late_majority,
                    applied_channel_tap_windows[1]) &
                        (1u << P2000T_GREEN_CHANNEL);
                decoded |= select_window(
                    early_majority, centre_majority, late_majority,
                    applied_channel_tap_windows[2]) &
                        (1u << P2000T_BLUE_CHANNEL);
            }
            packed = (packed << 4u) | decoded;

            if (!measure_quality || (pixel & (QUALITY_X_STRIDE - 1u)) != 0u) {
                continue;
            }

            const unsigned early_quality =
                signal_quality_triplet[early_triplet];
            const unsigned centre_quality =
                signal_quality_triplet[centre_triplet];
            const unsigned late_quality =
                signal_quality_triplet[late_triplet];
            const unsigned centre_disagreements =
                centre_quality & 0x0fu;
            const unsigned pixel_disagreements =
                rgb_bit_count[centre_disagreements];
            disagreements += pixel_disagreements;
            early_centre_differences += rgb_bit_count[
                (centre_quality >> 4u) & 0x0fu];
            centre_late_differences += rgb_bit_count[
                (centre_quality >> 8u) & 0x0fu];
            centre_outliers += rgb_bit_count[
                (centre_quality >> 12u) & 0x0fu];
            bin_disagreements += pixel_disagreements;

            red_early +=
                (early_quality >> P2000T_RED_CHANNEL) & 1u;
            red_centre +=
                (centre_quality >> P2000T_RED_CHANNEL) & 1u;
            red_late +=
                (late_quality >> P2000T_RED_CHANNEL) & 1u;
            green_early +=
                (early_quality >> P2000T_GREEN_CHANNEL) & 1u;
            green_centre +=
                (centre_quality >> P2000T_GREEN_CHANNEL) & 1u;
            green_late +=
                (late_quality >> P2000T_GREEN_CHANNEL) & 1u;
            blue_early +=
                (early_quality >> P2000T_BLUE_CHANNEL) & 1u;
            blue_centre +=
                (centre_quality >> P2000T_BLUE_CHANNEL) & 1u;
            blue_late +=
                (late_quality >> P2000T_BLUE_CHANNEL) & 1u;

            if (++pixels_in_bin == P2000T_CAPTURE_WIDTH /
                    P2000T_QUALITY_BIN_COUNT / QUALITY_X_STRIDE) {
                quality_bin_disagreements[bin++] += bin_disagreements;
                pixels_in_bin = 0;
                bin_disagreements = 0;
            }
        }
        destination[output_word] = packed;
    }
    if (!measure_quality) {
        return;
    }
    quality_triplets += P2000T_CAPTURE_WIDTH / QUALITY_X_STRIDE;
    quality_disagreements += disagreements;
    quality_early_centre_differences += early_centre_differences;
    quality_centre_late_differences += centre_late_differences;
    quality_centre_outliers += centre_outliers;
    quality_channel_window_disagreements[0][0] += red_early;
    quality_channel_window_disagreements[0][1] += red_centre;
    quality_channel_window_disagreements[0][2] += red_late;
    quality_channel_window_disagreements[1][0] += green_early;
    quality_channel_window_disagreements[1][1] += green_centre;
    quality_channel_window_disagreements[1][2] += green_late;
    quality_channel_window_disagreements[2][0] += blue_early;
    quality_channel_window_disagreements[2][1] += blue_centre;
    quality_channel_window_disagreements[2][2] += blue_late;
}

static void reset_quality_statistics(void) {
    quality_frames = 0;
    quality_triplets = 0;
    quality_disagreements = 0;
    quality_early_centre_differences = 0;
    quality_centre_late_differences = 0;
    quality_centre_outliers = 0;
    for (unsigned bin = 0; bin < P2000T_QUALITY_BIN_COUNT; ++bin) {
        quality_bin_disagreements[bin] = 0;
    }
    for (unsigned channel = 0; channel < P2000T_RGB_CHANNEL_COUNT;
         ++channel) {
        for (unsigned window = 0; window < P2000T_TAP_WINDOW_COUNT;
             ++window) {
            quality_channel_window_disagreements[channel][window] = 0;
        }
    }
}

/** Refresh commands only while the previous frame's TX DMA is exhausted. */
static void update_tx_commands(void) {
    tx_commands[0] = FRAME_LINE_COUNT;
    tx_commands[1] = first_visible_scanline - 1u;
    const uint32_t phase_delay =
        (uint32_t)(NOMINAL_PHASE_DELAY_COUNT + sample_phase);
    for (unsigned line = 0; line < P2000T_CAPTURE_HEIGHT; ++line) {
        tx_commands[2u + line * 2u] = phase_delay;
        tx_commands[3u + line * 2u] = LINE_PIXEL_COUNT;
    }
}

static void arm_raw_line_dma(unsigned raw_buffer_index) {
    dma_channel_set_write_addr(capture_rx_dma,
                               raw_line_buffers[raw_buffer_index], false);
    dma_channel_set_trans_count(capture_rx_dma, RAW_LINE_WORD_COUNT, true);
}

static void arm_capture_frame(void) {
    dma_channel_set_write_addr(capture_rx_dma,
                               raw_line_buffers[raw_fill_index], false);
    dma_channel_set_trans_count(capture_rx_dma, RAW_LINE_WORD_COUNT, false);
    dma_channel_set_read_addr(capture_tx_dma, tx_commands, false);
    dma_channel_set_trans_count(capture_tx_dma,
                                CAPTURE_TX_COMMAND_COUNT, false);
    dma_start_channel_mask((1u << capture_rx_dma) |
                           (1u << capture_tx_dma));
}

static unsigned choose_next_fill_buffer(void) {
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_FREE) {
            return i;
        }
    }

    unsigned oldest = CAPTURE_BUFFER_COUNT;
    uint32_t oldest_sequence = UINT32_MAX;
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_READY &&
            buffer_sequences[i] < oldest_sequence) {
            oldest = i;
            oldest_sequence = buffer_sequences[i];
        }
    }
    hard_assert(oldest < CAPTURE_BUFFER_COUNT);
    ++stale_frames_replaced;
    return oldest;
}

static void __not_in_flash_func(capture_dma_irq)(void) {
    const uint32_t irq_started = time_us_32();
    dma_hw->ints1 = 1u << capture_rx_dma;
    const unsigned completed_raw = raw_fill_index;
    const unsigned completed_line = capture_line_index;
    const bool final_line =
        completed_line + 1u == P2000T_CAPTURE_HEIGHT;

    if (!final_line) {
        raw_fill_index = completed_raw ^ 1u;
        capture_line_index = completed_line + 1u;
        arm_raw_line_dma(raw_fill_index);
    }

    decode_raw_line(
        raw_line_buffers[completed_raw],
        capture_buffers[capture_fill_index] +
            completed_line * P2000T_CAPTURE_WORDS_PER_LINE,
        quality_measurement_enabled &&
            (completed_line & (QUALITY_Y_STRIDE - 1u)) == 0u);
    if (!final_line) {
        const uint32_t elapsed = time_us_32() - irq_started;
        if (elapsed > maximum_line_decode_us) {
            maximum_line_decode_us = elapsed;
        }
        return;
    }

    const uint64_t now = time_us_64();
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (last_frame_time_us != 0u) {
        last_frame_period_us = (uint32_t)(now - last_frame_time_us);
    }
    last_frame_time_us = now;

    const unsigned completed = capture_fill_index;
    buffer_sequences[completed] = ++captured_frames;
    buffer_states[completed] = BUFFER_READY;
    if (quality_measurement_enabled) {
        ++quality_frames;
    }

    const unsigned next = choose_next_fill_buffer();
    capture_fill_index = next;
    buffer_states[next] = BUFFER_FILLING;
    capture_line_index = 0;
    raw_fill_index = completed_raw ^ 1u;
    applied_horizontal_offset = horizontal_offset;
    spin_unlock(buffer_lock, saved);

    update_tx_commands();
    if (measured_phase != sample_phase) {
        measured_phase = sample_phase;
        reset_quality_statistics();
    }
    for (unsigned channel = 0; channel < P2000T_RGB_CHANNEL_COUNT;
         ++channel) {
        applied_channel_tap_windows[channel] =
            channel_tap_windows[channel];
    }
    arm_capture_frame();
}

static void initialize_capture_pio(void) {
    hard_assert(clock_get_hz(clk_sys) == REQUIRED_SYSTEM_CLOCK_HZ);
    capture_program_offset =
        pio_add_program(capture_pio, &p2000t_capture_program);
    capture_sm = pio_claim_unused_sm(capture_pio, true);

    for (unsigned pin = P2000T_INPUT_PIN_BASE;
         pin < P2000T_INPUT_PIN_BASE + 4u; ++pin) {
        pio_gpio_init(capture_pio, pin);
        gpio_disable_pulls(pin);
    }

    pio_sm_config config =
        p2000t_capture_program_get_default_config(capture_program_offset);
    sm_config_set_in_pins(&config, P2000T_INPUT_PIN_BASE);
    sm_config_set_jmp_pin(&config, P2000T_SYNC_PIN);
    sm_config_set_in_shift(&config, false, true, 20);
    sm_config_set_clkdiv_int_frac8(&config, CAPTURE_CLOCK_DIVIDER, 0);
    pio_sm_set_consecutive_pindirs(capture_pio, capture_sm,
                                   P2000T_INPUT_PIN_BASE, 4, false);
    pio_sm_init(capture_pio, capture_sm, capture_program_offset, &config);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);
}

static void initialize_capture_dma(void) {
    capture_rx_dma = dma_claim_unused_channel(true);
    capture_tx_dma = dma_claim_unused_channel(true);

    dma_channel_config rx = dma_channel_get_default_config(capture_rx_dma);
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_32);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, true);
    channel_config_set_dreq(
        &rx, pio_get_dreq(capture_pio, capture_sm, false));
    dma_channel_configure(capture_rx_dma, &rx,
                          raw_line_buffers[0],
                          &capture_pio->rxf[capture_sm],
                          RAW_LINE_WORD_COUNT, false);

    dma_channel_config tx = dma_channel_get_default_config(capture_tx_dma);
    channel_config_set_transfer_data_size(&tx, DMA_SIZE_32);
    channel_config_set_read_increment(&tx, true);
    channel_config_set_write_increment(&tx, false);
    channel_config_set_dreq(
        &tx, pio_get_dreq(capture_pio, capture_sm, true));
    dma_channel_configure(capture_tx_dma, &tx,
                          &capture_pio->txf[capture_sm], tx_commands,
                          CAPTURE_TX_COMMAND_COUNT, false);

    dma_channel_set_irq1_enabled(capture_rx_dma, true);
    irq_set_exclusive_handler(DMA_IRQ_1, capture_dma_irq);
    irq_set_priority(DMA_IRQ_1, 0x80);
    irq_set_enabled(DMA_IRQ_1, true);
}

void p2000t_capture_start(void) {
    bi_decl(bi_4pins_with_names(
        P2000T_SYNC_PIN, "P2000T CSYNC_IN",
        P2000T_RED_PIN, "P2000T RED_IN",
        P2000T_GREEN_PIN, "P2000T GREEN_IN",
        P2000T_BLUE_PIN, "P2000T BLUE_IN"));

    buffer_lock =
        spin_lock_instance((unsigned)spin_lock_claim_unused(true));
    initialize_majority_lookup();
    update_tx_commands();

    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        buffer_states[i] = BUFFER_FREE;
        buffer_sequences[i] = 0;
    }
    capture_fill_index = 0;
    raw_fill_index = 0;
    capture_line_index = 0;
    buffer_states[0] = BUFFER_FILLING;

    initialize_capture_pio();
    initialize_capture_dma();
    arm_capture_frame();
    pio_sm_set_enabled(capture_pio, capture_sm, true);
}

bool p2000t_capture_signal_present(void) {
    const uint64_t now = time_us_64();
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    const uint64_t frame_time = last_frame_time_us;
    const uint32_t period = last_frame_period_us;
    spin_unlock(buffer_lock, saved);
    return timing_is_locked(now, frame_time, period);
}

int p2000t_capture_acquire_latest_frame(uint32_t *sequence) {
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    int latest = -1;
    uint32_t latest_sequence = 0;
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_READY &&
            (latest < 0 || buffer_sequences[i] >= latest_sequence)) {
            latest = (int)i;
            latest_sequence = buffer_sequences[i];
        }
    }
    if (latest >= 0) {
        buffer_states[latest] = BUFFER_IN_USE;
        *sequence = latest_sequence;
    }
    spin_unlock(buffer_lock, saved);
    return latest;
}

void p2000t_capture_release_frame(unsigned buffer_index) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (buffer_states[buffer_index] == BUFFER_IN_USE) {
        buffer_states[buffer_index] = BUFFER_FREE;
    }
    spin_unlock(buffer_lock, saved);
}

const uint32_t *p2000t_capture_buffer(unsigned buffer_index) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    return capture_buffers[buffer_index];
}

void p2000t_capture_get_stats(p2000t_capture_stats_t *stats) {
    const uint64_t now = time_us_64();
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    *stats = (p2000t_capture_stats_t) {
        .captured_frames = captured_frames,
        .stale_frames_replaced = stale_frames_replaced,
        .last_frame_period_us = last_frame_period_us,
        .first_visible_scanline = first_visible_scanline,
        .maximum_line_decode_us = maximum_line_decode_us,
        .sample_phase = sample_phase,
        .measured_phase = measured_phase,
        .horizontal_offset = horizontal_offset,
        .quality_frames = quality_frames,
        .quality_triplets = quality_triplets,
        .quality_disagreements = quality_disagreements,
        .quality_early_centre_differences =
            quality_early_centre_differences,
        .quality_centre_late_differences =
            quality_centre_late_differences,
        .quality_centre_outliers = quality_centre_outliers,
        .quality_measurement_enabled = quality_measurement_enabled,
        .signal_present = timing_is_locked(now, last_frame_time_us,
                                           last_frame_period_us),
    };
    for (unsigned bin = 0; bin < P2000T_QUALITY_BIN_COUNT; ++bin) {
        stats->quality_bin_disagreements[bin] =
            quality_bin_disagreements[bin];
    }
    for (unsigned channel = 0; channel < P2000T_RGB_CHANNEL_COUNT;
         ++channel) {
        stats->channel_tap_windows[channel] =
            applied_channel_tap_windows[channel];
        for (unsigned window = 0; window < P2000T_TAP_WINDOW_COUNT;
             ++window) {
            stats->quality_channel_window_disagreements[channel][window] =
                quality_channel_window_disagreements[channel][window];
        }
    }
    spin_unlock(buffer_lock, saved);
}

bool p2000t_capture_set_first_visible_scanline(unsigned scanline) {
    if (scanline < P2000T_MIN_FIRST_VISIBLE_SCANLINE ||
        scanline > P2000T_MAX_FIRST_VISIBLE_SCANLINE) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    first_visible_scanline = scanline;
    spin_unlock(buffer_lock, saved);
    return true;
}

bool p2000t_capture_set_sample_phase(int phase) {
    if (phase < P2000T_MIN_SAMPLE_PHASE ||
        phase > P2000T_MAX_SAMPLE_PHASE) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    sample_phase = phase;
    spin_unlock(buffer_lock, saved);
    return true;
}

bool p2000t_capture_set_channel_tap_windows(unsigned red,
                                            unsigned green,
                                            unsigned blue) {
    if (red >= P2000T_TAP_WINDOW_COUNT ||
        green >= P2000T_TAP_WINDOW_COUNT ||
        blue >= P2000T_TAP_WINDOW_COUNT) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    channel_tap_windows[0] = (uint8_t)red;
    channel_tap_windows[1] = (uint8_t)green;
    channel_tap_windows[2] = (uint8_t)blue;
    spin_unlock(buffer_lock, saved);
    return true;
}

void p2000t_capture_set_quality_measurement(bool enabled) {
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    quality_measurement_enabled = enabled;
    maximum_line_decode_us = 0;
    reset_quality_statistics();
    spin_unlock(buffer_lock, saved);
}

bool p2000t_capture_set_horizontal_offset(unsigned pixels) {
    if (pixels > P2000T_MAX_HORIZONTAL_OFFSET ||
        pixels % RAW_PIXELS_PER_WORD != 0u) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    horizontal_offset = pixels;
    spin_unlock(buffer_lock, saved);
    return true;
}
