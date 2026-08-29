/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p2000t_diagnostics.c
 * @brief Triggered Pico 2 PIO2 logic-analyzer acquisition and USB transport.
 */

#include "p2000t_diagnostics.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "p2000t_capture.h"
#include "p2000t_control_protocol.h"
#include "p2000t_diagnostic_protocol.h"
#include "p2000t_diagnostics.pio.h"
#include "p2000t_shared_scratch.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

#if !defined(PICO_RP2350) || !PICO_RP2350
#error "High-resolution diagnostics require Pico 2 PIO2 and SRAM"
#endif

enum {
    TRIGGER_PIO_CLOCK_DIVIDER = 2,
    RAW_PIO_CLOCK_DIVIDER = 1,
    TIMING_PIO_CLOCK_DIVIDER = 4,
    DIAGNOSTIC_TX_SERVICE_BUDGET = 64,
    DIAGNOSTIC_TX_DRAIN_US = 10000,
    CAPTURE_OVERRUN_TOLERANCE_US = 50,
};

typedef enum {
    DIAGNOSTIC_IDLE,
    DIAGNOSTIC_SENDING_SESSION,
    DIAGNOSTIC_CAPTURING_TIMING,
    DIAGNOSTIC_SENDING_TIMING,
    DIAGNOSTIC_CAPTURING_RAW,
    DIAGNOSTIC_SENDING_RAW,
    DIAGNOSTIC_SENDING_COMPLETE,
    DIAGNOSTIC_DRAINING_TX,
    DIAGNOSTIC_WAITING_ACK,
} diagnostic_state_t;

/** The timing trace is the largest record; raw bursts reuse the same SRAM. */
#define diagnostic_samples p2000t_high_resolution_scratch
static uint8_t diagnostic_header[P2000T_DIAGNOSTIC_HEADER_SIZE];

static diagnostic_state_t diagnostic_state;
static bool resources_initialized;
static bool cancel_requested;
static PIO diagnostic_pio = pio2;
static unsigned trigger_sm;
static unsigned sampler_sm;
static unsigned trigger_offset;
static unsigned timing_sampler_offset;
static unsigned rgbs_sampler_offset;
static int sample_dma;
static int stop_dma;
static uint32_t pio_disable_value;
static unsigned requested_start_line;
static unsigned requested_line_count;
static unsigned requested_repetitions;
static unsigned current_repetition;
static uint32_t session_id;
static uint32_t record_sequence;
static const uint8_t *tx_payload;
static size_t tx_payload_size;
static size_t tx_header_offset;
static size_t tx_payload_offset;
static size_t tx_guard_offset;
static uint8_t tx_guard[P2000T_DIAGNOSTIC_GUARD_SIZE];
static volatile bool capture_finished;
static volatile uint32_t capture_started_us;
static volatile uint32_t capture_duration_us;
static volatile bool capture_stalled;
static uint32_t expected_capture_duration_us;
static diagnostic_state_t drained_record_state;
static uint32_t tx_drain_started_us;
static unsigned pending_host_action;

static void __not_in_flash_func(diagnostic_trigger_irq)(void) {
    pio_interrupt_clear(diagnostic_pio, 1u);
    capture_started_us = time_us_32();
}

static void __not_in_flash_func(diagnostic_dma_irq)(void) {
    dma_irqn_acknowledge_channel(2u, (unsigned)stop_dma);
    capture_duration_us = time_us_32() - capture_started_us;
    capture_stalled =
        (diagnostic_pio->fdebug & (1u << sampler_sm)) != 0u;
    capture_finished = true;
}

static void store_u16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static uint32_t diagnostic_crc32(const uint8_t *data, size_t length) {
    static const uint32_t nibble_table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        crc = (crc >> 4u) ^ nibble_table[crc & 0x0fu];
        crc = (crc >> 4u) ^ nibble_table[crc & 0x0fu];
    }
    return ~crc;
}

