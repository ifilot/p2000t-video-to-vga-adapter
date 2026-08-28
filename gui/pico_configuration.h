/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_PICO_CONFIGURATION_H
#define P2000T_PICO_CONFIGURATION_H

#include <array>

#include <QByteArray>
#include <QtGlobal>

#include "p2000t_control_protocol.h"

struct PicoConfiguration {
    int vertical = P2000T_CONTROL_DEFAULT_VERTICAL;
    int phase = P2000T_CONTROL_DEFAULT_PHASE;
    int oddLinePhase = P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE;
    int sampleRateTrim = P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM;
    int sampleReconstruction = P2000T_CONTROL_DEFAULT_SAMPLE_RECONSTRUCTION;
    int captureEngine = P2000T_CAPTURE_ENGINE_TWO_TAP;
    int windowSamples = 1;
    quint32 windowedFrames = 0;
    quint32 lastCorrectedSamples = 0;
    quint32 lastAmbiguousSamples = 0;
    quint32 lastRedCorrections = 0;
    quint32 lastGreenCorrections = 0;
    quint32 lastBlueCorrections = 0;
    quint32 lineDeadlineMisses = 0;
    bool windowSupported = false;
    bool engineSwitchPending = false;
    int horizontal = P2000T_CONTROL_DEFAULT_HORIZONTAL;
    int artwork = P2000T_CONTROL_DEFAULT_ARTWORK;
    std::array<quint16, P2000T_CONTROL_PALETTE_COLORS> palette = {
        0x0000, 0x000f, 0x00f0, 0x00ff, 0x0f00, 0x0f0f, 0x0ff0, 0x0fff,
    };
    bool storedAvailable = false;
    bool matchesStored = false;
    bool saveFailed = false;
};

/** Encode one fixed-size, CRC-protected host control packet. */
QByteArray makePicoControlPacket(quint8 opcode, quint8 argument = 0,
                                 quint32 value = 0);

/** Decode and validate a firmware configuration-state payload. */
bool decodePicoConfiguration(const QByteArray &payload,
                             PicoConfiguration *configuration);

#endif
