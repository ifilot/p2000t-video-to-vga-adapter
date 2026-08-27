/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_ADAPTER_SERIAL_PORT_H
#define P2000T_ADAPTER_SERIAL_PORT_H

/**
 * @file adapter_serial_port.h
 * @brief Cross-platform discovery and byte-stream access for Pico USB CDC.
 */

#include <memory>

#include <QByteArray>
#include <QString>
#include <QStringList>

/**
 * Enumerate likely Raspberry Pi Pico USB CDC ports on the host platform.
 *
 * Every candidate is subsequently verified through the firmware console, so
 * a broad platform device-name match cannot connect to an unrelated adapter.
 */
QStringList picoSerialPorts();

/** Synchronous wrapper around native Windows or POSIX serial-port APIs. */
class AdapterSerialPort final {
public:
    AdapterSerialPort();
    ~AdapterSerialPort();

    AdapterSerialPort(const AdapterSerialPort &) = delete;
    AdapterSerialPort &operator=(const AdapterSerialPort &) = delete;

    /** Open and configure a USB CDC serial port for binary traffic. */
    bool open(const QString &portName);
    /** Close the native port and clear its user-facing name. */
    void close();
    /** Return whether the wrapper currently owns an open native port. */
    bool isOpen() const;
    /** Return the platform-native port name associated with the handle. */
    QString name() const;
    /** Write an entire command, retrying until every byte is accepted. */
    bool write(const QByteArray &data);

    /**
     * Drain currently queued input without waiting for future data.
     *
     * @param ok Receives false when a native communication call fails.
     * @return All bytes available during this bounded drain operation.
     */
    QByteArray readAvailable(bool *ok);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

#endif
