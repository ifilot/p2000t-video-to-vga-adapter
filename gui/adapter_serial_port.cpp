/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file adapter_serial_port.cpp
 * @brief Native Windows and POSIX implementations of Pico USB CDC access.
 */

#include "adapter_serial_port.h"

#include <algorithm>
#include <array>
#include <cerrno>

#include <QDir>

#ifdef Q_OS_WIN

#include <cwchar>
#include <vector>

#include <windows.h>
#include <devguid.h>
#include <setupapi.h>

namespace {

/** Raspberry Pi USB vendor identifier used to shortlist Pico CDC devices. */
constexpr quint16 kPicoVendorId = 0x2e8a;

}  // namespace

QStringList picoSerialPorts() {
    QStringList ports;
    HDEVINFO deviceInfo = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (deviceInfo == INVALID_HANDLE_VALUE) {
        return ports;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device = {};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(deviceInfo, index, &device)) {
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceRegistryPropertyW(
            deviceInfo, &device, SPDRP_HARDWAREID, nullptr, nullptr, 0,
            &required);
        if (required == 0) {
            continue;
        }

        std::vector<BYTE> storage(required + sizeof(wchar_t), 0);
        if (!SetupDiGetDeviceRegistryPropertyW(
                deviceInfo, &device, SPDRP_HARDWAREID, nullptr,
                storage.data(), required, nullptr)) {
            continue;
        }

        bool isPico = false;
        const auto *hardwareId =
            reinterpret_cast<const wchar_t *>(storage.data());
        while (*hardwareId != L'\0') {
            const QString id = QString::fromWCharArray(hardwareId).toUpper();
            if (id.contains(QStringLiteral("VID_%1").arg(
                    kPicoVendorId, 4, 16, QLatin1Char('0')).toUpper())) {
                isPico = true;
                break;
            }
            hardwareId += std::wcslen(hardwareId) + 1;
        }
        if (!isPico) {
            continue;
        }

        HKEY deviceKey = SetupDiOpenDevRegKey(
            deviceInfo, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (deviceKey == INVALID_HANDLE_VALUE) {
            continue;
        }

        std::array<wchar_t, 64> portName = {};
        DWORD type = 0;
        DWORD bytes = static_cast<DWORD>(portName.size() * sizeof(wchar_t));
        const LSTATUS result = RegQueryValueExW(
            deviceKey, L"PortName", nullptr, &type,
            reinterpret_cast<LPBYTE>(portName.data()), &bytes);
        RegCloseKey(deviceKey);
        if (result == ERROR_SUCCESS && type == REG_SZ) {
            const QString name = QString::fromWCharArray(portName.data());
            if (name.startsWith(QStringLiteral("COM"),
                                Qt::CaseInsensitive)) {
                ports.append(name);
            }
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);
    ports.removeDuplicates();
    ports.sort(Qt::CaseInsensitive);
    return ports;
}

struct AdapterSerialPort::Implementation {
    HANDLE handle = INVALID_HANDLE_VALUE;
    QString name;
};

AdapterSerialPort::AdapterSerialPort()
    : implementation_(std::make_unique<Implementation>()) {}

AdapterSerialPort::~AdapterSerialPort() {
    close();
}

bool AdapterSerialPort::open(const QString &portName) {
    close();
    const QString path = QStringLiteral("\\\\.\\") + portName;
    implementation_->handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (implementation_->handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    SetupComm(implementation_->handle, 1024 * 1024, 4096);
    DCB state = {};
    state.DCBlength = sizeof(state);
    if (!GetCommState(implementation_->handle, &state)) {
        close();
        return false;
    }
    state.BaudRate = CBR_115200;
    state.ByteSize = 8;
    state.Parity = NOPARITY;
    state.StopBits = ONESTOPBIT;
    state.fBinary = TRUE;
    state.fParity = FALSE;
    state.fOutxCtsFlow = FALSE;
    state.fOutxDsrFlow = FALSE;
    state.fDtrControl = DTR_CONTROL_ENABLE;
    state.fOutX = FALSE;
    state.fInX = FALSE;
    state.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(implementation_->handle, &state)) {
        close();
        return false;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.WriteTotalTimeoutConstant = 500;
    if (!SetCommTimeouts(implementation_->handle, &timeouts)) {
        close();
        return false;
    }
    PurgeComm(implementation_->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    EscapeCommFunction(implementation_->handle, SETDTR);
    implementation_->name = portName;
    return true;
}

void AdapterSerialPort::close() {
    if (implementation_->handle != INVALID_HANDLE_VALUE) {
        EscapeCommFunction(implementation_->handle, CLRDTR);
        CloseHandle(implementation_->handle);
        implementation_->handle = INVALID_HANDLE_VALUE;
    }
    implementation_->name.clear();
}

bool AdapterSerialPort::isOpen() const {
    return implementation_->handle != INVALID_HANDLE_VALUE;
}

QString AdapterSerialPort::name() const {
    return implementation_->name;
}

bool AdapterSerialPort::write(const QByteArray &data) {
    if (!isOpen()) {
        return false;
    }
    qsizetype offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        if (!WriteFile(implementation_->handle, data.constData() + offset,
                       static_cast<DWORD>(data.size() - offset), &written,
                       nullptr) || written == 0) {
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }
    return true;
}

QByteArray AdapterSerialPort::readAvailable(bool *ok) {
    QByteArray result;
    *ok = isOpen();
    if (!*ok) {
        return result;
    }

    std::array<char, 16384> buffer = {};
    for (unsigned pass = 0; pass < 16; ++pass) {
        DWORD errors = 0;
        COMSTAT status = {};
        if (!ClearCommError(implementation_->handle, &errors, &status)) {
            *ok = false;
            break;
        }
        if (status.cbInQue == 0) {
            break;
        }
        const DWORD requested = std::min<DWORD>(
            status.cbInQue, static_cast<DWORD>(buffer.size()));
        DWORD received = 0;
        if (!ReadFile(implementation_->handle, buffer.data(), requested,
                      &received, nullptr)) {
            *ok = false;
            break;
        }
        if (received == 0) {
            break;
        }
        result.append(buffer.data(), static_cast<qsizetype>(received));
    }
    return result;
}

#else

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

QStringList picoSerialPorts() {
    QDir devices(QStringLiteral("/dev"));
#ifdef Q_OS_MACOS
    const QStringList patterns = {QStringLiteral("cu.usbmodem*")};
#else
    const QStringList patterns = {QStringLiteral("ttyACM*")};
#endif
    const QStringList entries = devices.entryList(
        patterns, QDir::System | QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase);
    QStringList ports;
    ports.reserve(entries.size());
    for (const QString &entry : entries) {
        ports.append(devices.absoluteFilePath(entry));
    }
    return ports;
}

struct AdapterSerialPort::Implementation {
    int descriptor = -1;
    QString name;
};

AdapterSerialPort::AdapterSerialPort()
    : implementation_(std::make_unique<Implementation>()) {}

AdapterSerialPort::~AdapterSerialPort() {
    close();
}

bool AdapterSerialPort::open(const QString &portName) {
    close();
    const QByteArray nativeName = portName.toLocal8Bit();
    implementation_->descriptor = ::open(
        nativeName.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (implementation_->descriptor < 0) {
        return false;
    }

    termios state = {};
    if (tcgetattr(implementation_->descriptor, &state) != 0) {
        close();
        return false;
    }
    cfmakeraw(&state);
    cfsetispeed(&state, B115200);
    cfsetospeed(&state, B115200);
    state.c_cflag |= CLOCAL | CREAD;
    state.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
#ifdef CRTSCTS
    state.c_cflag &= ~CRTSCTS;
#endif
    state.c_cflag |= CS8;
    state.c_cc[VMIN] = 0;
    state.c_cc[VTIME] = 0;
    if (tcsetattr(implementation_->descriptor, TCSANOW, &state) != 0) {
        close();
        return false;
    }
    tcflush(implementation_->descriptor, TCIOFLUSH);
#ifdef TIOCMBIS
    int modemBits = TIOCM_DTR | TIOCM_RTS;
    ioctl(implementation_->descriptor, TIOCMBIS, &modemBits);
#endif
    implementation_->name = portName;
    return true;
}

void AdapterSerialPort::close() {
    if (implementation_->descriptor >= 0) {
#ifdef TIOCMBIC
        int modemBits = TIOCM_DTR | TIOCM_RTS;
        ioctl(implementation_->descriptor, TIOCMBIC, &modemBits);
#endif
        ::close(implementation_->descriptor);
        implementation_->descriptor = -1;
    }
    implementation_->name.clear();
}

bool AdapterSerialPort::isOpen() const {
    return implementation_->descriptor >= 0;
}

QString AdapterSerialPort::name() const {
    return implementation_->name;
}

bool AdapterSerialPort::write(const QByteArray &data) {
    if (!isOpen()) {
        return false;
    }
    qsizetype offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(
            implementation_->descriptor, data.constData() + offset,
            static_cast<size_t>(data.size() - offset));
        if (written > 0) {
            offset += static_cast<qsizetype>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd writable = {implementation_->descriptor, POLLOUT, 0};
            if (::poll(&writable, 1, 500) > 0) {
                continue;
            }
        }
        return false;
    }
    return true;
}

QByteArray AdapterSerialPort::readAvailable(bool *ok) {
    QByteArray result;
    *ok = isOpen();
    if (!*ok) {
        return result;
    }

    std::array<char, 16384> buffer = {};
    for (unsigned pass = 0; pass < 16; ++pass) {
        const ssize_t received = ::read(
            implementation_->descriptor, buffer.data(), buffer.size());
        if (received > 0) {
            result.append(buffer.data(), static_cast<qsizetype>(received));
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        *ok = false;
        break;
    }
    return result;
}

#endif
