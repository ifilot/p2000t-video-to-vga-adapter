# P2000T video-to-VGA adapter

[![Build firmware](https://github.com/ifilot/p2000t-video-to-vga-adapter/actions/workflows/build.yml/badge.svg)](https://github.com/ifilot/p2000t-video-to-vga-adapter/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later%20%2F%20CERN--OHL--S--2.0-blue.svg)](#license)
[![Latest release](https://img.shields.io/github/v/release/ifilot/p2000t-video-to-vga-adapter?label=version)](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest)

Hardware and firmware for converting the Philips P2000T RGBS output to
640×480 VGA using a Raspberry Pi Pico or Pico 2.

## Downloads

- [Latest Pico firmware](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-pico.uf2)
- [Latest Pico 2 firmware](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-pico2.uf2)

## Features

- 480×240 RGBS capture using PIO and DMA
- Triple-buffered, frame-boundary VGA updates
- Horizontally qualified CSYNC capture
- 640×480 at 60 Hz RGB444 VGA output
- Three selectable on-screen signal-loss designs
- USB serial controls and capture statistics
- Pico and experimental Pico 2 builds

## Building

Install the Arm GNU toolchain, CMake, and Ninja. On Ubuntu or Debian, install
the required build packages with:

```sh
sudo apt update
sudo apt install cmake ninja-build gcc-arm-none-eabi \
  libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib git python3
```

CMake downloads the pinned Pico SDK 2.1.1, pico-extras 2.1.1, and required
host utilities into the ignored `.ci-dependencies` directory on first use.
Existing SDK checkouts can be selected instead by setting `PICO_SDK_PATH` and
`PICO_EXTRAS_PATH`.

Configure and build the Pico firmware:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico
cmake --build build --parallel
```

For Pico 2, use a separate build directory:

```sh
cmake -S . -B cmake-build-pico2 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2
cmake --build cmake-build-pico2 --parallel
```

The UF2 file is written below the build directory at
`src/p2000t-vid2vga-firmware.uf2`. GitHub Actions publishes separate Pico and
Pico 2 build artifacts. Pushing a tag creates a GitHub Release containing both
UF2 files under stable, version-independent filenames.

Both boards currently run at 252 MHz and 1.30 V to obtain exact 126 MHz
capture and 25.2 MHz VGA clocks. The Pico 2 configuration compiles but remains
an experimental overclock requiring hardware testing.

The firmware embeds concepts 04, 05, and 08 as compact 16-color RGB444
indexed images so their scanlines can be expanded within the VGA timing
deadline. Amber circuit is the power-on default. While connected over USB,
press `1` for green phosphor, `2` for synthwave, or `3` for amber circuit. The
choice takes effect atomically at the next VGA frame and is not persisted
across a power cycle.

The complete concept catalog and reproduction instructions are in
`assets/no_connection_concepts/README.md`. Custom 640×480 artwork can still be
encoded with `tools/encode_no_connection.py` after installing Pillow.

## USB controls

Connect to the Pico USB serial port and press `h` for help. Useful commands are
`s` for status, `[`/`]` for vertical position, `,`/`.` for sample phase, and
`<`/`>` for horizontal position. Use `1`, `2`, or `3` to select the
no-connection artwork. Phase 0 is the default.

## License

Firmware sources are GPL-3.0-or-later. Hardware design files are licensed
under CERN-OHL-S-2.0; see [pcb/LICENSE.md](pcb/LICENSE.md).
