/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_CODEX_LAB_BRIDGE_H
#define P2000T_CODEX_LAB_BRIDGE_H

#include <optional>

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

enum class CodexLabCommand {
    Status,
    Experiment,
    Diagnostic,
    Save,
    Cancel,
    Shutdown
};

struct CodexLabRequest {
    QString id;
    CodexLabCommand command = CodexLabCommand::Status;
    QString tag;
    QString referenceRun;
    std::optional<int> phase;
    std::optional<int> oddLinePhase;
    std::optional<int> rateTrim;
    std::optional<int> reconstruction;
    int settlingFrames = 2;
    int captureFrames = 10;
    int diagnosticStartLine = 1;
    int diagnosticLineCount = 16;
    int diagnosticRepetitions = 100;
};

struct CodexLabEnvelope {
    QString filename;
    QByteArray payload;
};

/** Shared-directory request/response transport between WSL Codex and Qt. */
class CodexLabBridge {
  public:
    bool initialize(const QString &rootDirectory, QString *error = nullptr);
    QString rootDirectory() const;
    QList<CodexLabEnvelope> takeRequests();
    bool writeResponse(const QString &id, const QJsonObject &response) const;
    bool writeStatus(const QJsonObject &status) const;

    static bool decodeRequest(const QByteArray &payload,
                              CodexLabRequest *request,
                              QString *error = nullptr);

  private:
    bool writeJsonAtomically(const QString &path,
                             const QJsonObject &object) const;

    QString root_directory_;
};

#endif
