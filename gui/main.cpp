/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.cpp
 * @brief Minimal Qt 6 viewer for Pico 2 RGB111 screen captures.
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

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
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "adapter_serial_port.h"
#include "codex_lab_bridge.h"
#include "configuration_dialog.h"
#include "engineering_autotune.h"
#include "no_signal_screen.h"
#include "p2000t_diagnostic_protocol.h"
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

quint64 loadU64(const char *data) {
    return static_cast<quint64>(loadU32(data)) |
           (static_cast<quint64>(loadU32(data + 4)) << 32u);
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

QString reconstructionName(int reconstruction) {
    switch (reconstruction) {
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW:
        return QStringLiteral("raw");
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP:
        return QStringLiteral("guarded");
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SHARP_GUARDED:
        return QStringLiteral("sharp");
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CENTER:
        return QStringLiteral("window-center");
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CHANNEL_MAJORITY:
        return QStringLiteral("window-channel");
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_EARLY:
        return QStringLiteral("window-early");
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_LATE:
        return QStringLiteral("window-late");
    case P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CONFIDENCE_GUARD:
        return QStringLiteral("window-confidence");
    default:
        return QStringLiteral("invalid");
    }
}

struct FrameReconstructionDiagnostics {
    int reconstruction = P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW;
    int captureEngine = P2000T_CAPTURE_ENGINE_TWO_TAP;
    int samplesPerOutput = 1;
    quint32 correctedSamples = 0;
    quint32 ambiguousSamples = 0;
    quint32 redCorrections = 0;
    quint32 greenCorrections = 0;
    quint32 blueCorrections = 0;
    bool deadlineMiss = false;
};

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

struct EngineeringAutotuneOptions {
    int settlingFrames = 2;
    int framesPerCandidate = 8;
    int validationFrames = 100;

    int ratePhaseCandidates() const {
        return (P2000T_CONTROL_MAX_PHASE - P2000T_CONTROL_MIN_PHASE + 1) *
               (P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM -
                P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM + 1);
    }

    int oddPhaseCandidates() const {
        return P2000T_CONTROL_MAX_ODD_LINE_PHASE -
               P2000T_CONTROL_MIN_ODD_LINE_PHASE + 1;
    }

    int retainedFrames() const {
        return (ratePhaseCandidates() + oddPhaseCandidates()) *
                   framesPerCandidate +
               validationFrames;
    }

    int sourceFrames() const {
        return retainedFrames() +
               (ratePhaseCandidates() + oddPhaseCandidates() + 1) *
                   settlingFrames;
    }
};

struct EngineeringCandidateResult {
    QString stage;
    int phase = 0;
    int oddLinePhase = 0;
    int rateTrim = 0;
    EngineeringCandidateMetrics metrics;
    QString modalFilename;
};

struct DiagnosticCaptureOptions {
    int startLine = P2000T_CONTROL_DEFAULT_VERTICAL;
    int lineCount = P2000T_DIAGNOSTIC_MAX_LINES;
    int repetitions = 100;

    quint64 estimatedBytes() const {
        return P2000T_DIAGNOSTIC_HEADER_SIZE *
                   static_cast<quint64>(repetitions + 3) +
               P2000T_DIAGNOSTIC_TIMING_PAYLOAD_SIZE +
               static_cast<quint64>(repetitions) * lineCount *
                   P2000T_DIAGNOSTIC_SAMPLES_PER_NOMINAL_LINE / 2u;
    }
};

class DiagnosticCaptureDialog final : public QDialog {
  public:
    explicit DiagnosticCaptureDialog(const PicoConfiguration &configuration,
                                     QWidget *parent = nullptr)
        : QDialog(parent) {
        setWindowTitle(QStringLiteral("Record high-resolution diagnostics"));
        setModal(true);
        auto *layout = new QVBoxLayout(this);
        auto *intro = new QLabel(
            QStringLiteral(
                "The Pico 2 will first record a 63 MHz CSYNC timing trace, "
                "then repeatedly sample the conditioned RGBS inputs at "
                "126 MHz. Records are saved losslessly with their original "
                "PIO word packing and CRC-32."),
            this);
        intro->setWordWrap(true);
        layout->addWidget(intro);

        auto *form = new QFormLayout();
        start_line_ = new QSpinBox(this);
        start_line_->setRange(P2000T_DIAGNOSTIC_MIN_START_LINE,
                              P2000T_DIAGNOSTIC_MAX_START_LINE);
        start_line_->setValue(configuration.vertical);
        line_count_ = new QSpinBox(this);
        line_count_->setRange(1, P2000T_DIAGNOSTIC_MAX_LINES);
        line_count_->setValue(P2000T_DIAGNOSTIC_MAX_LINES);
        repetitions_ = new QSpinBox(this);
        repetitions_->setRange(1, P2000T_DIAGNOSTIC_MAX_REPETITIONS);
        repetitions_->setValue(100);
        form->addRow(QStringLiteral("First physical source line:"),
                     start_line_);
        form->addRow(QStringLiteral("Contiguous lines per burst:"),
                     line_count_);
        form->addRow(QStringLiteral("Repeated bursts:"), repetitions_);
        layout->addLayout(form);
        estimate_ = new QLabel(this);
        estimate_->setWordWrap(true);
        layout->addWidget(estimate_);
        connect(line_count_, &QSpinBox::valueChanged, this,
                [this] { updateEstimate(); });
        connect(repetitions_, &QSpinBox::valueChanged, this,
                [this] { updateEstimate(); });

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)
            ->setText(QStringLiteral("Choose output folder..."));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        updateEstimate();
    }

    DiagnosticCaptureOptions options() const {
        return {start_line_->value(), line_count_->value(),
                repetitions_->value()};
    }

  private:
    void updateEstimate() {
        const DiagnosticCaptureOptions value = options();
        estimate_->setText(
            QStringLiteral(
                "One approximately 22 ms timing trace plus %1 raw bursts; "
                "estimated lossless data size %2 MiB.")
                .arg(value.repetitions)
                .arg(value.estimatedBytes() / (1024.0 * 1024.0), 0, 'f', 1));
    }

    QSpinBox *start_line_ = nullptr;
    QSpinBox *line_count_ = nullptr;
    QSpinBox *repetitions_ = nullptr;
    QLabel *estimate_ = nullptr;
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
                         ? QStringLiteral("guarded second tap")
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

class EngineeringAutotuneDialog final : public QDialog {
  public:
    explicit EngineeringAutotuneDialog(QWidget *parent = nullptr)
        : QDialog(parent) {
        setWindowTitle(QStringLiteral("Engineering-screen autotune"));
        setModal(true);
        auto *layout = new QVBoxLayout(this);
        auto *intro = new QLabel(
            QStringLiteral(
                "Display the supplied SAA5050 engineering screen and leave "
                "it unchanged. The viewer will use sharp raw reconstruction, "
                "exhaustively tune sample phase and line-rate trim, tune the "
                "odd-line phase separately, and validate the winner. It "
                "scores temporal changes and one-dot color transients. The "
                "winner is applied live but is not written to Pico flash."),
            this);
        intro->setWordWrap(true);
        layout->addWidget(intro);

        auto *form = new QFormLayout();
        settling_frames_ = makeSpinBox(0, 20, 2);
        frames_per_candidate_ = makeSpinBox(3, 20, 8);
        validation_frames_ = makeSpinBox(20, 500, 100);
        form->addRow(QStringLiteral("Frames to settle:"), settling_frames_);
        form->addRow(QStringLiteral("Frames per candidate:"),
                     frames_per_candidate_);
        form->addRow(QStringLiteral("Winner validation frames:"),
                     validation_frames_);
        layout->addLayout(form);

        estimate_ = new QLabel(this);
        estimate_->setWordWrap(true);
        layout->addWidget(estimate_);
        for (auto *spinner :
             {settling_frames_, frames_per_candidate_, validation_frames_}) {
            connect(spinner, &QSpinBox::valueChanged, this,
                    [this] { updateEstimate(); });
        }

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)
            ->setText(QStringLiteral("Choose log folder and start..."));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        updateEstimate();
    }

    EngineeringAutotuneOptions options() const {
        return {settling_frames_->value(), frames_per_candidate_->value(),
                validation_frames_->value()};
    }

  private:
    QSpinBox *makeSpinBox(int minimum, int maximum, int value) {
        auto *spinner = new QSpinBox(this);
        spinner->setRange(minimum, maximum);
        spinner->setValue(value);
        return spinner;
    }

    void updateEstimate() {
        const EngineeringAutotuneOptions value = options();
        estimate_->setText(
            QStringLiteral(
                "%1 phase/rate candidates + %2 odd-line candidates + "
                "validation; %3 retained PNGs and approximately %4 minutes "
                "at 25 streamed frames/s. Cancellation restores the original "
                "live settings.")
                .arg(value.ratePhaseCandidates())
                .arg(value.oddPhaseCandidates())
                .arg(value.retainedFrames())
                .arg(value.sourceFrames() / 1500.0, 0, 'f', 1));
    }

    QSpinBox *settling_frames_ = nullptr;
    QSpinBox *frames_per_candidate_ = nullptr;
    QSpinBox *validation_frames_ = nullptr;
    QLabel *estimate_ = nullptr;
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
        diagnostic_watchdog_.setSingleShot(true);
        diagnostic_watchdog_.setInterval(5000);
        connect(&diagnostic_watchdog_, &QTimer::timeout, this, [this] {
            finishDiagnosticCapture(
                false, QStringLiteral("No complete diagnostic record was "
                                      "received for five seconds."));
        });
        lab_ack_timer_.setSingleShot(true);
        lab_ack_timer_.setInterval(1500);
        connect(&lab_ack_timer_, &QTimer::timeout, this,
                [this] { labAcknowledgementTimedOut(); });
        lab_poll_timer_.setInterval(100);
        connect(&lab_poll_timer_, &QTimer::timeout, this,
                [this] { pollCodexLabBridge(); });
        connect(refresh, &QPushButton::clicked, this,
                [this] { refreshPorts(); });
        connect(connect_, &QPushButton::clicked, this,
                [this] { toggleConnection(); });
        connect(save_, &QPushButton::clicked, this, [this] { saveFrame(); });

        initializeCodexLabBridge();

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
    enum class CalibrationMode { ManualSweep, EngineeringAutotune };
    enum class AutotuneStage { RateAndPhase, OddLinePhase, Validation };

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

        engineering_autotune_action_ = adapter->addAction(
            QStringLiteral("Run &engineering-screen autotune..."));
        engineering_autotune_action_->setToolTip(QStringLiteral(
            "Autonomously tune and score sampling against the SAA5050 test "
            "screen"));
        connect(engineering_autotune_action_, &QAction::triggered, this,
                [this] { startEngineeringAutotune(); });
        engineering_autotune_action_->setEnabled(false);

        codex_lab_action_ =
            adapter->addAction(QStringLiteral("Codex &lab bridge status..."));
        codex_lab_action_->setToolTip(QStringLiteral(
            "Show the shared command directory used for Codex experiments"));
        connect(codex_lab_action_, &QAction::triggered, this,
                [this] { showCodexLabBridgeStatus(); });

        diagnostic_action_ = adapter->addAction(
            QStringLiteral("Record high-resolution &diagnostics..."));
        diagnostic_action_->setToolTip(QStringLiteral(
            "Record raw 126 MHz RGBS bursts and a CSYNC timing trace"));
        connect(diagnostic_action_, &QAction::triggered, this,
                [this] { startDiagnosticCapture(); });
        diagnostic_action_->setEnabled(false);

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

    void initializeCodexLabBridge() {
        QSettings settings;
        const QString environmentRoot = qEnvironmentVariable("P2000T_LAB_ROOT");
        QString root = environmentRoot;
        if (root.isEmpty()) {
            root = settings.value(QStringLiteral("codexLab/rootDirectory"))
                       .toString();
        }
        if (root.isEmpty()) {
#if defined(Q_OS_WIN)
            root = QDir(QStringLiteral("D:/")).exists()
                       ? QStringLiteral("D:/tmp/p2000t-codex-lab")
                       : QDir(QDir::tempPath())
                             .filePath(QStringLiteral("p2000t-codex-lab"));
#else
            root = QDir(QStringLiteral("/mnt/d/tmp")).exists()
                       ? QStringLiteral("/mnt/d/tmp/p2000t-codex-lab")
                       : QDir(QDir::tempPath())
                             .filePath(QStringLiteral("p2000t-codex-lab"));
#endif
        }
        QString error;
        lab_bridge_available_ = lab_bridge_.initialize(root, &error);
        if (lab_bridge_available_) {
            if (environmentRoot.isEmpty()) {
                settings.setValue(QStringLiteral("codexLab/rootDirectory"),
                                  root);
            }
            lab_poll_timer_.start();
            writeCodexLabStatus();
        } else {
            lab_bridge_error_ = error;
        }
    }

    QJsonObject liveSettingsJson(const PicoConfiguration &configuration) const {
        QJsonObject settings;
        settings.insert(QStringLiteral("first_visible_line"),
                        configuration.vertical);
        settings.insert(QStringLiteral("phase"), configuration.phase);
        settings.insert(QStringLiteral("odd_line_phase"),
                        configuration.oddLinePhase);
        settings.insert(QStringLiteral("rate_trim"),
                        configuration.sampleRateTrim);
        settings.insert(QStringLiteral("reconstruction"),
                        reconstructionName(configuration.sampleReconstruction));
        settings.insert(QStringLiteral("capture_engine"),
                        configuration.captureEngine ==
                                P2000T_CAPTURE_ENGINE_WINDOWED
                            ? QStringLiteral("windowed")
                            : QStringLiteral("two-tap"));
        settings.insert(QStringLiteral("samples_per_output"),
                        configuration.windowSamples);
        settings.insert(QStringLiteral("window_supported"),
                        configuration.windowSupported);
        settings.insert(QStringLiteral("engine_switch_pending"),
                        configuration.engineSwitchPending);
        settings.insert(QStringLiteral("windowed_frames"),
                        static_cast<qint64>(configuration.windowedFrames));
        settings.insert(QStringLiteral("line_deadline_misses"),
                        static_cast<qint64>(configuration.lineDeadlineMisses));
        settings.insert(
            QStringLiteral("last_corrected_samples"),
            static_cast<qint64>(configuration.lastCorrectedSamples));
        settings.insert(
            QStringLiteral("last_ambiguous_samples"),
            static_cast<qint64>(configuration.lastAmbiguousSamples));
        settings.insert(QStringLiteral("last_red_corrections"),
                        static_cast<qint64>(configuration.lastRedCorrections));
        settings.insert(
            QStringLiteral("last_green_corrections"),
            static_cast<qint64>(configuration.lastGreenCorrections));
        settings.insert(QStringLiteral("last_blue_corrections"),
                        static_cast<qint64>(configuration.lastBlueCorrections));
        settings.insert(QStringLiteral("horizontal_start"),
                        configuration.horizontal);
        settings.insert(QStringLiteral("stored_available"),
                        configuration.storedAvailable);
        settings.insert(QStringLiteral("matches_stored"),
                        configuration.matchesStored);
        settings.insert(QStringLiteral("save_failed"),
                        configuration.saveFailed);
        return settings;
    }

    QJsonObject codexLabStatus() const {
        QJsonObject status;
        status.insert(QStringLiteral("protocol"), 1);
        status.insert(QStringLiteral("host_version"),
                      QStringLiteral(P2000T_VIEWER_VERSION));
        status.insert(QStringLiteral("updated_utc"),
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        status.insert(QStringLiteral("host_pid"),
                      static_cast<qint64>(QCoreApplication::applicationPid()));
#if defined(P2000T_LAB_AGENT_ONLY)
        status.insert(QStringLiteral("host_mode"),
                      QStringLiteral("headless_lab_agent"));
#else
        status.insert(QStringLiteral("host_mode"), QStringLiteral("viewer"));
#endif
        status.insert(QStringLiteral("connected"), connected_);
        status.insert(QStringLiteral("signal_present"), last_signal_present_);
        status.insert(QStringLiteral("busy"),
                      lab_experiment_active_ || lab_save_pending_ ||
                          !diagnostic_lab_request_id_.isEmpty());
        status.insert(QStringLiteral("manual_operation_active"),
                      calibration_sweep_active_ || diagnostic_active_ ||
                          configuration_dialog_ != nullptr);
        status.insert(
            QStringLiteral("active_request_id"),
            lab_experiment_active_
                ? lab_request_.id
                : (lab_save_pending_
                       ? lab_save_request_id_
                       : diagnostic_lab_request_id_));
        status.insert(QStringLiteral("bridge_root"),
                      lab_bridge_.rootDirectory());
        status.insert(QStringLiteral("live_settings"),
                      liveSettingsJson(configuration_));
        return status;
    }

    void writeCodexLabStatus() {
        if (lab_bridge_available_) {
            lab_bridge_.writeStatus(codexLabStatus());
        }
    }

    void showCodexLabBridgeStatus() {
        if (!lab_bridge_available_) {
            QMessageBox::critical(this, QStringLiteral("Codex lab bridge"),
                                  lab_bridge_error_);
            return;
        }
        QMessageBox::information(
            this, QStringLiteral("Codex lab bridge"),
            QStringLiteral(
                "The Codex lab bridge is active. It accepts status, "
                "non-persistent experiment, lossless diagnostic, explicit "
                "save, persistent factory-reset, and cancel "
                "commands in:\n\n%1\n\n"
                "Connection: %2\nP2000T signal: %3\nExperiment: %4")
                .arg(QDir::toNativeSeparators(lab_bridge_.rootDirectory()),
                     connected_ ? QStringLiteral("connected")
                                : QStringLiteral("disconnected"),
                     last_signal_present_ ? QStringLiteral("present")
                                          : QStringLiteral("absent"),
                     lab_experiment_active_ ? lab_request_.id
                                            : QStringLiteral("idle")));
    }

    void respondToLabRequest(const QString &id, bool ok, const QString &state,
                             const QString &error = QString(),
                             const QJsonObject &extra = QJsonObject()) {
        QJsonObject response = extra;
        response.insert(QStringLiteral("protocol"), 1);
        response.insert(QStringLiteral("id"), id);
        response.insert(QStringLiteral("ok"), ok);
        response.insert(QStringLiteral("state"), state);
        response.insert(QStringLiteral("completed_utc"),
                        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        if (!error.isEmpty()) {
            response.insert(QStringLiteral("error"), error);
        }
        lab_bridge_.writeResponse(id, response);
    }

    void pollCodexLabBridge() {
        if (!lab_bridge_available_) {
            return;
        }
        if (++lab_status_poll_count_ >= 10) {
            lab_status_poll_count_ = 0;
            writeCodexLabStatus();
        }
        if (lab_experiment_active_ && !lab_waiting_for_configuration_ &&
            lab_frame_watchdog_.isValid() &&
            lab_frame_watchdog_.elapsed() > 5000) {
            finishLabExperiment(false,
                                QStringLiteral("No valid source frame was "
                                               "received for five seconds."),
                                true);
        }
        const QList<CodexLabEnvelope> envelopes = lab_bridge_.takeRequests();
        for (const CodexLabEnvelope &envelope : envelopes) {
            const QString filenameId =
                QFileInfo(envelope.filename).completeBaseName();
            CodexLabRequest request;
            QString error;
            if (!CodexLabBridge::decodeRequest(envelope.payload, &request,
                                               &error)) {
                respondToLabRequest(filenameId, false,
                                    QStringLiteral("rejected"), error);
                continue;
            }
            if (request.id != filenameId) {
                respondToLabRequest(
                    filenameId, false, QStringLiteral("rejected"),
                    QStringLiteral("JSON id does not match request filename"));
                continue;
            }
            if (request.command == CodexLabCommand::Status) {
                respondToLabRequest(request.id, true, QStringLiteral("status"),
                                    QString(), codexLabStatus());
                continue;
            }
            if (request.command == CodexLabCommand::Shutdown) {
                if (lab_experiment_active_ || lab_save_pending_ ||
                    calibration_sweep_active_ || diagnostic_active_) {
                    respondToLabRequest(
                        request.id, false, QStringLiteral("busy"),
                        QStringLiteral("Cancel the active operation before "
                                       "shutting down the lab agent."));
                } else {
                    respondToLabRequest(request.id, true,
                                        QStringLiteral("shutting_down"));
                    QTimer::singleShot(100, qApp, &QCoreApplication::quit);
                }
                continue;
            }
            if (request.command == CodexLabCommand::Cancel) {
                if (lab_save_pending_) {
                    respondToLabRequest(
                        request.id, false, QStringLiteral("busy"),
                        QStringLiteral("A flash save is already in progress "
                                       "and cannot be cancelled."));
                } else if (lab_experiment_active_) {
                    const QString cancelledId = lab_request_.id;
                    finishLabExperiment(false,
                                        QStringLiteral("Cancelled by Codex "
                                                       "lab command."),
                                        true);
                    QJsonObject extra;
                    extra.insert(QStringLiteral("cancelled_request_id"),
                                 cancelledId);
                    respondToLabRequest(request.id, true,
                                        QStringLiteral("cancelled"), QString(),
                                        extra);
                } else if (diagnostic_active_ &&
                           !diagnostic_lab_request_id_.isEmpty()) {
                    const QString cancelledId = diagnostic_lab_request_id_;
                    requestDiagnosticCancellation();
                    QJsonObject extra;
                    extra.insert(QStringLiteral("cancelled_request_id"),
                                 cancelledId);
                    respondToLabRequest(request.id, true,
                                        QStringLiteral("cancelling"),
                                        QString(), extra);
                } else {
                    respondToLabRequest(request.id, true,
                                        QStringLiteral("idle"));
                }
                continue;
            }
            if (lab_experiment_active_ || calibration_sweep_active_ ||
                diagnostic_active_ || configuration_dialog_ != nullptr ||
                lab_save_pending_) {
                respondToLabRequest(
                    request.id, false, QStringLiteral("busy"),
                    QStringLiteral("The viewer is already running another "
                                   "capture or configuration operation."));
                continue;
            }
            if (request.command == CodexLabCommand::Save ||
                request.command == CodexLabCommand::FactoryReset) {
                if (!connected_) {
                    respondToLabRequest(
                        request.id, false, QStringLiteral("unavailable"),
                        QStringLiteral("The Pico is not connected."));
                    continue;
                }
                lab_save_pending_ = true;
                lab_factory_reset_pending_ =
                    request.command == CodexLabCommand::FactoryReset;
                lab_save_request_id_ = request.id;
                const quint8 opcode =
                    lab_factory_reset_pending_
                        ? P2000T_CONTROL_FACTORY_DEFAULTS
                        : P2000T_CONTROL_SAVE_SETTINGS;
                if (!sendControl(makePicoControlPacket(opcode))) {
                    lab_save_pending_ = false;
                    lab_factory_reset_pending_ = false;
                    lab_save_request_id_.clear();
                    respondToLabRequest(
                        request.id, false, QStringLiteral("unavailable"),
                        QStringLiteral("The Pico connection was lost while "
                                       "saving settings."));
                    continue;
                }
                writeCodexLabStatus();
                QTimer::singleShot(250, this, [this] {
                    if (lab_save_pending_ && connected_) {
                        sendControl(
                            makePicoControlPacket(P2000T_CONTROL_GET_SETTINGS));
                    }
                });
                QTimer::singleShot(750, this, [this] {
                    if (!lab_save_pending_) {
                        return;
                    }
                    const QString id = lab_save_request_id_;
                    const bool connected = connected_;
                    const bool factoryReset = lab_factory_reset_pending_;
                    const bool knownGood =
                        configuration_.phase ==
                            P2000T_CONTROL_PICO2_DEFAULT_PHASE &&
                        configuration_.oddLinePhase ==
                            P2000T_CONTROL_PICO2_DEFAULT_ODD_LINE_PHASE &&
                        configuration_.sampleRateTrim ==
                            P2000T_CONTROL_DEFAULT_SAMPLE_RATE_TRIM &&
                        configuration_.sampleReconstruction ==
                            P2000T_CONTROL_PICO2_DEFAULT_SAMPLE_RECONSTRUCTION;
                    const bool saved =
                        connected && configuration_.storedAvailable &&
                        configuration_.matchesStored &&
                        !configuration_.saveFailed &&
                        (!factoryReset || knownGood);
                    const bool rejected =
                        connected && configuration_.saveFailed;
                    lab_save_pending_ = false;
                    lab_factory_reset_pending_ = false;
                    lab_save_request_id_.clear();
                    const QJsonObject extra = codexLabStatus();
                    respondToLabRequest(
                        id, saved,
                        saved ? (factoryReset
                                     ? QStringLiteral("factory_reset")
                                     : QStringLiteral("saved"))
                              : (connected ? QStringLiteral("save_failed")
                                           : QStringLiteral("unavailable")),
                        saved
                            ? QString()
                            : (rejected
                                   ? QStringLiteral(
                                         "The Pico rejected the flash save.")
                                   : (connected
                                          ? (factoryReset
                                                 ? QStringLiteral(
                                                       "The Pico did not "
                                                       "acknowledge the known-"
                                                       "good saved defaults.")
                                                 : QStringLiteral(
                                                       "The Pico did not "
                                                       "acknowledge matching "
                                                       "saved settings."))
                                          : QStringLiteral(
                                                "The Pico disconnected before "
                                                "the save was acknowledged."))),
                        extra);
                    writeCodexLabStatus();
                });
                continue;
            }
            if (!connected_ || !last_signal_present_) {
                respondToLabRequest(
                    request.id, false, QStringLiteral("unavailable"),
                    connected_ ? QStringLiteral("No P2000T signal is present.")
                               : QStringLiteral("The Pico is not connected."));
                continue;
            }
            if (request.command == CodexLabCommand::Diagnostic) {
                startLabDiagnosticCapture(request);
                continue;
            }
            startLabExperiment(request);
        }
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
        engineering_autotune_action_->setEnabled(false);
        diagnostic_action_->setEnabled(false);
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
        if (lab_experiment_active_) {
            finishLabExperiment(
                false, QStringLiteral("Viewer disconnected during experiment."),
                true);
        }
        if (calibration_sweep_active_) {
            endCalibrationSweep(SweepEnd::Disconnected);
        }
        if (diagnostic_active_) {
            closeDiagnosticFiles();
            diagnostic_active_ = false;
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
        engineering_autotune_action_->setEnabled(false);
        diagnostic_action_->setEnabled(false);
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
        if (lab_experiment_active_) {
            return;
        }
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
            if (QMessageBox::question(
                    this, QStringLiteral("Factory reset Pico"),
                    QStringLiteral("Restore the known-good capture, palette, "
                                   "and artwork defaults and save them to Pico "
                                   "flash?"),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes) {
                return;
            }
            if (sendControl(
                    makePicoControlPacket(P2000T_CONTROL_FACTORY_DEFAULTS))) {
                statusBar()->showMessage(
                    QStringLiteral("Factory defaults restored and saved"),
                    3000);
            }
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

    bool sendCalibrationValues(int phase, int oddLinePhase, int rateTrim,
                               int reconstruction = -1) {
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
        if (reconstruction >= 0) {
            packets +=
                makePicoControlPacket(P2000T_CONTROL_SET_SAMPLE_RECONSTRUCTION,
                                      0, static_cast<quint32>(reconstruction));
        }
        // Request one final state record after all three changes. Firmware may
        // coalesce intermediate records, so this response must describe the
        // complete tuple the sweep is about to label and capture.
        packets += makePicoControlPacket(P2000T_CONTROL_GET_SETTINGS);
        return sendControl(packets);
    }

    bool writeLabRunJson(const QString &filename,
                         const QJsonObject &object) const {
        QSaveFile output(QDir(lab_run_directory_).filePath(filename));
        if (!output.open(QIODevice::WriteOnly)) {
            return false;
        }
        const QByteArray data = QJsonDocument(object).toJson();
        return output.write(data) == data.size() && output.commit();
    }

    void transmitLabTarget() {
        if (sendCalibrationValues(
                lab_target_configuration_.phase,
                lab_target_configuration_.oddLinePhase,
                lab_target_configuration_.sampleRateTrim,
                lab_target_configuration_.sampleReconstruction)) {
            lab_ack_timer_.start();
        }
    }

    void startLabExperiment(const CodexLabRequest &request) {
        lab_request_ = request;
        lab_original_configuration_ = configuration_;
        lab_target_configuration_ = configuration_;
        lab_target_configuration_.phase =
            request.phase.value_or(configuration_.phase);
        lab_target_configuration_.oddLinePhase =
            request.oddLinePhase.value_or(configuration_.oddLinePhase);
        lab_target_configuration_.sampleRateTrim =
            request.rateTrim.value_or(configuration_.sampleRateTrim);
        lab_target_configuration_.sampleReconstruction =
            request.reconstruction.value_or(
                configuration_.sampleReconstruction);
        const bool targetWindowed =
            lab_target_configuration_.sampleReconstruction >=
            P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_FIRST;
        lab_target_configuration_.captureEngine =
            targetWindowed ? P2000T_CAPTURE_ENGINE_WINDOWED
                           : P2000T_CAPTURE_ENGINE_TWO_TAP;
        lab_target_configuration_.windowSamples = targetWindowed ? 3 : 1;
        lab_target_configuration_.engineSwitchPending = false;

        QDir root(lab_bridge_.rootDirectory());
        const QString relativeRun = QStringLiteral("runs/%1").arg(request.id);
        if (root.exists(relativeRun) || !root.mkpath(relativeRun)) {
            respondToLabRequest(
                request.id, false, QStringLiteral("rejected"),
                QStringLiteral("The run directory already exists or could "
                               "not be created."));
            return;
        }
        lab_run_directory_ = root.filePath(relativeRun);
        lab_manifest_.setFileName(
            QDir(lab_run_directory_).filePath(QStringLiteral("frames.csv")));
        if (request.captureFrames > 0 &&
            !lab_manifest_.open(QIODevice::WriteOnly | QIODevice::Text)) {
            respondToLabRequest(request.id, false, QStringLiteral("failed"),
                                QStringLiteral("Could not create frames.csv."));
            return;
        }
        if (lab_manifest_.isOpen()) {
            QTextStream manifest(&lab_manifest_);
            manifest << "filename,frame_at_setting,sequence,"
                        "capture_timestamp_us,received_utc,"
                        "reconstruction,capture_engine,samples_per_output,"
                        "corrected_samples,ambiguous_samples,red_corrections,"
                        "green_corrections,blue_corrections,deadline_miss\n";
            manifest.flush();
        }

        QJsonObject experiment;
        experiment.insert(QStringLiteral("protocol"), 1);
        experiment.insert(QStringLiteral("id"), request.id);
        experiment.insert(QStringLiteral("tag"), request.tag);
        if (!request.referenceRun.isEmpty()) {
            experiment.insert(QStringLiteral("reference_run"),
                              request.referenceRun);
        }
        experiment.insert(
            QStringLiteral("started_utc"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        experiment.insert(QStringLiteral("settle_frames"),
                          request.settlingFrames);
        experiment.insert(QStringLiteral("capture_frames"),
                          request.captureFrames);
        experiment.insert(QStringLiteral("original_live_settings"),
                          liveSettingsJson(lab_original_configuration_));
        experiment.insert(QStringLiteral("target_live_settings"),
                          liveSettingsJson(lab_target_configuration_));
        experiment.insert(QStringLiteral("writes_flash"), false);
        if (!writeLabRunJson(QStringLiteral("request.json"), experiment)) {
            if (lab_manifest_.isOpen()) {
                lab_manifest_.close();
            }
            respondToLabRequest(
                request.id, false, QStringLiteral("failed"),
                QStringLiteral("Could not write request.json."));
            return;
        }

        lab_accumulator_ =
            std::make_unique<EngineeringCandidateAccumulator>(viewerPalette());
        lab_frames_captured_ = 0;
        lab_corrected_samples_ = 0;
        lab_ambiguous_samples_ = 0;
        lab_red_corrections_ = 0;
        lab_green_corrections_ = 0;
        lab_blue_corrections_ = 0;
        lab_deadline_miss_frames_ = 0;
        lab_settling_frames_ = 0;
        lab_acknowledgement_retries_ = 0;
        lab_waiting_for_configuration_ = true;
        lab_experiment_active_ = true;
        lab_frame_watchdog_.invalidate();
        configuration_action_->setEnabled(false);
        calibration_sweep_action_->setEnabled(false);
        engineering_autotune_action_->setEnabled(false);
        diagnostic_action_->setEnabled(false);
        writeCodexLabStatus();
        statusBar()->showMessage(
            QStringLiteral("Codex lab: applying experiment %1")
                .arg(request.id));
        transmitLabTarget();
    }

    void labAcknowledgementTimedOut() {
        if (!lab_experiment_active_ || !lab_waiting_for_configuration_) {
            return;
        }
        if (lab_acknowledgement_retries_ < kCalibrationAcknowledgementRetries) {
            ++lab_acknowledgement_retries_;
            transmitLabTarget();
            return;
        }
        finishLabExperiment(
            false,
            QStringLiteral(
                "The Pico did not acknowledge phase %1, odd-line phase %2, "
                "rate trim %3, reconstruction %4 after %5 attempts.")
                .arg(lab_target_configuration_.phase)
                .arg(lab_target_configuration_.oddLinePhase)
                .arg(lab_target_configuration_.sampleRateTrim)
                .arg(lab_target_configuration_.sampleReconstruction)
                .arg(kCalibrationAcknowledgementRetries + 1),
            true);
    }

    void handleLabConfiguration(const PicoConfiguration &configuration) {
        const bool targetWindowed =
            lab_target_configuration_.sampleReconstruction >=
            P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_FIRST;
        const int targetEngine = targetWindowed ? P2000T_CAPTURE_ENGINE_WINDOWED
                                                : P2000T_CAPTURE_ENGINE_TWO_TAP;
        if (!lab_experiment_active_ || !lab_waiting_for_configuration_ ||
            configuration.phase != lab_target_configuration_.phase ||
            configuration.oddLinePhase !=
                lab_target_configuration_.oddLinePhase ||
            configuration.sampleRateTrim !=
                lab_target_configuration_.sampleRateTrim ||
            configuration.sampleReconstruction !=
                lab_target_configuration_.sampleReconstruction ||
            configuration.captureEngine != targetEngine ||
            configuration.engineSwitchPending) {
            return;
        }
        lab_ack_timer_.stop();
        lab_waiting_for_configuration_ = false;
        lab_settling_frames_ = lab_request_.settlingFrames;
        lab_frame_watchdog_.restart();
        statusBar()->showMessage(
            QStringLiteral("Codex lab: experiment %1 acknowledged; settling")
                .arg(lab_request_.id));
        if (lab_settling_frames_ == 0 && lab_request_.captureFrames == 0) {
            finishLabExperiment(true);
        }
    }

    void handleLabFrame(quint32 sequence) {
        if (!lab_experiment_active_ || lab_waiting_for_configuration_) {
            return;
        }
        lab_frame_watchdog_.restart();
        if (lab_settling_frames_ > 0) {
            --lab_settling_frames_;
            if (lab_settling_frames_ == 0 && lab_request_.captureFrames == 0) {
                finishLabExperiment(true);
            }
            return;
        }
        if (lab_request_.captureFrames == 0) {
            finishLabExperiment(true);
            return;
        }

        const int imageNumber = lab_frames_captured_ + 1;
        const QString filename = QStringLiteral("frame_%1_seq_%2.png")
                                     .arg(imageNumber, 3, 10, QLatin1Char('0'))
                                     .arg(static_cast<qulonglong>(sequence), 10,
                                          10, QLatin1Char('0'));
        const QString path = QDir(lab_run_directory_).filePath(filename);
        if (!current_frame_.save(path, "PNG") || lab_accumulator_ == nullptr ||
            !lab_accumulator_->addFrame(current_frame_)) {
            finishLabExperiment(
                false, QStringLiteral("Could not save or score %1.").arg(path),
                true);
            return;
        }
        {
            QTextStream manifest(&lab_manifest_);
            manifest << filename << ',' << imageNumber << ',' << sequence << ','
                     << current_capture_timestamp_us_ << ','
                     << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                     << ','
                     << reconstructionName(
                            current_frame_diagnostics_.reconstruction)
                     << ','
                     << (current_frame_diagnostics_.captureEngine ==
                                 P2000T_CAPTURE_ENGINE_WINDOWED
                             ? QStringLiteral("windowed")
                             : QStringLiteral("two-tap"))
                     << ',' << current_frame_diagnostics_.samplesPerOutput
                     << ',' << current_frame_diagnostics_.correctedSamples
                     << ',' << current_frame_diagnostics_.ambiguousSamples
                     << ',' << current_frame_diagnostics_.redCorrections << ','
                     << current_frame_diagnostics_.greenCorrections << ','
                     << current_frame_diagnostics_.blueCorrections << ','
                     << (current_frame_diagnostics_.deadlineMiss ? 1 : 0)
                     << '\n';
            manifest.flush();
        }
        lab_manifest_.flush();
        if (lab_manifest_.error() != QFileDevice::NoError) {
            finishLabExperiment(
                false, QStringLiteral("Could not update frames.csv."), true);
            return;
        }
        ++lab_frames_captured_;
        lab_corrected_samples_ += current_frame_diagnostics_.correctedSamples;
        lab_ambiguous_samples_ += current_frame_diagnostics_.ambiguousSamples;
        lab_red_corrections_ += current_frame_diagnostics_.redCorrections;
        lab_green_corrections_ += current_frame_diagnostics_.greenCorrections;
        lab_blue_corrections_ += current_frame_diagnostics_.blueCorrections;
        if (current_frame_diagnostics_.deadlineMiss) {
            ++lab_deadline_miss_frames_;
        }
        statusBar()->showMessage(
            QStringLiteral("Codex lab: experiment %1 captured frame %2 of %3")
                .arg(lab_request_.id)
                .arg(lab_frames_captured_)
                .arg(lab_request_.captureFrames));
        if (lab_frames_captured_ >= lab_request_.captureFrames) {
            finishLabExperiment(true);
        }
    }

    void finishLabExperiment(bool success, const QString &error = QString(),
                             bool restoreOriginal = false) {
        if (!lab_experiment_active_) {
            return;
        }
        const CodexLabRequest request = lab_request_;
        const PicoConfiguration original = lab_original_configuration_;
        lab_experiment_active_ = false;
        lab_waiting_for_configuration_ = false;
        lab_ack_timer_.stop();
        lab_frame_watchdog_.invalidate();
        if (lab_manifest_.isOpen()) {
            lab_manifest_.close();
        }

        QJsonObject result;
        result.insert(QStringLiteral("run_relative_directory"),
                      QStringLiteral("runs/%1").arg(request.id));
        result.insert(QStringLiteral("run_directory"),
                      QDir::toNativeSeparators(lab_run_directory_));
        result.insert(QStringLiteral("tag"), request.tag);
        result.insert(QStringLiteral("captured_frames"), lab_frames_captured_);
        result.insert(QStringLiteral("target_live_settings"),
                      liveSettingsJson(lab_target_configuration_));
        result.insert(QStringLiteral("written_to_flash"), false);
        QJsonObject reconstructionDiagnostics;
        reconstructionDiagnostics.insert(
            QStringLiteral("mode"),
            reconstructionName(lab_target_configuration_.sampleReconstruction));
        reconstructionDiagnostics.insert(
            QStringLiteral("capture_engine"),
            lab_target_configuration_.captureEngine ==
                    P2000T_CAPTURE_ENGINE_WINDOWED
                ? QStringLiteral("windowed")
                : QStringLiteral("two-tap"));
        reconstructionDiagnostics.insert(
            QStringLiteral("samples_per_output"),
            lab_target_configuration_.windowSamples);
        reconstructionDiagnostics.insert(
            QStringLiteral("corrected_samples_total"),
            static_cast<qint64>(lab_corrected_samples_));
        reconstructionDiagnostics.insert(
            QStringLiteral("ambiguous_samples_total"),
            static_cast<qint64>(lab_ambiguous_samples_));
        reconstructionDiagnostics.insert(
            QStringLiteral("red_corrections_total"),
            static_cast<qint64>(lab_red_corrections_));
        reconstructionDiagnostics.insert(
            QStringLiteral("green_corrections_total"),
            static_cast<qint64>(lab_green_corrections_));
        reconstructionDiagnostics.insert(
            QStringLiteral("blue_corrections_total"),
            static_cast<qint64>(lab_blue_corrections_));
        reconstructionDiagnostics.insert(QStringLiteral("deadline_miss_frames"),
                                         lab_deadline_miss_frames_);
        if (lab_frames_captured_ > 0) {
            reconstructionDiagnostics.insert(
                QStringLiteral("corrected_samples_per_frame"),
                static_cast<double>(lab_corrected_samples_) /
                    lab_frames_captured_);
            reconstructionDiagnostics.insert(
                QStringLiteral("ambiguous_samples_per_frame"),
                static_cast<double>(lab_ambiguous_samples_) /
                    lab_frames_captured_);
        }
        result.insert(QStringLiteral("reconstruction_diagnostics"),
                      reconstructionDiagnostics);
        if (success && lab_accumulator_ != nullptr &&
            !lab_accumulator_->empty()) {
            EngineeringCandidateMetrics metrics = lab_accumulator_->finalize();
            const QString modalFilename = QStringLiteral("modal.png");
            if (!metrics.modalImage.save(
                    QDir(lab_run_directory_).filePath(modalFilename), "PNG")) {
                success = false;
                restoreOriginal = true;
                result.insert(QStringLiteral("error"),
                              QStringLiteral("Could not save modal.png."));
            } else {
                if (!request.referenceRun.isEmpty()) {
                    const QString referencePath =
                        QDir(lab_bridge_.rootDirectory())
                            .filePath(QStringLiteral("runs/%1/modal.png")
                                          .arg(request.referenceRun));
                    const QImage reference(referencePath);
                    const EngineeringFidelityMetrics fidelity =
                        compareEngineeringModals(metrics.modalImage, reference);
                    if (!fidelity.valid) {
                        success = false;
                        restoreOriginal = true;
                        result.insert(
                            QStringLiteral("error"),
                            QStringLiteral("Reference modal for run %1 is "
                                           "missing or has incompatible "
                                           "dimensions.")
                                .arg(request.referenceRun));
                    } else {
                        result.insert(QStringLiteral("reference_run"),
                                      request.referenceRun);
                        result.insert(QStringLiteral("fidelity"),
                                      fidelityJson(fidelity));
                    }
                }
                result.insert(QStringLiteral("metrics"), metricsJson(metrics));
                result.insert(QStringLiteral("modal_png"), modalFilename);
                metrics.modalImage = QImage();
            }
        }
        result.insert(QStringLiteral("ok"), success);
        result.insert(QStringLiteral("state"), success
                                                   ? QStringLiteral("complete")
                                                   : QStringLiteral("failed"));
        result.insert(QStringLiteral("completed_utc"),
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        QString finalError = error;
        if (!success && finalError.isEmpty()) {
            finalError = result.value(QStringLiteral("error")).toString();
        }
        if (!finalError.isEmpty()) {
            result.insert(QStringLiteral("error"), finalError);
        }
        writeLabRunJson(QStringLiteral("result.json"), result);
        respondToLabRequest(request.id, success,
                            success ? QStringLiteral("complete")
                                    : QStringLiteral("failed"),
                            finalError, result);

        if (restoreOriginal && connected_) {
            sendCalibrationValues(original.phase, original.oddLinePhase,
                                  original.sampleRateTrim,
                                  original.sampleReconstruction);
        }
        lab_accumulator_.reset();
        configuration_action_->setEnabled(
            connected_ && !calibration_sweep_active_ && !diagnostic_active_);
        calibration_sweep_action_->setEnabled(connected_ &&
                                              !diagnostic_active_);
        engineering_autotune_action_->setEnabled(connected_ &&
                                                 !diagnostic_active_);
        diagnostic_action_->setEnabled(connected_ &&
                                       !calibration_sweep_active_);
        statusBar()->showMessage(
            success ? QStringLiteral("Codex lab experiment %1 complete")
                          .arg(request.id)
                    : QStringLiteral("Codex lab experiment %1 failed: %2")
                          .arg(request.id, finalError),
            5000);
        writeCodexLabStatus();
    }

    int calibrationSettingIndex() const {
        if (calibration_mode_ == CalibrationMode::EngineeringAutotune) {
            if (autotune_stage_ == AutotuneStage::RateAndPhase) {
                const int rateCount = P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM -
                                      P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM + 1;
                return (calibration_phase_ - P2000T_CONTROL_MIN_PHASE) *
                           rateCount +
                       (calibration_rate_trim_ -
                        P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM) +
                       1;
            }
            if (autotune_stage_ == AutotuneStage::OddLinePhase) {
                return autotune_options_.ratePhaseCandidates() +
                       calibration_odd_line_phase_ -
                       P2000T_CONTROL_MIN_ODD_LINE_PHASE + 1;
            }
            return autotune_options_.ratePhaseCandidates() +
                   autotune_options_.oddPhaseCandidates() + 1;
        }
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
        const int settingCount =
            calibration_mode_ == CalibrationMode::EngineeringAutotune
                ? autotune_options_.ratePhaseCandidates() +
                      autotune_options_.oddPhaseCandidates() + 1
                : calibration_options_.settingCount();
        const QString stage =
            calibration_mode_ == CalibrationMode::EngineeringAutotune
                ? QStringLiteral("%1: ").arg(autotuneStageName())
                : QString();
        calibration_progress_->setLabelText(
            QStringLiteral(
                "%1setting %2 of %3: phase %4, rate trim %5, odd-line phase "
                "%6\n%7 of %8 retained frames written")
                .arg(stage)
                .arg(calibrationSettingIndex())
                .arg(settingCount)
                .arg(calibration_phase_)
                .arg(calibration_rate_trim_)
                .arg(calibration_odd_line_phase_)
                .arg(calibration_images_written_)
                .arg(calibration_total_images_));
    }

    void transmitCalibrationTarget() {
        if (sendCalibrationValues(
                calibration_phase_, calibration_odd_line_phase_,
                calibration_rate_trim_,
                calibration_mode_ == CalibrationMode::EngineeringAutotune
                    ? P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW
                    : -1)) {
            calibration_ack_timer_.start();
        }
    }

    void sendCalibrationTarget() {
        calibration_waiting_for_configuration_ = true;
        calibration_acknowledgement_retries_ = 0;
        calibration_settling_frames_ = 0;
        calibration_frame_at_setting_ = 0;
        if (calibration_mode_ == CalibrationMode::EngineeringAutotune) {
            resetAutotuneAccumulator();
        }
        updateCalibrationProgressLabel();
        statusBar()->showMessage(
            QStringLiteral("%1: applying phase %2, rate %3, odd phase %4")
                .arg(calibration_mode_ == CalibrationMode::EngineeringAutotune
                         ? QStringLiteral("Engineering autotune")
                         : QStringLiteral("Calibration sweep"))
                .arg(calibration_phase_)
                .arg(calibration_rate_trim_)
                .arg(calibration_odd_line_phase_));
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
                .arg(calibration_odd_line_phase_)
                .arg(calibration_rate_trim_)
                .arg(kCalibrationAcknowledgementRetries + 1)
                .arg(configuration_.phase)
                .arg(configuration_.oddLinePhase)
                .arg(configuration_.sampleRateTrim));
    }

    void startCalibrationSweep() {
        if (!connected_ || calibration_sweep_active_ || diagnostic_active_ ||
            lab_experiment_active_) {
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
                        "frame_at_setting,sequence,capture_timestamp_us,"
                        "received_utc\n";
            manifest.flush();
        }

        calibration_options_ = options;
        calibration_mode_ = CalibrationMode::ManualSweep;
        calibration_original_configuration_ = configuration_;
        calibration_directory_ = outputDirectory;
        calibration_phase_ = options.firstPhase;
        calibration_rate_trim_ = options.firstRateTrim;
        calibration_odd_line_phase_ = options.oddLinePhase;
        calibration_images_written_ = 0;
        calibration_total_images_ = options.imageCount();
        calibration_sweep_active_ = true;

        configuration_action_->setEnabled(false);
        calibration_sweep_action_->setEnabled(false);
        engineering_autotune_action_->setEnabled(false);
        diagnostic_action_->setEnabled(false);
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

    QString autotuneStageName() const {
        switch (autotune_stage_) {
        case AutotuneStage::RateAndPhase:
            return QStringLiteral("rate_phase");
        case AutotuneStage::OddLinePhase:
            return QStringLiteral("odd_line_phase");
        case AutotuneStage::Validation:
            return QStringLiteral("validation");
        }
        return QStringLiteral("unknown");
    }

    std::array<QRgb, 8> viewerPalette() const {
        std::array<QRgb, 8> result = {};
        for (size_t index = 0; index < result.size(); ++index) {
            const quint16 color = configuration_.palette[index];
            result[index] =
                qRgb((color & 0x0fu) * 17, ((color >> 4u) & 0x0fu) * 17,
                     ((color >> 8u) & 0x0fu) * 17);
        }
        return result;
    }

    void resetAutotuneAccumulator() {
        autotune_accumulator_ =
            std::make_unique<EngineeringCandidateAccumulator>(viewerPalette());
    }

    int calibrationFramesForCurrentSetting() const {
        if (calibration_mode_ != CalibrationMode::EngineeringAutotune) {
            return calibration_options_.framesPerSetting;
        }
        return autotune_stage_ == AutotuneStage::Validation
                   ? autotune_options_.validationFrames
                   : autotune_options_.framesPerCandidate;
    }

    int calibrationSettlingFrames() const {
        return calibration_mode_ == CalibrationMode::EngineeringAutotune
                   ? autotune_options_.settlingFrames
                   : calibration_options_.settlingFrames;
    }

    void startEngineeringAutotune() {
        if (!connected_ || calibration_sweep_active_ || diagnostic_active_ ||
            lab_experiment_active_) {
            return;
        }
        if (!last_signal_present_) {
            QMessageBox::warning(
                this, QStringLiteral("No P2000T signal"),
                QStringLiteral("Display the static SAA5050 engineering "
                               "screen before starting autotune."));
            return;
        }
        EngineeringAutotuneDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const EngineeringAutotuneOptions options = dialog.options();

        QSettings viewerSettings;
        QString initialDirectory =
            viewerSettings.value(QStringLiteral("autotune/lastParentDirectory"))
                .toString();
        if (initialDirectory.isEmpty() || !QDir(initialDirectory).exists()) {
            initialDirectory = QStandardPaths::writableLocation(
                QStandardPaths::PicturesLocation);
        }
        if (initialDirectory.isEmpty()) {
            initialDirectory = QDir::currentPath();
        }
        const QString parentDirectory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose engineering autotune log folder"),
            initialDirectory,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (parentDirectory.isEmpty()) {
            return;
        }
        viewerSettings.setValue(QStringLiteral("autotune/lastParentDirectory"),
                                parentDirectory);

        QDir parent(parentDirectory);
        const QString stem = QStringLiteral("p2000t-autotune-%1")
                                 .arg(QDateTime::currentDateTime().toString(
                                     QStringLiteral("yyyyMMdd-HHmmss")));
        QString directoryName = stem;
        for (int suffix = 2; parent.exists(directoryName); ++suffix) {
            directoryName = QStringLiteral("%1-%2").arg(stem).arg(suffix);
        }
        if (!parent.mkdir(directoryName)) {
            QMessageBox::critical(
                this, QStringLiteral("Could not create autotune folder"),
                QStringLiteral("Could not create %1 inside %2.")
                    .arg(directoryName, parentDirectory));
            return;
        }
        const QString outputDirectory = parent.filePath(directoryName);

        QJsonObject original;
        original.insert(QStringLiteral("phase"), configuration_.phase);
        original.insert(QStringLiteral("odd_line_phase"),
                        configuration_.oddLinePhase);
        original.insert(QStringLiteral("rate_trim"),
                        configuration_.sampleRateTrim);
        original.insert(QStringLiteral("sample_reconstruction"),
                        configuration_.sampleReconstruction);
        QJsonObject parameters;
        parameters.insert(QStringLiteral("settling_frames"),
                          options.settlingFrames);
        parameters.insert(QStringLiteral("frames_per_candidate"),
                          options.framesPerCandidate);
        parameters.insert(QStringLiteral("validation_frames"),
                          options.validationFrames);
        parameters.insert(QStringLiteral("rate_phase_candidates"),
                          options.ratePhaseCandidates());
        parameters.insert(QStringLiteral("odd_phase_candidates"),
                          options.oddPhaseCandidates());
        QJsonObject session;
        session.insert(QStringLiteral("format"),
                       QStringLiteral("p2000t-engineering-autotune-v1"));
        session.insert(QStringLiteral("viewer_version"),
                       QStringLiteral(P2000T_VIEWER_VERSION));
        session.insert(QStringLiteral("started_utc"),
                       QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        session.insert(QStringLiteral("source_screen"),
                       QStringLiteral("SAA5050 engineering screen"));
        session.insert(QStringLiteral("forced_reconstruction"),
                       QStringLiteral("raw"));
        session.insert(QStringLiteral("first_visible_source_line"),
                       configuration_.vertical);
        session.insert(QStringLiteral("corrected_physical_odd_lines_map_to"),
                       (configuration_.vertical & 1) == 0
                           ? QStringLiteral("logical_odd_rows")
                           : QStringLiteral("logical_even_rows"));
        session.insert(QStringLiteral("scoring_formula"),
                       QStringLiteral("instability_ppm + 4 * "
                                      "one_dot_artifact_ppm"));
        session.insert(QStringLiteral("original_live_settings"), original);
        session.insert(QStringLiteral("parameters"), parameters);
        QFile sessionFile(
            QDir(outputDirectory).filePath(QStringLiteral("session.json")));
        if (!sessionFile.open(QIODevice::WriteOnly) ||
            sessionFile.write(QJsonDocument(session).toJson()) < 0) {
            QMessageBox::critical(
                this, QStringLiteral("Could not start engineering autotune"),
                QStringLiteral("Could not write session.json in %1.")
                    .arg(outputDirectory));
            return;
        }
        sessionFile.close();

        calibration_manifest_.setFileName(
            QDir(outputDirectory).filePath(QStringLiteral("frames.csv")));
        autotune_candidates_file_.setFileName(
            QDir(outputDirectory).filePath(QStringLiteral("candidates.csv")));
        if (!calibration_manifest_.open(QIODevice::WriteOnly |
                                        QIODevice::Text) ||
            !autotune_candidates_file_.open(QIODevice::WriteOnly |
                                            QIODevice::Text)) {
            if (calibration_manifest_.isOpen()) {
                calibration_manifest_.close();
            }
            QMessageBox::critical(
                this, QStringLiteral("Could not start engineering autotune"),
                QStringLiteral("Could not create CSV logs in %1.")
                    .arg(outputDirectory));
            return;
        }
        {
            QTextStream frames(&calibration_manifest_);
            frames << "filename,stage,phase,odd_line_phase,rate_trim,"
                      "frame_at_setting,sequence,capture_timestamp_us,"
                      "received_utc\n";
            frames.flush();
            QTextStream candidates(&autotune_candidates_file_);
            candidates
                << "stage,phase,odd_line_phase,rate_trim,frames,"
                   "unstable_pixels,unstable_even_lines,unstable_odd_lines,"
                   "instability_ppm,even_instability_ppm,"
                   "odd_instability_ppm,isolated_pixels,third_color_pixels,"
                   "artifact_ppm,horizontal_transitions,score,even_score,"
                   "odd_score,modal_filename\n";
            candidates.flush();
        }

        autotune_options_ = options;
        autotune_odd_lines_use_logical_odd_ =
            (configuration_.vertical & 1) == 0;
        calibration_mode_ = CalibrationMode::EngineeringAutotune;
        autotune_stage_ = AutotuneStage::RateAndPhase;
        calibration_original_configuration_ = configuration_;
        calibration_directory_ = outputDirectory;
        calibration_phase_ = P2000T_CONTROL_MIN_PHASE;
        calibration_rate_trim_ = P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM;
        calibration_odd_line_phase_ = P2000T_CONTROL_DEFAULT_ODD_LINE_PHASE;
        calibration_images_written_ = 0;
        calibration_total_images_ = options.retainedFrames();
        autotune_results_.clear();
        resetAutotuneAccumulator();
        calibration_sweep_active_ = true;

        configuration_action_->setEnabled(false);
        calibration_sweep_action_->setEnabled(false);
        engineering_autotune_action_->setEnabled(false);
        diagnostic_action_->setEnabled(false);
        calibration_progress_ = new QProgressDialog(
            QString(), QStringLiteral("Cancel and restore settings"), 0,
            calibration_total_images_, this);
        calibration_progress_->setWindowTitle(
            QStringLiteral("Engineering-screen autotune"));
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
            configuration.oddLinePhase != calibration_odd_line_phase_ ||
            configuration.sampleRateTrim != calibration_rate_trim_ ||
            (calibration_mode_ == CalibrationMode::EngineeringAutotune &&
             configuration.sampleReconstruction !=
                 P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW)) {
            return;
        }
        calibration_ack_timer_.stop();
        calibration_waiting_for_configuration_ = false;
        calibration_settling_frames_ = calibrationSettlingFrames();
        calibration_frame_at_setting_ = 0;
        statusBar()->showMessage(
            QStringLiteral("%1: settling phase %2, rate %3, odd phase %4")
                .arg(calibration_mode_ == CalibrationMode::EngineeringAutotune
                         ? QStringLiteral("Engineering autotune")
                         : QStringLiteral("Calibration sweep"))
                .arg(calibration_phase_)
                .arg(calibration_rate_trim_)
                .arg(calibration_odd_line_phase_));
    }

    static QJsonObject metricsJson(const EngineeringCandidateMetrics &metrics) {
        QJsonObject result;
        result.insert(QStringLiteral("frames"), metrics.frames);
        result.insert(QStringLiteral("unstable_pixels"),
                      static_cast<qint64>(metrics.unstablePixels));
        result.insert(QStringLiteral("unstable_even_lines"),
                      static_cast<qint64>(metrics.unstableEvenLines));
        result.insert(QStringLiteral("unstable_odd_lines"),
                      static_cast<qint64>(metrics.unstableOddLines));
        result.insert(QStringLiteral("instability_ppm"),
                      metrics.instabilityPpm);
        result.insert(QStringLiteral("even_instability_ppm"),
                      metrics.evenInstabilityPpm);
        result.insert(QStringLiteral("odd_instability_ppm"),
                      metrics.oddInstabilityPpm);
        result.insert(QStringLiteral("robust_unstable_pixels"),
                      static_cast<qint64>(metrics.robustUnstablePixels));
        result.insert(QStringLiteral("robust_instability_ppm"),
                      metrics.robustInstabilityPpm);
        result.insert(QStringLiteral("robust_even_instability_ppm"),
                      metrics.robustEvenInstabilityPpm);
        result.insert(QStringLiteral("robust_odd_instability_ppm"),
                      metrics.robustOddInstabilityPpm);
        result.insert(QStringLiteral("median_frame_mismatches"),
                      static_cast<qint64>(metrics.medianFrameMismatches));
        result.insert(QStringLiteral("robust_frame_threshold"),
                      static_cast<qint64>(metrics.robustFrameThreshold));
        result.insert(QStringLiteral("robust_frames"), metrics.robustFrames);
        result.insert(QStringLiteral("coherent_outlier_frames"),
                      metrics.coherentOutlierFrames);
        result.insert(QStringLiteral("isolated_pixels"),
                      static_cast<qint64>(metrics.isolatedPixels));
        result.insert(QStringLiteral("third_color_pixels"),
                      static_cast<qint64>(metrics.thirdColorPixels));
        result.insert(QStringLiteral("artifact_ppm"), metrics.artifactPpm);
        result.insert(QStringLiteral("horizontal_transitions"),
                      static_cast<qint64>(metrics.horizontalTransitions));
        result.insert(QStringLiteral("score"), metrics.score);
        result.insert(QStringLiteral("even_score"), metrics.evenScore);
        result.insert(QStringLiteral("odd_score"), metrics.oddScore);
        return result;
    }

    static QJsonObject
    fidelityJson(const EngineeringFidelityMetrics &fidelity) {
        QJsonObject result;
        result.insert(QStringLiteral("pixels"),
                      static_cast<qint64>(fidelity.pixels));
        result.insert(QStringLiteral("mismatched_pixels"),
                      static_cast<qint64>(fidelity.mismatchedPixels));
        result.insert(QStringLiteral("erased_pixels"),
                      static_cast<qint64>(fidelity.erasedPixels));
        result.insert(QStringLiteral("filled_pixels"),
                      static_cast<qint64>(fidelity.filledPixels));
        result.insert(QStringLiteral("recolored_pixels"),
                      static_cast<qint64>(fidelity.recoloredPixels));
        result.insert(QStringLiteral("mismatch_ppm"), fidelity.mismatchPpm);
        return result;
    }

    bool finishAutotuneCandidate() {
        if (autotune_accumulator_ == nullptr ||
            autotune_accumulator_->empty()) {
            return false;
        }
        EngineeringCandidateResult candidate;
        candidate.stage = autotuneStageName();
        candidate.phase = calibration_phase_;
        candidate.oddLinePhase = calibration_odd_line_phase_;
        candidate.rateTrim = calibration_rate_trim_;
        candidate.metrics = autotune_accumulator_->finalize();
        candidate.modalFilename =
            QStringLiteral("modal_%1_phase_%2_rate_%3_odd_%4.png")
                .arg(candidate.stage, signedSweepValue(candidate.phase),
                     signedSweepValue(candidate.rateTrim),
                     signedSweepValue(candidate.oddLinePhase));
        const QString modalPath =
            QDir(calibration_directory_).filePath(candidate.modalFilename);
        if (!candidate.metrics.modalImage.save(modalPath, "PNG")) {
            return false;
        }
        candidate.metrics.modalImage = QImage();
        {
            QTextStream csv(&autotune_candidates_file_);
            const auto &m = candidate.metrics;
            csv << candidate.stage << ',' << candidate.phase << ','
                << candidate.oddLinePhase << ',' << candidate.rateTrim << ','
                << m.frames << ',' << m.unstablePixels << ','
                << m.unstableEvenLines << ',' << m.unstableOddLines << ','
                << QString::number(m.instabilityPpm, 'f', 6) << ','
                << QString::number(m.evenInstabilityPpm, 'f', 6) << ','
                << QString::number(m.oddInstabilityPpm, 'f', 6) << ','
                << m.isolatedPixels << ',' << m.thirdColorPixels << ','
                << QString::number(m.artifactPpm, 'f', 6) << ','
                << m.horizontalTransitions << ','
                << QString::number(m.score, 'f', 6) << ','
                << QString::number(m.evenScore, 'f', 6) << ','
                << QString::number(m.oddScore, 'f', 6) << ','
                << candidate.modalFilename << '\n';
            csv.flush();
        }
        autotune_candidates_file_.flush();
        if (autotune_candidates_file_.error() != QFileDevice::NoError) {
            return false;
        }
        autotune_results_.push_back(std::move(candidate));
        return true;
    }

    const EngineeringCandidateResult *
    bestAutotuneCandidate(const QString &stage, bool physicalOddOnly,
                          bool physicalEvenOnly = false) const {
        const EngineeringCandidateResult *best = nullptr;
        for (const auto &candidate : autotune_results_) {
            if (candidate.stage != stage) {
                continue;
            }
            const auto selectedScore =
                [this, physicalOddOnly,
                 physicalEvenOnly](const EngineeringCandidateResult &c) {
                    if (physicalOddOnly) {
                        return autotune_odd_lines_use_logical_odd_
                                   ? c.metrics.oddScore
                                   : c.metrics.evenScore;
                    }
                    if (physicalEvenOnly) {
                        return autotune_odd_lines_use_logical_odd_
                                   ? c.metrics.evenScore
                                   : c.metrics.oddScore;
                    }
                    return c.metrics.score;
                };
            const double value = selectedScore(candidate);
            const double bestValue =
                best == nullptr ? 0.0 : selectedScore(*best);
            const int magnitude = std::abs(candidate.phase) +
                                  std::abs(candidate.oddLinePhase) +
                                  std::abs(candidate.rateTrim);
            const int bestMagnitude = best == nullptr
                                          ? 0
                                          : std::abs(best->phase) +
                                                std::abs(best->oddLinePhase) +
                                                std::abs(best->rateTrim);
            if (best == nullptr || value < bestValue - 1e-9 ||
                (std::abs(value - bestValue) <= 1e-9 &&
                 magnitude < bestMagnitude)) {
                best = &candidate;
            }
        }
        return best;
    }

    bool writeAutotuneResult() {
        const EngineeringCandidateResult *ratePhase =
            bestAutotuneCandidate(QStringLiteral("rate_phase"), false, true);
        const EngineeringCandidateResult *odd =
            bestAutotuneCandidate(QStringLiteral("odd_line_phase"), true);
        const EngineeringCandidateResult *validation =
            bestAutotuneCandidate(QStringLiteral("validation"), false);
        if (ratePhase == nullptr || odd == nullptr || validation == nullptr) {
            return false;
        }
        auto settingJson = [](const EngineeringCandidateResult &candidate) {
            QJsonObject setting;
            setting.insert(QStringLiteral("phase"), candidate.phase);
            setting.insert(QStringLiteral("odd_line_phase"),
                           candidate.oddLinePhase);
            setting.insert(QStringLiteral("rate_trim"), candidate.rateTrim);
            setting.insert(QStringLiteral("sample_reconstruction"),
                           QStringLiteral("raw"));
            setting.insert(QStringLiteral("metrics"),
                           metricsJson(candidate.metrics));
            setting.insert(QStringLiteral("modal_png"),
                           candidate.modalFilename);
            return setting;
        };
        QJsonObject result;
        result.insert(QStringLiteral("format"),
                      QStringLiteral("p2000t-engineering-autotune-result-v1"));
        result.insert(QStringLiteral("completed_utc"),
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        result.insert(QStringLiteral("status"), QStringLiteral("complete"));
        result.insert(QStringLiteral("selection_rule"),
                      QStringLiteral("lowest physical-even-line score for "
                                     "base phase/rate; "
                                     "lowest physical-odd-line score for odd "
                                     "phase; "
                                     "smallest absolute correction breaks "
                                     "exact ties"));
        result.insert(QStringLiteral("physical_odd_lines_scored_as"),
                      autotune_odd_lines_use_logical_odd_
                          ? QStringLiteral("logical_odd_rows")
                          : QStringLiteral("logical_even_rows"));
        result.insert(QStringLiteral("rate_phase_winner"),
                      settingJson(*ratePhase));
        result.insert(QStringLiteral("recommended_live_settings"),
                      settingJson(*odd));
        result.insert(QStringLiteral("validation"), settingJson(*validation));
        result.insert(QStringLiteral("written_to_flash"), false);
        QFile output(QDir(calibration_directory_)
                         .filePath(QStringLiteral("result.json")));
        return output.open(QIODevice::WriteOnly) &&
               output.write(QJsonDocument(result).toJson()) >= 0;
    }

    void advanceAutotune() {
        if (autotune_stage_ == AutotuneStage::RateAndPhase) {
            if (calibration_rate_trim_ < P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM) {
                ++calibration_rate_trim_;
                sendCalibrationTarget();
                return;
            }
            if (calibration_phase_ < P2000T_CONTROL_MAX_PHASE) {
                calibration_rate_trim_ = P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM;
                ++calibration_phase_;
                sendCalibrationTarget();
                return;
            }
            const EngineeringCandidateResult *best = bestAutotuneCandidate(
                QStringLiteral("rate_phase"), false, true);
            if (best == nullptr) {
                endCalibrationSweep(SweepEnd::Failed,
                                    QStringLiteral("No phase/rate candidate "
                                                   "could be scored."));
                return;
            }
            calibration_phase_ = best->phase;
            calibration_rate_trim_ = best->rateTrim;
            calibration_odd_line_phase_ = P2000T_CONTROL_MIN_ODD_LINE_PHASE;
            autotune_stage_ = AutotuneStage::OddLinePhase;
            sendCalibrationTarget();
            return;
        }
        if (autotune_stage_ == AutotuneStage::OddLinePhase) {
            if (calibration_odd_line_phase_ <
                P2000T_CONTROL_MAX_ODD_LINE_PHASE) {
                ++calibration_odd_line_phase_;
                sendCalibrationTarget();
                return;
            }
            const EngineeringCandidateResult *best =
                bestAutotuneCandidate(QStringLiteral("odd_line_phase"), true);
            if (best == nullptr) {
                endCalibrationSweep(SweepEnd::Failed,
                                    QStringLiteral("No odd-line candidate "
                                                   "could be scored."));
                return;
            }
            autotune_winner_phase_ = best->phase;
            autotune_winner_rate_trim_ = best->rateTrim;
            autotune_winner_odd_line_phase_ = best->oddLinePhase;
            calibration_phase_ = autotune_winner_phase_;
            calibration_rate_trim_ = autotune_winner_rate_trim_;
            calibration_odd_line_phase_ = autotune_winner_odd_line_phase_;
            autotune_stage_ = AutotuneStage::Validation;
            sendCalibrationTarget();
            return;
        }
        if (!writeAutotuneResult()) {
            endCalibrationSweep(
                SweepEnd::Failed,
                QStringLiteral("Could not write the final result.json."));
            return;
        }
        endCalibrationSweep(SweepEnd::Completed);
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
        const QString stagePrefix =
            calibration_mode_ == CalibrationMode::EngineeringAutotune
                ? autotuneStageName() + QLatin1Char('_')
                : QString();
        const QString filename =
            QStringLiteral("%1phase_%2_rate_%3_odd_%4_frame_%5_seq_%6.png")
                .arg(stagePrefix, signedSweepValue(calibration_phase_),
                     signedSweepValue(calibration_rate_trim_),
                     signedSweepValue(calibration_odd_line_phase_))
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
            manifest << filename << ',';
            if (calibration_mode_ == CalibrationMode::EngineeringAutotune) {
                manifest << autotuneStageName() << ',';
            }
            manifest << calibration_phase_ << ',' << calibration_odd_line_phase_
                     << ',' << calibration_rate_trim_ << ',' << imageNumber
                     << ',' << sequence << ',' << current_capture_timestamp_us_
                     << ','
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
        if (calibration_mode_ == CalibrationMode::EngineeringAutotune &&
            (autotune_accumulator_ == nullptr ||
             !autotune_accumulator_->addFrame(current_frame_))) {
            endCalibrationSweep(
                SweepEnd::Failed,
                QStringLiteral("Could not add a retained frame to the "
                               "autotune scorer."));
            return;
        }

        ++calibration_frame_at_setting_;
        ++calibration_images_written_;
        calibration_progress_->setValue(calibration_images_written_);
        updateCalibrationProgressLabel();
        if (calibration_frame_at_setting_ <
            calibrationFramesForCurrentSetting()) {
            return;
        }

        if (calibration_mode_ == CalibrationMode::EngineeringAutotune) {
            if (!finishAutotuneCandidate()) {
                endCalibrationSweep(
                    SweepEnd::Failed,
                    QStringLiteral("Could not score or save the current "
                                   "autotune candidate."));
                return;
            }
            advanceAutotune();
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
        const bool autotune =
            calibration_mode_ == CalibrationMode::EngineeringAutotune;

        if (calibration_manifest_.isOpen()) {
            calibration_manifest_.close();
        }
        if (autotune_candidates_file_.isOpen()) {
            autotune_candidates_file_.close();
        }
        if (calibration_progress_ != nullptr) {
            calibration_progress_->close();
            calibration_progress_->deleteLater();
            calibration_progress_ = nullptr;
        }

        bool settingsSent = true;
        if (end != SweepEnd::Disconnected && connected_) {
            if (autotune && end == SweepEnd::Completed) {
                settingsSent = sendCalibrationValues(
                    autotune_winner_phase_, autotune_winner_odd_line_phase_,
                    autotune_winner_rate_trim_,
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW);
            } else {
                settingsSent = sendCalibrationValues(
                    original.phase, original.oddLinePhase,
                    original.sampleRateTrim, original.sampleReconstruction);
            }
        }
        configuration_action_->setEnabled(connected_);
        calibration_sweep_action_->setEnabled(connected_);
        engineering_autotune_action_->setEnabled(connected_);
        diagnostic_action_->setEnabled(connected_);
        if (!settingsSent) {
            return;
        }

        if (end == SweepEnd::Completed) {
            if (autotune) {
                statusBar()->showMessage(
                    QStringLiteral("Engineering autotune complete; winner "
                                   "applied live"),
                    7000);
                QMessageBox::information(
                    this, QStringLiteral("Engineering autotune complete"),
                    QStringLiteral(
                        "The validated winner is now active:\n\n"
                        "phase %1, rate trim %2, odd-line phase %3, raw "
                        "reconstruction\n\n"
                        "It has not been saved to Pico flash. Saved %4 "
                        "retained frames, modal images, candidates.csv, and "
                        "result.json in:\n%5")
                        .arg(autotune_winner_phase_)
                        .arg(autotune_winner_rate_trim_)
                        .arg(autotune_winner_odd_line_phase_)
                        .arg(imagesWritten)
                        .arg(QDir::toNativeSeparators(directory)));
                return;
            }
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
                QStringLiteral("%1 cancelled after %2 PNGs; settings restored")
                    .arg(autotune ? QStringLiteral("Engineering autotune")
                                  : QStringLiteral("Calibration sweep"))
                    .arg(imagesWritten),
                5000);
        } else if (end == SweepEnd::Failed) {
            QMessageBox::critical(
                this,
                autotune ? QStringLiteral("Engineering autotune stopped")
                         : QStringLiteral("Calibration sweep stopped"),
                QStringLiteral("%1\n\nThe original adapter settings have "
                               "been restored. Partial results remain in:\n%2")
                    .arg(detail, QDir::toNativeSeparators(directory)));
        }
    }

    void closeDiagnosticFiles() {
        diagnostic_watchdog_.stop();
        if (diagnostic_records_.isOpen()) {
            diagnostic_records_.close();
        }
        if (diagnostic_manifest_.isOpen()) {
            diagnostic_manifest_.close();
        }
        if (diagnostic_progress_ != nullptr) {
            disconnect(diagnostic_progress_, nullptr, this, nullptr);
            diagnostic_progress_->close();
            diagnostic_progress_->deleteLater();
            diagnostic_progress_ = nullptr;
        }
    }

    void restartScreenStream() {
        if (!connected_) {
            return;
        }
        receive_buffer_.clear();
        const char command =
            static_cast<char>(encoding_->currentData().toInt());
        QByteArray request(1, command);
        request.append(makePicoControlPacket(P2000T_CONTROL_GET_SETTINGS));
        if (!serial_.write(request)) {
            connectionLost();
        }
    }

    void finishDiagnosticCapture(bool cancelled,
                                 const QString &detail = QString()) {
        if (!diagnostic_active_) {
            return;
        }
        const QString directory = diagnostic_directory_;
        const int records = diagnostic_records_written_;
        const QString labRequestId = diagnostic_lab_request_id_;
        diagnostic_lab_request_id_.clear();
        diagnostic_active_ = false;
        diagnostic_watchdog_.stop();
        closeDiagnosticFiles();
        configuration_action_->setEnabled(connected_);
        calibration_sweep_action_->setEnabled(connected_);
        engineering_autotune_action_->setEnabled(connected_);
        diagnostic_action_->setEnabled(connected_);
        if (connected_ && detail.isEmpty()) {
            restartScreenStream();
        }
        if (!labRequestId.isEmpty()) {
            if (!detail.isEmpty() && connected_) {
                disconnectAdapter();
            }
            const bool success = detail.isEmpty() && !cancelled;
            QJsonObject result;
            result.insert(QStringLiteral("protocol"), 1);
            result.insert(QStringLiteral("id"), labRequestId);
            result.insert(QStringLiteral("ok"), success);
            result.insert(QStringLiteral("state"),
                          success ? QStringLiteral("complete")
                          : cancelled ? QStringLiteral("cancelled")
                                      : QStringLiteral("failed"));
            result.insert(QStringLiteral("completed_utc"),
                          QDateTime::currentDateTimeUtc().toString(
                              Qt::ISODate));
            result.insert(QStringLiteral("diagnostic_directory"),
                          QDir::toNativeSeparators(directory));
            result.insert(QStringLiteral("diagnostic_relative_directory"),
                          QStringLiteral("diagnostics/%1").arg(labRequestId));
            result.insert(QStringLiteral("raw_bursts"), records);
            result.insert(QStringLiteral("start_line"),
                          diagnostic_options_.startLine);
            result.insert(QStringLiteral("line_count"),
                          diagnostic_options_.lineCount);
            result.insert(QStringLiteral("requested_repetitions"),
                          diagnostic_options_.repetitions);
            if (!detail.isEmpty()) {
                result.insert(QStringLiteral("error"), detail);
            }
            QSaveFile resultFile(
                QDir(directory).filePath(QStringLiteral("result.json")));
            if (resultFile.open(QIODevice::WriteOnly)) {
                const QByteArray data = QJsonDocument(result).toJson();
                if (resultFile.write(data) == data.size()) {
                    resultFile.commit();
                }
            }
            respondToLabRequest(
                labRequestId, success,
                success ? QStringLiteral("complete")
                : cancelled ? QStringLiteral("cancelled")
                            : QStringLiteral("failed"),
                detail, result);
            statusBar()->showMessage(
                success
                    ? QStringLiteral("Codex diagnostic %1 complete: %2 bursts")
                          .arg(labRequestId)
                          .arg(records)
                    : QStringLiteral("Codex diagnostic %1 did not complete")
                          .arg(labRequestId),
                5000);
            writeCodexLabStatus();
            return;
        }
        if (!detail.isEmpty()) {
            if (connected_) {
                disconnectAdapter();
            }
            QMessageBox::critical(
                this, QStringLiteral("Diagnostic recording failed"),
                QStringLiteral("%1\n\nPartial data remains in:\n%2")
                    .arg(detail, QDir::toNativeSeparators(directory)));
        } else if (cancelled) {
            statusBar()->showMessage(
                QStringLiteral("Diagnostic recording cancelled; partial "
                               "records retained"),
                5000);
        } else {
            statusBar()->showMessage(
                QStringLiteral("Diagnostic recording complete: %1 raw bursts")
                    .arg(records),
                5000);
            QMessageBox::information(
                this, QStringLiteral("Diagnostic recording complete"),
                QStringLiteral("Saved the lossless session in:\n%1")
                    .arg(QDir::toNativeSeparators(directory)));
        }
    }

    void requestDiagnosticCancellation() {
        if (!diagnostic_active_ || diagnostic_cancel_requested_) {
            return;
        }
        diagnostic_cancel_requested_ = true;
        if (diagnostic_progress_ != nullptr) {
            diagnostic_progress_->setLabelText(
                QStringLiteral("Cancelling after the current USB record..."));
            diagnostic_progress_->setCancelButton(nullptr);
        }
        if (!sendControl(
                makePicoControlPacket(P2000T_CONTROL_CANCEL_DIAGNOSTICS))) {
            return;
        }
    }

    void startLabDiagnosticCapture(const CodexLabRequest &request) {
        QDir root(lab_bridge_.rootDirectory());
        const QString relativeDirectory =
            QStringLiteral("diagnostics/%1").arg(request.id);
        if (root.exists(relativeDirectory) ||
            !root.mkpath(relativeDirectory)) {
            respondToLabRequest(
                request.id, false, QStringLiteral("rejected"),
                QStringLiteral("The diagnostic directory already exists or "
                               "could not be created."));
            return;
        }

        diagnostic_directory_ = root.filePath(relativeDirectory);
        diagnostic_records_.setFileName(
            QDir(diagnostic_directory_)
                .filePath(QStringLiteral("capture.p2td")));
        diagnostic_manifest_.setFileName(
            QDir(diagnostic_directory_)
                .filePath(QStringLiteral("manifest.csv")));
        if (!diagnostic_records_.open(QIODevice::WriteOnly) ||
            !diagnostic_manifest_.open(QIODevice::WriteOnly |
                                       QIODevice::Text)) {
            closeDiagnosticFiles();
            respondToLabRequest(
                request.id, false, QStringLiteral("failed"),
                QStringLiteral("Could not create the diagnostic files."));
            return;
        }
        {
            QTextStream manifest(&diagnostic_manifest_);
            manifest << "record_sequence,type,file_offset,record_size,"
                        "payload_size,session_id,sample_rate_hz,sample_count,"
                        "start_line,line_count,repetition,repetitions,"
                        "capture_sequence,capture_duration_us,"
                        "expected_duration_us,flags,captured_utc\n";
            manifest.flush();
        }

        const DiagnosticCaptureOptions options{
            request.diagnosticStartLine, request.diagnosticLineCount,
            request.diagnosticRepetitions};
        QJsonObject session;
        session.insert(QStringLiteral("format"),
                       QStringLiteral("P2000T high-resolution diagnostics"));
        session.insert(QStringLiteral("protocol_version"),
                       P2000T_DIAGNOSTIC_PROTOCOL_VERSION);
        session.insert(QStringLiteral("viewer_version"),
                       QStringLiteral(P2000T_VIEWER_VERSION));
        session.insert(QStringLiteral("lab_request_id"), request.id);
        session.insert(QStringLiteral("tag"), request.tag);
        session.insert(QStringLiteral("started_utc"),
                       QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        session.insert(QStringLiteral("serial_port"), serial_.name());
        session.insert(QStringLiteral("requested_start_line"),
                       options.startLine);
        session.insert(QStringLiteral("requested_line_count"),
                       options.lineCount);
        session.insert(QStringLiteral("requested_repetitions"),
                       options.repetitions);
        session.insert(QStringLiteral("first_visible_line"),
                       configuration_.vertical);
        session.insert(QStringLiteral("sample_phase"), configuration_.phase);
        session.insert(QStringLiteral("odd_line_phase"),
                       configuration_.oddLinePhase);
        session.insert(QStringLiteral("sample_rate_trim"),
                       configuration_.sampleRateTrim);
        session.insert(QStringLiteral("horizontal_offset"),
                       configuration_.horizontal);
        QFile sessionFile(QDir(diagnostic_directory_)
                              .filePath(QStringLiteral("session.json")));
        if (!sessionFile.open(QIODevice::WriteOnly) ||
            sessionFile.write(
                QJsonDocument(session).toJson(QJsonDocument::Indented)) < 0) {
            closeDiagnosticFiles();
            respondToLabRequest(
                request.id, false, QStringLiteral("failed"),
                QStringLiteral("Could not write diagnostic session.json."));
            return;
        }
        sessionFile.close();

        diagnostic_options_ = options;
        diagnostic_records_written_ = 0;
        diagnostic_session_id_ = 0u;
        diagnostic_last_record_sequence_ = 0u;
        diagnostic_timing_received_ = false;
        diagnostic_waiting_for_guard_ = false;
        diagnostic_guard_action_ = 0;
        diagnostic_record_retries_ = 0;
        diagnostic_guard_sequence_ = 0u;
        diagnostic_cancel_requested_ = false;
        diagnostic_lab_request_id_ = request.id;
        diagnostic_active_ = true;
        configuration_action_->setEnabled(false);
        calibration_sweep_action_->setEnabled(false);
        engineering_autotune_action_->setEnabled(false);
        diagnostic_action_->setEnabled(false);
        receive_buffer_.clear();
        const quint32 packedValue =
            (static_cast<quint32>(options.repetitions) << 16u) |
            static_cast<quint32>(options.startLine);
        QByteArray command(1, 'q');
        command += makePicoControlPacket(
            P2000T_CONTROL_START_DIAGNOSTICS,
            static_cast<quint8>(options.lineCount), packedValue);
        if (!serial_.write(command)) {
            finishDiagnosticCapture(
                false, QStringLiteral("The Pico connection was lost while "
                                      "starting diagnostics."));
            return;
        }
        diagnostic_watchdog_.start();
        statusBar()->showMessage(
            QStringLiteral("Codex diagnostic %1 recording %2 bursts")
                .arg(request.id)
                .arg(options.repetitions));
        writeCodexLabStatus();
    }

    void startDiagnosticCapture() {
        if (!connected_ || calibration_sweep_active_ || diagnostic_active_ ||
            lab_experiment_active_) {
            return;
        }
        if (!last_signal_present_) {
            QMessageBox::warning(
                this, QStringLiteral("No P2000T signal"),
                QStringLiteral("A stable P2000T source image is required "
                               "before recording diagnostics."));
            return;
        }
        DiagnosticCaptureDialog dialog(configuration_, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const DiagnosticCaptureOptions options = dialog.options();

        QSettings settings;
        QString parentDirectory =
            settings.value(QStringLiteral("diagnostics/lastParentDirectory"))
                .toString();
        if (parentDirectory.isEmpty() || !QDir(parentDirectory).exists()) {
            parentDirectory = QStandardPaths::writableLocation(
                QStandardPaths::DocumentsLocation);
        }
        if (parentDirectory.isEmpty()) {
            parentDirectory = QDir::currentPath();
        }
        parentDirectory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose diagnostic session folder"),
            parentDirectory,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (parentDirectory.isEmpty()) {
            return;
        }
        settings.setValue(QStringLiteral("diagnostics/lastParentDirectory"),
                          parentDirectory);

        QDir parent(parentDirectory);
        const QString stem = QStringLiteral("p2000t-diagnostics-%1")
                                 .arg(QDateTime::currentDateTime().toString(
                                     QStringLiteral("yyyyMMdd-HHmmss")));
        QString directoryName = stem;
        for (int suffix = 2; parent.exists(directoryName); ++suffix) {
            directoryName = QStringLiteral("%1-%2").arg(stem).arg(suffix);
        }
        if (!parent.mkdir(directoryName)) {
            QMessageBox::critical(
                this, QStringLiteral("Could not create diagnostic folder"),
                QStringLiteral("Could not create %1 inside %2.")
                    .arg(directoryName, parentDirectory));
            return;
        }
        diagnostic_directory_ = parent.filePath(directoryName);
        diagnostic_records_.setFileName(
            QDir(diagnostic_directory_)
                .filePath(QStringLiteral("capture.p2td")));
        diagnostic_manifest_.setFileName(
            QDir(diagnostic_directory_)
                .filePath(QStringLiteral("manifest.csv")));
        if (!diagnostic_records_.open(QIODevice::WriteOnly) ||
            !diagnostic_manifest_.open(QIODevice::WriteOnly |
                                       QIODevice::Text)) {
            closeDiagnosticFiles();
            QMessageBox::critical(
                this, QStringLiteral("Could not start diagnostics"),
                QStringLiteral("Could not create session files in %1.")
                    .arg(diagnostic_directory_));
            return;
        }
        {
            QTextStream manifest(&diagnostic_manifest_);
            manifest << "record_sequence,type,file_offset,record_size,"
                        "payload_size,session_id,sample_rate_hz,sample_count,"
                        "start_line,line_count,repetition,repetitions,"
                        "capture_sequence,capture_duration_us,"
                        "expected_duration_us,flags,captured_utc\n";
            manifest.flush();
        }

        QJsonObject session;
        session.insert(QStringLiteral("format"),
                       QStringLiteral("P2000T high-resolution diagnostics"));
        session.insert(QStringLiteral("protocol_version"),
                       P2000T_DIAGNOSTIC_PROTOCOL_VERSION);
        session.insert(QStringLiteral("viewer_version"),
                       QStringLiteral(P2000T_VIEWER_VERSION));
        session.insert(QStringLiteral("started_utc"),
                       QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        session.insert(QStringLiteral("serial_port"), serial_.name());
        session.insert(QStringLiteral("requested_start_line"),
                       options.startLine);
        session.insert(QStringLiteral("requested_line_count"),
                       options.lineCount);
        session.insert(QStringLiteral("requested_repetitions"),
                       options.repetitions);
        session.insert(QStringLiteral("first_visible_line"),
                       configuration_.vertical);
        session.insert(QStringLiteral("sample_phase"), configuration_.phase);
        session.insert(QStringLiteral("odd_line_phase"),
                       configuration_.oddLinePhase);
        session.insert(QStringLiteral("sample_rate_trim"),
                       configuration_.sampleRateTrim);
        session.insert(QStringLiteral("horizontal_offset"),
                       configuration_.horizontal);
        QFile sessionFile(QDir(diagnostic_directory_)
                              .filePath(QStringLiteral("session.json")));
        if (!sessionFile.open(QIODevice::WriteOnly) ||
            sessionFile.write(
                QJsonDocument(session).toJson(QJsonDocument::Indented)) < 0) {
            closeDiagnosticFiles();
            QMessageBox::critical(
                this, QStringLiteral("Could not start diagnostics"),
                QStringLiteral("Could not write session.json in %1.")
                    .arg(diagnostic_directory_));
            return;
        }
        sessionFile.close();

        diagnostic_options_ = options;
        diagnostic_records_written_ = 0;
        diagnostic_session_id_ = 0u;
        diagnostic_last_record_sequence_ = 0u;
        diagnostic_timing_received_ = false;
        diagnostic_waiting_for_guard_ = false;
        diagnostic_guard_action_ = 0;
        diagnostic_record_retries_ = 0;
        diagnostic_guard_sequence_ = 0u;
        diagnostic_cancel_requested_ = false;
        diagnostic_active_ = true;
        configuration_action_->setEnabled(false);
        calibration_sweep_action_->setEnabled(false);
        engineering_autotune_action_->setEnabled(false);
        diagnostic_action_->setEnabled(false);
        diagnostic_progress_ = new QProgressDialog(
            QStringLiteral("Waiting for the CSYNC timing trace..."),
            QStringLiteral("Cancel recording"), 0, options.repetitions + 1,
            this);
        diagnostic_progress_->setWindowTitle(
            QStringLiteral("High-resolution diagnostics"));
        diagnostic_progress_->setWindowModality(Qt::WindowModal);
        diagnostic_progress_->setMinimumDuration(0);
        diagnostic_progress_->setAutoClose(false);
        diagnostic_progress_->setAutoReset(false);
        connect(diagnostic_progress_, &QProgressDialog::canceled, this,
                [this] { requestDiagnosticCancellation(); });
        diagnostic_progress_->show();

        receive_buffer_.clear();
        const quint32 packedValue =
            (static_cast<quint32>(options.repetitions) << 16u) |
            static_cast<quint32>(options.startLine);
        QByteArray request(1, 'q');
        request += makePicoControlPacket(P2000T_CONTROL_START_DIAGNOSTICS,
                                         static_cast<quint8>(options.lineCount),
                                         packedValue);
        if (!serial_.write(request)) {
            connectionLost();
        } else {
            diagnostic_watchdog_.start();
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

    void parseDiagnosticRecords() {
        const QByteArray magic(P2000T_DIAGNOSTIC_MAGIC, 4);
        const QByteArray guardMagic(P2000T_DIAGNOSTIC_GUARD_MAGIC,
                                    P2000T_DIAGNOSTIC_GUARD_MAGIC_SIZE);
        while (diagnostic_active_) {
            if (diagnostic_waiting_for_guard_) {
                const qsizetype guardOffset =
                    receive_buffer_.indexOf(guardMagic);
                if (guardOffset < 0) {
                    if (receive_buffer_.size() >= guardMagic.size()) {
                        receive_buffer_.remove(
                            0, receive_buffer_.size() - guardMagic.size() + 1);
                    }
                    return;
                }
                receive_buffer_.remove(0, guardOffset + guardMagic.size());
                diagnostic_waiting_for_guard_ = false;
                if (diagnostic_guard_action_ != 0) {
                    const quint8 opcode =
                        diagnostic_guard_action_ == 2
                            ? P2000T_CONTROL_RETRY_DIAGNOSTIC_RECORD
                            : P2000T_CONTROL_ACK_DIAGNOSTIC_RECORD;
                    const quint32 sequence = diagnostic_guard_sequence_;
                    diagnostic_guard_action_ = 0;
                    diagnostic_guard_sequence_ = 0u;
                    if (!sendControl(makePicoControlPacket(opcode, 0u,
                                                           sequence))) {
                        return;
                    }
                }
            }
            const qsizetype magicOffset = receive_buffer_.indexOf(magic);
            if (magicOffset < 0) {
                if (receive_buffer_.size() > 3) {
                    receive_buffer_.remove(0, receive_buffer_.size() - 3);
                }
                return;
            }
            if (magicOffset != 0) {
                receive_buffer_.remove(0, magicOffset);
            }
            if (receive_buffer_.size() < P2000T_DIAGNOSTIC_HEADER_SIZE) {
                return;
            }
            const char *header = receive_buffer_.constData();
            const quint8 version = static_cast<quint8>(header[4]);
            const quint8 type =
                static_cast<quint8>(header[P2000T_DIAGNOSTIC_TYPE_OFFSET]);
            const quint16 headerSize = loadU16(&header[6]);
            const quint32 flags =
                loadU32(&header[P2000T_DIAGNOSTIC_FLAGS_OFFSET]);
            const quint32 recordSequence =
                loadU32(&header[P2000T_DIAGNOSTIC_RECORD_SEQUENCE_OFFSET]);
            const quint32 payloadSize =
                loadU32(&header[P2000T_DIAGNOSTIC_PAYLOAD_SIZE_OFFSET]);
            const quint32 expectedCrc =
                loadU32(&header[P2000T_DIAGNOSTIC_CRC32_OFFSET]);
            const quint32 sessionId =
                loadU32(&header[P2000T_DIAGNOSTIC_SESSION_ID_OFFSET]);
            const quint32 sampleRate =
                loadU32(&header[P2000T_DIAGNOSTIC_SAMPLE_RATE_OFFSET]);
            const quint32 sampleCount =
                loadU32(&header[P2000T_DIAGNOSTIC_SAMPLE_COUNT_OFFSET]);
            const quint16 startLine =
                loadU16(&header[P2000T_DIAGNOSTIC_START_LINE_OFFSET]);
            const quint16 lineCount =
                loadU16(&header[P2000T_DIAGNOSTIC_LINE_COUNT_OFFSET]);
            const quint16 repetition =
                loadU16(&header[P2000T_DIAGNOSTIC_REPETITION_OFFSET]);
            const quint16 repetitions =
                loadU16(&header[P2000T_DIAGNOSTIC_REPETITIONS_OFFSET]);
            const quint8 bitsPerSample = static_cast<quint8>(
                header[P2000T_DIAGNOSTIC_BITS_PER_SAMPLE_OFFSET]);
            const quint32 captureSequence =
                loadU32(&header[P2000T_DIAGNOSTIC_CAPTURE_SEQUENCE_OFFSET]);
            const quint32 captureDuration =
                loadU32(&header[P2000T_DIAGNOSTIC_CAPTURE_DURATION_OFFSET]);
            const quint32 expectedDuration =
                loadU32(&header[P2000T_DIAGNOSTIC_EXPECTED_DURATION_OFFSET]);

            const bool commonValid =
                version == P2000T_DIAGNOSTIC_PROTOCOL_VERSION &&
                headerSize == P2000T_DIAGNOSTIC_HEADER_SIZE &&
                type >= P2000T_DIAGNOSTIC_RECORD_SESSION &&
                type <= P2000T_DIAGNOSTIC_RECORD_COMPLETE &&
                payloadSize <= P2000T_DIAGNOSTIC_TIMING_PAYLOAD_SIZE &&
                startLine == diagnostic_options_.startLine &&
                lineCount == diagnostic_options_.lineCount &&
                repetitions == diagnostic_options_.repetitions &&
                recordSequence == diagnostic_last_record_sequence_ + 1u &&
                (diagnostic_session_id_ == 0u ||
                 sessionId == diagnostic_session_id_);
            const bool orderValid =
                (diagnostic_last_record_sequence_ == 0u &&
                 type == P2000T_DIAGNOSTIC_RECORD_SESSION) ||
                (type == P2000T_DIAGNOSTIC_RECORD_TIMING &&
                 !diagnostic_timing_received_ &&
                 diagnostic_records_written_ == 0) ||
                (type == P2000T_DIAGNOSTIC_RECORD_RAW_RGBS &&
                 diagnostic_timing_received_ &&
                 repetition == diagnostic_records_written_ + 1) ||
                type == P2000T_DIAGNOSTIC_RECORD_COMPLETE;
            const bool sessionValid =
                type != P2000T_DIAGNOSTIC_RECORD_SESSION ||
                (payloadSize == 0u && sampleRate == 0u && sampleCount == 0u &&
                 repetition == 0u);
            const bool timingValid =
                type != P2000T_DIAGNOSTIC_RECORD_TIMING ||
                (payloadSize == P2000T_DIAGNOSTIC_TIMING_PAYLOAD_SIZE &&
                 sampleRate == P2000T_DIAGNOSTIC_TIMING_SAMPLE_RATE_HZ &&
                 sampleCount == P2000T_DIAGNOSTIC_TIMING_SAMPLE_COUNT &&
                 bitsPerSample == 1u && repetition == 0u);
            const quint32 expectedRawSamples =
                static_cast<quint32>(lineCount) *
                P2000T_DIAGNOSTIC_SAMPLES_PER_NOMINAL_LINE;
            const bool rawValid =
                type != P2000T_DIAGNOSTIC_RECORD_RAW_RGBS ||
                (payloadSize == expectedRawSamples / 2u &&
                 sampleRate == P2000T_DIAGNOSTIC_RAW_SAMPLE_RATE_HZ &&
                 sampleCount == expectedRawSamples && bitsPerSample == 4u &&
                 repetition >= 1u && repetition <= repetitions);
            const bool completeValid =
                type != P2000T_DIAGNOSTIC_RECORD_COMPLETE ||
                (payloadSize == 0u && sampleRate == 0u && sampleCount == 0u);
            if (!commonValid || !orderValid || !sessionValid || !timingValid ||
                !rawValid || !completeValid) {
                if (diagnostic_last_record_sequence_ == 0u) {
                    /* A screen-frame payload may already have been queued in
                       USB when the viewer requested the mode switch. Ignore
                       an accidental P2DG byte pattern until the authenticated
                       session record establishes the diagnostic stream. */
                    receive_buffer_.remove(0, 1);
                    continue;
                }
                finishDiagnosticCapture(
                    false, QStringLiteral("Received an invalid diagnostic "
                                          "record header."));
                return;
            }
            const qsizetype recordSize =
                static_cast<qsizetype>(headerSize) + payloadSize;
            if (receive_buffer_.size() < recordSize) {
                return;
            }
            const QByteArray payload = receive_buffer_.mid(
                headerSize, static_cast<qsizetype>(payloadSize));
            const quint32 actualCrc = crc32(payload);
            if (actualCrc != expectedCrc) {
                QFile rejected(QDir(diagnostic_directory_)
                                   .filePath(QStringLiteral(
                                       "rejected-record.bin")));
                if (rejected.open(QIODevice::WriteOnly)) {
                    rejected.write(receive_buffer_.constData(), recordSize);
                    rejected.close();
                }
                if (++diagnostic_record_retries_ > 3) {
                    finishDiagnosticCapture(
                        false, QStringLiteral(
                                   "CRC-32 validation failed four times for "
                                   "diagnostic record %1 (expected %2, "
                                   "received %3).")
                                   .arg(recordSequence)
                                   .arg(expectedCrc, 8, 16,
                                        QLatin1Char('0'))
                                   .arg(actualCrc, 8, 16,
                                        QLatin1Char('0')));
                    return;
                }
                receive_buffer_.remove(0, recordSize);
                diagnostic_waiting_for_guard_ = true;
                diagnostic_guard_action_ = 2;
                diagnostic_guard_sequence_ = recordSequence;
                diagnostic_watchdog_.start();
                continue;
            }
            const qint64 fileOffset = diagnostic_records_.pos();
            const QByteArray record = receive_buffer_.left(recordSize);
            receive_buffer_.remove(0, recordSize);
            if (diagnostic_records_.write(record) != record.size() ||
                !diagnostic_records_.flush()) {
                finishDiagnosticCapture(
                    false, QStringLiteral("Could not append capture.p2td."));
                return;
            }
            const QString typeName = type == P2000T_DIAGNOSTIC_RECORD_SESSION
                                         ? QStringLiteral("session")
                                     : type == P2000T_DIAGNOSTIC_RECORD_TIMING
                                         ? QStringLiteral("timing")
                                     : type == P2000T_DIAGNOSTIC_RECORD_RAW_RGBS
                                         ? QStringLiteral("raw_rgbs")
                                         : QStringLiteral("complete");
            {
                QTextStream manifest(&diagnostic_manifest_);
                manifest << recordSequence << ',' << typeName << ','
                         << fileOffset << ',' << recordSize << ','
                         << payloadSize << ',' << sessionId << ',' << sampleRate
                         << ',' << sampleCount << ',' << startLine << ','
                         << lineCount << ',' << repetition << ',' << repetitions
                         << ',' << captureSequence << ',' << captureDuration
                         << ',' << expectedDuration << ',' << flags << ','
                         << QDateTime::currentDateTimeUtc().toString(
                                Qt::ISODateWithMs)
                         << '\n';
                manifest.flush();
            }
            diagnostic_manifest_.flush();
            if (diagnostic_manifest_.error() != QFileDevice::NoError) {
                finishDiagnosticCapture(
                    false, QStringLiteral("Could not append manifest.csv."));
                return;
            }
            diagnostic_last_record_sequence_ = recordSequence;
            diagnostic_record_retries_ = 0;
            diagnostic_waiting_for_guard_ =
                type != P2000T_DIAGNOSTIC_RECORD_COMPLETE;
            if (type == P2000T_DIAGNOSTIC_RECORD_TIMING ||
                type == P2000T_DIAGNOSTIC_RECORD_RAW_RGBS) {
                diagnostic_guard_action_ = 1;
                diagnostic_guard_sequence_ = recordSequence;
            }
            diagnostic_watchdog_.start();
            if (diagnostic_session_id_ == 0u) {
                diagnostic_session_id_ = sessionId;
            }
            if (type == P2000T_DIAGNOSTIC_RECORD_TIMING) {
                diagnostic_timing_received_ = true;
                if (diagnostic_progress_ != nullptr) {
                    diagnostic_progress_->setValue(1);
                    diagnostic_progress_->setLabelText(
                        QStringLiteral("Timing trace saved; waiting for raw "
                                       "burst 1 of %1...")
                            .arg(repetitions));
                }
            } else if (type == P2000T_DIAGNOSTIC_RECORD_RAW_RGBS) {
                ++diagnostic_records_written_;
                if (diagnostic_progress_ != nullptr) {
                    diagnostic_progress_->setValue(repetition + 1);
                    diagnostic_progress_->setLabelText(
                        QStringLiteral("Saved raw burst %1 of %2")
                            .arg(repetition)
                            .arg(repetitions));
                }
            } else if (type == P2000T_DIAGNOSTIC_RECORD_COMPLETE) {
                const bool cancelled =
                    (flags & P2000T_DIAGNOSTIC_FLAG_CANCELLED) != 0u;
                finishDiagnosticCapture(cancelled);
                return;
            }
        }
    }

    void parseRecords() {
        if (diagnostic_active_) {
            parseDiagnosticRecords();
            return;
        }
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
            if (receive_buffer_.size() < P2000T_STREAM_LEGACY_HEADER_SIZE) {
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
            const bool supported_header =
                (version == P2000T_STREAM_LEGACY_PROTOCOL_VERSION &&
                 header_size == P2000T_STREAM_LEGACY_HEADER_SIZE) ||
                (version == P2000T_STREAM_PROTOCOL_VERSION &&
                 header_size == P2000T_STREAM_HEADER_SIZE);
            if (!supported_header) {
                receive_buffer_.remove(0, 1);
                continue;
            }
            if (receive_buffer_.size() < header_size) {
                return;
            }
            const quint32 payload_size = loadU32(&header[24]);
            const quint32 expected_crc = loadU32(&header[28]);
            const quint32 uncompressed_size = loadU32(&header[32]);
            const quint32 sequence = loadU32(&header[8]);
            const quint64 capture_timestamp_us =
                version >= P2000T_STREAM_PROTOCOL_VERSION &&
                        (flags & P2000T_STREAM_FLAG_CAPTURE_TIMESTAMP_US) != 0u
                    ? loadU64(
                          &header[P2000T_STREAM_CAPTURE_TIMESTAMP_US_OFFSET])
                    : 0u;
            const quint8 artwork =
                static_cast<quint8>(header[P2000T_STREAM_ARTWORK_OFFSET]);
            FrameReconstructionDiagnostics frameDiagnostics;
            frameDiagnostics.reconstruction = static_cast<quint8>(
                header[P2000T_STREAM_RECONSTRUCTION_OFFSET]);
            frameDiagnostics.captureEngine = static_cast<quint8>(
                header[P2000T_STREAM_CAPTURE_ENGINE_OFFSET]);
            frameDiagnostics.samplesPerOutput = static_cast<quint8>(
                header[P2000T_STREAM_SAMPLES_PER_OUTPUT_OFFSET]);
            frameDiagnostics.correctedSamples =
                loadU32(&header[P2000T_STREAM_CORRECTED_SAMPLES_OFFSET]);
            frameDiagnostics.ambiguousSamples =
                loadU32(&header[P2000T_STREAM_AMBIGUOUS_SAMPLES_OFFSET]);
            frameDiagnostics.redCorrections =
                loadU32(&header[P2000T_STREAM_RED_CORRECTIONS_OFFSET]);
            frameDiagnostics.greenCorrections =
                loadU32(&header[P2000T_STREAM_GREEN_CORRECTIONS_OFFSET]);
            frameDiagnostics.blueCorrections =
                loadU32(&header[P2000T_STREAM_BLUE_CORRECTIONS_OFFSET]);
            frameDiagnostics.deadlineMiss =
                (flags & P2000T_STREAM_FLAG_CAPTURE_DEADLINE_MISS) != 0u;
            const bool signal_present =
                (flags & P2000T_STREAM_FLAG_SIGNAL_PRESENT) != 0u;
            const bool configuration_record =
                encoding == P2000T_STREAM_ENCODING_CONFIGURATION &&
                (flags & P2000T_STREAM_FLAG_CONFIGURATION_STATE) != 0u;
            if (configuration_record) {
                const bool valid_configuration_header =
                    supported_header &&
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
                configuration_action_->setEnabled(!calibration_sweep_active_ &&
                                                  !diagnostic_active_ &&
                                                  !lab_experiment_active_);
                calibration_sweep_action_->setEnabled(
                    !calibration_sweep_active_ && !diagnostic_active_ &&
                    !lab_experiment_active_);
                engineering_autotune_action_->setEnabled(
                    !calibration_sweep_active_ && !diagnostic_active_ &&
                    !lab_experiment_active_);
                diagnostic_action_->setEnabled(!calibration_sweep_active_ &&
                                               !diagnostic_active_ &&
                                               !lab_experiment_active_);
                if (configuration_dialog_ != nullptr) {
                    configuration_dialog_->setConfiguration(configuration_);
                }
                handleCalibrationConfiguration(configuration_);
                handleLabConfiguration(configuration_);
                continue;
            }
            const quint16 required_flags = P2000T_STREAM_FLAG_PLANAR_RGB111 |
                                           P2000T_STREAM_FLAG_PIXELS_MSB_FIRST;
            const bool valid =
                supported_header &&
                (encoding == P2000T_STREAM_ENCODING_RAW ||
                 encoding == P2000T_STREAM_ENCODING_PACKBITS) &&
                width == P2000T_STREAM_WIDTH &&
                height == P2000T_STREAM_HEIGHT &&
                stride == P2000T_STREAM_PLANE_STRIDE &&
                header_size == P2000T_STREAM_HEADER_SIZE &&
                payload_size <= static_cast<quint32>(kMaximumPayloadSize) &&
                (flags & required_flags) == required_flags &&
                artwork < P2000T_STREAM_ARTWORK_COUNT &&
                frameDiagnostics.reconstruction >=
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW &&
                frameDiagnostics.reconstruction <
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_COUNT &&
                (frameDiagnostics.captureEngine ==
                     P2000T_CAPTURE_ENGINE_TWO_TAP ||
                 frameDiagnostics.captureEngine ==
                     P2000T_CAPTURE_ENGINE_WINDOWED) &&
                (frameDiagnostics.samplesPerOutput == 1 ||
                 frameDiagnostics.samplesPerOutput == 3) &&
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
                current_frame_diagnostics_ = frameDiagnostics;
                current_capture_timestamp_us_ = capture_timestamp_us;
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
            current_frame_diagnostics_ = frameDiagnostics;
            current_capture_timestamp_us_ = capture_timestamp_us;
            ++frame_count_;
            updateStatus(sequence, payload_size, true, artwork);
            handleCalibrationFrame(sequence);
            handleLabFrame(sequence);
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
            QStringLiteral("%1 | frame %2 | Pico %3 us | %4 FPS | %5 MB/s | "
                           "payload %6 B | CRC errors %7")
                .arg(serial_.name())
                .arg(sequence)
                .arg(current_capture_timestamp_us_)
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
    QTimer diagnostic_watchdog_;
    QTimer lab_poll_timer_;
    QTimer lab_ack_timer_;
    QElapsedTimer rate_timer_;
    QElapsedTimer lab_frame_watchdog_;
    QComboBox *ports_ = nullptr;
    QComboBox *encoding_ = nullptr;
    QPushButton *connect_ = nullptr;
    QPushButton *save_ = nullptr;
    FrameView *view_ = nullptr;
    QAction *connect_action_ = nullptr;
    QAction *configuration_action_ = nullptr;
    QAction *calibration_sweep_action_ = nullptr;
    QAction *engineering_autotune_action_ = nullptr;
    QAction *codex_lab_action_ = nullptr;
    QAction *diagnostic_action_ = nullptr;
    ConfigurationDialog *configuration_dialog_ = nullptr;
    QProgressDialog *calibration_progress_ = nullptr;
    PicoConfiguration configuration_;
    PicoConfiguration calibration_original_configuration_;
    CalibrationSweepOptions calibration_options_;
    QByteArray receive_buffer_;
    QImage current_frame_;
    QFile calibration_manifest_;
    QFile autotune_candidates_file_;
    QFile lab_manifest_;
    QFile diagnostic_records_;
    QFile diagnostic_manifest_;
    QString calibration_directory_;
    bool connected_ = false;
    bool last_signal_present_ = false;
    bool calibration_sweep_active_ = false;
    bool diagnostic_active_ = false;
    bool diagnostic_cancel_requested_ = false;
    bool calibration_waiting_for_configuration_ = false;
    bool lab_bridge_available_ = false;
    bool lab_experiment_active_ = false;
    bool lab_waiting_for_configuration_ = false;
    bool lab_save_pending_ = false;
    bool lab_factory_reset_pending_ = false;
    CalibrationMode calibration_mode_ = CalibrationMode::ManualSweep;
    AutotuneStage autotune_stage_ = AutotuneStage::RateAndPhase;
    int calibration_phase_ = 0;
    int calibration_rate_trim_ = 0;
    int calibration_odd_line_phase_ = 0;
    int calibration_settling_frames_ = 0;
    int calibration_frame_at_setting_ = 0;
    int calibration_images_written_ = 0;
    int calibration_total_images_ = 0;
    int calibration_acknowledgement_retries_ = 0;
    int autotune_winner_phase_ = 0;
    int autotune_winner_rate_trim_ = 0;
    int autotune_winner_odd_line_phase_ = 0;
    bool autotune_odd_lines_use_logical_odd_ = true;
    int lab_status_poll_count_ = 0;
    int lab_settling_frames_ = 0;
    int lab_frames_captured_ = 0;
    quint64 lab_corrected_samples_ = 0;
    quint64 lab_ambiguous_samples_ = 0;
    quint64 lab_red_corrections_ = 0;
    quint64 lab_green_corrections_ = 0;
    quint64 lab_blue_corrections_ = 0;
    int lab_deadline_miss_frames_ = 0;
    int lab_acknowledgement_retries_ = 0;
    int diagnostic_records_written_ = 0;
    quint32 diagnostic_session_id_ = 0;
    quint32 diagnostic_last_record_sequence_ = 0;
    bool diagnostic_timing_received_ = false;
    bool diagnostic_waiting_for_guard_ = false;
    int diagnostic_guard_action_ = 0;
    int diagnostic_record_retries_ = 0;
    quint32 diagnostic_guard_sequence_ = 0;
    DiagnosticCaptureOptions diagnostic_options_;
    EngineeringAutotuneOptions autotune_options_;
    CodexLabBridge lab_bridge_;
    CodexLabRequest lab_request_;
    PicoConfiguration lab_original_configuration_;
    PicoConfiguration lab_target_configuration_;
    FrameReconstructionDiagnostics current_frame_diagnostics_;
    quint64 current_capture_timestamp_us_ = 0;
    std::unique_ptr<EngineeringCandidateAccumulator> autotune_accumulator_;
    std::unique_ptr<EngineeringCandidateAccumulator> lab_accumulator_;
    std::vector<EngineeringCandidateResult> autotune_results_;
    QString diagnostic_directory_;
    QString lab_run_directory_;
    QString lab_bridge_error_;
    QString lab_save_request_id_;
    QString diagnostic_lab_request_id_;
    QProgressDialog *diagnostic_progress_ = nullptr;
    quint64 frame_count_ = 0;
    quint64 byte_count_ = 0;
    quint64 crc_errors_ = 0;
};

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
#if defined(P2000T_LAB_AGENT_ONLY)
    const bool headless = true;
#else
    const bool headless = QCoreApplication::arguments().contains(
        QStringLiteral("--lab-headless"));
#endif
    QCoreApplication::setApplicationName(
        headless ? QStringLiteral("P2000T Lab Agent")
                 : QStringLiteral("P2000T Capture"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(P2000T_VIEWER_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("Ivo Filot"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("ivofilot.nl"));
    application.setWindowIcon(
        QIcon(QStringLiteral(":/icons/p2000t-capture.png")));
    application.setQuitOnLastWindowClosed(!headless);
    CaptureWindow window;
    if (!headless) {
        window.show();
    }
    return application.exec();
}
