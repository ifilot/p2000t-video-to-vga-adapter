# Changelog

Notable project changes are recorded here.

## 0.4.1 - 2026-08-29

- Promoted the parity-aware `window-confidence` policy to the Pico 2 factory
  default with phase 0, physical odd-line correction +1, and rate trim 0.
  Existing saved settings remain authoritative and flash-compatible.
- The confidence guard trusts a paired half-dot only when its three samples are
  unanimous and the other half is uncertain. On the default viewport it guards
  physical even source lines and preserves two unanimous but different colors
  as a genuine edge.
- On boot, Pico 2 starts the six-tap engine with its lightweight center policy
  and adopts the confidence policy at a frame boundary after VGA startup. This
  avoids reconfiguring DMA and keeps the validated live-transition behavior.
- Raised the capture DMA interrupt priority to keep six-tap reconstruction
  ahead of the next scanline deadline during startup and sustained capture.
- Added reference-modal erased/filled/recolored pixel diagnostics and robust
  temporal logging which separates coherent high-change frames from the
  ordinary instability total.
- Raised raw diagnostic sampling from 126 MHz to 252 MHz without changing the
  production capture clock. Diagnostic records now use a guarded ACK/retry
  exchange so a CRC failure can be recovered without losing the requested
  burst, and the viewer preserves rejected records for investigation.
- Added direct diagnostic recording to the Codex lab bridge and an offline
  sampling-aperture analyzer for comparing reconstructed output against a
  captured reference image.

## 0.4.0 - 2026-08-28

- Added a frame-boundary-switchable Pico 2 capture engine which records two
  three-tick 126 MHz windows per source dot into ping-pong scanline buffers and
  reconstructs the normal 480x240 framebuffer without another full-frame
  allocation.
- Added sharp guarded reconstruction plus window-center, per-channel majority,
  atomic-early, and atomic-late policies. Pico 2 now defaults to the balanced
  window-center timing selected across the caption and engineering test pages.
- Extended the CRC-protected flash record to persist every reconstruction mode
  while retaining read compatibility with v0.3.x settings records.
- Added exact per-frame reconstruction telemetry to the USB stream and Codex
  lab results, including corrected/ambiguous samples, RGB channel corrections,
  active engine, and scanline-deadline failures.
- Fixed tagged release packaging so the generated SAA5050 cartridge binary is
  downloaded with the firmware/viewer artifacts before publishing a release.
### Added

- Pico 2 continuous USB capture streaming for 480×240 planar RGB111 frames,
  with per-frame CRC-32, raw/PackBits selection, non-blocking TinyUSB
  backpressure, and a target rate of 25 FPS.
- Minimal Qt 6 desktop capture viewer for Windows, Linux, and macOS, including
  native Pico CDC discovery, CRC validation, and lossless PNG screenshots.
- MSYS2/UCRT64 Windows viewer build, test, packaging, and release automation.
- P2000M-inspired NSIS graphical Windows installer with Program Files
  deployment, shortcuts, upgrade-aware identity, and a standard uninstaller.
- Viewer menus for application actions and live Pico 2 capture alignment and
  no-connection artwork configuration.
- Late-1990s Linux desktop-inspired icons for viewer connection,
  configuration, exit, and about actions.
- Interactive Pico 2 configuration dialog with exact capture-alignment
  spinners, artwork selection, and eight RGB444 color editors.
- CRC-protected flash persistence and structured USB request/state packets for
  saving and retrieving the complete adapter configuration.
- Tunable odd-source-line phase correction in 7.94 ns steps, available through
  the USB console, structured control protocol, and Pico 2 configuration UI.
- Tunable whole-line capture-rate correction in 1/256 PIO-divider steps, with
  live console and GUI control plus flash persistence.
- Automated viewer calibration sweeps over sample phase and horizontal rate
  trim, with acknowledged setting changes, configurable settling, repeated
  PNG captures, CSV/session metadata, cancellation, and settings restoration.
- Persisted raw/guarded-second-tap SAA5050 source-dot reconstruction, applied
  frame-atomically to VGA and identically to Pico 2 USB captures, with console
  and GUI controls. The mode was selected from offline temporal analysis of a
  1,250-frame calibration sweep.
- Guarded reconstruction now rejects a second-tap color which matches neither
  its own first tap nor the following dot's first tap, suppressing the
  one-clock RGB transition states measured by high-resolution diagnostics.
- Pico 2 high-resolution diagnostic recording with a full-frame 63 MHz CSYNC
  trace, repeated sync-triggered 126 MHz raw RGBS bursts, capture-duration
  overrun detection, CRC-protected lossless records, viewer session recording,
  and a reproducible analysis utility.