static void initialize_resources(void) {
    if (resources_initialized) {
        return;
    }
    trigger_offset = pio_add_program(
        diagnostic_pio, &p2000t_diagnostic_trigger_program);
    timing_sampler_offset = pio_add_program(
        diagnostic_pio, &p2000t_diagnostic_timing_sampler_program);
    rgbs_sampler_offset = pio_add_program(
        diagnostic_pio, &p2000t_diagnostic_rgbs_sampler_program);
    trigger_sm = pio_claim_unused_sm(diagnostic_pio, true);
    sampler_sm = pio_claim_unused_sm(diagnostic_pio, true);
    sample_dma = dma_claim_unused_channel(true);
    stop_dma = dma_claim_unused_channel(true);
    pio_set_irq0_source_enabled(diagnostic_pio, pis_interrupt1, true);
    irq_set_exclusive_handler(PIO2_IRQ_0, diagnostic_trigger_irq);
    irq_set_priority(PIO2_IRQ_0, 0x80);
    irq_set_enabled(PIO2_IRQ_0, true);
    dma_irqn_set_channel_enabled(2u, (unsigned)stop_dma, true);
    irq_set_exclusive_handler(DMA_IRQ_2, diagnostic_dma_irq);
    irq_set_priority(DMA_IRQ_2, 0x80);
    irq_set_enabled(DMA_IRQ_2, true);
    resources_initialized = true;
}

static void stop_capture_hardware(void) {
    if (!resources_initialized) {
        return;
    }
    dma_channel_abort(sample_dma);
    dma_channel_abort(stop_dma);
    pio_sm_set_enabled(diagnostic_pio, trigger_sm, false);
    pio_sm_set_enabled(diagnostic_pio, sampler_sm, false);
    pio_sm_clear_fifos(diagnostic_pio, trigger_sm);
    pio_sm_clear_fifos(diagnostic_pio, sampler_sm);
    pio_interrupt_clear(diagnostic_pio, 0u);
    pio_interrupt_clear(diagnostic_pio, 1u);
}

static void initialize_trigger(void) {
    pio_sm_config config = p2000t_diagnostic_trigger_program_get_default_config(
        trigger_offset);
    sm_config_set_jmp_pin(&config, P2000T_SYNC_PIN);
    sm_config_set_clkdiv_int_frac8(&config, TRIGGER_PIO_CLOCK_DIVIDER, 0u);
    pio_sm_init(diagnostic_pio, trigger_sm, trigger_offset, &config);
    pio_sm_clear_fifos(diagnostic_pio, trigger_sm);
    pio_sm_restart(diagnostic_pio, trigger_sm);
}

