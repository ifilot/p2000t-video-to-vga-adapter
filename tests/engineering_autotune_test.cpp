/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <array>
#include <cstdlib>
#include <iostream>

#include <QImage>

#include "engineering_autotune.h"

namespace {

const std::array<QRgb, 8> kPalette = {
    qRgb(0, 0, 0),       qRgb(255, 0, 0),   qRgb(0, 255, 0),
    qRgb(255, 255, 0),   qRgb(0, 0, 255),   qRgb(255, 0, 255),
    qRgb(0, 255, 255),   qRgb(255, 255, 255),
};

QImage makeFrame() {
    QImage image(8, 4, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            row[x] = kPalette[x < 4 ? 4 : 7];
        }
    }
    return image;
}

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    EngineeringCandidateAccumulator stable(kPalette);
    const QImage base = makeFrame();
    require(stable.addFrame(base) && stable.addFrame(base),
            "stable frames were rejected");
    const EngineeringCandidateMetrics stableMetrics = stable.finalize();
    require(stableMetrics.unstablePixels == 0,
            "stable image reported temporal changes");
    require(stableMetrics.isolatedPixels == 0 &&
                stableMetrics.thirdColorPixels == 0,
            "clean edge reported a spatial transient");

    EngineeringCandidateAccumulator changing(kPalette);
    QImage changed = base;
    reinterpret_cast<QRgb *>(changed.scanLine(0))[2] = kPalette[6];
    reinterpret_cast<QRgb *>(changed.scanLine(1))[2] = kPalette[6];
    require(changing.addFrame(base) && changing.addFrame(base) &&
                changing.addFrame(changed),
            "changing frames were rejected");
    const EngineeringCandidateMetrics changingMetrics = changing.finalize();
    require(changingMetrics.unstablePixels == 1,
            "modal temporal difference count is incorrect");
    require(changingMetrics.unstableEvenLines == 1 &&
                changingMetrics.unstableOddLines == 0,
            "logical odd/even temporal split is incorrect");

    EngineeringCandidateAccumulator transient(kPalette);
    QImage third = base;
    reinterpret_cast<QRgb *>(third.scanLine(0))[4] = kPalette[6];
    reinterpret_cast<QRgb *>(third.scanLine(1))[4] = kPalette[6];
    require(transient.addFrame(third), "transient frame was rejected");
    const EngineeringCandidateMetrics transientMetrics = transient.finalize();
    require(transientMetrics.thirdColorPixels == 1,
            "one-pixel third color was not detected");
    require(transientMetrics.score > stableMetrics.score,
            "artifact did not worsen the quality score");
    require(!transientMetrics.modalImage.isNull(),
            "modal diagnostic image was not produced");
    return EXIT_SUCCESS;
}
