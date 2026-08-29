/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "configuration_dialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

ConfigurationDialog::ConfigurationDialog(const PicoConfiguration &configuration,
                                         QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Configure Pico 2"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/retro/configure.png")));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        QStringLiteral("Tune the capture geometry and the eight RGB444 "
                       "source colors. Every change is applied immediately "
                       "to the Pico 2 and the viewer. Saving to flash remains "
                       "a separate action."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *capture = new QGroupBox(QStringLiteral("Capture alignment"), this);
    auto *captureForm = new QFormLayout(capture);
    vertical_ = new QSpinBox(capture);
    vertical_->setRange(P2000T_CONTROL_MIN_VERTICAL,
                        P2000T_CONTROL_MAX_VERTICAL);
    vertical_->setSuffix(QStringLiteral(" scanline"));
    phase_ = new QSpinBox(capture);
    phase_->setRange(P2000T_CONTROL_MIN_PHASE, P2000T_CONTROL_MAX_PHASE);
    phase_->setPrefix(QStringLiteral("Phase "));
    phase_->setSuffix(QStringLiteral(" tick"));
    odd_line_phase_ = new QSpinBox(capture);
    odd_line_phase_->setRange(P2000T_CONTROL_MIN_ODD_LINE_PHASE,
                              P2000T_CONTROL_MAX_ODD_LINE_PHASE);
    odd_line_phase_->setPrefix(QStringLiteral("Phase "));
    odd_line_phase_->setSuffix(QStringLiteral(" tick"));
    odd_line_phase_->setToolTip(QStringLiteral(
        "Extra delay for odd-numbered physical P2000T source lines; "
        "positive values sample them later."));
    sample_rate_trim_ = new QSpinBox(capture);
    sample_rate_trim_->setRange(P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM,
                                P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM);
    sample_rate_trim_->setPrefix(QStringLiteral("Trim "));
    sample_rate_trim_->setSuffix(QStringLiteral(" step"));
    sample_rate_trim_->setToolTip(QStringLiteral(
        "Adjust the complete horizontal sampling interval in 1/256 PIO "
        "divider steps. Positive values make the captured line wider; "
        "each step moves its right edge by about 0.94 pixel."));
    sample_reconstruction_ = new QComboBox(capture);
    sample_reconstruction_->addItem(QStringLiteral("Raw two taps (legacy)"),
                                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW);
    sample_reconstruction_->addItem(
        QStringLiteral("Guarded second tap (stable)"),
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP);
    sample_reconstruction_->addItem(
        QStringLiteral("Sharp guarded taps"),
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SHARP_GUARDED);
    sample_reconstruction_->addItem(
        QStringLiteral("126 MHz windows: center"),
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CENTER);
    sample_reconstruction_->addItem(
        QStringLiteral("126 MHz windows: channel majority"),
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CHANNEL_MAJORITY);
    sample_reconstruction_->addItem(
        QStringLiteral("126 MHz windows: atomic early endpoint"),
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_EARLY);
    sample_reconstruction_->addItem(
        QStringLiteral("126 MHz windows: atomic late endpoint"),
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_LATE);
    sample_reconstruction_->addItem(
        QStringLiteral("126 MHz windows: confidence guard"),
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CONFIDENCE_GUARD);
    sample_reconstruction_->setToolTip(QStringLiteral(
        "The SAA5050 emits 240 nominal source dots. Stable reconstruction "
        "offers the legacy duplicate filter, a sharp two-tap filter, and Pico "
        "2 three-sample 126 MHz windows. All Pico 2 reconstruction modes can "
        "be saved to flash."));
    horizontal_ = new QSpinBox(capture);
    horizontal_->setRange(
        P2000T_CONTROL_MIN_HORIZONTAL / P2000T_CONTROL_HORIZONTAL_STEP,
        P2000T_CONTROL_MAX_HORIZONTAL / P2000T_CONTROL_HORIZONTAL_STEP);
    horizontal_->setSuffix(QStringLiteral(" characters (6 dots each)"));
    artwork_ = new QComboBox(capture);
    artwork_->addItems({QStringLiteral("Green phosphor"),
                        QStringLiteral("Synthwave"),
                        QStringLiteral("Amber circuit")});
    captureForm->addRow(QStringLiteral("First visible line:"), vertical_);
    captureForm->addRow(QStringLiteral("Fine sample phase:"), phase_);
    captureForm->addRow(QStringLiteral("Odd-line correction:"),
                        odd_line_phase_);
    captureForm->addRow(QStringLiteral("Horizontal rate trim:"),
                        sample_rate_trim_);
    captureForm->addRow(QStringLiteral("Source-dot reconstruction:"),
                        sample_reconstruction_);
    captureForm->addRow(QStringLiteral("Horizontal start:"), horizontal_);
    captureForm->addRow(QStringLiteral("No-signal artwork:"), artwork_);
    connect(vertical_, &QSpinBox::valueChanged, this,
            [this] { applyLiveChange(); });
    connect(phase_, &QSpinBox::valueChanged, this,
            [this] { applyLiveChange(); });
    connect(odd_line_phase_, &QSpinBox::valueChanged, this,
            [this] { applyLiveChange(); });
    connect(sample_rate_trim_, &QSpinBox::valueChanged, this,
            [this] { applyLiveChange(); });
    connect(sample_reconstruction_, &QComboBox::currentIndexChanged, this,
            [this] { applyLiveChange(); });
    connect(horizontal_, &QSpinBox::valueChanged, this,
            [this] { applyLiveChange(); });
    connect(artwork_, &QComboBox::currentIndexChanged, this,
            [this] { applyLiveChange(); });
    layout->addWidget(capture);

    auto *palette =
        new QGroupBox(QStringLiteral("P2000T eight-color palette"), this);
    auto *paletteGrid = new QGridLayout(palette);
    paletteGrid->addWidget(new QLabel(QStringLiteral("Color"), palette), 0, 0);
    paletteGrid->addWidget(new QLabel(QStringLiteral("Preview"), palette), 0,
                           1);
    paletteGrid->addWidget(new QLabel(QStringLiteral("Red"), palette), 0, 2);
    paletteGrid->addWidget(new QLabel(QStringLiteral("Green"), palette), 0, 3);
    paletteGrid->addWidget(new QLabel(QStringLiteral("Blue"), palette), 0, 4);
    static const std::array<const char *, P2000T_CONTROL_PALETTE_COLORS> names =
        {"Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"};
    for (int index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        paletteGrid->addWidget(
            new QLabel(QString::fromLatin1(names[static_cast<size_t>(index)]),
                       palette),
            index + 1, 0);
        color_buttons_[static_cast<size_t>(index)] =
            new QPushButton(QStringLiteral("Choose..."), palette);
        color_buttons_[static_cast<size_t>(index)]->setMinimumWidth(96);
        paletteGrid->addWidget(color_buttons_[static_cast<size_t>(index)],
                               index + 1, 1);
        connect(color_buttons_[static_cast<size_t>(index)],
                &QPushButton::clicked, this,
                [this, index] { chooseColor(index); });
        const std::array<
            std::array<QSpinBox *, P2000T_CONTROL_PALETTE_COLORS> *, 3>
            channels = {&red_, &green_, &blue_};
        for (int channel = 0; channel < 3; ++channel) {
            auto *spinner = new QSpinBox(palette);
            spinner->setRange(0, 15);
            spinner->setAlignment(Qt::AlignRight);
            (*channels[static_cast<size_t>(channel)])[static_cast<size_t>(
                index)] = spinner;
            paletteGrid->addWidget(spinner, index + 1, channel + 2);
            connect(spinner, &QSpinBox::valueChanged, this, [this, index] {
                updateColorButton(index);
                applyLiveChange();
            });
        }
    }
    layout->addWidget(palette);

    auto *storage = new QGroupBox(QStringLiteral("Persistent settings"), this);
    auto *storageLayout = new QHBoxLayout(storage);
    storage_status_ = new QLabel(storage);
    storage_status_->setWordWrap(true);
    storageLayout->addWidget(storage_status_, 1);
    reload_ = new QPushButton(QStringLiteral("Reload from Pico"), storage);
    auto *defaults =
        new QPushButton(QStringLiteral("Factory reset"), storage);
    defaults->setToolTip(QStringLiteral(
        "Restore the known-good firmware defaults and save them to Pico "
        "flash immediately."));
    auto *save = new QPushButton(QStringLiteral("Save to Pico"), storage);
    storageLayout->addWidget(reload_);
    storageLayout->addWidget(defaults);
    storageLayout->addWidget(save);
    connect(reload_, &QPushButton::clicked, this,
            [this] { emit reloadRequested(); });
    connect(defaults, &QPushButton::clicked, this,
            [this] { emit defaultsRequested(); });
    connect(save, &QPushButton::clicked, this,
            [this] { emit saveRequested(this->configuration()); });
    layout->addWidget(storage);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);

    setConfiguration(configuration);
    resize(620, sizeHint().height());
}

