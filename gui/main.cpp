/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.cpp
 * @brief Minimal Qt 6 viewer for Pico 2 RGB111 screen captures.
 */

#include <array>
#include <cstdint>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "adapter_serial_port.h"
#include "configuration_dialog.h"
#include "no_signal_screen.h"
#include "p2000t_packbits.h"
#include "p2000t_stream_protocol.h"
#include "pico_configuration.h"

namespace {

constexpr qsizetype kMaximumPayloadSize = 65536;

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

constexpr std::array<quint32, 256> makeCrc32Table() {
    std::array<quint32, 256> table = {};
    for (quint32 value = 0; value < table.size(); ++value) {
        quint32 remainder = value;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            remainder =
                (remainder >> 1u) ^ ((remainder & 1u) != 0u ? 0xedb88320u : 0u);
        }
        table[value] = remainder;
    }
    return table;
}

constexpr auto kCrc32Table = makeCrc32Table();

quint32 crc32(const QByteArray &data) {
    quint32 crc = 0xffffffffu;
    for (const unsigned char byte : data) {
        crc = (crc >> 8u) ^ kCrc32Table[(crc ^ byte) & 0xffu];
    }
    return ~crc;
}

struct CalibrationSweepOptions {
    int firstPhase = P2000T_CONTROL_MIN_PHASE;
    int lastPhase = P2000T_CONTROL_MAX_PHASE;
    int firstRateTrim = P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM;
    int lastRateTrim = P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM;
    int oddLinePhase = P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE;
    int settlingFrames = 2;
    int framesPerSetting = 3;

    int settingCount() const {
        return (lastPhase - firstPhase + 1) *
               (lastRateTrim - firstRateTrim + 1);
    }

    int imageCount() const {
        return settingCount() * framesPerSetting;
    }
};

class CalibrationSweepDialog final : public QDialog {
  public:
    explicit CalibrationSweepDialog(const PicoConfiguration &configuration,
                                    QWidget *parent = nullptr)
        : QDialog(parent), fixed_odd_line_phase_(configuration.oddLinePhase) {
        setWindowTitle(QStringLiteral("Capture calibration sweep"));
        setModal(true);

        auto *layout = new QVBoxLayout(this);
        auto *intro = new QLabel(
            QStringLiteral(
                "The viewer will sweep fine sample phase and horizontal "
                "rate trim. It saves multiple consecutive source frames "
                "for every pair so intermittent artifacts remain visible. "
                "The current odd-line correction (%1) and source-dot "
                "reconstruction mode (%2) stay fixed.")
                .arg(configuration.oddLinePhase)
                .arg(configuration.sampleReconstruction ==
                             P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP
                         ? QStringLiteral("second tap")
                         : QStringLiteral("raw")),
            this);
        intro->setWordWrap(true);
        layout->addWidget(intro);

        auto *form = new QFormLayout();
        first_phase_ =
            makeSpinBox(P2000T_CONTROL_MIN_PHASE, P2000T_CONTROL_MAX_PHASE,
                        P2000T_CONTROL_MIN_PHASE);
        last_phase_ =
            makeSpinBox(P2000T_CONTROL_MIN_PHASE, P2000T_CONTROL_MAX_PHASE,
                        P2000T_CONTROL_MAX_PHASE);
        first_rate_ = makeSpinBox(P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM,
                                  P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM,
                                  P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM);
        last_rate_ = makeSpinBox(P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM,
                                 P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM,
                                 P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM);
        settling_frames_ = makeSpinBox(0, 20, 2);
        frames_per_setting_ = makeSpinBox(1, 50, 3);
        form->addRow(QStringLiteral("First sample phase:"), first_phase_);
        form->addRow(QStringLiteral("Last sample phase:"), last_phase_);
        form->addRow(QStringLiteral("First rate trim:"), first_rate_);
        form->addRow(QStringLiteral("Last rate trim:"), last_rate_);
        form->addRow(QStringLiteral("Frames to settle:"), settling_frames_);
        form->addRow(QStringLiteral("PNGs per setting:"), frames_per_setting_);
        layout->addLayout(form);

        estimate_ = new QLabel(this);
        estimate_->setWordWrap(true);
        layout->addWidget(estimate_);

        const std::array<QSpinBox *, 6> spinners = {
            first_phase_, first_rate_,      last_phase_,
            last_rate_,   settling_frames_, frames_per_setting_};
        for (auto *spinner : spinners) {
            connect(spinner, &QSpinBox::valueChanged, this,
                    [this] { updateEstimate(); });
        }

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)
            ->setText(QStringLiteral("Choose output folder..."));
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (first_phase_->value() > last_phase_->value() ||
                first_rate_->value() > last_rate_->value()) {
                QMessageBox::warning(
                    this, QStringLiteral("Invalid sweep range"),
                    QStringLiteral("Each first value must be less than or "
                                   "equal to its last value."));
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        updateEstimate();
    }

    CalibrationSweepOptions options() const {
        CalibrationSweepOptions result;
        result.firstPhase = first_phase_->value();
        result.lastPhase = last_phase_->value();
        result.firstRateTrim = first_rate_->value();
        result.lastRateTrim = last_rate_->value();
        result.oddLinePhase = fixed_odd_line_phase_;
        result.settlingFrames = settling_frames_->value();
        result.framesPerSetting = frames_per_setting_->value();
        return result;
    }

  private:
    QSpinBox *makeSpinBox(int minimum, int maximum, int value) {
        auto *spinner = new QSpinBox(this);
        spinner->setRange(minimum, maximum);
        spinner->setValue(value);
        return spinner;
    }

