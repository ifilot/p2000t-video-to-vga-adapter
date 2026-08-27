# Changelog

Notable project changes are recorded here.

## Unreleased

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

### Changed

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
