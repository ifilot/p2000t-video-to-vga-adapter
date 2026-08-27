# P2000T video-to-VGA adapter

![Two Philips P2000T computers displaying the original RGBS output and converted VGA output](assets/p2000t-video-to-vga-hero.jpg)

[![Build firmware](https://github.com/ifilot/p2000t-video-to-vga-adapter/actions/workflows/build.yml/badge.svg)](https://github.com/ifilot/p2000t-video-to-vga-adapter/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later%20%2F%20CERN--OHL--S--2.0-blue.svg)](#license)
[![Latest release](https://img.shields.io/github/v/release/ifilot/p2000t-video-to-vga-adapter?label=version)](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest)

Hardware and firmware for converting the Philips P2000T RGBS output to
640×480 VGA using a Raspberry Pi Pico or Pico 2.

## Downloads

- [Latest Pico firmware](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-pico.uf2)
- [Latest Pico 2 firmware](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-pico2.uf2)
- [Latest Windows capture viewer installer](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-capture-windows-setup.exe)
- [Latest portable Windows capture viewer](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-capture-windows.zip)

## Features

- 480×240 RGBS capture using PIO and DMA
- Triple-buffered, frame-boundary VGA updates
- Horizontally qualified CSYNC capture
- 640×480 at 60 Hz RGB444 VGA output
- Three selectable on-screen signal-loss designs
- On-board LED breathing while seeking and blinking during valid capture
- USB serial controls and capture statistics
- Pico 2 USB screen streaming at a target 25 FPS with a Qt 6 viewer
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

## USB controls

Connect to the Pico USB serial port and press `h` for help. Useful commands are
`s` for status, `[`/`]` for vertical position, `,`/`.` for sample phase, and
`<`/`>` for horizontal position. Use `1`, `2`, or `3` to select the
no-connection artwork. Phase 0 is the default.

On Pico 2, `c` starts the recommended continuous PackBits screen stream and
`r` starts an uncompressed stream. Press `q` or Escape to stop binary mode and
return to the console. The alignment and artwork commands remain available
silently while a binary stream is active, allowing the desktop viewer to
configure the adapter without interrupting capture. Screen streaming is
intentionally excluded from the Pico/RP2040 build for now so its limited SRAM
and established VGA timing remain unchanged.

## Pico 2 desktop capture viewer

The Windows viewer shows the P2000T screen live over USB and can save snapshots
as PNG files. It connects to the first detected Pico 2 automatically. When no
P2000T signal is present, it displays the same selected no-connection screen as
the VGA output.

Use **Adapter > Configure Pico 2** to adjust alignment, choose the
no-connection artwork, and tune all eight colors. Changes take effect
immediately, which makes calibration easy. Settings can be restored, reset to
their defaults, or saved to the Pico 2 for the next power-on.

Download either the installer or portable ZIP from [Downloads](#downloads).
The installer adds the viewer to the Start menu and includes an uninstaller.

## License

Firmware and viewer sources are
[GPL-3.0-or-later](LICENSES/GPL-3.0-or-later.txt). Hardware design files are
licensed under CERN-OHL-S-2.0; see [pcb/LICENSE.md](pcb/LICENSE.md).