    void updateEstimate() {
        const int phases =
            qMax(0, last_phase_->value() - first_phase_->value() + 1);
        const int rates =
            qMax(0, last_rate_->value() - first_rate_->value() + 1);
        const int settings = phases * rates;
        const int images = settings * frames_per_setting_->value();
        const int sourceFrames = settings * (settling_frames_->value() +
                                             frames_per_setting_->value());
        estimate_->setText(
            QStringLiteral(
                "%1 setting pairs, %2 PNGs; approximately %3 seconds at "
                "25 streamed frames/s. The original settings are restored "
                "when the sweep finishes or is cancelled.")
                .arg(settings)
                .arg(images)
                .arg((sourceFrames + 24) / 25));
    }

    QSpinBox *first_phase_ = nullptr;
    QSpinBox *last_phase_ = nullptr;
    QSpinBox *first_rate_ = nullptr;
    QSpinBox *last_rate_ = nullptr;
    QSpinBox *settling_frames_ = nullptr;
    QSpinBox *frames_per_setting_ = nullptr;
    QLabel *estimate_ = nullptr;
    int fixed_odd_line_phase_ = P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE;
};

class FrameView final : public QWidget {
  public:
    explicit FrameView(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(480, 480);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setFrame(const QImage &frame) {
        frame_ = frame;
        update();
    }

    QSize sizeHint() const override {
        return QSize(720, 720);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (frame_.isNull()) {
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("Waiting for Pico 2 capture..."));
            return;
        }
        const QSize target = frame_.size().scaled(size(), Qt::KeepAspectRatio);
        const QRect destination(QPoint((width() - target.width()) / 2,
                                       (height() - target.height()) / 2),
                                target);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(destination, frame_);
    }

  private:
    QImage frame_;
};

class CaptureWindow final : public QMainWindow {
  public:
    CaptureWindow() {
        setWindowTitle(QStringLiteral("P2000T VID2VGA Capture %1")
                           .arg(QStringLiteral(P2000T_VIEWER_VERSION)));

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        auto *controls = new QHBoxLayout();
        ports_ = new QComboBox(central);
        encoding_ = new QComboBox(central);
        encoding_->addItem(QStringLiteral("PackBits (recommended)"), 'c');
        encoding_->addItem(QStringLiteral("Raw RGB111"), 'r');
        auto *refresh = new QPushButton(QStringLiteral("Refresh"), central);
        connect_ = new QPushButton(QStringLiteral("Connect"), central);
        connect_->setIcon(QIcon(QStringLiteral(":/icons/retro/connect.png")));
        save_ = new QPushButton(QStringLiteral("Save PNG"), central);
        save_->setEnabled(false);
        controls->addWidget(new QLabel(QStringLiteral("Pico port:"), central));
        controls->addWidget(ports_, 1);
        controls->addWidget(refresh);
        controls->addWidget(encoding_);
        controls->addWidget(connect_);
        controls->addWidget(save_);
        layout->addLayout(controls);
        view_ = new FrameView(central);
        layout->addWidget(view_, 1);
        setCentralWidget(central);
        resize(900, 760);

        createMenus();

        poll_timer_.setInterval(2);
        connect(&poll_timer_, &QTimer::timeout, this, [this] { pollSerial(); });
        calibration_ack_timer_.setSingleShot(true);
        calibration_ack_timer_.setInterval(1500);
        connect(&calibration_ack_timer_, &QTimer::timeout, this,
                [this] { calibrationAcknowledgementTimedOut(); });
        connect(refresh, &QPushButton::clicked, this,
                [this] { refreshPorts(); });
        connect(connect_, &QPushButton::clicked, this,
                [this] { toggleConnection(); });
        connect(save_, &QPushButton::clicked, this, [this] { saveFrame(); });

        refreshPorts();
        if (ports_->count() == 0) {
            statusBar()->showMessage(
                QStringLiteral("No Pico 2 USB port detected"));
        } else {
            statusBar()->showMessage(
                QStringLiteral("Pico detected; connecting automatically..."));
            QTimer::singleShot(0, this, [this] { connectAdapter(false); });
        }
    }

    ~CaptureWindow() override {
        disconnectAdapter();
    }

  private:
    enum class SweepEnd { Completed, Cancelled, Failed, Disconnected };

    static constexpr int kCalibrationAcknowledgementRetries = 2;

