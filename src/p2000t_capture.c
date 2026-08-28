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
#include "p2000t_control_protocol.h"
#include "pico/binary_info.h"
#include "pico/stdlib.h"

#if defined(PICO_RP2350) && PICO_RP2350
#include "p2000t_window_capture.pio.h"
#endif

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
    FIRST_LINE_START_OVERHEAD_TICKS = QUALIFIED_LINE_START_OVERHEAD_TICKS -
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
#if defined(PICO_RP2350) && PICO_RP2350
    WINDOW_SAMPLES_PER_OUTPUT = 3,
    /**< Consecutive 126 MHz samples surrounding each retained sample. */
    WINDOW_SAMPLES_PER_SOURCE_DOT = 2 * WINDOW_SAMPLES_PER_OUTPUT,
    WINDOW_SAMPLES_PER_LINE = P2000T_CAPTURE_WIDTH * WINDOW_SAMPLES_PER_OUTPUT,
    WINDOW_WORDS_PER_LINE = WINDOW_SAMPLES_PER_LINE / 8,
    WINDOW_LOOKUP_SIZE = 4096,
    WINDOW_POLICY_COUNT = 4,
    WINDOW_CENTER_ALIGNMENT_TICKS = 4,
/**< Extra start delay aligning window centers to normal ticks 6 and 16. */
#endif
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
#if defined(PICO_RP2350) && PICO_RP2350
_Static_assert(WINDOW_SAMPLES_PER_LINE % 8u == 0u,
               "Windowed lines must contain complete DMA words");
_Static_assert(WINDOW_WORDS_PER_LINE == 180,
               "Six-tap scanlines must contain 180 packed words");
#endif

/** PIO instance dedicated to RGBS capture. */
static PIO capture_pio = pio1;

/** Claimed capture state-machine index within capture_pio. */
static unsigned capture_sm;

/** PIO1 instruction space is claimed once; later mode switches overwrite it. */
static bool capture_program_memory_claimed;

/** DMA channel transferring packed PIO RX words into frame storage. */
static int capture_rx_dma;

#if defined(PICO_RP2350) && PICO_RP2350
/** Second ping-pong RX channel used only by six-tap scanline capture. */
static int capture_window_rx_dma;
#endif

/** DMA channel streaming per-frame commands into the PIO TX FIFO. */
static int capture_tx_dma;

/** Spinlock protecting buffer lifecycle, timing, and capture settings. */
static spin_lock_t *buffer_lock;

/** Triple-buffered packed frame storage written only by capture DMA. */
static uint32_t capture_buffers[CAPTURE_BUFFER_COUNT]
                               [P2000T_CAPTURE_WORDS_PER_FRAME];

/** Reconstruction evidence remains attached to its immutable frame buffer. */
static p2000t_reconstruction_diagnostics_t
    buffer_diagnostics[CAPTURE_BUFFER_COUNT];

#if defined(PICO_RP2350) && PICO_RP2350
/** Two raw line buffers let DMA capture one line while the IRQ decodes one. */
static uint32_t window_line_buffers[2][WINDOW_WORDS_PER_LINE]
    __attribute__((aligned(4)));

/** Prebuilt policies avoid table construction inside a frame-boundary IRQ. */
static uint32_t window_lookup_tables[WINDOW_POLICY_COUNT][WINDOW_LOOKUP_SIZE];

/** Evidence accumulated while the current six-tap frame is being decoded. */
static p2000t_reconstruction_diagnostics_t current_window_diagnostics;

/** Evidence from the most recently published six-tap frame. */
static p2000t_reconstruction_diagnostics_t last_window_diagnostics;

/** Zero-based line expected from the next completed window DMA channel. */
static unsigned window_completed_lines;

/** Number of complete frames produced by the windowed engine. */
static uint32_t windowed_frames;

/** Total late or coalesced scanline completion observations. */
static uint32_t line_deadline_misses;
#endif

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

/** Whether p2000t_capture_start has initialized locks and hardware. */
static bool capture_started;

/** Live mode requested by core 0 and adopted on a source-frame boundary. */
static unsigned requested_reconstruction_mode =
    P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION;