void ConfigurationDialog::applyLiveChange() {
    storage_status_->setText(QStringLiteral(
        "Applying live; use Save to Pico to keep these settings after "
        "restart."));
    storage_status_->setStyleSheet(QStringLiteral("color: #205080;"));
    emit configurationChanged(configuration());
}

PicoConfiguration ConfigurationDialog::configuration() const {
    PicoConfiguration result;
    result.vertical = vertical_->value();
    result.phase = phase_->value();
    result.oddLinePhase = odd_line_phase_->value();
    result.sampleRateTrim = sample_rate_trim_->value();
    result.sampleReconstruction = sample_reconstruction_->currentData().toInt();
    result.horizontal = horizontal_->value() * P2000T_CONTROL_HORIZONTAL_STEP;
    result.artwork = artwork_->currentIndex();
    for (int index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        result.palette[static_cast<size_t>(index)] = static_cast<quint16>(
            red_[static_cast<size_t>(index)]->value() |
            (green_[static_cast<size_t>(index)]->value() << 4) |
            (blue_[static_cast<size_t>(index)]->value() << 8));
    }
    return result;
}

void ConfigurationDialog::setConfiguration(
    const PicoConfiguration &configuration) {
    const QSignalBlocker verticalBlock(vertical_);
    const QSignalBlocker phaseBlock(phase_);
    const QSignalBlocker oddLinePhaseBlock(odd_line_phase_);
    const QSignalBlocker sampleRateTrimBlock(sample_rate_trim_);
    const QSignalBlocker sampleReconstructionBlock(sample_reconstruction_);
    const QSignalBlocker horizontalBlock(horizontal_);
    const QSignalBlocker artworkBlock(artwork_);
    vertical_->setValue(configuration.vertical);
    phase_->setValue(configuration.phase);
    odd_line_phase_->setValue(configuration.oddLinePhase);
    sample_rate_trim_->setValue(configuration.sampleRateTrim);
    const int reconstructionIndex =
        sample_reconstruction_->findData(configuration.sampleReconstruction);
    sample_reconstruction_->setCurrentIndex(
        reconstructionIndex >= 0 ? reconstructionIndex : 0);
    horizontal_->setValue(configuration.horizontal /
                          P2000T_CONTROL_HORIZONTAL_STEP);
    artwork_->setCurrentIndex(configuration.artwork);
    for (int index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
        const quint16 color = configuration.palette[static_cast<size_t>(index)];
        const QSignalBlocker redBlock(red_[static_cast<size_t>(index)]);
        const QSignalBlocker greenBlock(green_[static_cast<size_t>(index)]);
        const QSignalBlocker blueBlock(blue_[static_cast<size_t>(index)]);
        red_[static_cast<size_t>(index)]->setValue(color & 0x0fu);
        green_[static_cast<size_t>(index)]->setValue((color >> 4u) & 0x0fu);
        blue_[static_cast<size_t>(index)]->setValue((color >> 8u) & 0x0fu);
        updateColorButton(index);
    }
    updateStorageStatus(configuration);
}

