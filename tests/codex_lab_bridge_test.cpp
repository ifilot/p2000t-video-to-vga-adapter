/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdlib>
#include <iostream>

#include "codex_lab_bridge.h"
#include "p2000t_control_protocol.h"

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    static_assert(P2000T_CONTROL_PICO2_DEFAULT_PHASE == 0);
    static_assert(P2000T_CONTROL_PICO2_DEFAULT_ODD_LINE_PHASE == 1);
    static_assert(
        P2000T_CONTROL_PICO2_DEFAULT_SAMPLE_RECONSTRUCTION ==
        P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_LATE);

    CodexLabRequest request;
    QString error;
    require(
        CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"phase-test-01","command":"experiment","tag":"edge study","settings":{"phase":-2,"odd_line_phase":3,"rate_trim":1,"reconstruction":"raw"},"settle_frames":4,"capture_frames":25})",
            &request, &error),
        "valid experiment request was rejected");
    require(request.command == CodexLabCommand::Experiment &&
                request.phase == -2 && request.oddLinePhase == 3 &&
                request.rateTrim == 1 && request.reconstruction == 0 &&
                request.settlingFrames == 4 && request.captureFrames == 25,
            "experiment fields were decoded incorrectly");

    require(CodexLabBridge::decodeRequest(
                R"({"protocol":1,"id":"status.1","command":"status"})",
                &request, &error) &&
                request.command == CodexLabCommand::Status,
            "status request was rejected");
    require(CodexLabBridge::decodeRequest(
                R"({"protocol":1,"id":"save.1","command":"save"})", &request,
                &error) &&
                request.command == CodexLabCommand::Save,
            "save request was rejected");
    require(CodexLabBridge::decodeRequest(
                R"({"protocol":1,"id":"reset.1","command":"factory-reset"})",
                &request, &error) &&
                request.command == CodexLabCommand::FactoryReset,
            "factory-reset request was rejected");
    require(CodexLabBridge::decodeRequest(
                R"({"protocol":1,"id":"shutdown-1","command":"shutdown"})",
                &request, &error) &&
                request.command == CodexLabCommand::Shutdown,
            "shutdown request was rejected");
    require(!CodexLabBridge::decodeRequest(
                R"({"protocol":1,"id":"bad/path","command":"status"})",
                &request, &error),
            "unsafe request id was accepted");
    require(
        !CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"bad-phase","command":"experiment","settings":{"phase":99}})",
            &request, &error),
        "out-of-range phase was accepted");
    require(
        !CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"fraction","command":"experiment","capture_frames":1.5})",
            &request, &error),
        "fractional frame count was accepted");
    require(
        CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"window","command":"experiment","settings":{"reconstruction":"window-early"},"capture_frames":2})",
            &request, &error) &&
            request.reconstruction ==
                P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_COLOR_EARLY,
        "window reconstruction mode was rejected");
    require(
        CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"sharp","command":"experiment","settings":{"reconstruction":"sharp"}})",
            &request, &error) &&
            request.reconstruction ==
                P2000T_CONTROL_SAMPLE_RECONSTRUCTION_SHARP_GUARDED,
        "sharp reconstruction mode was rejected");
    require(
        CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"confidence","command":"experiment","reference_run":"baseline-01","settings":{"reconstruction":"window-confidence"}})",
            &request, &error) &&
            request.reconstruction ==
                P2000T_CONTROL_SAMPLE_RECONSTRUCTION_WINDOW_CONFIDENCE_GUARD &&
            request.referenceRun == QStringLiteral("baseline-01"),
        "confidence reconstruction mode or reference was rejected");
    require(
        CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"raw-trace","command":"diagnostic","start_line":280,"line_count":16,"repetitions":100})",
            &request, &error) &&
            request.command == CodexLabCommand::Diagnostic &&
            request.diagnosticStartLine == 280 &&
            request.diagnosticLineCount == 16 &&
            request.diagnosticRepetitions == 100,
        "diagnostic request was rejected or decoded incorrectly");
    require(
        !CodexLabBridge::decodeRequest(
            R"({"protocol":1,"id":"bad-trace","command":"diagnostic","start_line":297,"line_count":16,"repetitions":100})",
            &request, &error),
        "out-of-range diagnostic start line was accepted");
    return EXIT_SUCCESS;
}
