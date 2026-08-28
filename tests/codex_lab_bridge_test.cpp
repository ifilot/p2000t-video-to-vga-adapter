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
    return EXIT_SUCCESS;
}
