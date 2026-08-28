/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_ENGINEERING_AUTOTUNE_H
#define P2000T_ENGINEERING_AUTOTUNE_H

#include <array>
#include <cstdint>
#include <vector>

#include <QByteArray>
#include <QImage>

struct EngineeringCandidateMetrics {
    int frames = 0;
    quint64 unstablePixels = 0;
    quint64 unstableEvenLines = 0;
    quint64 unstableOddLines = 0;
    quint64 robustUnstablePixels = 0;
    quint64 robustUnstableEvenLines = 0;
    quint64 robustUnstableOddLines = 0;
    quint64 medianFrameMismatches = 0;
    quint64 robustFrameThreshold = 0;
    int robustFrames = 0;
    int coherentOutlierFrames = 0;
    quint64 isolatedPixels = 0;
    quint64 isolatedEvenLines = 0;
    quint64 isolatedOddLines = 0;
    quint64 thirdColorPixels = 0;
    quint64 thirdColorEvenLines = 0;
    quint64 thirdColorOddLines = 0;
    quint64 horizontalTransitions = 0;
    double instabilityPpm = 0.0;
    double evenInstabilityPpm = 0.0;
    double oddInstabilityPpm = 0.0;
    double robustInstabilityPpm = 0.0;
    double robustEvenInstabilityPpm = 0.0;
    double robustOddInstabilityPpm = 0.0;
    double artifactPpm = 0.0;
    double score = 0.0;
    double evenScore = 0.0;
    double oddScore = 0.0;
    QImage modalImage;
};

struct EngineeringFidelityMetrics {
    bool valid = false;
    quint64 pixels = 0;
    quint64 mismatchedPixels = 0;
    quint64 erasedPixels = 0;
    quint64 filledPixels = 0;
    quint64 recoloredPixels = 0;
    double mismatchPpm = 0.0;
};

/** Compare two modal images while counting each duplicated scanline once. */
EngineeringFidelityMetrics compareEngineeringModals(const QImage &candidate,
                                                    const QImage &reference);

/**
 * Accumulates repeated viewer frames for one sampling setting.
 *
 * Only one row from each vertically duplicated viewer row-pair is evaluated.
 * Pixel values are converted back to one of the eight RGB111 palette indices,
 * so customized viewer colors do not change the measurements.
 */
class EngineeringCandidateAccumulator {
  public:
    explicit EngineeringCandidateAccumulator(
        const std::array<QRgb, 8> &palette);

    bool addFrame(const QImage &frame);
    bool empty() const;
    void clear();
    EngineeringCandidateMetrics finalize() const;

  private:
    quint8 colorIndex(QRgb color) const;

    std::array<QRgb, 8> palette_;
    std::vector<QByteArray> frames_;
    int width_ = 0;
    int logical_height_ = 0;
};

#endif
