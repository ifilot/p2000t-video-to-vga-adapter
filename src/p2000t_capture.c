/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p2000t_capture.c
 * @brief Direct PIO/DMA acquisition of packed 480x240 P2000T RGBS frames.
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
    CAPTURE_TX_COMMAND_COUNT = 2 + 2 * P2000T_CAPTURE_HEIGHT,
    FRAME_LINE_COUNT = P2000T_CAPTURE_HEIGHT - 1,
    SOURCE_DOT_LOOP_COUNT = P2000T_CAPTURE_WIDTH / 2 - 1,
    CAPTURE_TICKS_PER_SOURCE_DOT = 21,
    /* The former fixed 1024-cycle PIO loop plus its SET instruction are
       folded into the programmable delay loop to make room for horizontal
       sync gating. */
    COMBINED_DELAY_COUNT_BIAS = 1025,
    /* NOP [31] followed by the confirming JMP PIN delays acceptance by 33
       PIO clocks. Subtract it so pixel sampling retains its original phase. */
    HORIZONTAL_SYNC_QUALIFICATION_TICKS = 33,
    NOMINAL_PHASE_DELAY_COUNT = 111,
    MINIMUM_FRAME_PERIOD_US = 19000,
    MAXIMUM_FRAME_PERIOD_US = 21200,
    SIGNAL_LOSS_TIMEOUT_US = 100000,
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
_Static_assert(P2000T_CAPTURE_WIDTH % 2u == 0u,
               "Each nominal source dot must produce two samples");
_Static_assert(P2000T_CAPTURE_WORDS_PER_LINE == 60,
               "Each packed scanline must contain sixty words");

static PIO capture_pio = pio1;
static unsigned capture_sm;
static int capture_rx_dma;
static int capture_tx_dma;
static spin_lock_t *buffer_lock;

static uint32_t capture_buffers[CAPTURE_BUFFER_COUNT]
                               [P2000T_CAPTURE_WORDS_PER_FRAME];
static uint32_t tx_commands[CAPTURE_TX_COMMAND_COUNT];
static buffer_state_t buffer_states[CAPTURE_BUFFER_COUNT];
static uint32_t buffer_sequences[CAPTURE_BUFFER_COUNT];
static unsigned capture_fill_index;
static uint32_t captured_frames;
static uint32_t stale_frames_replaced;
static uint32_t last_frame_period_us;
static uint64_t last_frame_time_us;
static unsigned first_visible_scanline =
    P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE;
static int sample_phase = P2000T_DEFAULT_SAMPLE_PHASE;
static unsigned horizontal_offset = P2000T_DEFAULT_HORIZONTAL_OFFSET;

static bool timing_is_locked(uint64_t now, uint64_t last_frame_time,
                             uint32_t frame_period) {
    return last_frame_time != 0u &&
        frame_period >= MINIMUM_FRAME_PERIOD_US &&
        frame_period <= MAXIMUM_FRAME_PERIOD_US &&
        now - last_frame_time <= SIGNAL_LOSS_TIMEOUT_US;
}

/** Refresh commands only while the previous frame's TX DMA is exhausted. */
static void update_tx_commands(void) {
    tx_commands[0] = FRAME_LINE_COUNT;
    tx_commands[1] = first_visible_scanline - 1u;
    const uint32_t qualified_start_delay = (uint32_t)(
        COMBINED_DELAY_COUNT_BIAS + NOMINAL_PHASE_DELAY_COUNT -
        HORIZONTAL_SYNC_QUALIFICATION_TICKS +
        (int)horizontal_offset * CAPTURE_TICKS_PER_SOURCE_DOT +
        sample_phase);
    for (unsigned line = 0; line < P2000T_CAPTURE_HEIGHT; ++line) {
        /* The first visible line arrives through the initial sync-counting
           path and therefore has not incurred the qualified-edge delay. */
        tx_commands[2u + line * 2u] = qualified_start_delay +
            (line == 0u ? HORIZONTAL_SYNC_QUALIFICATION_TICKS : 0u);
        tx_commands[3u + line * 2u] = SOURCE_DOT_LOOP_COUNT;
    }
}

static void arm_capture_frame(void) {
    dma_channel_set_write_addr(capture_rx_dma,
                               capture_buffers[capture_fill_index], false);
    dma_channel_set_trans_count(capture_rx_dma,
                                P2000T_CAPTURE_WORDS_PER_FRAME, false);
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
    dma_hw->ints1 = 1u << capture_rx_dma;

    const uint64_t now = time_us_64();
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (last_frame_time_us != 0u) {
        last_frame_period_us = (uint32_t)(now - last_frame_time_us);
    }
    last_frame_time_us = now;

    const unsigned completed = capture_fill_index;
    buffer_sequences[completed] = ++captured_frames;
    buffer_states[completed] = BUFFER_READY;

    capture_fill_index = choose_next_fill_buffer();
    buffer_states[capture_fill_index] = BUFFER_FILLING;
    update_tx_commands();
    spin_unlock(buffer_lock, saved);

    arm_capture_frame();
}

static void initialize_capture_pio(void) {
    hard_assert(clock_get_hz(clk_sys) == REQUIRED_SYSTEM_CLOCK_HZ);
    const unsigned program_offset =
        pio_add_program(capture_pio, &p2000t_capture_program);
    capture_sm = pio_claim_unused_sm(capture_pio, true);

    for (unsigned pin = P2000T_INPUT_PIN_BASE;
         pin < P2000T_INPUT_PIN_BASE + 4u; ++pin) {
        pio_gpio_init(capture_pio, pin);
        gpio_disable_pulls(pin);
    }

    pio_sm_config config =
        p2000t_capture_program_get_default_config(program_offset);
    sm_config_set_in_pins(&config, P2000T_INPUT_PIN_BASE);
    sm_config_set_jmp_pin(&config, P2000T_SYNC_PIN);
    sm_config_set_in_shift(&config, false, true, 32);
    sm_config_set_clkdiv_int_frac8(&config, CAPTURE_CLOCK_DIVIDER, 0);
    pio_sm_set_consecutive_pindirs(capture_pio, capture_sm,
                                   P2000T_INPUT_PIN_BASE, 4, false);
    pio_sm_init(capture_pio, capture_sm, program_offset, &config);
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
                          capture_buffers[0],
                          &capture_pio->rxf[capture_sm],
                          P2000T_CAPTURE_WORDS_PER_FRAME, false);

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
    update_tx_commands();

    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        buffer_states[i] = BUFFER_FREE;
        buffer_sequences[i] = 0;
    }
    capture_fill_index = 0;
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
        .sample_phase = sample_phase,
        .horizontal_offset = horizontal_offset,
        .signal_present = timing_is_locked(now, last_frame_time_us,
                                           last_frame_period_us),
    };
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

bool p2000t_capture_set_horizontal_offset(unsigned pixels) {
    if (pixels > P2000T_MAX_HORIZONTAL_OFFSET) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    horizontal_offset = pixels;
    spin_unlock(buffer_lock, saved);
    return true;
}
