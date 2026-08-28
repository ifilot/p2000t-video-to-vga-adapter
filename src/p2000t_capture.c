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

/** PIO timing, DMA command, buffering, and signal-validation constants. */
enum {
    REQUIRED_SYSTEM_CLOCK_HZ = 252000000,
    /**< System clock required for exact capture and VGA frequencies. */
    CAPTURE_PIO_CLOCK_HZ = 126000000,
    /**< Nominal clock driving the capture state machine at trim zero. */
    CAPTURE_CLOCK_DIVIDER = 2,
    /**< Integer divider from the system clock to capture PIO. */
    CAPTURE_CLOCK_DIVIDER_FRACTION = 0,
    /**< Nominal fractional divider in 1/256 steps. */
    NOMINAL_CAPTURE_CLOCK_DIVIDER_FIXED =
        CAPTURE_CLOCK_DIVIDER * 256 + CAPTURE_CLOCK_DIVIDER_FRACTION,
    /**< Nominal PIO divider in the hardware's 16:8 fixed-point format. */
    CAPTURE_BUFFER_COUNT = 3,
    /**< Buffers used for filling, ready, and displayed frame states. */
    CAPTURE_TX_COMMAND_COUNT = 2 + 2 * P2000T_CAPTURE_HEIGHT,
    /**< PIO TX words supplied for each complete source frame. */
    FRAME_LINE_COUNT = P2000T_CAPTURE_HEIGHT - 1,
    /**< PIO decrementing-loop value for all captured source lines. */
    SOURCE_DOT_LOOP_COUNT = P2000T_CAPTURE_WIDTH / 2 - 1,
    /**< PIO decrementing-loop value for all source dots in one line. */
    CAPTURE_TICKS_PER_SOURCE_DOT = 21,
    /**< PIO clocks occupied by one nominal 6 MHz source dot. */
    /* The former fixed 1024-cycle PIO loop plus its SET instruction are
       folded into the programmable delay loop to make room for horizontal
       sync gating. */
    COMBINED_DELAY_COUNT_BIAS = 1025,
    /**< Fixed horizontal-delay component expressed as a loop count. */
    /* NOP [31] followed by the confirming JMP PIN delays acceptance by 33
       PIO clocks. Subtract it so pixel sampling retains its original phase. */
    HORIZONTAL_SYNC_QUALIFICATION_TICKS = 33,
    /**< Qualified-edge delay compensated in each line command. */
    FIRST_LINE_SYNC_COMPENSATION_TICKS =
        HORIZONTAL_SYNC_QUALIFICATION_TICKS - 1,
    /**< Initial edge-counting path enters the delay one tick later. */
    QUALIFIED_LINE_START_OVERHEAD_TICKS = 45,
    /**< PIO clocks from detected sync through the first sample, excluding
         the programmable delay-loop count. */
    FIRST_LINE_START_OVERHEAD_TICKS =
        QUALIFIED_LINE_START_OVERHEAD_TICKS -
        FIRST_LINE_SYNC_COMPENSATION_TICKS,
    /**< Corresponding overhead for the initial edge-counting path. */
    NOMINAL_PHASE_DELAY_COUNT = 111,
    /**< Baseline fine-delay loop count at phase zero. */
    MINIMUM_FRAME_PERIOD_US = 19000,
    /**< Shortest source frame period considered credible. */
    MAXIMUM_FRAME_PERIOD_US = 21200,
    /**< Longest source frame period considered credible. */
    SIGNAL_LOSS_TIMEOUT_US = 100000,
    /**< Maximum age of the last complete frame before signal loss. */
};

/** Lifecycle of one member of the triple-buffered capture pool. */
typedef enum {
    BUFFER_FREE,    /**< Available for the next DMA capture. */
    BUFFER_FILLING, /**< Currently being written by the RX DMA channel. */
    BUFFER_READY,   /**< Complete and available for display. */
    BUFFER_IN_USE,  /**< Claimed by the VGA core for read-only display. */
} buffer_state_t;

_Static_assert(REQUIRED_SYSTEM_CLOCK_HZ / CAPTURE_CLOCK_DIVIDER ==
                   CAPTURE_PIO_CLOCK_HZ,
               "Capture PIO must use an exact integer clock divider");
