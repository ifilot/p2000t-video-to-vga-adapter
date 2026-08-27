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
    payload[14] = 1;
    for (int index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        storeU16(
            &payload.data()[P2000T_CONFIGURATION_PALETTE_OFFSET + index * 2],
            static_cast<quint16>(index * 0x111));
    }

    PicoConfiguration decoded;
    assert(decodePicoConfiguration(payload, &decoded));
    assert(decoded.vertical == 61);
    assert(decoded.phase == -4);
    assert(decoded.horizontal == 42);
    assert(decoded.artwork == 1);
    assert(decoded.palette[7] == 0x777);
    assert(decoded.storedAvailable);
    assert(decoded.matchesStored);
    assert(decoded.saveFailed);

    payload[14] = 9;
    assert(!decodePicoConfiguration(payload, &decoded));
    return 0;
}