    void createMenus() {
        auto *exit =
            new QAction(QIcon(QStringLiteral(":/icons/retro/exit.png")),
                        QStringLiteral("E&xit"), this);
        exit->setShortcut(QKeySequence::Quit);
        connect(exit, &QAction::triggered, this, &QWidget::close);
        menuBar()->addMenu(QStringLiteral("&File"))->addAction(exit);

        auto *adapter = menuBar()->addMenu(QStringLiteral("&Adapter"));
        connect_action_ = adapter->addAction(
            QIcon(QStringLiteral(":/icons/retro/connect.png")),
            QStringLiteral("&Connect"));
        connect(connect_action_, &QAction::triggered, this,
                [this] { toggleConnection(); });
        auto *refresh = adapter->addAction(QStringLiteral("&Refresh ports"));
        connect(refresh, &QAction::triggered, this, [this] { refreshPorts(); });
        adapter->addSeparator();

        configuration_action_ = adapter->addAction(
            QIcon(QStringLiteral(":/icons/retro/configure.png")),
            QStringLiteral("&Configure Pico 2..."));
        connect(configuration_action_, &QAction::triggered, this,
                [this] { openConfigurationDialog(); });
        configuration_action_->setEnabled(false);

        calibration_sweep_action_ =
            adapter->addAction(QStringLiteral("Capture calibration &sweep..."));
        calibration_sweep_action_->setToolTip(QStringLiteral(
            "Sweep phase and horizontal rate trim into a folder of PNGs"));
        connect(calibration_sweep_action_, &QAction::triggered, this,
                [this] { startCalibrationSweep(); });
        calibration_sweep_action_->setEnabled(false);

        auto *help = menuBar()->addMenu(QStringLiteral("&Help"));
        auto *about =
            help->addAction(QIcon(QStringLiteral(":/icons/retro/about.png")),
                            QStringLiteral("&About"));
        connect(about, &QAction::triggered, this, [this] {
            QMessageBox::about(
                this, QStringLiteral("About P2000T VID2VGA Capture"),
                QStringLiteral(
                    "<h3>P2000T VID2VGA Capture %1</h3>"
                    "<p>Live USB capture viewer and configuration utility "
                    "for the Raspberry Pi Pico 2 P2000T video-to-VGA "
                    "adapter.</p>"
                    "<p>Copyright &copy; 2026 Ivo Filot</p>"
                    "<p>This program is free software, licensed under the "
                    "GNU General Public License version 3 or later. It is "
                    "provided without warranty.</p>"
                    "<p>Built with Qt %2. Qt is used under the GNU Lesser "
                    "General Public License version 3.</p>"
                    "<p>License texts and notices are in the "
                    "<code>licenses</code> directory distributed with the "
                    "application.<br>Source code: "
                    "github.com/ifilot/p2000t-video-to-vga-adapter</p>")
                    .arg(QStringLiteral(P2000T_VIEWER_VERSION),
                         QString::fromLatin1(qVersion())));
        });

        auto *about_qt = help->addAction(QStringLiteral("About &Qt"));
        connect(about_qt, &QAction::triggered, this, [this] {
            QMessageBox::aboutQt(this, QStringLiteral("About Qt"));
        });
    }

    void refreshPorts() {
        const QString selected = ports_->currentText();
        ports_->clear();
        ports_->addItems(picoSerialPorts());
        const int previous = ports_->findText(selected);
        if (previous >= 0) {
            ports_->setCurrentIndex(previous);
        }
        connect_->setEnabled(connected_ || ports_->count() != 0);
        if (connect_action_ != nullptr) {
            connect_action_->setEnabled(connected_ || ports_->count() != 0);
        }
    }

    void toggleConnection() {
        if (connected_) {
            disconnectAdapter();
            return;
        }
        connectAdapter(true);
    }

    bool connectAdapter(bool reportError) {
        if (connected_) {
            return true;
        }
        const QString port = ports_->currentText();
        if (port.isEmpty() || !serial_.open(port)) {
            statusBar()->showMessage(
                QStringLiteral("Could not open %1").arg(port));
            if (reportError) {
                QMessageBox::critical(
                    this, QStringLiteral("Connection failed"),
                    QStringLiteral("Could not open %1.").arg(port));
            }
            return false;
        }
        connected_ = true;
        last_signal_present_ = false;
        receive_buffer_.clear();
        frame_count_ = 0;
        crc_errors_ = 0;
        byte_count_ = 0;
        rate_timer_.restart();
        ports_->setEnabled(false);
        encoding_->setEnabled(false);
        connect_->setText(QStringLiteral("Disconnect"));
        connect_action_->setText(QStringLiteral("&Disconnect"));
        configuration_action_->setEnabled(false);
        poll_timer_.start();
        statusBar()->showMessage(
            QStringLiteral("Initializing %1...").arg(port));

        // Opening CDC asserts DTR. Allow the firmware's connection banner to
        // finish before selecting its one-character binary screen command.
        QTimer::singleShot(350, this, [this] {
            if (!connected_) {
                return;
            }
            const char command =
                static_cast<char>(encoding_->currentData().toInt());
            QByteArray request(1, command);
            request.append(makePicoControlPacket(P2000T_CONTROL_GET_SETTINGS));
            if (!serial_.write(request)) {
                connectionLost();
            }
        });
        return true;
    }

    void disconnectAdapter() {
        if (calibration_sweep_active_) {
            endCalibrationSweep(SweepEnd::Disconnected);
        }
        if (configuration_dialog_ != nullptr) {
            configuration_dialog_->reject();
        }
        if (serial_.isOpen()) {
            serial_.write(QByteArray(1, 'q'));
        }
        poll_timer_.stop();
        serial_.close();
        connected_ = false;
        last_signal_present_ = false;
        ports_->setEnabled(true);
        encoding_->setEnabled(true);
        connect_->setText(QStringLiteral("Connect"));
        connect_->setEnabled(ports_->count() != 0);
        connect_action_->setText(QStringLiteral("&Connect"));
        connect_action_->setEnabled(ports_->count() != 0);
        configuration_action_->setEnabled(false);
        calibration_sweep_action_->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Disconnected"));
    }

    bool sendControl(const QByteArray &packets) {
        if (!connected_) {
            return false;
        }
        if (!serial_.write(packets)) {
            connectionLost();
            return false;
        }
        return true;
    }