void ConfigurationDialog::chooseColor(int index) {
    const PicoConfiguration current = configuration();
    const quint16 rgb444 = current.palette[static_cast<size_t>(index)];
    const QColor initial((rgb444 & 0x0fu) * 17, ((rgb444 >> 4u) & 0x0fu) * 17,
                         ((rgb444 >> 8u) & 0x0fu) * 17);
    const QColor selected = QColorDialog::getColor(
        initial, this, QStringLiteral("Choose palette color"));
    if (!selected.isValid()) {
        return;
    }
    {
        const QSignalBlocker redBlock(red_[static_cast<size_t>(index)]);
        const QSignalBlocker greenBlock(green_[static_cast<size_t>(index)]);
        const QSignalBlocker blueBlock(blue_[static_cast<size_t>(index)]);
        red_[static_cast<size_t>(index)]->setValue((selected.red() + 8) / 17);
        green_[static_cast<size_t>(index)]->setValue((selected.green() + 8) /
                                                     17);
        blue_[static_cast<size_t>(index)]->setValue((selected.blue() + 8) / 17);
    }
    updateColorButton(index);
    applyLiveChange();
}

void ConfigurationDialog::updateColorButton(int index) {
    const int red = red_[static_cast<size_t>(index)]->value() * 17;
    const int green = green_[static_cast<size_t>(index)]->value() * 17;
    const int blue = blue_[static_cast<size_t>(index)]->value() * 17;
    const QColor color(red, green, blue);
    const QColor text = color.lightness() < 128 ? Qt::white : Qt::black;
    color_buttons_[static_cast<size_t>(index)]->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; "
                       "border: 1px solid #555; padding: 4px; }")
            .arg(color.name(), text.name()));
}

void ConfigurationDialog::updateStorageStatus(
    const PicoConfiguration &configuration) {
    reload_->setEnabled(configuration.storedAvailable);
    if (configuration.saveFailed) {
        storage_status_->setText(
            QStringLiteral("The last flash save failed; settings were not "
                           "stored."));
        storage_status_->setStyleSheet(QStringLiteral("color: #a00000;"));
    } else if (!configuration.storedAvailable) {
        storage_status_->setText(QStringLiteral(
            "No valid saved settings are present on this Pico."));
    } else if (configuration.matchesStored) {
        storage_status_->setText(
            QStringLiteral("Current settings match the saved flash copy."));
    } else {
        storage_status_->setText(QStringLiteral(
            "Current settings differ from the saved flash copy."));
    }
    if (!configuration.saveFailed) {
        storage_status_->setStyleSheet(QString());
    }
}
