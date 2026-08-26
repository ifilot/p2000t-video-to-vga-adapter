# P2000T video-to-VGA adapter

Hardware and firmware for converting the Philips P2000T RGBS output to
640×480 VGA using a Raspberry Pi Pico or Pico 2.

## Features

- 480×240 RGBS capture using PIO and DMA
- Triple-buffered, frame-boundary VGA updates
- Horizontally qualified CSYNC capture
- 640×480 at 60 Hz RGB444 VGA output
- On-screen signal-loss status
- USB serial controls and capture statistics
- Pico and experimental Pico 2 builds

## Connections

The PCB conditions the P2000T signals before they reach the Pico. Do not treat
the GPIO mapping below as a replacement for the input-conditioning circuit.

| Signal | Prototype v1, default | Corrected PCB v2 |
|---|---:|---:|
| CSYNC input | GP17 | GP16 |
| Red input | GP16 | GP17 |
| Green input | GP19 | GP18 |
| Blue input | GP18 | GP19 |
| VGA RGB444 | GP0–GP11 | GP0–GP11 |
| VGA HSYNC | GP12 | GP12 |
| VGA VSYNC | GP13 | GP13 |

Set `P2000T_PROTOTYPE_V1_MIRRORED_DIN=OFF` when building for the corrected
PCB mapping.

## Building

Install the Arm GNU toolchain, CMake, Ninja, Pico SDK 2.1.1, and pico-extras
2.1.1. Set `PICO_SDK_PATH` and `PICO_EXTRAS_PATH`, then run:

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
`src/p2000t-vid2vga-firmware.uf2`. GitHub Actions also publishes separate
Pico and Pico 2 build artifacts.

Both boards currently run at 252 MHz and 1.30 V to obtain exact 126 MHz
capture and 25.2 MHz VGA clocks. The Pico 2 configuration compiles but remains
an experimental overclock requiring hardware testing.

## USB controls

Connect to the Pico USB serial port and press `h` for help. Useful commands are
`s` for status, `[`/`]` for vertical position, `,`/`.` for sample phase, and
`<`/`>` for horizontal position. Phase 0 is the default.

## Project layout

- `src/` — Pico firmware and PIO capture program
- `pcb/` — KiCad hardware design
- `screentest/` — P2000T SAA5050 test cartridge
- `.github/workflows/` — Pico and Pico 2 CI builds

## License

Firmware sources are GPL-3.0-or-later. Hardware design files are licensed
under CERN-OHL-S-2.0; see [pcb/LICENSE.md](pcb/LICENSE.md).