    void sendConfiguration(const PicoConfiguration &configuration, bool save) {
        QByteArray packets;
        packets += makePicoControlPacket(P2000T_CONTROL_SET_VERTICAL, 0,
                                         configuration.vertical);
        packets += makePicoControlPacket(
            P2000T_CONTROL_SET_PHASE, 0,
            static_cast<quint32>(static_cast<qint32>(configuration.phase)));
        packets +=
            makePicoControlPacket(P2000T_CONTROL_SET_ODD_LINE_PHASE, 0,
                                  static_cast<quint32>(static_cast<qint32>(
                                      configuration.oddLinePhase)));
        packets +=
            makePicoControlPacket(P2000T_CONTROL_SET_SAMPLE_RATE_TRIM, 0,
                                  static_cast<quint32>(static_cast<qint32>(
                                      configuration.sampleRateTrim)));
        packets += makePicoControlPacket(
            P2000T_CONTROL_SET_SAMPLE_RECONSTRUCTION, 0,
            static_cast<quint32>(configuration.sampleReconstruction));
        packets += makePicoControlPacket(P2000T_CONTROL_SET_HORIZONTAL, 0,
                                         configuration.horizontal);
        packets += makePicoControlPacket(P2000T_CONTROL_SET_ARTWORK, 0,
                                         configuration.artwork);
        for (int index = 0; index < P2000T_CONTROL_PALETTE_COLORS; ++index) {
            packets += makePicoControlPacket(
                P2000T_CONTROL_SET_PALETTE, static_cast<quint8>(index),
                configuration.palette[static_cast<size_t>(index)]);
        }
        if (save) {
            packets += makePicoControlPacket(P2000T_CONTROL_SAVE_SETTINGS);
        }
        if (sendControl(packets)) {
            statusBar()->showMessage(
                save ? QStringLiteral("Settings saved to Pico flash")
                     : QStringLiteral("Settings applied to Pico"),
                2500);
        }
    }

    void openConfigurationDialog() {
        ConfigurationDialog dialog(configuration_, this);
        configuration_dialog_ = &dialog;
        connect(&dialog, &ConfigurationDialog::configurationChanged, this,
                [this](const PicoConfiguration &configuration) {
                    sendConfiguration(configuration, false);
                });
        connect(&dialog, &ConfigurationDialog::saveRequested, this,
                [this](const PicoConfiguration &configuration) {
                    sendConfiguration(configuration, true);
                });
        connect(&dialog, &ConfigurationDialog::reloadRequested, this, [this] {
            sendControl(makePicoControlPacket(P2000T_CONTROL_LOAD_SETTINGS));
        });
        connect(&dialog, &ConfigurationDialog::defaultsRequested, this, [this] {
            sendControl(makePicoControlPacket(P2000T_CONTROL_FACTORY_DEFAULTS));
        });
        sendControl(makePicoControlPacket(P2000T_CONTROL_GET_SETTINGS));
        dialog.exec();
        configuration_dialog_ = nullptr;
    }

    static QString signedSweepValue(int value) {
        return QStringLiteral("%1%2")
            .arg(value >= 0 ? QStringLiteral("p") : QStringLiteral("m"))
            .arg(qAbs(value), 2, 10, QLatin1Char('0'));
    }

    bool sendCalibrationValues(int phase, int oddLinePhase, int rateTrim) {
        QByteArray packets;
        packets += makePicoControlPacket(
            P2000T_CONTROL_SET_PHASE, 0,
            static_cast<quint32>(static_cast<qint32>(phase)));
        packets += makePicoControlPacket(
            P2000T_CONTROL_SET_ODD_LINE_PHASE, 0,
            static_cast<quint32>(static_cast<qint32>(oddLinePhase)));
        packets += makePicoControlPacket(
            P2000T_CONTROL_SET_SAMPLE_RATE_TRIM, 0,
            static_cast<quint32>(static_cast<qint32>(rateTrim)));
        // Request one final state record after all three changes. Firmware may
        // coalesce intermediate records, so this response must describe the
        // complete tuple the sweep is about to label and capture.
        packets += makePicoControlPacket(P2000T_CONTROL_GET_SETTINGS);
        return sendControl(packets);
    }

    int calibrationSettingIndex() const {
        const int rateCount = calibration_options_.lastRateTrim -
                              calibration_options_.firstRateTrim + 1;
        return (calibration_phase_ - calibration_options_.firstPhase) *
                   rateCount +
               (calibration_rate_trim_ - calibration_options_.firstRateTrim) +
               1;
    }

    void updateCalibrationProgressLabel() {
        if (calibration_progress_ == nullptr) {
            return;
        }
        calibration_progress_->setLabelText(
            QStringLiteral(
                "Setting %1 of %2: phase %3, rate trim %4, odd-line phase "
                "%5\n%6 of %7 PNGs written")
                .arg(calibrationSettingIndex())
                .arg(calibration_options_.settingCount())
                .arg(calibration_phase_)
                .arg(calibration_rate_trim_)
                .arg(calibration_options_.oddLinePhase)
                .arg(calibration_images_written_)
                .arg(calibration_options_.imageCount()));
    }

    void transmitCalibrationTarget() {
        if (sendCalibrationValues(calibration_phase_,
                                  calibration_options_.oddLinePhase,
                                  calibration_rate_trim_)) {
            calibration_ack_timer_.start();
        }
    }

    void sendCalibrationTarget() {
        calibration_waiting_for_configuration_ = true;
        calibration_acknowledgement_retries_ = 0;
        calibration_settling_frames_ = 0;
        calibration_frame_at_setting_ = 0;
        updateCalibrationProgressLabel();
        statusBar()->showMessage(
            QStringLiteral("Calibration sweep: applying phase %1, rate %2")
                .arg(calibration_phase_)
                .arg(calibration_rate_trim_));
        transmitCalibrationTarget();
    }