static void arm_capture(bool timing) {
    stop_capture_hardware();
    initialize_trigger();

    const unsigned sampler_offset =
        timing ? timing_sampler_offset : rgbs_sampler_offset;
    pio_sm_config config =
        timing
            ? p2000t_diagnostic_timing_sampler_program_get_default_config(
                  sampler_offset)
            : p2000t_diagnostic_rgbs_sampler_program_get_default_config(
                  sampler_offset);
    sm_config_set_in_pins(&config,
                          timing ? P2000T_SYNC_PIN : P2000T_INPUT_PIN_BASE);
    sm_config_set_in_shift(&config, false, true, 32u);
    sm_config_set_clkdiv_int_frac8(
        &config, timing ? TIMING_PIO_CLOCK_DIVIDER : RAW_PIO_CLOCK_DIVIDER,
        0u);
    pio_sm_init(diagnostic_pio, sampler_sm, sampler_offset, &config);
    pio_sm_clear_fifos(diagnostic_pio, sampler_sm);
    pio_sm_restart(diagnostic_pio, sampler_sm);
    pio_interrupt_clear(diagnostic_pio, 0u);
    pio_interrupt_clear(diagnostic_pio, 1u);

    dma_channel_config dma = dma_channel_get_default_config(sample_dma);
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_read_increment(&dma, false);
    channel_config_set_write_increment(&dma, true);
    channel_config_set_dreq(
        &dma, pio_get_dreq(diagnostic_pio, sampler_sm, false));
    channel_config_set_chain_to(&dma, (unsigned)stop_dma);
    const unsigned words =
        timing ? P2000T_DIAGNOSTIC_TIMING_WORD_COUNT
               : requested_line_count *
                     P2000T_DIAGNOSTIC_SAMPLES_PER_NOMINAL_LINE / 8u;
    dma_channel_configure(sample_dma, &dma, diagnostic_samples,
                          &diagnostic_pio->rxf[sampler_sm], words, false);

    /* The chained single-word write stops both diagnostic state machines
       immediately after the requested RX word count. This prevents the
       sampler's expected post-capture FIFO fill from setting RXSTALL, so any
       sticky RXSTALL observed by the completion IRQ represents a real gap in
       the acquired time axis. */
    dma_channel_config stop = dma_channel_get_default_config(stop_dma);
    channel_config_set_transfer_data_size(&stop, DMA_SIZE_32);
    channel_config_set_read_increment(&stop, false);
    channel_config_set_write_increment(&stop, false);
    channel_config_set_dreq(&stop, DREQ_FORCE);
    pio_disable_value = 0u;
    dma_channel_configure(stop_dma, &stop, &diagnostic_pio->ctrl,
                          &pio_disable_value, 1u, false);
    capture_finished = false;
    capture_started_us = 0u;
    capture_duration_us = 0u;
    capture_stalled = false;
    diagnostic_pio->fdebug = 1u << sampler_sm;
    expected_capture_duration_us =
        timing ? (uint32_t)(((uint64_t)P2000T_DIAGNOSTIC_TIMING_SAMPLE_COUNT *
                             1000000u +
                             P2000T_DIAGNOSTIC_TIMING_SAMPLE_RATE_HZ / 2u) /
                            P2000T_DIAGNOSTIC_TIMING_SAMPLE_RATE_HZ)
               : requested_line_count * 64u;

    /* Line numbering matches the main capture engine: zero loops once, so
       target line N is represented as N-1 in the trigger command. */
    pio_sm_put(diagnostic_pio, trigger_sm, requested_start_line - 1u);
    dma_start_channel_mask(1u << sample_dma);
    pio_enable_sm_mask_in_sync(diagnostic_pio,
                               (1u << trigger_sm) | (1u << sampler_sm));
    diagnostic_state = timing ? DIAGNOSTIC_CAPTURING_TIMING
                              : DIAGNOSTIC_CAPTURING_RAW;
}

static void build_header(unsigned type, uint32_t flags, uint32_t sample_rate,
                         uint32_t sample_count, unsigned bits_per_sample,
                         uint32_t payload_size) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    memset(diagnostic_header, 0, sizeof(diagnostic_header));
    memcpy(diagnostic_header, P2000T_DIAGNOSTIC_MAGIC, 4u);
    diagnostic_header[4] = P2000T_DIAGNOSTIC_PROTOCOL_VERSION;
    diagnostic_header[P2000T_DIAGNOSTIC_TYPE_OFFSET] = (uint8_t)type;
    store_u16(&diagnostic_header[6], P2000T_DIAGNOSTIC_HEADER_SIZE);
    if (capture.signal_present) {
        flags |= P2000T_DIAGNOSTIC_FLAG_SIGNAL_PRESENT;
    }
    const bool data_record = type == P2000T_DIAGNOSTIC_RECORD_TIMING ||
                             type == P2000T_DIAGNOSTIC_RECORD_RAW_RGBS;
    if (data_record && capture_duration_us >
        expected_capture_duration_us + CAPTURE_OVERRUN_TOLERANCE_US) {
        flags |= P2000T_DIAGNOSTIC_FLAG_CAPTURE_OVERRUN;
    }
    if (data_record && capture_stalled) {
        flags |= P2000T_DIAGNOSTIC_FLAG_PIO_RX_STALL;
    }
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_FLAGS_OFFSET], flags);
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_RECORD_SEQUENCE_OFFSET],
              ++record_sequence);
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_PAYLOAD_SIZE_OFFSET],
              payload_size);
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_CRC32_OFFSET],
              diagnostic_crc32((const uint8_t *)diagnostic_samples,
                               payload_size));
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_SESSION_ID_OFFSET],
              session_id);
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_SAMPLE_RATE_OFFSET],
              sample_rate);
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_SAMPLE_COUNT_OFFSET],
              sample_count);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_START_LINE_OFFSET],
              (uint16_t)requested_start_line);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_LINE_COUNT_OFFSET],
              (uint16_t)requested_line_count);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_REPETITION_OFFSET],
              (uint16_t)current_repetition);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_REPETITIONS_OFFSET],
              (uint16_t)requested_repetitions);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_SAMPLES_PER_LINE_OFFSET],
              P2000T_DIAGNOSTIC_SAMPLES_PER_NOMINAL_LINE);
    diagnostic_header[P2000T_DIAGNOSTIC_BITS_PER_SAMPLE_OFFSET] =
        (uint8_t)bits_per_sample;
    diagnostic_header[P2000T_DIAGNOSTIC_SYNC_CHANNEL_OFFSET] =
        P2000T_SYNC_CHANNEL;
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_FIRST_VISIBLE_OFFSET],
              (uint16_t)capture.first_visible_scanline);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_HORIZONTAL_OFFSET],
              (uint16_t)capture.horizontal_offset);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_PHASE_OFFSET],
              (uint16_t)capture.sample_phase);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_ODD_PHASE_OFFSET],
              (uint16_t)capture.odd_line_phase);
    store_u16(&diagnostic_header[P2000T_DIAGNOSTIC_RATE_TRIM_OFFSET],
              (uint16_t)capture.sample_rate_trim);
    diagnostic_header[P2000T_DIAGNOSTIC_CHANNEL_MAP_OFFSET] =
        (uint8_t)(P2000T_SYNC_CHANNEL | (P2000T_RED_CHANNEL << 2u) |
                  (P2000T_GREEN_CHANNEL << 4u) |
                  (P2000T_BLUE_CHANNEL << 6u));
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_CAPTURE_SEQUENCE_OFFSET],
              capture.captured_frames);
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_CAPTURE_DURATION_OFFSET],
              data_record ? capture_duration_us : 0u);
    store_u32(&diagnostic_header[P2000T_DIAGNOSTIC_EXPECTED_DURATION_OFFSET],
              data_record ? expected_capture_duration_us : 0u);
}

