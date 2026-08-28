/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engineering_autotune.h"

#include <algorithm>
#include <array>

namespace {

constexpr double kArtifactWeight = 4.0;
constexpr int kUnknownColor = 8;

double partsPerMillion(quint64 count, quint64 total) {
    return total == 0u ? 0.0
                       : static_cast<double>(count) * 1000000.0 /
                             static_cast<double>(total);
}

} // namespace

EngineeringCandidateAccumulator::EngineeringCandidateAccumulator(
    const std::array<QRgb, 8> &palette)
    : palette_(palette) {}

quint8 EngineeringCandidateAccumulator::colorIndex(QRgb color) const {
    for (quint8 index = 0; index < palette_.size(); ++index) {
        if (palette_[index] == color) {
            return index;
        }
    }
    return kUnknownColor;
}

bool EngineeringCandidateAccumulator::addFrame(const QImage &frame) {
    if (frame.isNull() || frame.width() <= 0 || frame.height() <= 0 ||
        (frame.height() & 1) != 0) {
        return false;
    }
    const int logicalHeight = frame.height() / 2;
    if (!frames_.empty() &&
        (frame.width() != width_ || logicalHeight != logical_height_)) {
        return false;
    }
    width_ = frame.width();
    logical_height_ = logicalHeight;
    QByteArray indexed(width_ * logical_height_, Qt::Uninitialized);
    for (int y = 0; y < logical_height_; ++y) {
        const auto *pixels =
            reinterpret_cast<const QRgb *>(frame.constScanLine(y * 2));
        auto *destination = reinterpret_cast<quint8 *>(indexed.data()) +
                            static_cast<qsizetype>(y) * width_;
        for (int x = 0; x < width_; ++x) {
            destination[x] = colorIndex(pixels[x]);
        }
    }
    frames_.push_back(std::move(indexed));
    return true;
}

bool EngineeringCandidateAccumulator::empty() const {
    return frames_.empty();
}

void EngineeringCandidateAccumulator::clear() {
    frames_.clear();
    width_ = 0;
    logical_height_ = 0;
}