/** Mode which produced the frame currently being captured. */
static unsigned active_reconstruction_mode =
    P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION;

/** Active PIO/DMA engine identifier from p2000t_control_protocol.h. */
static unsigned active_capture_engine = P2000T_CAPTURE_ENGINE_TWO_TAP;

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

/** Return whether a reconstruction mode requires the Pico 2 six-tap engine. */
static bool reconstruction_uses_window(unsigned reconstruction) {
    return reconstruction >= P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_FIRST;
}

#if defined(PICO_RP2350) && PICO_RP2350
/** Build all policies once before line-rate interrupts are enabled. */
static void initialize_window_lookup_tables(void) {
    for (unsigned policy = 0; policy < WINDOW_POLICY_COUNT; ++policy) {
        for (unsigned packed = 0; packed < WINDOW_LOOKUP_SIZE; ++packed) {
            window_lookup_tables[policy][packed] = p2000t_window_lookup_entry(
                (uint8_t)(packed >> 8u), (uint8_t)(packed >> 4u),
                (uint8_t)packed, (p2000t_window_policy_t)policy,
                P2000T_RED_CHANNEL, P2000T_GREEN_CHANNEL, P2000T_BLUE_CHANNEL);
        }
    }
}
#endif

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
                   sample_phase
#if defined(PICO_RP2350) && PICO_RP2350
                   + (active_capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED
                          ? WINDOW_CENTER_ALIGNMENT_TICKS
                          : 0)
#endif
        );
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
            nominal_command, first_line ? FIRST_LINE_START_OVERHEAD_TICKS
                                        : QUALIFIED_LINE_START_OVERHEAD_TICKS);
        tx_commands[3u + line * 2u] = SOURCE_DOT_LOOP_COUNT;
    }
}

/**
 * @brief Arm both DMA channels to capture one complete source frame.
 */
static void arm_two_tap_capture_frame(void) {
    dma_channel_set_write_addr(capture_rx_dma,
                               capture_buffers[capture_fill_index], false);
    dma_channel_set_trans_count(capture_rx_dma, P2000T_CAPTURE_WORDS_PER_FRAME,
                                false);
    dma_channel_set_read_addr(capture_tx_dma, tx_commands, false);
    dma_channel_set_trans_count(capture_tx_dma, CAPTURE_TX_COMMAND_COUNT,
                                false);
    dma_start_channel_mask((1u << capture_rx_dma) | (1u << capture_tx_dma));
}

#if defined(PICO_RP2350) && PICO_RP2350
/** Arm the ping-pong scanline DMAs and the continuous frame command stream. */
static void arm_window_capture_frame(void) {
    window_completed_lines = 0u;
    current_window_diagnostics = (p2000t_reconstruction_diagnostics_t){
        .reconstruction_mode = (uint8_t)active_reconstruction_mode,
        .capture_engine = P2000T_CAPTURE_ENGINE_WINDOWED,
        .samples_per_output = WINDOW_SAMPLES_PER_OUTPUT,
    };
    dma_channel_set_write_addr(capture_rx_dma, window_line_buffers[0], false);
    dma_channel_set_trans_count(capture_rx_dma, WINDOW_WORDS_PER_LINE, false);
    dma_channel_set_write_addr(capture_window_rx_dma, window_line_buffers[1],
                               false);
    dma_channel_set_trans_count(capture_window_rx_dma, WINDOW_WORDS_PER_LINE,
                                false);
    dma_channel_set_read_addr(capture_tx_dma, tx_commands, false);
    dma_channel_set_trans_count(capture_tx_dma, CAPTURE_TX_COMMAND_COUNT,
                                false);
    dma_start_channel_mask((1u << capture_rx_dma) | (1u << capture_tx_dma));
}
#endif

