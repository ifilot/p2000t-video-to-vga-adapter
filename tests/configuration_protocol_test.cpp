/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cassert>

#include <QByteArray>

#include "pico_configuration.h"
#include "p2000t_stream_protocol.h"

namespace {

void storeU16(char *destination, quint16 value) {
    destination[0] = static_cast<char>(value);
    destination[1] = static_cast<char>(value >> 8u);
}

void storeU32(char *destination, quint32 value) {
    destination[0] = static_cast<char>(value);
    destination[1] = static_cast<char>(value >> 8u);
    destination[2] = static_cast<char>(value >> 16u);
    destination[3] = static_cast<char>(value >> 24u);
}

} // namespace

int main() {
    static_assert(P2000T_STREAM_LEGACY_PROTOCOL_VERSION == 3);
    static_assert(P2000T_STREAM_PROTOCOL_VERSION == 4);
    static_assert(P2000T_STREAM_LEGACY_HEADER_SIZE == 64);
    static_assert(P2000T_STREAM_CAPTURE_TIMESTAMP_US_OFFSET + 8 ==
                  P2000T_STREAM_HEADER_SIZE);
    const QByteArray packet = makePicoControlPacket(P2000T_CONTROL_SET_PHASE, 0,
                                                    static_cast<quint32>(-7));
    assert(packet.size() == P2000T_CONTROL_PACKET_SIZE);
    assert(static_cast<quint8>(packet[0]) == P2000T_CONTROL_MAGIC_0);
    assert(static_cast<quint8>(packet[1]) == P2000T_CONTROL_MAGIC_1);
    assert(static_cast<quint8>(packet[3]) == P2000T_CONTROL_SET_PHASE);
    assert(static_cast<quint8>(packet[6]) == 0xf9u);
    assert(static_cast<quint8>(packet[9]) == 0xffu);

    QByteArray payload(P2000T_CONFIGURATION_STATE_SIZE, '\0');
    payload.replace(0, 4, P2000T_CONFIGURATION_STATE_MAGIC);
    payload[4] = P2000T_CONFIGURATION_STATE_VERSION;
    payload[5] = P2000T_CONFIGURATION_FLAG_STORED_AVAILABLE |
                 P2000T_CONFIGURATION_FLAG_MATCHES_STORED |
                 P2000T_CONFIGURATION_FLAG_SAVE_FAILED;
    storeU16(&payload.data()[6], P2000T_CONFIGURATION_STATE_SIZE);
    storeU16(&payload.data()[8], 61);
    storeU16(&payload.data()[10], static_cast<quint16>(-4));
    storeU16(&payload.data()[12], 42);
    payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] = static_cast<char>(
        1 | P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK |
        ((static_cast<quint8>(-3) & P2000T_CONFIGURATION_RATE_TRIM_VALUE_MASK)
         << P2000T_CONFIGURATION_RATE_TRIM_SHIFT));
    payload[P2000T_CONFIGURATION_ODD_LINE_PHASE_OFFSET] = static_cast<char>(-3);
    for (int index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        storeU16(
            &payload.data()[P2000T_CONFIGURATION_PALETTE_OFFSET + index * 2],
            static_cast<quint16>(index * 0x111));
    }
    payload[P2000T_CONFIGURATION_RUNTIME_RECONSTRUCTION_OFFSET] =
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_EARLY;
    payload[P2000T_CONFIGURATION_CAPTURE_ENGINE_OFFSET] =
        P2000T_CAPTURE_ENGINE_WINDOWED;
    payload[P2000T_CONFIGURATION_WINDOW_SAMPLES_OFFSET] = 3;
    payload[P2000T_CONFIGURATION_CAPTURE_FLAGS_OFFSET] =
        P2000T_CONFIGURATION_CAPTURE_FLAG_WINDOW_SUPPORTED;
    storeU32(&payload.data()[P2000T_CONFIGURATION_WINDOW_FRAMES_OFFSET], 123);
    storeU32(&payload.data()[P2000T_CONFIGURATION_LAST_CORRECTED_OFFSET], 45);
    storeU32(&payload.data()[P2000T_CONFIGURATION_LAST_AMBIGUOUS_OFFSET], 6);
    storeU32(&payload.data()[P2000T_CONFIGURATION_LINE_DEADLINE_MISSES_OFFSET],
             2);

    PicoConfiguration decoded;
    assert(decodePicoConfiguration(payload, &decoded));
    assert(decoded.vertical == 61);
    assert(decoded.phase == -4);
    assert(decoded.oddLinePhase == -3);
    assert(decoded.sampleRateTrim == -3);
    assert(decoded.sampleReconstruction ==
           P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_EARLY);
    assert(decoded.captureEngine == P2000T_CAPTURE_ENGINE_WINDOWED);
    assert(decoded.windowSamples == 3);
    assert(decoded.windowSupported);
    assert(decoded.windowedFrames == 123);
    assert(decoded.lastCorrectedSamples == 45);
    assert(decoded.lastAmbiguousSamples == 6);
    assert(decoded.lineDeadlineMisses == 2);
    assert(decoded.horizontal == 42);
    assert(decoded.artwork == 1);
    assert(decoded.palette[7] == 0x777);
    assert(decoded.storedAvailable);
    assert(decoded.matchesStored);
    assert(decoded.saveFailed);

    payload[P2000T_CONFIGURATION_ODD_LINE_PHASE_OFFSET] =
        P2000T_CONTROL_MAX_ODD_LINE_PHASE + 1;
    assert(!decodePicoConfiguration(payload, &decoded));
    payload[P2000T_CONFIGURATION_ODD_LINE_PHASE_OFFSET] = static_cast<char>(-3);

    payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] = 3;
    assert(!decodePicoConfiguration(payload, &decoded));

    payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] =
        static_cast<char>(1 | ((P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM + 1)
                               << P2000T_CONFIGURATION_RATE_TRIM_SHIFT));
    assert(!decodePicoConfiguration(payload, &decoded));

    payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] = static_cast<char>(
        1 |
        ((static_cast<quint8>(-3) & P2000T_CONFIGURATION_RATE_TRIM_VALUE_MASK)
         << P2000T_CONFIGURATION_RATE_TRIM_SHIFT));
    QByteArray legacy = payload.left(P2000T_CONFIGURATION_STATE_LEGACY_SIZE);
    storeU16(&legacy.data()[6], P2000T_CONFIGURATION_STATE_LEGACY_SIZE);
    legacy[4] = P2000T_CONFIGURATION_STATE_RATE_TRIM_VERSION;
    assert(decodePicoConfiguration(legacy, &decoded));
    assert(decoded.sampleRateTrim == -3);
    assert(decoded.sampleReconstruction ==
           P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION);

    legacy[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] |=
        P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK;
    assert(!decodePicoConfiguration(legacy, &decoded));

    legacy[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] = 1;
    legacy[4] = P2000T_CONFIGURATION_STATE_ODD_LINE_PHASE_VERSION;
    assert(decodePicoConfiguration(legacy, &decoded));
    assert(decoded.oddLinePhase == -3);
    assert(decoded.sampleRateTrim == P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM);

    legacy[4] = P2000T_CONFIGURATION_STATE_LEGACY_VERSION;
    legacy[P2000T_CONFIGURATION_ODD_LINE_PHASE_OFFSET] = 0;
    assert(decodePicoConfiguration(legacy, &decoded));
    assert(decoded.oddLinePhase == P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE);
    assert(decoded.sampleRateTrim == P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM);
    return 0;
}
