# Changelog

Notable project changes are recorded here.

## Unreleased

### Added

- P2000M-style signal-loss status screen.
- GitHub Actions builds and downloadable artifacts for Pico and Pico 2.
- Automatic GitHub Releases with stable Pico and Pico 2 UF2 filenames when a
  tag is pushed.
- Pico 2 RP2350 build support using the experimental 252 MHz clock setup.

### Changed

- Board selection is now controlled with `PICO_BOARD` in a separate CMake
  build directory.

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