_Static_assert(P2000T_CAPTURE_WIDTH % 2u == 0u,
               "Each nominal source dot must produce two samples");
_Static_assert(P2000T_CAPTURE_WORDS_PER_LINE == 60,
               "Each packed scanline must contain sixty words");

/** PIO instance dedicated to RGBS capture. */
static PIO capture_pio = pio1;

/** Claimed capture state-machine index within capture_pio. */
static unsigned capture_sm;

/** DMA channel transferring packed PIO RX words into frame storage. */
static int capture_rx_dma;

/** DMA channel streaming per-frame commands into the PIO TX FIFO. */
static int capture_tx_dma;

/** Spinlock protecting buffer lifecycle, timing, and capture settings. */
static spin_lock_t *buffer_lock;

/** Triple-buffered packed frame storage written only by capture DMA. */
static uint32_t capture_buffers[CAPTURE_BUFFER_COUNT]
                               [P2000T_CAPTURE_WORDS_PER_FRAME];

/** PIO command stream rebuilt between complete frame transfers. */
static uint32_t tx_commands[CAPTURE_TX_COMMAND_COUNT];

/** Current lifecycle state of each capture buffer. */
static buffer_state_t buffer_states[CAPTURE_BUFFER_COUNT];

/** Independent short-lived USB reader hold for each complete frame. */
#if defined(PICO_RP2350) && PICO_RP2350
static bool buffer_usb_holds[CAPTURE_BUFFER_COUNT];
#endif

/** Monotonic source-frame sequence assigned to each completed buffer. */
static uint32_t buffer_sequences[CAPTURE_BUFFER_COUNT];

/** Index of the buffer currently targeted by capture RX DMA. */
static unsigned capture_fill_index;

/** Number of complete frames received since capture started. */
static uint32_t captured_frames;

/** Number of unconsumed ready frames replaced by a newer capture. */
static uint32_t stale_frames_replaced;

/** Interval between the two most recently completed frames. */
static uint32_t last_frame_period_us;

/** Absolute microsecond timestamp of the most recent complete frame. */
static uint64_t last_frame_time_us;

/** Configured one-based source line at which visible capture starts. */
static unsigned first_visible_scanline = P2000T_DEFAULT_FIRST_VISIBLE_SCANLINE;

/** Fine horizontal sampling phase in nominal 126 MHz PIO clock ticks. */
static int sample_phase = P2000T_DEFAULT_SAMPLE_PHASE;

/** Extra phase applied only to odd-numbered physical source lines. */
static int odd_line_phase = P2000T_DEFAULT_ODD_LINE_PHASE;

/** Signed 1/256 addition to the nominal capture PIO clock divider. */
static int sample_rate_trim = P2000T_DEFAULT_SAMPLE_RATE_TRIM;

/** Divider trim currently programmed into the running capture state machine. */
static int applied_sample_rate_trim = P2000T_DEFAULT_SAMPLE_RATE_TRIM;

/** Coarse horizontal sampling offset in nominal source dots. */
static unsigned horizontal_offset = P2000T_DEFAULT_HORIZONTAL_OFFSET;

/**
 * @brief Validate recent complete-frame timing as a credible source signal.
 *
 * @param now Current absolute timestamp in microseconds.
 * @param last_frame_time Timestamp of the most recent complete frame.
 * @param frame_period Interval between the two most recent frames.
 * @return true when a recent frame has an in-range period; otherwise false.
 */
static bool timing_is_locked(uint64_t now, uint64_t last_frame_time,
                             uint32_t frame_period) {
    return last_frame_time != 0u && frame_period >= MINIMUM_FRAME_PERIOD_US &&
           frame_period <= MAXIMUM_FRAME_PERIOD_US &&
           now - last_frame_time <= SIGNAL_LOSS_TIMEOUT_US;
}

/**
 * @brief Compare wrapping 32-bit frame sequence numbers chronologically.
 *
 * The capture pool spans only three adjacent frames, so the signed modular
 * difference remains unambiguous even when the sequence counter wraps.
 *
 * @param candidate Sequence number being considered.
 * @param reference Existing sequence number used as the comparison point.
 * @return true when candidate is chronologically newer than reference.
 */