static void begin_record(unsigned type, uint32_t flags, uint32_t sample_rate,
                         uint32_t sample_count, unsigned bits_per_sample,
                         size_t payload_size, diagnostic_state_t state) {
    build_header(type, flags, sample_rate, sample_count, bits_per_sample,
                 (uint32_t)payload_size);
    tx_payload = (const uint8_t *)diagnostic_samples;
    tx_payload_size = payload_size;
    tx_header_offset = 0u;
    tx_payload_offset = 0u;
    tx_guard_offset = 0u;
    if (payload_size >= P2000T_DIAGNOSTIC_GUARD_DUPLICATE_SIZE) {
        memcpy(tx_guard,
               tx_payload + payload_size -
                                P2000T_DIAGNOSTIC_GUARD_DUPLICATE_SIZE,
               P2000T_DIAGNOSTIC_GUARD_DUPLICATE_SIZE);
    } else {
        memset(tx_guard, 0, P2000T_DIAGNOSTIC_GUARD_DUPLICATE_SIZE);
    }
    memcpy(&tx_guard[P2000T_DIAGNOSTIC_GUARD_DUPLICATE_SIZE],
           P2000T_DIAGNOSTIC_GUARD_MAGIC,
           P2000T_DIAGNOSTIC_GUARD_MAGIC_SIZE);
    diagnostic_state = state;
}

static void begin_complete_record(void) {
    begin_record(P2000T_DIAGNOSTIC_RECORD_COMPLETE,
                 cancel_requested ? P2000T_DIAGNOSTIC_FLAG_CANCELLED : 0u,
                 0u, 0u, 0u, 0u, DIAGNOSTIC_SENDING_COMPLETE);
}

static void advance_after_transmit(diagnostic_state_t completed_state) {
    switch (completed_state) {
    case DIAGNOSTIC_SENDING_SESSION:
        if (cancel_requested) {
            begin_complete_record();
        } else {
            arm_capture(true);
        }
        break;
    case DIAGNOSTIC_SENDING_TIMING:
        if (cancel_requested) {
            begin_complete_record();
        } else {
            current_repetition = 1u;
            arm_capture(false);
        }
        break;
    case DIAGNOSTIC_SENDING_RAW:
        if (cancel_requested || current_repetition >= requested_repetitions) {
            begin_complete_record();
        } else {
            ++current_repetition;
            arm_capture(false);
        }
        break;
    case DIAGNOSTIC_SENDING_COMPLETE:
        diagnostic_state = DIAGNOSTIC_IDLE;
        break;
    default:
        break;
    }
}

