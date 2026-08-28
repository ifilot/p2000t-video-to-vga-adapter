/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pico_configuration.h"

#include <cstring>

namespace {

quint16 crc16(const char *data, qsizetype length) {
    quint16 crc = 0xffffu;
    for (qsizetype index = 0; index < length; ++index) {
        crc ^= static_cast<quint16>(static_cast<unsigned char>(data[index]))
               << 8u;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = static_cast<quint16>((crc << 1u) ^
                                       ((crc & 0x8000u) != 0u ? 0x1021u : 0u));
        }
    }
    return crc;
}

quint16 loadU16(const char *data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return static_cast<quint16>(bytes[0]) |
           (static_cast<quint16>(bytes[1]) << 8u);
}

quint32 loadU32(const char *data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return static_cast<quint32>(bytes[0]) |
           (static_cast<quint32>(bytes[1]) << 8u) |
           (static_cast<quint32>(bytes[2]) << 16u) |
           (static_cast<quint32>(bytes[3]) << 24u);
}

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

QByteArray makePicoControlPacket(quint8 opcode, quint8 argument,
                                 quint32 value) {
    QByteArray packet(P2000T_CONTROL_PACKET_SIZE, '\0');
    packet[0] = static_cast<char>(P2000T_CONTROL_MAGIC_0);
    packet[1] = static_cast<char>(P2000T_CONTROL_MAGIC_1);
    packet[2] = static_cast<char>(P2000T_CONTROL_VERSION);
    packet[3] = static_cast<char>(opcode);
    packet[4] = static_cast<char>(argument);
    storeU32(&packet.data()[6], value);
    storeU16(&packet.data()[P2000T_CONTROL_CRC_OFFSET],
             crc16(packet.constData(), P2000T_CONTROL_CRC_OFFSET));
    return packet;
}