/** Arm the active two-tap or six-tap engine for one complete source frame. */
static void arm_capture_frame(void) {
#if defined(PICO_RP2350) && PICO_RP2350
    if (active_capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED) {
        arm_window_capture_frame();
        return;
    }
#endif
    arm_two_tap_capture_frame();
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

/** Configure the state machine and RX DMAs for the newly active engine. */
static void configure_capture_engine(void);

/**
 * @brief Publish one complete buffer and prepare the active mode for another.
 *
 * @param diagnostics Evidence associated with the just-completed frame.
 */
static void __not_in_flash_func(finish_capture_frame)(
    const p2000t_reconstruction_diagnostics_t *diagnostics) {
    const uint64_t now = time_us_64();
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    if (last_frame_time_us != 0u) {
        last_frame_period_us = (uint32_t)(now - last_frame_time_us);
    }
    last_frame_time_us = now;

    const unsigned completed = capture_fill_index;
    buffer_diagnostics[completed] = *diagnostics;
#if defined(PICO_RP2350) && PICO_RP2350
    if (diagnostics->capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED) {
        last_window_diagnostics = *diagnostics;
    }
#endif
    buffer_sequences[completed] = ++captured_frames;
    buffer_states[completed] = BUFFER_READY;

    capture_fill_index = choose_next_fill_buffer();
    buffer_states[capture_fill_index] = BUFFER_FILLING;
    const unsigned requested = requested_reconstruction_mode;
    const unsigned requested_engine = reconstruction_uses_window(requested)
                                          ? P2000T_CAPTURE_ENGINE_WINDOWED
                                          : P2000T_CAPTURE_ENGINE_TWO_TAP;
    const bool engine_changed = requested_engine != active_capture_engine;
    active_reconstruction_mode = requested;
    active_capture_engine = requested_engine;
    spin_unlock(buffer_lock, saved);

    /* The VGA core needs buffer_lock at its frame boundary. Rebuilding all
       240 line commands while holding it can make scanvideo miss a physical
       line deadline and substitute its blank fallback scanline. Settings are
       changed only by core 0 outside interrupt context, so they cannot change
       between this IRQ's unlock and return. */
    if (engine_changed) {
        configure_capture_engine();
    }
    update_tx_commands();
    apply_sample_rate_trim();
    arm_capture_frame();
}

#if defined(PICO_RP2350) && PICO_RP2350
/** Decode one 720-byte six-tap line into the normal packed frame geometry. */
static void __not_in_flash_func(reconstruct_window_line)(
    const uint32_t source[WINDOW_WORDS_PER_LINE], uint32_t *destination) {
    const p2000t_window_policy_t policy =
        (p2000t_window_policy_t)(active_reconstruction_mode -
                                 P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_FIRST);
    const uint32_t *lookup = window_lookup_tables[policy];
    for (unsigned group = 0; group < P2000T_CAPTURE_WORDS_PER_LINE; ++group) {
        destination[group] = p2000t_reconstruct_window_group(
            source[group * 3u], source[group * 3u + 1u],
            source[group * 3u + 2u], lookup, &current_window_diagnostics);
    }
}

/** Service one or two completed ping-pong line buffers in chronological order.
 */
static void __not_in_flash_func(service_window_dma_irq)(uint32_t pending) {
    const uint32_t rx_mask = 1u << capture_rx_dma;
    const uint32_t alternate_mask = 1u << capture_window_rx_dma;
    const uint32_t relevant = pending & (rx_mask | alternate_mask);
    dma_hw->ints1 = relevant;

    if (relevant == (rx_mask | alternate_mask)) {
        ++current_window_diagnostics.line_deadline_misses;
        ++line_deadline_misses;
    }

    uint32_t remaining = relevant;
    while (remaining != 0u && window_completed_lines < P2000T_CAPTURE_HEIGHT) {
        const bool expected_alternate = (window_completed_lines & 1u) != 0u;
        const int expected_channel =
            expected_alternate ? capture_window_rx_dma : capture_rx_dma;
        const uint32_t expected_mask = 1u << expected_channel;
        int completed_channel = expected_channel;
        if ((remaining & expected_mask) == 0u) {
            completed_channel =
                expected_alternate ? capture_rx_dma : capture_window_rx_dma;
            ++current_window_diagnostics.line_deadline_misses;
            ++line_deadline_misses;
        }
        const unsigned buffer = completed_channel == capture_rx_dma ? 0u : 1u;
        reconstruct_window_line(
            window_line_buffers[buffer],
            &capture_buffers[capture_fill_index]
                            [window_completed_lines *
                             P2000T_CAPTURE_WORDS_PER_LINE]);
        ++window_completed_lines;
        remaining &= ~(1u << completed_channel);

        if (window_completed_lines < P2000T_CAPTURE_HEIGHT) {
            dma_channel_set_write_addr(completed_channel,
                                       window_line_buffers[buffer], false);
            dma_channel_set_trans_count(completed_channel,
                                        WINDOW_WORDS_PER_LINE, false);
        }
    }

    if (window_completed_lines == P2000T_CAPTURE_HEIGHT) {
        dma_channel_abort(capture_rx_dma);
        dma_channel_abort(capture_window_rx_dma);
        ++windowed_frames;
        finish_capture_frame(&current_window_diagnostics);
    } else {
        /* If both completions coalesced, the second channel attempted to chain
           into a still-complete first channel. Restart the now-rearmed channel
           explicitly so a transient CPU delay cannot permanently stop the
           ping-pong pipeline. The ordinary one-line path is already busy. */
        const int next_channel = (window_completed_lines & 1u) != 0u
                                     ? capture_window_rx_dma
                                     : capture_rx_dma;
        if (!dma_channel_is_busy(next_channel)) {
            dma_channel_start(next_channel);
        }
    }
}
#endif

/**
 * @brief Finalize complete two-tap frames or reconstruct window scanlines.
 *
 * The handler runs from SRAM to avoid flash stalls in the capture deadline.
 */
static void __not_in_flash_func(capture_dma_irq)(void) {
#if defined(PICO_RP2350) && PICO_RP2350
    const uint32_t pending = dma_hw->ints1;
    if (active_capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED) {
        service_window_dma_irq(pending);
        return;
    }
#endif
    dma_hw->ints1 = 1u << capture_rx_dma;
    const p2000t_reconstruction_diagnostics_t diagnostics = {
        .reconstruction_mode = (uint8_t)active_reconstruction_mode,
        .capture_engine = P2000T_CAPTURE_ENGINE_TWO_TAP,
        .samples_per_output = 1u,
    };
    finish_capture_frame(&diagnostics);
}

/**
 * @brief Load, configure, and reset the PIO capture state machine.
 */
static void initialize_capture_pio_resources(void) {
    hard_assert(clock_get_hz(clk_sys) == REQUIRED_SYSTEM_CLOCK_HZ);
    capture_sm = pio_claim_unused_sm(capture_pio, true);

    for (unsigned pin = P2000T_INPUT_PIN_BASE; pin < P2000T_INPUT_PIN_BASE + 4u;
         ++pin) {
        pio_gpio_init(capture_pio, pin);
        gpio_disable_pulls(pin);
    }
}

/** Replace the sole PIO1 program while its capture state machine is stopped. */
static void load_capture_program(void) {
    pio_sm_set_enabled(capture_pio, capture_sm, false);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);
    pio_sm_config config;
    const pio_program_t *program;
#if defined(PICO_RP2350) && PICO_RP2350
    if (active_capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED) {
        program = P2000T_SYNC_PIN == 16 ? &p2000t_window_capture_sync16_program
                                        : &p2000t_window_capture_sync17_program;
        config =
            P2000T_SYNC_PIN == 16
                ? p2000t_window_capture_sync16_program_get_default_config(0u)
                : p2000t_window_capture_sync17_program_get_default_config(0u);
    } else
#endif
    {
        program = &p2000t_capture_program;
        config = p2000t_capture_program_get_default_config(0u);
    }
    hard_assert(program->length == 32u);
    if (!capture_program_memory_claimed) {
        hard_assert(pio_add_program(capture_pio, program) == 0);
        capture_program_memory_claimed = true;
    } else {
        /* This runs from the DMA IRQ. Direct writes are safe because PIO1 is
           dedicated to capture and its sole state machine is disabled. Using
           pio_clear/add here would take the SDK's global claim lock and could
           deadlock if the IRQ pre-empted another resource claim. */
        for (unsigned instruction = 0; instruction < 32u; ++instruction) {
            capture_pio->instr_mem[instruction] =
                program->instructions[instruction];
        }
    }
    sm_config_set_in_pins(&config, P2000T_INPUT_PIN_BASE);
    sm_config_set_jmp_pin(&config, P2000T_SYNC_PIN);
    sm_config_set_in_shift(&config, false, true, 32);
    const int fixed_divider =
        NOMINAL_CAPTURE_CLOCK_DIVIDER_FIXED + sample_rate_trim;
    sm_config_set_clkdiv_int_frac8(&config, (unsigned)fixed_divider >> 8u,
                                   (uint8_t)fixed_divider);
    pio_sm_set_consecutive_pindirs(capture_pio, capture_sm,
                                   P2000T_INPUT_PIN_BASE, 4, false);
    pio_sm_init(capture_pio, capture_sm, 0u, &config);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);
    applied_sample_rate_trim = sample_rate_trim;
}