static bool sequence_is_newer(uint32_t candidate, uint32_t reference) {
    return (int32_t)(candidate - reference) > 0;
}

/**
 * @brief Keep the first sample anchored while changing the PIO divider.
 *
 * The divider affects both the 480-sample span and all preceding PIO delay
 * instructions. Scaling the sync-to-first-sample cycle count inversely leaves
 * the start fixed, making rate trim an independent right-edge adjustment.
 */
static uint32_t rate_compensated_start_delay(int nominal_command,
                                             unsigned overhead_ticks) {
    const int active_divider =
        NOMINAL_CAPTURE_CLOCK_DIVIDER_FIXED + sample_rate_trim;
    const int nominal_total = nominal_command + (int)overhead_ticks;
    const int compensated_total =
        (nominal_total * NOMINAL_CAPTURE_CLOCK_DIVIDER_FIXED +
         active_divider / 2) /
        active_divider;
    return (uint32_t)(compensated_total - (int)overhead_ticks);
}

/**
 * @brief Refresh the PIO TX commands for the next captured frame.
 *
 * This is called only while the prior frame's TX DMA transfer is exhausted.
 */
static void update_tx_commands(void) {
    tx_commands[0] = FRAME_LINE_COUNT;
    tx_commands[1] = first_visible_scanline - 1u;
    const uint32_t qualified_start_delay =
        (uint32_t)(COMBINED_DELAY_COUNT_BIAS + NOMINAL_PHASE_DELAY_COUNT -
                   HORIZONTAL_SYNC_QUALIFICATION_TICKS +
                   (int)horizontal_offset * CAPTURE_TICKS_PER_SOURCE_DOT +
                   sample_phase);
    for (unsigned line = 0; line < P2000T_CAPTURE_HEIGHT; ++line) {
        const unsigned source_scanline = first_visible_scanline + line;
        const int parity_phase =
            (source_scanline & 1u) != 0u ? odd_line_phase : 0;
        /* The first visible line arrives through the initial sync-counting
           path. It enters line_start_delay 32 rather than 33 clocks before
           the qualified path because skip_got_edge itself takes one tick. */
        const bool first_line = line == 0u;
        const int nominal_command =
            (int)qualified_start_delay +
            (first_line ? FIRST_LINE_SYNC_COMPENSATION_TICKS : 0) +
            parity_phase;
        tx_commands[2u + line * 2u] = rate_compensated_start_delay(
            nominal_command,
            first_line ? FIRST_LINE_START_OVERHEAD_TICKS
                       : QUALIFIED_LINE_START_OVERHEAD_TICKS);
        tx_commands[3u + line * 2u] = SOURCE_DOT_LOOP_COUNT;
    }
}

/**
 * @brief Arm both DMA channels to capture one complete source frame.
 */
static void arm_capture_frame(void) {
    dma_channel_set_write_addr(capture_rx_dma,
                               capture_buffers[capture_fill_index], false);
    dma_channel_set_trans_count(capture_rx_dma, P2000T_CAPTURE_WORDS_PER_FRAME,
                                false);
    dma_channel_set_read_addr(capture_tx_dma, tx_commands, false);
    dma_channel_set_trans_count(capture_tx_dma, CAPTURE_TX_COMMAND_COUNT,
                                false);
    dma_start_channel_mask((1u << capture_rx_dma) | (1u << capture_tx_dma));
}

/** Apply a validated signed fractional adjustment to the PIO divider. */
static void apply_sample_rate_trim(void) {
    if (applied_sample_rate_trim == sample_rate_trim) {
        return;
    }
    const int fixed_divider =
        NOMINAL_CAPTURE_CLOCK_DIVIDER_FIXED + sample_rate_trim;
    pio_sm_set_clkdiv_int_frac8(capture_pio, capture_sm,
                                (unsigned)fixed_divider >> 8u,
                                (uint8_t)fixed_divider);
    pio_sm_clkdiv_restart(capture_pio, capture_sm);
    applied_sample_rate_trim = sample_rate_trim;
}

/**
 * @brief Select storage for the next frame without touching displayed data.
 *
 * A free buffer is preferred. If both non-displayed buffers are ready, the
 * older unconsumed frame is replaced because only the newest frame matters.
 *
 * @return Index of a free or replaceable capture buffer.
 */