    void calibrationAcknowledgementTimedOut() {
        if (!calibration_sweep_active_ ||
            !calibration_waiting_for_configuration_) {
            return;
        }
        if (calibration_acknowledgement_retries_ <
            kCalibrationAcknowledgementRetries) {
            ++calibration_acknowledgement_retries_;
            statusBar()->showMessage(
                QStringLiteral("Calibration sweep: retrying phase %1, rate "
                               "%2 acknowledgment (%3/%4)")
                    .arg(calibration_phase_)
                    .arg(calibration_rate_trim_)
                    .arg(calibration_acknowledgement_retries_)
                    .arg(kCalibrationAcknowledgementRetries));
            transmitCalibrationTarget();
            return;
        }
        endCalibrationSweep(
            SweepEnd::Failed,
            QStringLiteral(
                "The Pico did not acknowledge phase %1, odd-line phase %2, "
                "rate trim %3 after %4 attempts. Its last reported values "
                "were phase %5, odd-line phase %6, rate trim %7.")
                .arg(calibration_phase_)
                .arg(calibration_options_.oddLinePhase)
                .arg(calibration_rate_trim_)
                .arg(kCalibrationAcknowledgementRetries + 1)
                .arg(configuration_.phase)
                .arg(configuration_.oddLinePhase)
                .arg(configuration_.sampleRateTrim));
    }