static void apply_host_action(bool retry) {
    if (retry) {
        tx_header_offset = 0u;
        tx_payload_offset = 0u;
        tx_guard_offset = 0u;
        diagnostic_state = drained_record_state;
    } else {
        advance_after_transmit(drained_record_state);
    }
}

static void service_transmit(void) {
    size_t budget = DIAGNOSTIC_TX_SERVICE_BUDGET;
    while (budget != 0u) {
        const uint32_t available = tud_cdc_write_available();
        if (available == 0u) {
            break;
        }
        const uint8_t *source;
        size_t remaining;
        enum { TX_HEADER, TX_PAYLOAD, TX_GUARD } part;
        if (tx_header_offset < P2000T_DIAGNOSTIC_HEADER_SIZE) {
            part = TX_HEADER;
            source = &diagnostic_header[tx_header_offset];
            remaining = P2000T_DIAGNOSTIC_HEADER_SIZE - tx_header_offset;
        } else if (tx_payload_offset < tx_payload_size) {
            part = TX_PAYLOAD;
            source = &tx_payload[tx_payload_offset];
            remaining = tx_payload_size - tx_payload_offset;
        } else {
            part = TX_GUARD;
            source = &tx_guard[tx_guard_offset];
            remaining = P2000T_DIAGNOSTIC_GUARD_SIZE - tx_guard_offset;
        }
        size_t count = remaining;
        if (count > available) {
            count = available;
        }
        if (count > budget) {
            count = budget;
        }
        const uint32_t written = tud_cdc_write(source, (uint32_t)count);
        if (written == 0u) {
            break;
        }
        budget -= written;
        if (part == TX_HEADER) {
            tx_header_offset += written;
        } else if (part == TX_PAYLOAD) {
            tx_payload_offset += written;
        } else {
            tx_guard_offset += written;
        }
        if (tx_header_offset == P2000T_DIAGNOSTIC_HEADER_SIZE &&
            tx_payload_offset == tx_payload_size &&
            tx_guard_offset == P2000T_DIAGNOSTIC_GUARD_SIZE) {
            drained_record_state = diagnostic_state;
            tx_drain_started_us = time_us_32();
            diagnostic_state = DIAGNOSTIC_DRAINING_TX;
            break;
        }
    }
    tud_cdc_write_flush();
}

bool p2000t_diagnostics_start(unsigned start_line, unsigned line_count,
                              unsigned repetitions) {
    p2000t_capture_stats_t capture;
    p2000t_capture_get_stats(&capture);
    if (diagnostic_state != DIAGNOSTIC_IDLE ||
        capture.capture_engine == P2000T_CAPTURE_ENGINE_WINDOWED ||
        capture.engine_switch_pending ||
        start_line < P2000T_DIAGNOSTIC_MIN_START_LINE ||
        start_line > P2000T_DIAGNOSTIC_MAX_START_LINE || line_count == 0u ||
        line_count > P2000T_DIAGNOSTIC_MAX_LINES || repetitions == 0u ||
        repetitions > P2000T_DIAGNOSTIC_MAX_REPETITIONS) {
        return false;
    }
    initialize_resources();
    requested_start_line = start_line;
    requested_line_count = line_count;
    requested_repetitions = repetitions;
    current_repetition = 0u;
    session_id = time_us_32();
    record_sequence = 0u;
    cancel_requested = false;
    pending_host_action = 0u;
    capture_duration_us = 0u;
    capture_stalled = false;
    expected_capture_duration_us = 0u;
    begin_record(P2000T_DIAGNOSTIC_RECORD_SESSION, 0u, 0u, 0u, 0u, 0u,
                 DIAGNOSTIC_SENDING_SESSION);
    return true;
}

void p2000t_diagnostics_cancel(void) {
    if (diagnostic_state == DIAGNOSTIC_IDLE) {
        return;
    }
    cancel_requested = true;
    if (diagnostic_state == DIAGNOSTIC_CAPTURING_TIMING ||
        diagnostic_state == DIAGNOSTIC_CAPTURING_RAW) {
        stop_capture_hardware();
        begin_complete_record();
    } else if (diagnostic_state == DIAGNOSTIC_WAITING_ACK) {
        begin_complete_record();
    } else if (diagnostic_state == DIAGNOSTIC_DRAINING_TX) {
        pending_host_action = 1u;
    }
}