static unsigned choose_next_fill_buffer(void) {
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_FREE
#if defined(PICO_RP2350) && PICO_RP2350
            && !buffer_usb_holds[i]
#endif
        ) {
            return i;
        }
    }

    unsigned oldest = CAPTURE_BUFFER_COUNT;
    uint32_t oldest_age = 0;
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_READY
#if defined(PICO_RP2350) && PICO_RP2350
            && !buffer_usb_holds[i]
#endif
        ) {
            const uint32_t age = captured_frames - buffer_sequences[i];
            if (oldest == CAPTURE_BUFFER_COUNT || age > oldest_age) {
                oldest = i;
                oldest_age = age;
            }
        }
    }
    hard_assert(oldest < CAPTURE_BUFFER_COUNT);
    ++stale_frames_replaced;
    return oldest;
}

/**
 * @brief Finalize a completed capture and immediately arm the next frame.
 *
 * The handler runs from SRAM to avoid flash stalls in the frame transition.
 */
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
    spin_unlock(buffer_lock, saved);

    /* The VGA core needs buffer_lock at its frame boundary. Rebuilding all
       240 line commands while holding it can make scanvideo miss a physical
       line deadline and substitute its blank fallback scanline. Settings are
       changed only by core 0 outside interrupt context, so they cannot change
       between this IRQ's unlock and return. */
    update_tx_commands();
    apply_sample_rate_trim();
    arm_capture_frame();
}

/**
 * @brief Load, configure, and reset the PIO capture state machine.
 */
static void initialize_capture_pio(void) {
    hard_assert(clock_get_hz(clk_sys) == REQUIRED_SYSTEM_CLOCK_HZ);
    const unsigned program_offset =
        pio_add_program(capture_pio, &p2000t_capture_program);
    capture_sm = pio_claim_unused_sm(capture_pio, true);

    for (unsigned pin = P2000T_INPUT_PIN_BASE; pin < P2000T_INPUT_PIN_BASE + 4u;
         ++pin) {
        pio_gpio_init(capture_pio, pin);
        gpio_disable_pulls(pin);
    }

    pio_sm_config config =
        p2000t_capture_program_get_default_config(program_offset);
    sm_config_set_in_pins(&config, P2000T_INPUT_PIN_BASE);
    sm_config_set_jmp_pin(&config, P2000T_SYNC_PIN);
    sm_config_set_in_shift(&config, false, true, 32);
    sm_config_set_clkdiv_int_frac8(&config, CAPTURE_CLOCK_DIVIDER,
                                   CAPTURE_CLOCK_DIVIDER_FRACTION);
    pio_sm_set_consecutive_pindirs(capture_pio, capture_sm,
                                   P2000T_INPUT_PIN_BASE, 4, false);
    pio_sm_init(capture_pio, capture_sm, program_offset, &config);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);
}

/**
 * @brief Claim and configure the paired RX and TX DMA channels.
 */
static void initialize_capture_dma(void) {
    capture_rx_dma = dma_claim_unused_channel(true);
    capture_tx_dma = dma_claim_unused_channel(true);

    dma_channel_config rx = dma_channel_get_default_config(capture_rx_dma);
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_32);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, true);
    channel_config_set_dreq(&rx, pio_get_dreq(capture_pio, capture_sm, false));
    dma_channel_configure(capture_rx_dma, &rx, capture_buffers[0],
                          &capture_pio->rxf[capture_sm],
                          P2000T_CAPTURE_WORDS_PER_FRAME, false);

    dma_channel_config tx = dma_channel_get_default_config(capture_tx_dma);
    channel_config_set_transfer_data_size(&tx, DMA_SIZE_32);
    channel_config_set_read_increment(&tx, true);
    channel_config_set_write_increment(&tx, false);
    channel_config_set_dreq(&tx, pio_get_dreq(capture_pio, capture_sm, true));
    dma_channel_configure(capture_tx_dma, &tx, &capture_pio->txf[capture_sm],
                          tx_commands, CAPTURE_TX_COMMAND_COUNT, false);

    dma_channel_set_irq1_enabled(capture_rx_dma, true);
    irq_set_exclusive_handler(DMA_IRQ_1, capture_dma_irq);
    irq_set_priority(DMA_IRQ_1, 0x80);
    irq_set_enabled(DMA_IRQ_1, true);
}

