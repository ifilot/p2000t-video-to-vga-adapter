/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "codex_lab_bridge.h"

#include <cmath>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>

#include "p2000t_control_protocol.h"
#include "p2000t_diagnostic_protocol.h"

namespace {

constexpr int kProtocolVersion = 1;

bool decodeInteger(const QJsonObject &object, const QString &name, int minimum,
                   int maximum, std::optional<int> *destination,
                   QString *error) {
    if (!object.contains(name)) {
        return true;
    }
    const QJsonValue value = object.value(name);
    if (!value.isDouble() || std::floor(value.toDouble()) != value.toDouble() ||
        value.toDouble() < minimum || value.toDouble() > maximum) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 must be an integer from %2 to %3")
                         .arg(name)
                         .arg(minimum)
                         .arg(maximum);
        }
        return false;
    }
    *destination = value.toInt();
    return true;
}

bool decodeRequiredInteger(const QJsonObject &object, const QString &name,
                           int minimum, int maximum, int defaultValue,
                           int *destination, QString *error) {
    std::optional<int> value;
    if (!decodeInteger(object, name, minimum, maximum, &value, error)) {
        return false;
    }
    *destination = value.value_or(defaultValue);
    return true;
}

} // namespace

bool CodexLabBridge::initialize(const QString &rootDirectory, QString *error) {
    QDir root(rootDirectory);
    if ((!root.exists() && !QDir().mkpath(rootDirectory)) ||
        !root.mkpath(QStringLiteral("requests")) ||
        !root.mkpath(QStringLiteral("responses")) ||
        !root.mkpath(QStringLiteral("archive")) ||
        !root.mkpath(QStringLiteral("runs")) ||
        !root.mkpath(QStringLiteral("diagnostics"))) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not create the bridge directory %1")
                         .arg(QDir::toNativeSeparators(rootDirectory));
        }
        return false;
    }
    root_directory_ = QDir(rootDirectory).absolutePath();
    return true;
}

QString CodexLabBridge::rootDirectory() const {
    return root_directory_;
}

QList<CodexLabEnvelope> CodexLabBridge::takeRequests() {
    QList<CodexLabEnvelope> result;
    if (root_directory_.isEmpty()) {
        return result;
    }
    QDir requests(QDir(root_directory_).filePath(QStringLiteral("requests")));
    const QFileInfoList files = requests.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    QDir archive(QDir(root_directory_).filePath(QStringLiteral("archive")));
    for (const QFileInfo &info : files) {
        QFile input(info.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly)) {
            continue;
        }
        CodexLabEnvelope envelope{info.fileName(), input.readAll()};
        input.close();
        QString archiveName = info.fileName();
        for (int suffix = 2; archive.exists(archiveName); ++suffix) {
            archiveName = QStringLiteral("%1-%2.json")
                              .arg(info.completeBaseName())
                              .arg(suffix);
        }
        if (QFile::rename(info.absoluteFilePath(),
                          archive.filePath(archiveName))) {
            result.push_back(std::move(envelope));
        }
    }
    return result;
}

bool CodexLabBridge::writeJsonAtomically(const QString &path,
                                         const QJsonObject &object) const {
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray data = QJsonDocument(object).toJson();
    return output.write(data) == data.size() && output.commit();
}

bool CodexLabBridge::writeResponse(const QString &id,
                                   const QJsonObject &response) const {
    if (root_directory_.isEmpty()) {
        return false;
    }
    return writeJsonAtomically(
        QDir(root_directory_)
            .filePath(QStringLiteral("responses/%1.json").arg(id)),
        response);
}

bool CodexLabBridge::writeStatus(const QJsonObject &status) const {
    if (root_directory_.isEmpty()) {
        return false;
    }
    return writeJsonAtomically(
        QDir(root_directory_).filePath(QStringLiteral("bridge-status.json")),
        status);
}