    void startCalibrationSweep() {
        if (!connected_ || calibration_sweep_active_) {
            return;
        }
        if (!last_signal_present_) {
            QMessageBox::warning(
                this, QStringLiteral("No P2000T signal"),
                QStringLiteral("A stable P2000T source image is required "
                               "before starting a calibration sweep."));
            return;
        }

        CalibrationSweepDialog dialog(configuration_, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const CalibrationSweepOptions options = dialog.options();

        QSettings viewerSettings;
        QString initialDirectory =
            viewerSettings
                .value(QStringLiteral("calibration/lastParentDirectory"))
                .toString();
        if (initialDirectory.isEmpty() || !QDir(initialDirectory).exists()) {
            initialDirectory = QStandardPaths::writableLocation(
                QStandardPaths::PicturesLocation);
        }
        if (initialDirectory.isEmpty()) {
            initialDirectory = QDir::currentPath();
        }
        const QString parentDirectory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose calibration analysis folder"),
            initialDirectory,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (parentDirectory.isEmpty()) {
            return;
        }
        viewerSettings.setValue(
            QStringLiteral("calibration/lastParentDirectory"), parentDirectory);

        QDir parent(parentDirectory);
        const QString stem = QStringLiteral("p2000t-analysis-%1")
                                 .arg(QDateTime::currentDateTime().toString(
                                     QStringLiteral("yyyyMMdd-HHmmss")));
        QString directoryName = stem;
        for (int suffix = 2; parent.exists(directoryName); ++suffix) {
            directoryName = QStringLiteral("%1-%2").arg(stem).arg(suffix);
        }
        if (!parent.mkdir(directoryName)) {
            QMessageBox::critical(
                this, QStringLiteral("Could not create analysis folder"),
                QStringLiteral("Could not create %1 inside %2.")
                    .arg(directoryName, parentDirectory));
            return;
        }

        const QString outputDirectory = parent.filePath(directoryName);
        QFile sessionFile(
            QDir(outputDirectory).filePath(QStringLiteral("session.txt")));
        if (!sessionFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(
                this, QStringLiteral("Could not start calibration sweep"),
                QStringLiteral("Could not write session.txt in %1.")
                    .arg(outputDirectory));
            return;
        }
        {
            QTextStream session(&sessionFile);
            session << "P2000T VID2VGA calibration sweep\n"
                    << "viewer_version=" << P2000T_VIEWER_VERSION << '\n'
                    << "started_utc="
                    << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                    << '\n'
                    << "phase_first=" << options.firstPhase << '\n'
                    << "phase_last=" << options.lastPhase << '\n'
                    << "rate_trim_first=" << options.firstRateTrim << '\n'
                    << "rate_trim_last=" << options.lastRateTrim << '\n'
                    << "odd_line_phase_fixed=" << options.oddLinePhase << '\n'
                    << "sample_reconstruction="
                    << (configuration_.sampleReconstruction ==
                                P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP
                            ? "second_tap"
                            : "raw")
                    << '\n'
                    << "settling_frames=" << options.settlingFrames << '\n'
                    << "frames_per_setting=" << options.framesPerSetting << '\n'
                    << "original_phase=" << configuration_.phase << '\n'
                    << "original_odd_line_phase=" << configuration_.oddLinePhase
                    << '\n'
                    << "original_rate_trim=" << configuration_.sampleRateTrim
                    << '\n'
                    << "original_first_visible_line=" << configuration_.vertical
                    << '\n'
                    << "original_horizontal_start=" << configuration_.horizontal
                    << '\n';
        }
        sessionFile.close();

        calibration_manifest_.setFileName(
            QDir(outputDirectory).filePath(QStringLiteral("manifest.csv")));
        if (!calibration_manifest_.open(QIODevice::WriteOnly |
                                        QIODevice::Text)) {
            QMessageBox::critical(
                this, QStringLiteral("Could not start calibration sweep"),
                QStringLiteral("Could not write manifest.csv in %1.")
                    .arg(outputDirectory));
            return;
        }
        {
            QTextStream manifest(&calibration_manifest_);
            manifest << "filename,phase,odd_line_phase,rate_trim,"
                        "frame_at_setting,sequence,captured_utc\n";
            manifest.flush();
        }

        calibration_options_ = options;
        calibration_original_configuration_ = configuration_;
        calibration_directory_ = outputDirectory;
        calibration_phase_ = options.firstPhase;
        calibration_rate_trim_ = options.firstRateTrim;
        calibration_images_written_ = 0;
        calibration_sweep_active_ = true;

        configuration_action_->setEnabled(false);
        calibration_sweep_action_->setEnabled(false);
        calibration_progress_ = new QProgressDialog(
            QString(), QStringLiteral("Cancel and restore settings"), 0,
            options.imageCount(), this);
        calibration_progress_->setWindowTitle(
            QStringLiteral("Calibration sweep"));
        calibration_progress_->setWindowModality(Qt::WindowModal);
        calibration_progress_->setAutoClose(false);
        calibration_progress_->setAutoReset(false);
        calibration_progress_->setMinimumDuration(0);
        calibration_progress_->setValue(0);
        connect(calibration_progress_, &QProgressDialog::canceled, this,
                [this] { endCalibrationSweep(SweepEnd::Cancelled); });
        calibration_progress_->show();
        sendCalibrationTarget();
    }

    void
    handleCalibrationConfiguration(const PicoConfiguration &configuration) {
        if (!calibration_sweep_active_ ||
            !calibration_waiting_for_configuration_) {
            return;
        }
        if (configuration.phase != calibration_phase_ ||
            configuration.oddLinePhase != calibration_options_.oddLinePhase ||
            configuration.sampleRateTrim != calibration_rate_trim_) {
            return;
        }
        calibration_ack_timer_.stop();
        calibration_waiting_for_configuration_ = false;
        calibration_settling_frames_ = calibration_options_.settlingFrames;
        calibration_frame_at_setting_ = 0;
        statusBar()->showMessage(
            QStringLiteral("Calibration sweep: settling phase %1, rate %2")
                .arg(calibration_phase_)
                .arg(calibration_rate_trim_));
    }

    void handleCalibrationFrame(quint32 sequence) {
        if (!calibration_sweep_active_ ||
            calibration_waiting_for_configuration_) {
            return;
        }
        if (calibration_settling_frames_ > 0) {
            --calibration_settling_frames_;
            return;
        }

        const int imageNumber = calibration_frame_at_setting_ + 1;
        const QString filename =
            QStringLiteral("phase_%1_rate_%2_odd_%3_frame_%4_seq_%5.png")
                .arg(signedSweepValue(calibration_phase_),
                     signedSweepValue(calibration_rate_trim_),
                     signedSweepValue(calibration_options_.oddLinePhase))
                .arg(imageNumber, 2, 10, QLatin1Char('0'))
                .arg(static_cast<qulonglong>(sequence), 10, 10,
                     QLatin1Char('0'));
        const QString path = QDir(calibration_directory_).filePath(filename);
        if (!current_frame_.save(path, "PNG")) {
            endCalibrationSweep(
                SweepEnd::Failed,
                QStringLiteral("Could not write %1.").arg(path));
            return;
        }

        {
            QTextStream manifest(&calibration_manifest_);
            manifest << filename << ',' << calibration_phase_ << ','
                     << calibration_options_.oddLinePhase << ','
                     << calibration_rate_trim_ << ',' << imageNumber << ','
                     << sequence << ','
                     << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                     << '\n';
            manifest.flush();
        }
        calibration_manifest_.flush();
        if (calibration_manifest_.error() != QFileDevice::NoError) {
            endCalibrationSweep(SweepEnd::Failed,
                                QStringLiteral("Could not update %1.")
                                    .arg(calibration_manifest_.fileName()));
            return;
        }

        ++calibration_frame_at_setting_;
        ++calibration_images_written_;
        calibration_progress_->setValue(calibration_images_written_);
        updateCalibrationProgressLabel();
        if (calibration_frame_at_setting_ <
            calibration_options_.framesPerSetting) {
            return;
        }

        if (calibration_rate_trim_ < calibration_options_.lastRateTrim) {
            ++calibration_rate_trim_;
        } else {
            calibration_rate_trim_ = calibration_options_.firstRateTrim;
            ++calibration_phase_;
        }
        if (calibration_phase_ > calibration_options_.lastPhase) {
            endCalibrationSweep(SweepEnd::Completed);
            return;
        }
        sendCalibrationTarget();
    }

    void endCalibrationSweep(SweepEnd end, const QString &detail = QString()) {
        if (!calibration_sweep_active_) {
            return;
        }
        calibration_sweep_active_ = false;
        calibration_ack_timer_.stop();
        const QString directory = calibration_directory_;
        const int imagesWritten = calibration_images_written_;
        const PicoConfiguration original = calibration_original_configuration_;

        if (calibration_manifest_.isOpen()) {
            calibration_manifest_.close();
        }
        if (calibration_progress_ != nullptr) {
            calibration_progress_->close();
            calibration_progress_->deleteLater();
            calibration_progress_ = nullptr;
        }

        bool restored = true;
        if (end != SweepEnd::Disconnected && connected_) {
            restored = sendCalibrationValues(
                original.phase, original.oddLinePhase, original.sampleRateTrim);
        }
        configuration_action_->setEnabled(connected_);
        calibration_sweep_action_->setEnabled(connected_);
        if (!restored) {
            return;
        }

        if (end == SweepEnd::Completed) {
            statusBar()->showMessage(
                QStringLiteral("Calibration sweep complete; settings restored"),
                5000);
            QMessageBox::information(
                this, QStringLiteral("Calibration sweep complete"),
                QStringLiteral("Saved %1 PNGs plus manifest.csv and "
                               "session.txt in:\n%2\n\nThe original adapter "
                               "settings have been restored.")
                    .arg(imagesWritten)
                    .arg(QDir::toNativeSeparators(directory)));
        } else if (end == SweepEnd::Cancelled) {
            statusBar()->showMessage(
                QStringLiteral("Calibration sweep cancelled after %1 PNGs; "
                               "settings restored")
                    .arg(imagesWritten),
                5000);
        } else if (end == SweepEnd::Failed) {
            QMessageBox::critical(
                this, QStringLiteral("Calibration sweep stopped"),
                QStringLiteral("%1\n\nThe original adapter settings have "
                               "been restored. Partial results remain in:\n%2")
                    .arg(detail, QDir::toNativeSeparators(directory)));
        }
    }

    void connectionLost() {
        disconnectAdapter();
        QMessageBox::warning(
            this, QStringLiteral("Connection lost"),
            QStringLiteral("The Pico 2 USB connection closed."));
    }

    void pollSerial() {
        bool ok = false;
        const QByteArray received = serial_.readAvailable(&ok);
        if (!ok) {
            connectionLost();
            return;
        }
        if (!received.isEmpty()) {
            byte_count_ += static_cast<quint64>(received.size());
            receive_buffer_.append(received);
            parseRecords();
        }
    }

    void parseRecords() {
        const QByteArray magic(P2000T_STREAM_MAGIC, 4);
        while (true) {
            const qsizetype magic_offset = receive_buffer_.indexOf(magic);
            if (magic_offset < 0) {
                if (receive_buffer_.size() > 3) {
                    receive_buffer_.remove(0, receive_buffer_.size() - 3);
                }
                return;
            }
            if (magic_offset != 0) {
                receive_buffer_.remove(0, magic_offset);
            }
            if (receive_buffer_.size() < P2000T_STREAM_HEADER_SIZE) {
                return;
            }

            const char *header = receive_buffer_.constData();
            const quint8 version = static_cast<quint8>(header[4]);
            const quint8 encoding = static_cast<quint8>(header[5]);
            const quint16 flags = loadU16(&header[6]);
            const quint16 width = loadU16(&header[16]);
            const quint16 height = loadU16(&header[18]);
            const quint16 stride = loadU16(&header[20]);
            const quint16 header_size = loadU16(&header[22]);
            const quint32 payload_size = loadU32(&header[24]);
            const quint32 expected_crc = loadU32(&header[28]);
            const quint32 uncompressed_size = loadU32(&header[32]);
            const quint32 sequence = loadU32(&header[8]);
            const quint8 artwork =
                static_cast<quint8>(header[P2000T_STREAM_ARTWORK_OFFSET]);
            const bool signal_present =
                (flags & P2000T_STREAM_FLAG_SIGNAL_PRESENT) != 0u;
            const bool configuration_record =
                encoding == P2000T_STREAM_ENCODING_CONFIGURATION &&
                (flags & P2000T_STREAM_FLAG_CONFIGURATION_STATE) != 0u;
            if (configuration_record) {
                const bool valid_configuration_header =
                    version == P2000T_STREAM_PROTOCOL_VERSION &&
                    header_size == P2000T_STREAM_HEADER_SIZE &&
                    payload_size == P2000T_CONFIGURATION_STATE_SIZE &&
                    uncompressed_size == P2000T_CONFIGURATION_STATE_SIZE;
                if (!valid_configuration_header) {
                    receive_buffer_.remove(0, 1);
                    continue;
                }
                const qsizetype record_size =
                    static_cast<qsizetype>(header_size) + payload_size;
                if (receive_buffer_.size() < record_size) {
                    return;
                }
                const QByteArray payload = receive_buffer_.mid(
                    header_size, static_cast<qsizetype>(payload_size));
                receive_buffer_.remove(0, record_size);
                PicoConfiguration configuration;
                if (crc32(payload) != expected_crc ||
                    !decodePicoConfiguration(payload, &configuration)) {
                    ++crc_errors_;
                    continue;
                }
                configuration_ = configuration;
                configuration_action_->setEnabled(!calibration_sweep_active_);
                calibration_sweep_action_->setEnabled(
                    !calibration_sweep_active_);
                if (configuration_dialog_ != nullptr) {
                    configuration_dialog_->setConfiguration(configuration_);
                }
                handleCalibrationConfiguration(configuration_);
                continue;
            }
            const quint16 required_flags = P2000T_STREAM_FLAG_PLANAR_RGB111 |
                                           P2000T_STREAM_FLAG_PIXELS_MSB_FIRST;
            const bool valid =
                version == P2000T_STREAM_PROTOCOL_VERSION &&
                (encoding == P2000T_STREAM_ENCODING_RAW ||
                 encoding == P2000T_STREAM_ENCODING_PACKBITS) &&
                width == P2000T_STREAM_WIDTH &&
                height == P2000T_STREAM_HEIGHT &&
                stride == P2000T_STREAM_PLANE_STRIDE &&
                header_size == P2000T_STREAM_HEADER_SIZE &&
                payload_size <= static_cast<quint32>(kMaximumPayloadSize) &&
                (flags & required_flags) == required_flags &&
                artwork < P2000T_STREAM_ARTWORK_COUNT &&
                ((signal_present &&
                  uncompressed_size == P2000T_STREAM_PAYLOAD_SIZE &&
                  payload_size != 0u) ||
                 (!signal_present && uncompressed_size == 0u &&
                  payload_size == 0u));
            if (!valid) {
                receive_buffer_.remove(0, 1);
                continue;
            }
            const qsizetype record_size =
                static_cast<qsizetype>(header_size) + payload_size;
            if (receive_buffer_.size() < record_size) {
                return;
            }

            const QByteArray payload = receive_buffer_.mid(
                header_size, static_cast<qsizetype>(payload_size));
            receive_buffer_.remove(0, record_size);
            if (!signal_present) {
                presentNoConnection(artwork);
                updateStatus(sequence, payload_size, false, artwork);
                continue;
            }

            QByteArray frame(P2000T_STREAM_PAYLOAD_SIZE, Qt::Uninitialized);
            bool decoded = false;
            if (encoding == P2000T_STREAM_ENCODING_RAW &&
                payload.size() == P2000T_STREAM_PAYLOAD_SIZE) {
                frame = payload;
                decoded = true;
            } else if (encoding == P2000T_STREAM_ENCODING_PACKBITS) {
                decoded = p2000t_packbits_decode(
                    reinterpret_cast<const uint8_t *>(payload.constData()),
                    static_cast<size_t>(payload.size()),
                    reinterpret_cast<uint8_t *>(frame.data()),
                    P2000T_STREAM_PAYLOAD_SIZE);
            }
            if (!decoded || crc32(frame) != expected_crc) {
                ++crc_errors_;
                updateStatus(sequence, payload_size, true, artwork);
                continue;
            }
            presentFrame(frame);
            ++frame_count_;
            updateStatus(sequence, payload_size, true, artwork);
            handleCalibrationFrame(sequence);
        }
    }

    void presentFrame(const QByteArray &frame) {
        QImage image(P2000T_STREAM_WIDTH, P2000T_STREAM_HEIGHT * 2,
                     QImage::Format_RGB32);
        const auto *planes =
            reinterpret_cast<const unsigned char *>(frame.constData());
        for (int y = 0; y < P2000T_STREAM_HEIGHT; ++y) {
            auto *first = reinterpret_cast<QRgb *>(image.scanLine(y * 2));
            auto *second = reinterpret_cast<QRgb *>(image.scanLine(y * 2 + 1));
            const int row = y * P2000T_STREAM_PLANE_STRIDE;
            for (int x = 0; x < P2000T_STREAM_WIDTH; ++x) {
                const int index = row + x / 8;
                const unsigned mask = 0x80u >> (x & 7);
                const unsigned colorIndex =
                    ((planes[index] & mask) != 0u ? 1u : 0u) |
                    ((planes[P2000T_STREAM_PLANE_SIZE + index] & mask) != 0u
                         ? 2u
                         : 0u) |
                    ((planes[2 * P2000T_STREAM_PLANE_SIZE + index] & mask) != 0u
                         ? 4u
                         : 0u);
                const quint16 color = configuration_.palette[colorIndex];
                first[x] =
                    qRgb((color & 0x0fu) * 17, ((color >> 4u) & 0x0fu) * 17,
                         ((color >> 8u) & 0x0fu) * 17);
                second[x] = first[x];
            }
        }
        current_frame_ = image;
        view_->setFrame(current_frame_);
        save_->setEnabled(true);
    }

    void presentNoConnection(quint8 artwork) {
        current_frame_ = renderP2000tNoSignalScreen(artwork);
        view_->setFrame(current_frame_);
        save_->setEnabled(true);
    }

    void updateStatus(quint32 sequence, quint32 payload_size,
                      bool signal_present, quint8 artwork) {
        configuration_.artwork = artwork;
        last_signal_present_ = signal_present;
        if (!signal_present) {
            statusBar()->showMessage(
                QStringLiteral("%1 | no P2000T signal | VGA screen: %2")
                    .arg(serial_.name(), p2000tArtworkName(artwork)));
            return;
        }
        const qint64 elapsed = rate_timer_.elapsed();
        const double seconds = elapsed > 0 ? elapsed / 1000.0 : 0.0;
        const double fps = seconds > 0.0 ? frame_count_ / seconds : 0.0;
        const double megabytes =
            seconds > 0.0 ? byte_count_ / seconds / 1000000.0 : 0.0;
        statusBar()->showMessage(
            QStringLiteral("%1 | frame %2 | %3 FPS | %4 MB/s | payload %5 B | "
                           "CRC errors %6")
                .arg(serial_.name())
                .arg(sequence)
                .arg(fps, 0, 'f', 1)
                .arg(megabytes, 0, 'f', 2)
                .arg(payload_size)
                .arg(crc_errors_));
    }

    void saveFrame() {
        if (current_frame_.isNull()) {
            return;
        }
        const QString filename = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save lossless capture"),
            QStringLiteral("p2000t-capture.png"),
            QStringLiteral("PNG images (*.png)"));
        if (!filename.isEmpty() && !current_frame_.save(filename, "PNG")) {
            QMessageBox::critical(
                this, QStringLiteral("Save failed"),
                QStringLiteral("Could not write %1.").arg(filename));
        }
    }

