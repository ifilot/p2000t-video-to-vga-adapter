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
- [SAA5050 screen-test cartridge](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-screentest.bin)
- [Latest Windows capture viewer installer](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-capture-windows-setup.exe)
- [Latest portable Windows capture viewer](https://github.com/ifilot/p2000t-video-to-vga-adapter/releases/latest/download/p2000t-vid2vga-capture-windows.zip)

## Features

- 480×240 RGBs capture using PIO and DMA
- Triple-buffered, frame-boundary VGA updates
- Horizontally qualified CSYNC capture
- 640×480 at 60 Hz RGB444 VGA output
- Three selectable on-screen signal-loss designs
- On-board LED breathing while seeking and blinking during valid capture
- USB serial controls and capture statistics
- Pico 2 USB screen streaming at a target 25 FPS with a Qt 6 viewer
- Selectable raw, legacy guarded, sharp guarded, and Pico 2 three-sample
  126 MHz window reconstruction modes for studying transition instability
- CRC-protected persistence for every Pico 2 reconstruction mode

## Building

Install the Arm GNU toolchain, CMake, and Ninja. On Ubuntu or Debian, install
the required build packages with:

```sh
sudo apt update
sudo apt install cmake ninja-build gcc-arm-none-eabi \
  libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib git python3
```

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
`;`/`'` for the odd-line phase correction. Use `{`/`}` to make the complete
captured line narrower or wider and `w` to reset that rate trim. `<`/`>` moves
the horizontal start, `d` toggles raw/guarded-second-tap reconstruction,
while `1`, `2`, or `3` selects the no-connection artwork.
Each phase tick is nominally 7.94 ns. Each rate step changes the PIO divider by
1/256 and moves the right edge by approximately 0.94 captured pixel while
leaving the sync-anchored left edge in place. Positive odd-line phase samples
odd physical source lines later; positive rate trim widens every line. Pico 2
v0.4.1 factory defaults use the parity-aware `window-confidence`
reconstruction with phase `0`, odd-line correction `+1`, and rate trim `0`;
the original Pico retains raw reconstruction with phase-zero defaults.

Raw reconstruction exposes both 12 MHz samples in every nominal 6 MHz
SAA5050 source dot. Guarded-second-tap reconstruction uses the later sample
for both corresponding VGA pixels unless it matches neither the first tap nor
the following dot's first tap; that isolated intermediate color is replaced
by the current dot's first tap. All reconstruction modes can be selected live
and saved in the v0.4.x flash record. Existing v0.3.x saved settings are
migrated when next saved.

For a measured source period near 20092 us, start with rate trim `+2`. The
left-edge compensation allows the existing sample phase to remain unchanged;
compare both edges and also try `+1` because one divider step is necessarily
coarser than the calculated ideal correction.

On Pico 2, `c` starts the recommended continuous PackBits screen stream and
`r` starts an uncompressed stream. Press `q` or Escape to stop binary mode and
return to the console. The alignment and artwork commands remain available
silently while a binary stream is active, allowing the desktop viewer to
configure the adapter without interrupting capture. 

> [!NOTE] 
> Screen streaming is intentionally excluded from the Pico/RP2040 build
> for now so its limited SRAM and established VGA timing remain unchanged.

## Pico 2 desktop capture viewer

The Windows viewer shows the P2000T screen live over USB and can save snapshots
as PNG files. It connects to the first detected Pico 2 automatically. When no
P2000T signal is present, it displays the same selected no-connection screen as
the VGA output.

Use **Adapter > Configure Pico 2** to adjust alignment, the independent
odd-line phase correction, and the horizontal sampling-rate trim; choose the
two-tap or 126 MHz window reconstruction; choose the no-connection artwork;
and tune all eight colors. Changes take effect
immediately, which makes calibration easy.
Settings can be restored, reset to their defaults, or saved to the Pico 2 for
the next power-on.

For capture-quality investigation, **Adapter > Record high-resolution
diagnostics** saves a full-frame 63 MHz CSYNC trace and repeated, sync-triggered
126 MHz raw RGBS bursts without reconstruction or palette conversion. See the
[diagnostic recording and analysis guide](docs/diagnostics.md).

For intermittent edge artifacts, **Adapter > Capture calibration sweep**
automatically tests an inclusive sample-phase x horizontal-rate range against
a static source screen. It waits a configurable number of frames after every
acknowledged change, saves multiple consecutive PNGs per setting, and records
the setting and source-frame sequence in a CSV manifest inside a timestamped
analysis directory. The odd-line correction remains fixed at its current
value, and the original live settings are restored after completion or
cancellation.

For unattended optimization, display the supplied SAA5050 screen-test
cartridge and choose **Adapter > Run engineering-screen autotune**. The viewer
forces sharp raw reconstruction, evaluates every phase/rate-trim pair, then
tunes the physical odd lines independently and validates the winner over a
longer run. It applies the result live without writing flash. Every retained
frame, modal candidate image, odd/even instability measurement, one-dot color
transient count, and selection decision is written to a timestamped analysis
directory. See the [engineering autotune guide](docs/engineering-autotune.md).

For AI-directed investigation, the invisible **Codex lab agent** owns the
Windows Pico connection without opening the viewer. Codex can query live state,
apply non-persistent sampling settings, explicitly persist a chosen tuple,
request settled multi-frame captures, receive modal/instability
metrics, inspect the PNGs, and adapt the next experiment without a person
operating a GUI. The Windows agent and WSL client exchange
atomic JSON files through `D:\tmp\p2000t-codex-lab`; see the
[Codex lab guide](docs/codex-lab.md).
The [windowed reconstruction design](docs/windowed-reconstruction.md) explains
the six-tap PIO path and its per-frame evidence counters.

Download either the installer or portable ZIP from [Downloads](#downloads).
The installer adds the viewer to the Start menu and includes an uninstaller.

## License

Firmware and viewer sources are
[GPL-3.0-or-later](LICENSES/GPL-3.0-or-later.txt). Hardware design files are
licensed under CERN-OHL-S-2.0; see [pcb/LICENSE.md](pcb/LICENSE.md).