void p2000t_capture_start(void) {
    bi_decl(bi_4pins_with_names(P2000T_SYNC_PIN, "P2000T CSYNC_IN",
                                P2000T_RED_PIN, "P2000T RED_IN",
                                P2000T_GREEN_PIN, "P2000T GREEN_IN",
                                P2000T_BLUE_PIN, "P2000T BLUE_IN"));

    buffer_lock = spin_lock_instance((unsigned)spin_lock_claim_unused(true));
    update_tx_commands();

    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        buffer_states[i] = BUFFER_FREE;
#if defined(PICO_RP2350) && PICO_RP2350
        buffer_usb_holds[i] = false;
#endif
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
    hard_assert(sequence != NULL);
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    int latest = -1;
    uint32_t latest_sequence = 0;
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        if (buffer_states[i] == BUFFER_READY &&
            (latest < 0 ||
             sequence_is_newer(buffer_sequences[i], latest_sequence))) {
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

#if defined(PICO_RP2350) && PICO_RP2350
int p2000t_capture_acquire_latest_frame_for_usb(uint32_t *sequence) {
    hard_assert(sequence != NULL);
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    int latest = -1;
    uint32_t latest_sequence = 0;
    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        const bool complete = buffer_states[i] == BUFFER_READY ||
                              buffer_states[i] == BUFFER_IN_USE;
        if (complete && !buffer_usb_holds[i] &&
            (latest < 0 ||
             sequence_is_newer(buffer_sequences[i], latest_sequence))) {
            latest = (int)i;
            latest_sequence = buffer_sequences[i];
        }
    }
    if (latest >= 0) {
        buffer_usb_holds[latest] = true;
        *sequence = latest_sequence;
    }
    spin_unlock(buffer_lock, saved);
    return latest;
}
#endif

void p2000t_capture_release_frame(unsigned buffer_index) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (buffer_states[buffer_index] == BUFFER_IN_USE) {
        buffer_states[buffer_index] = BUFFER_FREE;
    }
    spin_unlock(buffer_lock, saved);
}

#if defined(PICO_RP2350) && PICO_RP2350
void p2000t_capture_release_frame_from_usb(unsigned buffer_index) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    buffer_usb_holds[buffer_index] = false;
    spin_unlock(buffer_lock, saved);
}
#endif

const uint32_t *p2000t_capture_buffer(unsigned buffer_index) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    return capture_buffers[buffer_index];
}

void p2000t_capture_get_stats(p2000t_capture_stats_t *stats) {
    hard_assert(stats != NULL);
    const uint64_t now = time_us_64();
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    *stats = (p2000t_capture_stats_t){
        .captured_frames = captured_frames,
        .stale_frames_replaced = stale_frames_replaced,
        .last_frame_period_us = last_frame_period_us,
        .first_visible_scanline = first_visible_scanline,
        .sample_phase = sample_phase,
        .odd_line_phase = odd_line_phase,
        .sample_rate_trim = sample_rate_trim,
        .horizontal_offset = horizontal_offset,
        .signal_present =
            timing_is_locked(now, last_frame_time_us, last_frame_period_us),
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
    if (phase < P2000T_MIN_SAMPLE_PHASE || phase > P2000T_MAX_SAMPLE_PHASE) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    sample_phase = phase;
    spin_unlock(buffer_lock, saved);
    return true;
}

bool p2000t_capture_set_odd_line_phase(int phase) {
    if (phase < P2000T_MIN_ODD_LINE_PHASE ||
        phase > P2000T_MAX_ODD_LINE_PHASE) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    odd_line_phase = phase;
    spin_unlock(buffer_lock, saved);
    return true;
}

bool p2000t_capture_set_sample_rate_trim(int trim) {
    if (trim < P2000T_MIN_SAMPLE_RATE_TRIM ||
        trim > P2000T_MAX_SAMPLE_RATE_TRIM) {
        return false;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    sample_rate_trim = trim;
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
