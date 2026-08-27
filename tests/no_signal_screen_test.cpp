/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <array>
#include <cstdio>

#include <QImage>

#include "no_signal_screen.h"
#include "p2000t_stream_protocol.h"

namespace {

bool hasWhitePixels(const QImage &image, int top, int height) {
    for (int y = top; y < top + height; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) == qRgb(255, 255, 255)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

int main() {
    const std::array<quint8, P2000T_STREAM_ARTWORK_COUNT> artworks = {
        P2000T_STREAM_ARTWORK_GREEN_PHOSPHOR,
        P2000T_STREAM_ARTWORK_SYNTHWAVE,
        P2000T_STREAM_ARTWORK_AMBER_CIRCUIT,
    };
    std::array<QImage, P2000T_STREAM_ARTWORK_COUNT> images;
    for (std::size_t index = 0; index < artworks.size(); ++index) {
        images[index] = renderP2000tNoSignalScreen(artworks[index]);
        if (images[index].size() != QSize(640, 480) ||
            !hasWhitePixels(images[index], 6, 14) ||
            !hasWhitePixels(images[index], 26, 14) ||
            !hasWhitePixels(images[index], 392, 28) ||
            !hasWhitePixels(images[index], 434, 14)) {
            std::fprintf(stderr,
                         "No-signal composition failed for artwork %zu\n",
                         index);
            return 1;
        }
    }
    if (images[0] == images[1] || images[0] == images[2] ||
        images[1] == images[2]) {
        std::fputs("No-signal artwork resources are not unique\n", stderr);
        return 1;
    }
    return 0;
}