- Unattended engineering-screen autotuning in the viewer. It exhaustively
  searches phase and capture-rate trim, independently tunes physical odd-line
  phase, validates the winner, keeps reconstruction raw to prevent blur from
  gaming the score, and records labeled frames, modal PNGs, per-parity temporal
  and spatial metrics, CSV logs, and a machine-readable JSON recommendation.
- Invisible Windows Codex lab agent, shared bridge support in the viewer, and
  a WSL command-line client.
  Codex can directly query adapter state, apply acknowledged non-persistent
  timing/reconstruction settings, capture and score repeated frames, inspect
  the resulting PNG evidence, adapt subsequent experiments, or cancel and
  restore an experiment, explicitly save a chosen tuple after acknowledgement,
  or shut down the agent through an atomic JSON request/response directory.

### Changed

- Calibration sweeps now request an explicit complete setting state, retry a
  missing acknowledgement with a bounded timeout, report mismatched values,
  and remember the last selected analysis parent directory.
- Capture PIO and DMA are quiesced before a flash settings write, window lookup
  state is rebuilt while DMA remains stopped, and acquisition restarts at a
  clean frame boundary. This prevents flash-safe interrupt pauses from
  corrupting window-center reconstruction or stopping capture.
- Capture-buffer ownership now permits a short-lived Pico 2 USB reader without
  exposing a buffer that DMA may overwrite or changing the Pico/RP2040 memory
  footprint.
- The desktop viewer duplicates every source scanline for the correct 480×480
  square-pixel display and PNG geometry.
- USB state records now identify signal loss and the no-connection artwork
  actually displayed by the VGA core without transferring stale pixels.
- The viewer now reproduces the complete selected 640×480 no-connection VGA
  screen from embedded RGB444 artwork and a shared pixel-exact overlay layout.
- A transparent retro-computing application icon based on the real P2000T,
  DIN6, and VGA connector silhouettes is embedded in the Qt application and
  the multi-resolution Windows executable resources.
- Windows CI now validates the installer archive and smoke-tests a fresh
  install, same-directory upgrade, and complete uninstall on every commit.
- The viewer now attempts to connect to the first detected Pico USB port at
  startup, and Pico 2 firmware accepts silent configuration commands without
  interrupting its binary screen stream.
- Source-color lookup tables are triple-buffered and adopted at VGA frame
  boundaries so palette changes cannot split a frame between two palettes.
- Viewer configuration edits are now applied to the connected Pico 2
  immediately; flash persistence remains an explicit action.
- The on-board Pico LED now breathes slowly while seeking a P2000T signal and
  blinks at 0.5-second intervals while valid input is present.
- The first captured line now compensates for its distinct sync-entry path
  without the previous one-PIO-tick phase discrepancy.
- Capture timing commands are rebuilt outside the shared frame-buffer lock so
  the VGA core cannot be held off long enough to emit a missing scanline.

## 0.2.0 - 2026-08-27

### Added

- Green-phosphor, synthwave, and amber-circuit no-connection artwork, selectable
  at runtime with USB commands `1`, `2`, and `3`.
- Automatic CMake downloads for the pinned Pico SDK and pico-extras releases.

### Changed

- No-connection artwork now uses a timing-safe 16-color RGB444 indexed
  pipeline with one SRAM palette lookup per pair of pixels.
- Artwork selection is adopted atomically at VGA frame boundaries to prevent
  tearing.
- Linux build requirements and artwork controls are documented in the README.
- GitHub Actions uses the same self-contained CMake dependency setup as local
  builds.

## 0.1.1 - 2026-08-26

### Added

- P2000M-style signal-loss status screen.
- GitHub Actions builds and downloadable artifacts for Pico and Pico 2.
- Automatic GitHub Releases with stable Pico and Pico 2 UF2 filenames when a
  tag is pushed.
- Pico 2 RP2350 build support using the experimental 252 MHz clock setup.

### Changed

- Board selection is now controlled with `PICO_BOARD` in a separate CMake
  build directory.
- Source-frame and no-signal rendering are isolated in a dedicated,
  SRAM-resident renderer module.
- Firmware functions, parameters, constants, state, and public data structures
  now have Doxygen documentation.
- Frame-buffer sequence comparisons remain correct across 32-bit counter
  rollover.

### Removed

- Unused raw-pixel accessor, frame-byte constant, and lowercase font
  normalization left over from earlier capture experiments.

## 0.1.0 - 2026-08-26

### Added

- 480×240 raw RGBS capture with two samples per 6 MHz source dot.
- 640×480 at 60 Hz RGB444 VGA output with vertical line doubling.
- Triple-buffered full-frame capture and frame-boundary display updates.
- CSYNC holdoff and pulse qualification for improved line stability.
- USB status and timing-adjustment commands.
- P2000T SAA5050 screen-test cartridge.

### Changed

- Sample phase 0 is the default.

### Removed

- Obsolete standalone signal-probe firmware.