/**
 * @brief Claim and configure the paired RX and TX DMA channels.
 */
static void initialize_capture_dma_resources(void) {
    capture_rx_dma = dma_claim_unused_channel(true);
#if defined(PICO_RP2350) && PICO_RP2350
    capture_window_rx_dma = dma_claim_unused_channel(true);
#endif
    capture_tx_dma = dma_claim_unused_channel(true);

    dma_channel_config tx = dma_channel_get_default_config(capture_tx_dma);
    channel_config_set_transfer_data_size(&tx, DMA_SIZE_32);
    channel_config_set_read_increment(&tx, true);
    channel_config_set_write_increment(&tx, false);
    channel_config_set_dreq(&tx, pio_get_dreq(capture_pio, capture_sm, true));
    dma_channel_configure(capture_tx_dma, &tx, &capture_pio->txf[capture_sm],
                          tx_commands, CAPTURE_TX_COMMAND_COUNT, false);

    dma_channel_set_irq1_enabled(capture_rx_dma, true);
#if defined(PICO_RP2350) && PICO_RP2350
    dma_channel_set_irq1_enabled(capture_window_rx_dma, false);
#endif
    irq_set_exclusive_handler(DMA_IRQ_1, capture_dma_irq);
    irq_set_priority(DMA_IRQ_1, 0x80);
    irq_set_enabled(DMA_IRQ_1, true);
}