bool CodexLabBridge::decodeRequest(const QByteArray &payload,
                                   CodexLabRequest *request, QString *error) {
    if (request == nullptr) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid JSON: %1")
                         .arg(parseError.errorString());
        }
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("protocol")).toInt(-1) !=
        kProtocolVersion) {
        if (error != nullptr) {
            *error = QStringLiteral("protocol must be 1");
        }
        return false;
    }
    const QString id = object.value(QStringLiteral("id")).toString();
    static const QRegularExpression validId(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,79}$"));
    if (!validId.match(id).hasMatch()) {
        if (error != nullptr) {
            *error =
                QStringLiteral("id must contain 1-80 safe filename characters");
        }
        return false;
    }
    const QString command = object.value(QStringLiteral("command")).toString();
    CodexLabRequest result;
    result.id = id;
    result.tag = object.value(QStringLiteral("tag")).toString().left(160);
    const QString referenceRun =
        object.value(QStringLiteral("reference_run")).toString();
    if (!referenceRun.isEmpty() && !validId.match(referenceRun).hasMatch()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "reference_run must be a safe existing run identifier");
        }
        return false;
    }
    result.referenceRun = referenceRun;
    if (command == QStringLiteral("status")) {
        result.command = CodexLabCommand::Status;
    } else if (command == QStringLiteral("save")) {
        result.command = CodexLabCommand::Save;
    } else if (command == QStringLiteral("cancel")) {
        result.command = CodexLabCommand::Cancel;
    } else if (command == QStringLiteral("shutdown")) {
        result.command = CodexLabCommand::Shutdown;
    } else if (command == QStringLiteral("diagnostic")) {
        result.command = CodexLabCommand::Diagnostic;
        if (!decodeRequiredInteger(
                object, QStringLiteral("start_line"),
                P2000T_DIAGNOSTIC_MIN_START_LINE,
                P2000T_DIAGNOSTIC_MAX_START_LINE,
                P2000T_CONTROL_DEFAULT_VERTICAL,
                &result.diagnosticStartLine, error) ||
            !decodeRequiredInteger(object, QStringLiteral("line_count"), 1,
                                   P2000T_DIAGNOSTIC_MAX_LINES,
                                   P2000T_DIAGNOSTIC_MAX_LINES,
                                   &result.diagnosticLineCount, error) ||
            !decodeRequiredInteger(object, QStringLiteral("repetitions"), 1,
                                   P2000T_DIAGNOSTIC_MAX_REPETITIONS, 100,
                                   &result.diagnosticRepetitions, error)) {
            return false;
        }
    } else if (command == QStringLiteral("experiment")) {
        result.command = CodexLabCommand::Experiment;
        const QJsonValue settingsValue =
            object.value(QStringLiteral("settings"));
        if (!settingsValue.isUndefined() && !settingsValue.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("settings must be an object");
            }
            return false;
        }
        const QJsonObject settings = settingsValue.toObject();
        if (!decodeInteger(settings, QStringLiteral("phase"),
                           P2000T_CONTROL_MIN_PHASE, P2000T_CONTROL_MAX_PHASE,
                           &result.phase, error) ||
            !decodeInteger(settings, QStringLiteral("odd_line_phase"),
                           P2000T_CONTROL_MIN_ODD_LINE_PHASE,
                           P2000T_CONTROL_MAX_ODD_LINE_PHASE,
                           &result.oddLinePhase, error) ||
            !decodeInteger(settings, QStringLiteral("rate_trim"),
                           P2000T_CONTROL_MIN_SAMPLE_RATE_TRIM,
                           P2000T_CONTROL_MAX_SAMPLE_RATE_TRIM,
                           &result.rateTrim, error)) {
            return false;
        }
        if (settings.contains(QStringLiteral("reconstruction"))) {
            const QString reconstruction =
                settings.value(QStringLiteral("reconstruction")).toString();
            if (reconstruction == QStringLiteral("raw")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_RAW;
            } else if (reconstruction == QStringLiteral("guarded")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SECOND_TAP;
            } else if (reconstruction == QStringLiteral("sharp")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SHARP_GUARDED;
            } else if (reconstruction == QStringLiteral("window-center")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CENTER;
            } else if (reconstruction == QStringLiteral("window-channel")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CHANNEL_MAJORITY;
            } else if (reconstruction == QStringLiteral("window-early")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_EARLY;
            } else if (reconstruction == QStringLiteral("window-late")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_LATE;
            } else if (reconstruction == QStringLiteral("window-confidence")) {
                result.reconstruction =
                    P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CONFIDENCE_GUARD;
            } else {
                if (error != nullptr) {
                    *error = QStringLiteral(
                        "reconstruction must be raw, guarded, sharp, "
                        "window-center, window-channel, window-early, "
                        "window-late, or window-confidence");
                }
                return false;
            }
        }
        if (!decodeRequiredInteger(object, QStringLiteral("settle_frames"), 0,
                                   100, 2, &result.settlingFrames, error) ||
            !decodeRequiredInteger(object, QStringLiteral("capture_frames"), 0,
                                   500, 10, &result.captureFrames, error)) {
            return false;
        }
    } else {
        if (error != nullptr) {
            *error = QStringLiteral(
                "command must be status, experiment, diagnostic, save, "
                "cancel, or shutdown");
        }
        return false;
    }
    *request = std::move(result);
    return true;
}