bool decodePicoConfiguration(const QByteArray &payload,
                             PicoConfiguration *configuration) {
    if (configuration == nullptr ||
        payload.size() < P2000T_CONFIGURATION_STATE_LEGACY_SIZE ||
        std::memcmp(payload.constData(), P2000T_CONFIGURATION_STATE_MAGIC,
                    4u) != 0 ||
        loadU16(&payload.constData()[6]) !=
            static_cast<quint16>(payload.size())) {
        return false;
    }
    const quint8 version = static_cast<quint8>(payload[4]);
    if (version < P2000T_CONFIGURATION_STATE_LEGACY_VERSION ||
        version > P2000T_CONFIGURATION_STATE_VERSION ||
        (version < P2000T_CONFIGURATION_STATE_RUNTIME_RECONSTRUCTION_VERSION &&
         payload.size() != P2000T_CONFIGURATION_STATE_LEGACY_SIZE) ||
        (version >= P2000T_CONFIGURATION_STATE_RUNTIME_RECONSTRUCTION_VERSION &&
         payload.size() != P2000T_CONFIGURATION_STATE_SIZE)) {
        return false;
    }
    PicoConfiguration result;
    const quint8 flags = static_cast<quint8>(payload[5]);
    result.storedAvailable =
        (flags & P2000T_CONFIGURATION_FLAG_STORED_AVAILABLE) != 0u;
    result.matchesStored =
        (flags & P2000T_CONFIGURATION_FLAG_MATCHES_STORED) != 0u;
    result.saveFailed = (flags & P2000T_CONFIGURATION_FLAG_SAVE_FAILED) != 0u;
    result.vertical = loadU16(&payload.constData()[8]);
    result.phase = static_cast<qint16>(loadU16(&payload.constData()[10]));
    result.horizontal = loadU16(&payload.constData()[12]);
    const quint8 captureOptions = static_cast<quint8>(
        payload[P2000T_CONFIGURATION_CAPTURE_OPTIONS_OFFSET]);
    result.artwork = captureOptions & P2000T_CONFIGURATION_ARTWORK_MASK;
    if (version >= P2000T_CONFIGURATION_STATE_ODD_LINE_PHASE_VERSION) {
        result.oddLinePhase = static_cast<qint8>(static_cast<quint8>(
            payload[P2000T_CONFIGURATION_ODD_LINE_PHASE_OFFSET]));
    }
    if (version >= P2000T_CONFIGURATION_STATE_RATE_TRIM_VERSION) {
        const int encoded =
            (captureOptions >> P2000T_CONFIGURATION_RATE_TRIM_SHIFT) &
            P2000T_CONFIGURATION_RATE_TRIM_VALUE_MASK;
        result.sampleRateTrim = (encoded & 0x10) != 0 ? encoded - 32 : encoded;
    }
    if (version >= P2000T_CONFIGURATION_STATE_RUNTIME_RECONSTRUCTION_VERSION) {
        result.sampleReconstruction = static_cast<quint8>(
            payload[P2000T_CONFIGURATION_RUNTIME_RECONSTRUCTION_OFFSET]);
        result.captureEngine = static_cast<quint8>(
            payload[P2000T_CONFIGURATION_CAPTURE_ENGINE_OFFSET]);
        result.windowSamples = static_cast<quint8>(
            payload[P2000T_CONFIGURATION_WINDOW_SAMPLES_OFFSET]);
        const quint8 captureFlags = static_cast<quint8>(
            payload[P2000T_CONFIGURATION_CAPTURE_FLAGS_OFFSET]);
        result.windowSupported =
            (captureFlags &
             P2000T_CONFIGURATION_CAPTURE_FLAG_WINDOW_SUPPORTED) != 0u;
        result.engineSwitchPending =
            (captureFlags &
             P2000T_CONFIGURATION_CAPTURE_FLAG_ENGINE_SWITCH_PENDING) != 0u;
        result.windowedFrames = loadU32(
            &payload.constData()[P2000T_CONFIGURATION_WINDOW_FRAMES_OFFSET]);
        result.lastCorrectedSamples = loadU32(
            &payload.constData()[P2000T_CONFIGURATION_LAST_CORRECTED_OFFSET]);
        result.lastAmbiguousSamples = loadU32(
            &payload.constData()[P2000T_CONFIGURATION_LAST_AMBIGUOUS_OFFSET]);
        result.lastRedCorrections = loadU32(
            &payload
                 .constData()[P2000T_CONFIGURATION_LAST_RED_CORRECTED_OFFSET]);
        result.lastGreenCorrections =
            loadU32(&payload.constData()
                         [P2000T_CONFIGURATION_LAST_GREEN_CORRECTED_OFFSET]);
        result.lastBlueCorrections = loadU32(
            &payload
                 .constData()[P2000T_CONFIGURATION_LAST_BLUE_CORRECTED_OFFSET]);
        result.lineDeadlineMisses =
            loadU32(&payload.constData()
                         [P2000T_CONFIGURATION_LINE_DEADLINE_MISSES_OFFSET]);
    } else if (version >= P2000T_CONFIGURATION_STATE_RECONSTRUCTION_VERSION) {
        result.sampleReconstruction =
            (captureOptions &
             P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK) >>
            P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_SHIFT;
    } else if ((captureOptions &
                P2000T_CONFIGURATION_SAMPLE_RECONSTRUCTION_MASK) != 0u) {
        return false;
    }
    for (int index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        result.palette[static_cast<size_t>(index)] = loadU16(
            &payload
                 .constData()[P2000T_CONFIGURATION_PALETTE_OFFSET + index * 2]);
    }
    if (result.vertical < P2000T_CONTROL_MIN_VERTICAL ||
        result.vertical > P2000T_CONTROL_MAX_VERTICAL ||
        result.phase < P2000T_CONTROL_MIN_PHASE ||
        result.phase > P2000T_CONTROL_MAX_PHASE ||
        result.oddLinePhase < P2000T_CONTROL_MIN_ODD_LINE_PHASE ||
        result.oddLinePhase > P2000T_CONTROL_MAX_ODD_LINE_PHASE ||
        result.sampleRateTrim < P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM ||
        result.sampleRateTrim > P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM ||
        result.sampleReconstruction <
            P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW ||
        result.sampleReconstruction >=
            P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT ||
        result.captureEngine < P2000T_CAPTURE_ENGINE_TWO_TAP ||
        result.captureEngine > P2000T_CAPTURE_ENGINE_WINDOWED ||
        (result.captureEngine == P2000T_CAPTURE_ENGINE_WINDOWED &&
         !result.windowSupported) ||
        (result.windowSamples != 1 && result.windowSamples != 3) ||
        result.horizontal < P2000T_CONTROL_MIN_HORIZONTAL ||
        result.horizontal > P2000T_CONTROL_MAX_HORIZONTAL ||
        result.horizontal % P2000T_CONTROL_HORIZONTAL_STEP != 0 ||
        result.artwork >= 3) {
        return false;
    }
    for (const quint16 color : result.palette) {
        if (color > P2000T_CONTROL_RGB444_MAX) {
            return false;
        }
    }
    *configuration = result;
    return true;
}