/** Configure one RX channel for packed PIO words and an optional chain. */
static void configure_rx_channel(int channel, void *destination,
                                 uint32_t word_count, int chain_to) {
    dma_channel_config rx = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_32);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, true);
    channel_config_set_dreq(&rx, pio_get_dreq(capture_pio, capture_sm, false));
    channel_config_set_chain_to(&rx, (unsigned)chain_to);
    dma_channel_configure(channel, &rx, destination,
                          &capture_pio->rxf[capture_sm], word_count, false);
}

/** Stop old transfers, load the selected PIO program, and configure RX DMA. */
static void configure_capture_engine(void) {
    pio_sm_set_enabled(capture_pio, capture_sm, false);
    dma_channel_abort(capture_rx_dma);
#if defined(PICO_RP2350) && PICO_RP2350
    dma_channel_abort(capture_window_rx_dma);
#endif
    dma_channel_abort(capture_tx_dma);
    dma_hw->ints1 = 1u << capture_rx_dma;
#if defined(PICO_RP2350) && PICO_RP2350
    dma_hw->ints1 = 1u << capture_window_rx_dma;
#endif
    load_capture_program();

#if defined(PICO_RP2350) && PICO_RP2350
    if (active_capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED) {
        configure_rx_channel(capture_rx_dma, window_line_buffers[0],
                             WINDOW_WORDS_PER_LINE, capture_window_rx_dma);
        configure_rx_channel(capture_window_rx_dma, window_line_buffers[1],
                             WINDOW_WORDS_PER_LINE, capture_rx_dma);
        dma_channel_set_irq1_enabled(capture_rx_dma, true);
        dma_channel_set_irq1_enabled(capture_window_rx_dma, true);
        pio_sm_set_enabled(capture_pio, capture_sm, true);
        return;
    }
    dma_channel_set_irq1_enabled(capture_window_rx_dma, false);
#endif
    configure_rx_channel(capture_rx_dma, capture_buffers[capture_fill_index],
                         P2000T_CAPTURE_WORDS_PER_FRAME, capture_rx_dma);
    dma_channel_set_irq1_enabled(capture_rx_dma, true);
    pio_sm_set_enabled(capture_pio, capture_sm, true);
}

