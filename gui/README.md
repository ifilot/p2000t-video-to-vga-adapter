# P2000T VID2VGA Capture

This deliberately small Qt 6 application discovers Raspberry Pi Pico USB CDC
ports, starts the Pico 2 binary RGB111 screen stream, verifies every frame with
CRC-32, and displays every 480×240 capture scanline twice as a square-pixel
480×480 image. It supports lossless PackBits or raw records and can save the
current frame as a PNG. When the P2000T signal is absent, the viewer reports
which of the three embedded no-connection screens the VGA output is showing
and reproduces that complete 640×480 screen from the same RGB444 artwork,
bitmap font, and overlay coordinates used by the firmware.

The application carries its retro-computing icon in the Qt resources and as a
multi-resolution Windows executable icon; no separate image files are needed
beside the packaged executable. Its menu actions use a matching late-1990s
Linux desktop-inspired icon set embedded in the same resource bundle.

When launched, the viewer attempts to connect to the first detected Pico USB
CDC port and start the selected stream encoding. Manual port refresh and
connect/disconnect controls remain available. Adapter > Configure Pico 2 opens
a modal with exact spin boxes for capture alignment, an artwork selector, an
independent odd-line sampling-phase correction, and a whole-line sampling-rate
trim, raw/guarded/sharp reconstruction and Pico 2 126 MHz window policies,
plus eight RGB444 color rows with channel spinners and color pickers.
Every edit is sent to the Pico
immediately for live debugging. Reload from Pico retrieves the saved flash
copy, Factory defaults restores firmware defaults, and Save to Pico writes the
current values as a CRC-protected persistent copy. These controls require
firmware containing the matching structured control protocol. File > Exit and
Help > About provide the standard application menu entries.

**Adapter > Capture calibration sweep** automates repeatable alignment tests
against a static P2000T screen. Choose inclusive ranges for fine sample phase
and horizontal rate trim, the number of streamed frames to discard after each
change, and the number of consecutive PNGs to retain. The odd-line phase and
source-dot reconstruction mode stay at their current values. After choosing a
parent directory, the viewer creates a
timestamped `p2000t-analysis-*` directory containing lossless 480x480 PNGs,
`manifest.csv` with the requested settings and actual source-frame sequence,
and `session.txt` with the sweep and original settings. Progress can be
cancelled at any time; completed and cancelled sweeps restore the original
phase, odd-line phase, and rate trim. Partial images and the manifest are kept
when a sweep is cancelled or a write fails. The viewer remembers the last
selected parent directory. Setting acknowledgements are retried and time out
with the requested and last-reported values instead of leaving a sweep waiting
indefinitely.

**Adapter > Run engineering-screen autotune** is the unattended path for the
supplied SAA5050 test cartridge. It exhaustively scores all 357 phase/rate
combinations using unfiltered raw reconstruction, selects the base timing from
physical even lines, then tests all 21 odd-line corrections against physical
odd lines and validates the combined winner. The original live settings are
restored on cancellation or error. A successful winner remains active for
inspection but is deliberately not saved to flash. The output directory
contains every labeled retained frame, one modal PNG per candidate,
`frames.csv`, `candidates.csv`, `session.json`, and `result.json`. See
[the engineering autotune guide](../docs/engineering-autotune.md) for the
scoring definition and log schema.

The preferred AI workflow uses the separate, invisible
`p2000t-vid2vga-lab-agent.exe`; the viewer is not required. The agent and viewer
both support the same shared-directory **Codex lab bridge**. On a Windows
development machine its default root is
`D:\tmp\p2000t-codex-lab`. The repository client
`tools/p2000t_lab.py` can query status, shut down the agent, or submit one
acknowledged experiment at
a time, including optional phase, rate, odd-line phase, reconstruction,
reference-modal, settling-frame, and capture-frame controls. Successful
experiments leave their live setting active so Codex can adapt from it;
cancellation and errors restore
the pre-experiment timing. Only the separate, explicit `save` command writes
the current live tuple to Pico flash and waits for a matching stored-state
acknowledgement. Each run saves the request, exact frames and sequences, modal
image, spatial/temporal metrics, firmware correction counters, and result JSON. See
[the Codex lab guide](../docs/codex-lab.md).

The firmware command `c` starts the recommended PackBits stream, `r` starts a
raw stream, and `q` returns it to the normal USB console. Streaming is currently
compiled into the Pico 2 firmware only.

## High-resolution diagnostics

**Adapter > Record high-resolution diagnostics** records a lossless CSYNC
timing trace followed by repeated 252 MHz raw RGBS bursts covering up to sixteen
contiguous physical source lines. The viewer validates each CRC-protected record
before appending it to `capture.p2td` and writes a CSV byte index alongside it.
See [the diagnostic format and analysis guide](../docs/diagnostics.md).

## Windows build with MSYS2

Open an **MSYS2 UCRT64** shell and install the compiler, CMake, Ninja, and Qt 6:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-qt6-base
```

Configure and compile from the repository root:

```sh
cmake -S gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui --parallel
```

The executable is `build-gui/p2000t-vid2vga-capture.exe`. To prepare a
self-contained development directory with the required Qt DLLs:

```sh
mkdir -p build-gui/package
cp build-gui/p2000t-vid2vga-capture.exe \
  build-gui/p2000t-vid2vga-lab-agent.exe build-gui/package/
windeployqt6 --release --no-translations \
  --exclude-plugins qpdf,qsvg,qsvgicon \
  build-gui/package/p2000t-vid2vga-capture.exe
```

MSYS2 runtime DLLs reported by `ldd` may also need to be copied into the
package when it will run outside an MSYS2 UCRT64 environment. Redistributed Qt
DLLs must be accompanied by their corresponding terms from
`/ucrt64/share/licenses/qt6-base`. The SVG and PDF plugins are excluded because
the viewer does not use them. GitHub Actions performs both steps for the
release ZIP.

## Windows graphical installer

The preferred package is an NSIS 3 offline installer inspired by the P2000M
viewer. It installs the complete deployed application under Program Files,
creates desktop and Start-menu shortcuts, upgrades an existing installation
in place, and registers a conventional Windows uninstaller. Install NSIS 3
and make `makensis.exe` available on `PATH`, then run from PowerShell after
preparing a self-contained deployment directory:

```powershell
gui/packaging/create_windows_installer.ps1 `
  -Stage dist/viewer -Version 0.4.1 `
  -Output dist/p2000t-vid2vga-capture-windows-setup.exe `
  -WorkDirectory installer-work
```

The stage must contain `p2000t-vid2vga-capture.exe`, all runtime DLLs, and
`licenses/GPL-3.0-or-later.txt`. GitHub Actions constructs that tree, compiles
both the installer and portable ZIP on every push and pull request, checks the
installer archive with 7-Zip, and performs fresh-install, in-place-upgrade,
and uninstall smoke tests. Tagged releases publish only after this job passes.

The installer is currently unsigned because no Authenticode certificate is
configured. Windows SmartScreen may therefore request confirmation on first
launch.
