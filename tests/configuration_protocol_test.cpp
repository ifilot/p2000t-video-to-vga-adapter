/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cassert>

#include <QByteArray>

#include "pico_configuration.h"

namespace {

void storeU16(char *destination, quint16 value) {
    destination[0] = static_cast<char>(value);
    destination[1] = static_cast<char>(value >> 8u);
}

} // namespace

int main() {
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

    PicoConfiguration decoded;
    assert(decodePicoConfiguration(payload, &decoded));
    assert(decoded.vertical == 61);
    assert(decoded.phase == -4);
    assert(decoded.oddLinePhase == -3);
    assert(decoded.sampleRateTrim == -3);
    assert(decoded.sampleReconstruction ==
           P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP);
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
    payload[4] = P2000T_CONFIGURATION_STATE_RATE_TRIM_VERSION;
    assert(decodePicoConfiguration(payload, &decoded));
    assert(decoded.sampleRateTrim == -3);
    assert(decoded.sampleReconstruction ==
           P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION);

    payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] |=
        P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK;
    assert(!decodePicoConfiguration(payload, &decoded));

    payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET] = 1;
    payload[4] = P2000T_CONFIGURATION_STATE_ODD_LINE_PHASE_VERSION;
    assert(decodePicoConfiguration(payload, &decoded));
    assert(decoded.oddLinePhase == -3);
    assert(decoded.sampleRateTrim == P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM);

    payload[4] = P2000T_CONFIGURATION_STATE_LEGACY_VERSION;
    payload[P2000T_CONFIGURATION_ODD_LINE_PHASE_OFFSET] = 0;
    assert(decodePicoConfiguration(payload, &decoded));
    assert(decoded.oddLinePhase == P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE);
    assert(decoded.sampleRateTrim == P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM);
    return 0;
}
