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

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QByteArray>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QLabel>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "adapter_serial_port.h"
#include "no_signal_screen.h"
#include "p2000t_packbits.h"
#include "p2000t_stream_protocol.h"

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
            remainder = (remainder >> 1u) ^
                        ((remainder & 1u) != 0u ? 0xedb88320u : 0u);
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

    QSize sizeHint() const override { return QSize(720, 720); }

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
        const QRect destination(
            QPoint((width() - target.width()) / 2,
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
        connect(&poll_timer_, &QTimer::timeout, this,
                [this] { pollSerial(); });
        connect(refresh, &QPushButton::clicked, this,
                [this] { refreshPorts(); });
        connect(connect_, &QPushButton::clicked, this,
                [this] { toggleConnection(); });
        connect(save_, &QPushButton::clicked, this,
                [this] { saveFrame(); });

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

    ~CaptureWindow() override { disconnectAdapter(); }

private:
    void createMenus() {
        auto *exit = new QAction(
            QIcon(QStringLiteral(":/icons/retro/exit.png")),
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
        connect(refresh, &QAction::triggered, this,
                [this] { refreshPorts(); });
        adapter->addSeparator();

        configuration_menu_ = adapter->addMenu(
            QIcon(QStringLiteral(":/icons/retro/configure.png")),
            QStringLiteral("&Configure Pico 2"));
        auto addCommand = [this](QMenu *menu, const QString &text,
                                 char command, const QString &description) {
            QAction *action = menu->addAction(text);
            connect(action, &QAction::triggered, this,
                    [this, command, description] {
                        sendConfigurationCommand(command, description);
                    });
            return action;
        };

        auto *vertical = configuration_menu_->addMenu(
            QStringLiteral("&Vertical position"));
        addCommand(vertical, QStringLiteral("Move &up"), '[',
                   QStringLiteral("Vertical image position moved up"));
        addCommand(vertical, QStringLiteral("Move &down"), ']',
                   QStringLiteral("Vertical image position moved down"));
        vertical->addSeparator();
        addCommand(vertical, QStringLiteral("&Reset"), '0',
                   QStringLiteral("Vertical image position reset"));

        auto *phase = configuration_menu_->addMenu(
            QStringLiteral("&Sample phase"));
        addCommand(phase, QStringLiteral("Sample &earlier"), ',',
                   QStringLiteral("Sample phase moved earlier"));
        addCommand(phase, QStringLiteral("Sample &later"), '.',
                   QStringLiteral("Sample phase moved later"));
        phase->addSeparator();
        addCommand(phase, QStringLiteral("&Reset"), 'p',
                   QStringLiteral("Sample phase reset"));

        auto *horizontal = configuration_menu_->addMenu(
            QStringLiteral("&Horizontal start"));
        addCommand(horizontal, QStringLiteral("Start &earlier"), '<',
                   QStringLiteral("Horizontal capture starts earlier"));
        addCommand(horizontal, QStringLiteral("Start &later"), '>',
                   QStringLiteral("Horizontal capture starts later"));
        horizontal->addSeparator();
        addCommand(horizontal, QStringLiteral("&Reset"), 'x',
                   QStringLiteral("Horizontal capture start reset"));

        auto *artwork = configuration_menu_->addMenu(
            QStringLiteral("&No-connection artwork"));
        auto *artworkGroup = new QActionGroup(this);
        artworkGroup->setExclusive(true);
        const std::array<QString, P2000T_STREAM_ARTWORK_COUNT> names = {
            QStringLiteral("&Green phosphor"),
            QStringLiteral("&Synthwave"),
            QStringLiteral("&Amber circuit")};
        for (int index = 0; index < P2000T_STREAM_ARTWORK_COUNT; ++index) {
            QAction *action = artwork->addAction(names[index]);
            action->setCheckable(true);
            artworkGroup->addAction(action);
            artwork_actions_[static_cast<size_t>(index)] = action;
            connect(action, &QAction::triggered, this, [this, index] {
                sendConfigurationCommand(
                    static_cast<char>('1' + index),
                    QStringLiteral("No-connection artwork: %1")
                        .arg(p2000tArtworkName(static_cast<quint8>(index))));
            });
        }
        artwork_actions_[P2000T_STREAM_ARTWORK_AMBER_CIRCUIT]->setChecked(true);
        configuration_menu_->setEnabled(false);

        auto *help = menuBar()->addMenu(QStringLiteral("&Help"));
        auto *about = help->addAction(
            QIcon(QStringLiteral(":/icons/retro/about.png")),
            QStringLiteral("&About"));
        connect(about, &QAction::triggered, this, [this] {
            QMessageBox::about(
                this, QStringLiteral("About P2000T VID2VGA Capture"),
                QStringLiteral(
                    "<h3>P2000T VID2VGA Capture %1</h3>"
                    "<p>Live USB capture viewer and configuration utility "
                    "for the Raspberry Pi Pico 2 P2000T video-to-VGA "
                    "adapter.</p><p>Copyright &copy; 2026 Ivo Filot<br>"
                    "Licensed under GPL-3.0-or-later.</p>")
                    .arg(QStringLiteral(P2000T_VIEWER_VERSION)));
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
        receive_buffer_.clear();
        frame_count_ = 0;
        crc_errors_ = 0;
        byte_count_ = 0;
        rate_timer_.restart();
        ports_->setEnabled(false);
        encoding_->setEnabled(false);
        connect_->setText(QStringLiteral("Disconnect"));
        connect_action_->setText(QStringLiteral("&Disconnect"));
        configuration_menu_->setEnabled(true);
        poll_timer_.start();
        statusBar()->showMessage(QStringLiteral("Initializing %1...").arg(port));

        // Opening CDC asserts DTR. Allow the firmware's connection banner to
        // finish before selecting its one-character binary screen command.
        QTimer::singleShot(350, this, [this] {
            if (!connected_) {
                return;
            }
            const char command = static_cast<char>(encoding_->currentData().toInt());
            if (!serial_.write(QByteArray(1, command))) {
                connectionLost();
            }
        });
        return true;
    }

    void disconnectAdapter() {
        if (serial_.isOpen()) {
            serial_.write(QByteArray(1, 'q'));
        }
        poll_timer_.stop();
        serial_.close();
        connected_ = false;
        ports_->setEnabled(true);
        encoding_->setEnabled(true);
        connect_->setText(QStringLiteral("Connect"));
        connect_->setEnabled(ports_->count() != 0);
        connect_action_->setText(QStringLiteral("&Connect"));
        connect_action_->setEnabled(ports_->count() != 0);
        configuration_menu_->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Disconnected"));
    }

    void sendConfigurationCommand(char command, const QString &description) {
        if (!connected_) {
            return;
        }
        if (!serial_.write(QByteArray(1, command))) {
            connectionLost();
            return;
        }
        statusBar()->showMessage(description, 2000);
    }

    void connectionLost() {
        disconnectAdapter();
        QMessageBox::warning(this, QStringLiteral("Connection lost"),
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
            const quint8 artwork = static_cast<quint8>(
                header[P2000T_STREAM_ARTWORK_OFFSET]);
            const bool signal_present =
                (flags & P2000T_STREAM_FLAG_SIGNAL_PRESENT) != 0u;
            const quint16 required_flags =
                P2000T_STREAM_FLAG_PLANAR_RGB111 |
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
        }
    }

    void presentFrame(const QByteArray &frame) {
        QImage image(P2000T_STREAM_WIDTH, P2000T_STREAM_HEIGHT * 2,
                     QImage::Format_RGB32);
        const auto *planes =
            reinterpret_cast<const unsigned char *>(frame.constData());
        for (int y = 0; y < P2000T_STREAM_HEIGHT; ++y) {
            auto *first =
                reinterpret_cast<QRgb *>(image.scanLine(y * 2));
            auto *second =
                reinterpret_cast<QRgb *>(image.scanLine(y * 2 + 1));
            const int row = y * P2000T_STREAM_PLANE_STRIDE;
            for (int x = 0; x < P2000T_STREAM_WIDTH; ++x) {
                const int index = row + x / 8;
                const unsigned mask = 0x80u >> (x & 7);
                const int red = (planes[index] & mask) != 0u ? 255 : 0;
                const int green =
                    (planes[P2000T_STREAM_PLANE_SIZE + index] & mask) != 0u
                        ? 255
                        : 0;
                const int blue =
                    (planes[2 * P2000T_STREAM_PLANE_SIZE + index] & mask) != 0u
                        ? 255
                        : 0;
                first[x] = qRgb(red, green, blue);
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
        if (artwork < artwork_actions_.size()) {
            artwork_actions_[artwork]->setChecked(true);
        }
        if (!signal_present) {
            statusBar()->showMessage(
                QStringLiteral("%1 | no P2000T signal | VGA screen: %2")
                    .arg(serial_.name(), p2000tArtworkName(artwork)));
            return;
        }
        const qint64 elapsed = rate_timer_.elapsed();
        const double seconds = elapsed > 0 ? elapsed / 1000.0 : 0.0;
        const double fps = seconds > 0.0 ? frame_count_ / seconds : 0.0;
        const double megabytes = seconds > 0.0
                                     ? byte_count_ / seconds / 1000000.0
                                     : 0.0;
        statusBar()->showMessage(
            QStringLiteral("%1 | frame %2 | %3 FPS | %4 MB/s | payload %5 B | CRC errors %6")
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
            QMessageBox::critical(this, QStringLiteral("Save failed"),
                                  QStringLiteral("Could not write %1.").arg(filename));
        }
    }

    AdapterSerialPort serial_;
    QTimer poll_timer_;
    QElapsedTimer rate_timer_;
    QComboBox *ports_ = nullptr;
    QComboBox *encoding_ = nullptr;
    QPushButton *connect_ = nullptr;
    QPushButton *save_ = nullptr;
    FrameView *view_ = nullptr;
    QAction *connect_action_ = nullptr;
    QMenu *configuration_menu_ = nullptr;
    std::array<QAction *, P2000T_STREAM_ARTWORK_COUNT> artwork_actions_ = {};
    QByteArray receive_buffer_;
    QImage current_frame_;
    bool connected_ = false;
    quint64 frame_count_ = 0;
    quint64 byte_count_ = 0;
    quint64 crc_errors_ = 0;
};

}  // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("P2000T Capture"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(P2000T_VIEWER_VERSION));
    application.setWindowIcon(
        QIcon(QStringLiteral(":/icons/p2000t-capture.png")));
    CaptureWindow window;
    window.show();
    return application.exec();
}