EngineeringCandidateMetrics EngineeringCandidateAccumulator::finalize() const {
    EngineeringCandidateMetrics result;
    result.frames = static_cast<int>(frames_.size());
    if (frames_.empty()) {
        return result;
    }

    const qsizetype pixelCount =
        static_cast<qsizetype>(width_) * logical_height_;
    QByteArray modal(pixelCount, Qt::Uninitialized);
    for (qsizetype pixel = 0; pixel < pixelCount; ++pixel) {
        std::array<int, kUnknownColor + 1> counts = {};
        for (const QByteArray &frame : frames_) {
            ++counts[static_cast<quint8>(frame[pixel])];
        }
        const auto maximum = std::max_element(counts.begin(), counts.end());
        const int mode = static_cast<int>(maximum - counts.begin());
        modal[pixel] = static_cast<char>(mode);
        const quint64 unstable = frames_.size() - *maximum;
        result.unstablePixels += unstable;
        const int y = static_cast<int>(pixel / width_);
        if ((y & 1) == 0) {
            result.unstableEvenLines += unstable;
        } else {
            result.unstableOddLines += unstable;
        }
    }

    quint64 evenPixels = 0;
    quint64 oddPixels = 0;
    for (int y = 0; y < logical_height_; ++y) {
        const bool odd = (y & 1) != 0;
        if (odd) {
            oddPixels += width_;
        } else {
            evenPixels += width_;
        }
        const auto *row = reinterpret_cast<const quint8 *>(modal.constData()) +
                          static_cast<qsizetype>(y) * width_;
        for (int x = 1; x < width_; ++x) {
            result.horizontalTransitions += row[x] != row[x - 1] ? 1u : 0u;
        }
        for (int x = 1; x + 1 < width_; ++x) {
            const quint8 before = row[x - 1];
            const quint8 current = row[x];
            const quint8 after = row[x + 1];
            if (before == after && current != before) {
                ++result.isolatedPixels;
                odd ? ++result.isolatedOddLines : ++result.isolatedEvenLines;
            } else if (before != after && current != before &&
                       current != after) {
                ++result.thirdColorPixels;
                odd ? ++result.thirdColorOddLines
                    : ++result.thirdColorEvenLines;
            }
        }
    }

    struct FrameMismatchCounts {
        quint64 total = 0;
        quint64 even = 0;
        quint64 odd = 0;
    };
    std::vector<FrameMismatchCounts> frameMismatches;
    frameMismatches.reserve(frames_.size());
    std::vector<quint64> totals;
    totals.reserve(frames_.size());
    for (const QByteArray &frame : frames_) {
        FrameMismatchCounts counts;
        for (qsizetype pixel = 0; pixel < pixelCount; ++pixel) {
            if (frame[pixel] == modal[pixel]) {
                continue;
            }
            ++counts.total;
            const int y = static_cast<int>(pixel / width_);
            (y & 1) != 0 ? ++counts.odd : ++counts.even;
        }
        frameMismatches.push_back(counts);
        totals.push_back(counts.total);
    }
    std::sort(totals.begin(), totals.end());
    result.medianFrameMismatches = totals[totals.size() / 2u];
    result.robustFrameThreshold =
        std::max<quint64>(32u, result.medianFrameMismatches * 4u);
    for (const FrameMismatchCounts &counts : frameMismatches) {
        if (counts.total > result.robustFrameThreshold) {
            ++result.coherentOutlierFrames;
            continue;
        }
        ++result.robustFrames;
        result.robustUnstablePixels += counts.total;
        result.robustUnstableEvenLines += counts.even;
        result.robustUnstableOddLines += counts.odd;
    }

    const quint64 observations =
        static_cast<quint64>(pixelCount) * frames_.size();
    const quint64 evenObservations = evenPixels * frames_.size();
    const quint64 oddObservations = oddPixels * frames_.size();
    result.instabilityPpm =
        partsPerMillion(result.unstablePixels, observations);
    result.evenInstabilityPpm =
        partsPerMillion(result.unstableEvenLines, evenObservations);
    result.oddInstabilityPpm =
        partsPerMillion(result.unstableOddLines, oddObservations);
    result.robustInstabilityPpm =
        partsPerMillion(result.robustUnstablePixels,
                        static_cast<quint64>(pixelCount) * result.robustFrames);
    result.robustEvenInstabilityPpm = partsPerMillion(
        result.robustUnstableEvenLines, evenPixels * result.robustFrames);
    result.robustOddInstabilityPpm = partsPerMillion(
        result.robustUnstableOddLines, oddPixels * result.robustFrames);
    result.artifactPpm = partsPerMillion(
        result.isolatedPixels + result.thirdColorPixels, pixelCount);
    const double evenArtifactPpm = partsPerMillion(
        result.isolatedEvenLines + result.thirdColorEvenLines, evenPixels);
    const double oddArtifactPpm = partsPerMillion(
        result.isolatedOddLines + result.thirdColorOddLines, oddPixels);
    result.score = result.instabilityPpm + kArtifactWeight * result.artifactPpm;
    result.evenScore =
        result.evenInstabilityPpm + kArtifactWeight * evenArtifactPpm;
    result.oddScore =
        result.oddInstabilityPpm + kArtifactWeight * oddArtifactPpm;

    result.modalImage =
        QImage(width_, logical_height_ * 2, QImage::Format_RGB32);
    for (int y = 0; y < logical_height_; ++y) {
        auto *first =
            reinterpret_cast<QRgb *>(result.modalImage.scanLine(y * 2));
        auto *second =
            reinterpret_cast<QRgb *>(result.modalImage.scanLine(y * 2 + 1));
        const auto *source =
            reinterpret_cast<const quint8 *>(modal.constData()) +
            static_cast<qsizetype>(y) * width_;
        for (int x = 0; x < width_; ++x) {
            const int index = source[x];
            first[x] = index < static_cast<int>(palette_.size())
                           ? palette_[index]
                           : qRgb(255, 0, 255);
            second[x] = first[x];
        }
    }
    return result;
}

EngineeringFidelityMetrics compareEngineeringModals(const QImage &candidate,
                                                    const QImage &reference) {
    EngineeringFidelityMetrics result;
    if (candidate.isNull() || reference.isNull() ||
        candidate.size() != reference.size() || candidate.width() <= 0 ||
        candidate.height() <= 0 || (candidate.height() & 1) != 0) {
        return result;
    }
    result.valid = true;
    result.pixels = static_cast<quint64>(candidate.width()) *
                    static_cast<quint64>(candidate.height() / 2);
    constexpr QRgb black = 0xff000000u;
    for (int y = 0; y < candidate.height(); y += 2) {
        const auto *candidateRow =
            reinterpret_cast<const QRgb *>(candidate.constScanLine(y));
        const auto *referenceRow =
            reinterpret_cast<const QRgb *>(reference.constScanLine(y));
        for (int x = 0; x < candidate.width(); ++x) {
            const QRgb current = candidateRow[x] | 0xff000000u;
            const QRgb expected = referenceRow[x] | 0xff000000u;
            if (current == expected) {
                continue;
            }
            ++result.mismatchedPixels;
            if (current == black) {
                ++result.erasedPixels;
            } else if (expected == black) {
                ++result.filledPixels;
            } else {
                ++result.recoloredPixels;
            }
        }
    }
    result.mismatchPpm =
        partsPerMillion(result.mismatchedPixels, result.pixels);
    return result;
}