    AdapterSerialPort serial_;
    QTimer poll_timer_;
    QTimer calibration_ack_timer_;
    QElapsedTimer rate_timer_;
    QComboBox *ports_ = nullptr;
    QComboBox *encoding_ = nullptr;
    QPushButton *connect_ = nullptr;
    QPushButton *save_ = nullptr;
    FrameView *view_ = nullptr;
    QAction *connect_action_ = nullptr;
    QAction *configuration_action_ = nullptr;
    QAction *calibration_sweep_action_ = nullptr;
    ConfigurationDialog *configuration_dialog_ = nullptr;
    QProgressDialog *calibration_progress_ = nullptr;
    PicoConfiguration configuration_;
    PicoConfiguration calibration_original_configuration_;
    CalibrationSweepOptions calibration_options_;
    QByteArray receive_buffer_;
    QImage current_frame_;
    QFile calibration_manifest_;
    QString calibration_directory_;
    bool connected_ = false;
    bool last_signal_present_ = false;
    bool calibration_sweep_active_ = false;
    bool calibration_waiting_for_configuration_ = false;
    int calibration_phase_ = 0;
    int calibration_rate_trim_ = 0;
    int calibration_settling_frames_ = 0;
    int calibration_frame_at_setting_ = 0;
    int calibration_images_written_ = 0;
    int calibration_acknowledgement_retries_ = 0;
    quint64 frame_count_ = 0;
    quint64 byte_count_ = 0;
    quint64 crc_errors_ = 0;
};

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("P2000T Capture"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(P2000T_VIEWER_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("Ivo Filot"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("ivofilot.nl"));
    application.setWindowIcon(
        QIcon(QStringLiteral(":/icons/p2000t-capture.png")));
    CaptureWindow window;
    window.show();
    return application.exec();
}