void p2000t_diagnostics_stop(void) {
    stop_capture_hardware();
    diagnostic_state = DIAGNOSTIC_IDLE;
    tx_header_offset = 0u;
    tx_payload_offset = 0u;
    tx_payload_size = 0u;
    tx_guard_offset = 0u;
    tx_drain_started_us = 0u;
    pending_host_action = 0u;
}

bool p2000t_diagnostics_active(void) {
    return diagnostic_state != DIAGNOSTIC_IDLE;
}

bool p2000t_diagnostics_acknowledge(uint32_t sequence, bool retry) {
    if (sequence != record_sequence ||
        (drained_record_state != DIAGNOSTIC_SENDING_TIMING &&
         drained_record_state != DIAGNOSTIC_SENDING_RAW)) {
        return false;
    }
    const unsigned action = retry ? 2u : 1u;
    if (diagnostic_state == DIAGNOSTIC_DRAINING_TX) {
        pending_host_action = action;
        return true;
    }
    if (diagnostic_state != DIAGNOSTIC_WAITING_ACK) {
        return false;
    }
    apply_host_action(retry);
    return true;
}

void p2000t_diagnostics_service(void) {
    if (diagnostic_state == DIAGNOSTIC_IDLE) {
        return;
    }
    if (!stdio_usb_connected()) {
        p2000t_diagnostics_stop();
        return;
    }
    if (diagnostic_state == DIAGNOSTIC_DRAINING_TX) {
        tud_cdc_write_flush();
        if ((uint32_t)(time_us_32() - tx_drain_started_us) >=
            DIAGNOSTIC_TX_DRAIN_US) {
            if (drained_record_state == DIAGNOSTIC_SENDING_TIMING ||
                drained_record_state == DIAGNOSTIC_SENDING_RAW) {
                diagnostic_state = DIAGNOSTIC_WAITING_ACK;
                if (pending_host_action != 0u) {
                    const unsigned action = pending_host_action;
                    pending_host_action = 0u;
                    apply_host_action(action == 2u);
                }
            } else {
                advance_after_transmit(drained_record_state);
            }
        }
        return;
    }
    if (diagnostic_state == DIAGNOSTIC_CAPTURING_TIMING && capture_finished) {
        stop_capture_hardware();
        begin_record(P2000T_DIAGNOSTIC_RECORD_TIMING,
                     P2000T_DIAGNOSTIC_FLAG_WORDS_LITTLE_ENDIAN |
                         P2000T_DIAGNOSTIC_FLAG_SAMPLES_MSB_FIRST,
                     P2000T_DIAGNOSTIC_TIMING_SAMPLE_RATE_HZ,
                     P2000T_DIAGNOSTIC_TIMING_SAMPLE_COUNT, 1u,
                     P2000T_DIAGNOSTIC_TIMING_PAYLOAD_SIZE,
                     DIAGNOSTIC_SENDING_TIMING);
    } else if (diagnostic_state == DIAGNOSTIC_CAPTURING_RAW &&
               capture_finished) {
        stop_capture_hardware();
        const uint32_t sample_count =
            requested_line_count *
            P2000T_DIAGNOSTIC_SAMPLES_PER_NOMINAL_LINE;
        begin_record(P2000T_DIAGNOSTIC_RECORD_RAW_RGBS,
                     P2000T_DIAGNOSTIC_FLAG_WORDS_LITTLE_ENDIAN |
                         P2000T_DIAGNOSTIC_FLAG_SAMPLES_MSB_FIRST,
                     P2000T_DIAGNOSTIC_RAW_SAMPLE_RATE_HZ, sample_count, 4u,
                     sample_count / 2u, DIAGNOSTIC_SENDING_RAW);
    }
    if (diagnostic_state == DIAGNOSTIC_SENDING_SESSION ||
        diagnostic_state == DIAGNOSTIC_SENDING_TIMING ||
        diagnostic_state == DIAGNOSTIC_SENDING_RAW ||
        diagnostic_state == DIAGNOSTIC_SENDING_COMPLETE) {
        service_transmit();
    }
}