void p2000t_capture_start(void) {
    bi_decl(bi_4pins_with_names(P2000T_SYNC_PIN, "P2000T CSYNC_IN",
                                P2000T_RED_PIN, "P2000T RED_IN",
                                P2000T_GREEN_PIN, "P2000T GREEN_IN",
                                P2000T_BLUE_PIN, "P2000T BLUE_IN"));

    buffer_lock = spin_lock_instance((unsigned)spin_lock_claim_unused(true));
#if defined(PICO_RP2350) && PICO_RP2350
    initialize_window_lookup_tables();
#endif
    active_reconstruction_mode = requested_reconstruction_mode;
    active_capture_engine =
        reconstruction_uses_window(active_reconstruction_mode)
            ? P2000T_CAPTURE_ENGINE_WINDOWED
            : P2000T_CAPTURE_ENGINE_TWO_TAP;
    update_tx_commands();

    for (unsigned i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
        buffer_states[i] = BUFFER_FREE;
#if defined(PICO_RP2350) && PICO_RP2350
        buffer_usb_holds[i] = false;
#endif
        buffer_sequences[i] = 0;
        buffer_diagnostics[i] = (p2000t_reconstruction_diagnostics_t){0};
    }
    capture_fill_index = 0;
    buffer_states[0] = BUFFER_FILLING;

    initialize_capture_pio_resources();
    initialize_capture_dma_resources();
    configure_capture_engine();
    arm_capture_frame();
    pio_sm_set_enabled(capture_pio, capture_sm, true);
    capture_started = true;
}

void p2000t_capture_resume_after_flash(void) {
    hard_assert(capture_started);
    irq_set_enabled(DMA_IRQ_1, false);
    configure_capture_engine();
    update_tx_commands();
    apply_sample_rate_trim();
    arm_capture_frame();
    irq_set_enabled(DMA_IRQ_1, true);
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

void p2000t_capture_get_frame_diagnostics(
    unsigned buffer_index, p2000t_reconstruction_diagnostics_t *diagnostics) {
    hard_assert(buffer_index < CAPTURE_BUFFER_COUNT);
    hard_assert(diagnostics != NULL);
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    *diagnostics = buffer_diagnostics[buffer_index];
    spin_unlock(buffer_lock, saved);
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
#if defined(PICO_RP2350) && PICO_RP2350
        .windowed_frames = windowed_frames,
        .line_deadline_misses = line_deadline_misses,
        .last_corrected_samples = last_window_diagnostics.corrected_samples,
        .last_ambiguous_samples = last_window_diagnostics.ambiguous_samples,
        .last_red_corrections = last_window_diagnostics.red_corrections,
        .last_green_corrections = last_window_diagnostics.green_corrections,
        .last_blue_corrections = last_window_diagnostics.blue_corrections,
        .window_supported = true,
#else
        .window_supported = false,
#endif
        .reconstruction_mode = (uint8_t)requested_reconstruction_mode,
        .capture_engine = (uint8_t)active_capture_engine,
        .window_samples =
            active_capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED ? 3u : 1u,
        .engine_switch_pending =
            reconstruction_uses_window(requested_reconstruction_mode) !=
            (active_capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED),
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

bool p2000t_capture_set_reconstruction_mode(unsigned reconstruction) {
    if (reconstruction >= P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT) {
        return false;
    }
#if !defined(PICO_RP2350) || !PICO_RP2350
    if (reconstruction_uses_window(reconstruction)) {
        return false;
    }
#endif
    if (!capture_started) {
        requested_reconstruction_mode = reconstruction;
        active_reconstruction_mode = reconstruction;
        return true;
    }
    const uint32_t saved = spin_lock_blocking(buffer_lock);
    requested_reconstruction_mode = reconstruction;
    spin_unlock(buffer_lock, saved);
    return true;
}
