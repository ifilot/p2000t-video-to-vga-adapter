/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file no_signal_screen.cpp
 * @brief Desktop reproduction of the firmware's RGB444 no-signal renderer.
 */

#include "no_signal_screen.h"

#include <cstring>

#include <QRgb>

#include "p2000t_no_signal_layout.h"
#include "p2000t_stream_protocol.h"

namespace {

constexpr int kVgaWidth = 640;
constexpr int kVgaHeight = 480;
constexpr int kLogicalHeight = 240;
constexpr int kVerticalScale = kVgaHeight / kLogicalHeight;
constexpr QRgb kOverlayColor = qRgb(255, 255, 255);

QString artworkResource(quint8 artwork) {
    switch (artwork) {
    case P2000T_STREAM_ARTWORK_GREEN_PHOSPHOR:
        return QStringLiteral(":/no_connection/green_phosphor.png");
    case P2000T_STREAM_ARTWORK_SYNTHWAVE:
        return QStringLiteral(":/no_connection/synthwave.png");
    case P2000T_STREAM_ARTWORK_AMBER_CIRCUIT:
    default:
        return QStringLiteral(":/no_connection/amber_circuit.png");
    }
}

void setLogicalPixel(QImage &image, unsigned x, unsigned y, QRgb color) {
    if (x >= static_cast<unsigned>(image.width()) ||
        y >= static_cast<unsigned>(kLogicalHeight)) {
        return;
    }
    for (int repeat = 0; repeat < kVerticalScale; ++repeat) {
        auto *row = reinterpret_cast<QRgb *>(
            image.scanLine(static_cast<int>(y) * kVerticalScale + repeat));
        row[x] = color;
    }
}

void fillLogicalSpan(QImage &image, unsigned y, unsigned first,
                     unsigned pastLast, QRgb color) {
    for (unsigned x = first; x < pastLast; ++x) {
        setLogicalPixel(image, x, y, color);
    }
}

void renderText(QImage &image, const char *message, unsigned top,
                unsigned xScale, unsigned yScale) {
    const std::size_t length = std::strlen(message);
    const unsigned textWidth = static_cast<unsigned>(
        ((NO_SIGNAL_GLYPH_WIDTH + 1u) * length - 1u) * xScale);
    const unsigned textLeft = (kVgaWidth - textWidth) / 2u;
    for (unsigned glyphY = 0;
         glyphY < NO_SIGNAL_GLYPH_HEIGHT * yScale; ++glyphY) {
        const unsigned fontRow = glyphY / yScale;
        for (std::size_t index = 0; index < length; ++index) {
            const uint8_t bits =
                p2000t_no_signal_glyph_row(message[index], fontRow);
            const unsigned characterX =
                textLeft + static_cast<unsigned>(index) *
                               (NO_SIGNAL_GLYPH_WIDTH + 1u) * xScale;
            for (unsigned column = 0; column < NO_SIGNAL_GLYPH_WIDTH;
                 ++column) {
                if ((bits & (0x10u >> column)) == 0u) {
                    continue;
                }
                fillLogicalSpan(image, top + glyphY,
                                characterX + column * xScale,
                                characterX + (column + 1u) * xScale,
                                kOverlayColor);
            }
        }
    }
}

void renderDisconnectedIcon(QImage &image) {
    constexpr unsigned center = kVgaWidth / 2u;
    for (unsigned row = 0; row < NO_SIGNAL_ICON_HEIGHT; ++row) {
        const unsigned y = NO_SIGNAL_ICON_TOP + row;
        if (row >= 6u && row <= 7u) {
            fillLogicalSpan(image, y, center - 36u, center - 22u,
                            kOverlayColor);
        }
        if (row >= 3u && row <= 10u) {
            fillLogicalSpan(image, y, center - 22u, center - 10u,
                            kOverlayColor);
        }
        if ((row >= 4u && row <= 5u) ||
            (row >= 8u && row <= 9u)) {
            fillLogicalSpan(image, y, center - 10u, center - 4u,
                            kOverlayColor);
        }
        if (row == 3u || row == 10u) {
            fillLogicalSpan(image, y, center + 4u, center + 22u,
                            kOverlayColor);
        } else if (row > 3u && row < 10u) {
            setLogicalPixel(image, center + 4u, y, kOverlayColor);
            setLogicalPixel(image, center + 5u, y, kOverlayColor);
            setLogicalPixel(image, center + 20u, y, kOverlayColor);
            setLogicalPixel(image, center + 21u, y, kOverlayColor);
        }
        if (row >= 6u && row <= 7u) {
            fillLogicalSpan(image, y, center + 22u, center + 36u,
                            kOverlayColor);
        }
        if (row == 0u || row == 13u) {
            setLogicalPixel(image, center, y, kOverlayColor);
        } else if (row == 1u || row == 12u) {
            setLogicalPixel(image, center - 2u, y, kOverlayColor);
            setLogicalPixel(image, center + 2u, y, kOverlayColor);
        }
    }
}

}  // namespace

QString p2000tArtworkName(quint8 artwork) {
    switch (artwork) {
    case P2000T_STREAM_ARTWORK_GREEN_PHOSPHOR:
        return QStringLiteral("Green phosphor");
    case P2000T_STREAM_ARTWORK_SYNTHWAVE:
        return QStringLiteral("Synthwave");
    case P2000T_STREAM_ARTWORK_AMBER_CIRCUIT:
        return QStringLiteral("Amber circuit");
    default:
        return QStringLiteral("Unknown");
    }
}

QImage renderP2000tNoSignalScreen(quint8 artwork) {
    QImage image(artworkResource(artwork));
    if (image.size() != QSize(kVgaWidth, kVgaHeight)) {
        image = QImage(kVgaWidth, kVgaHeight, QImage::Format_RGB32);
        image.fill(Qt::black);
    } else {
        image = image.convertToFormat(QImage::Format_RGB32);
    }

    renderText(image, P2000T_NO_SIGNAL_TITLE, NO_SIGNAL_TITLE_TOP, 3u, 1u);
    renderText(image,
               P2000T_NO_SIGNAL_VERSION_PREFIX P2000T_VIEWER_VERSION,
               NO_SIGNAL_VERSION_TOP, 2u, 1u);
    renderDisconnectedIcon(image);
    renderText(image, P2000T_NO_SIGNAL_STATUS, NO_SIGNAL_STATUS_TOP, 2u, 1u);
    return image;
}
