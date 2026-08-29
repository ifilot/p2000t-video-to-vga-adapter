# Codex lab bridge

The Codex lab bridge uses the invisible `p2000t-vid2vga-lab-agent.exe` as a
local laboratory instrument. The agent retains ownership of the Windows Pico
serial port while Codex in WSL controls experiments through atomic files in a
shared directory. The graphical viewer is not required.
This is more reliable than attempting to open a Windows COM device from WSL
and prevents two processes from competing for the Pico.

The bridge starts automatically with the lab agent (and is also available when
a human deliberately runs the viewer). Its Windows default is:

```text
D:\tmp\p2000t-codex-lab
```

The same directory is `/mnt/d/tmp/p2000t-codex-lab` in WSL. Set
`P2000T_LAB_ROOT` before launching either component to override it.

## Codex-facing client

From the repository root:

```sh
python3 tools/p2000t_lab.py status
python3 tools/p2000t_lab.py save
python3 tools/p2000t_lab.py factory-reset
python3 tools/p2000t_lab.py experiment \
  --phase -2 --odd-phase 1 --rate-trim 2 \
  --reconstruction window-early --settle 3 --frames 40 \
  --tag "engineering screen, left-edge hypothesis"
python3 tools/p2000t_lab.py cancel
python3 tools/p2000t_lab.py shutdown
```

The client prints the structured response and returns nonzero when a request is
rejected or an experiment fails. Codex can then open the returned PNG paths,
compare metrics, and issue the next experiment. Settings omitted from an
experiment inherit their current live values. A zero-frame experiment applies
and acknowledges settings without saving images.

Experiments operate only on live phase, physical odd-line phase, line-rate
trim, and reconstruction. Available reconstruction names are `raw`, `guarded`,
`sharp`, `window-center`, `window-channel`, `window-early`, `window-late`, and
`window-confidence`. The latter adds the parity-aware unanimous-evidence
guard. Window modes require Pico 2 and acquire three
consecutive 126 MHz samples around every retained output sample. Experiments
never write flash.
Successful settings remain active for the next adaptive step; an error,
cancellation, signal loss, or disconnection restores the settings which were
active before that experiment. The separate, explicit `save` command persists
the current live tuple and succeeds only after the Pico reports that it matches
the stored tuple. `factory-reset` restores the firmware's known-good tuple,
palette, and artwork, persists them immediately, and likewise waits for a
matching saved-state acknowledgement.

## Directory protocol

The client atomically renames complete JSON files into `requests/`. The viewer
archives accepted input under `archive/` before execution and atomically writes
the corresponding `responses/<id>.json`. `bridge-status.json` is a once-per-
second heartbeat containing connection, signal, busy, and complete live-setting
state.

An experiment request has protocol version 1:

```json
{
  "protocol": 1,
  "id": "edge-study-001",
  "command": "experiment",
  "tag": "white-to-blue transition",
  "reference_run": "edge-study-baseline",
  "settings": {
    "phase": -2,
    "odd_line_phase": 1,
    "rate_trim": 2,
    "reconstruction": "raw"
  },
  "settle_frames": 3,
  "capture_frames": 40
}
```

Valid commands are `status`, `experiment`, `diagnostic`, `save`,
`factory-reset`, `cancel`, and `shutdown`.
Ranges are validated before any firmware command is sent. Every setting tuple
is followed by an explicit firmware state request; capture starts only after
the Pico reports the complete requested tuple. Missing acknowledgements are
retried and bounded by a timeout.

Each experiment creates `runs/<id>/` containing:

- `request.json` with target and original live settings;
- `frames.csv` and lossless, sequence-labeled PNGs;
- `modal.png` for a capture with at least one frame;
- `result.json` with temporal instability, physical row-parity diagnostics,
  robust low-change-frame metrics, coherent high-change-frame counts,
  one-pixel island/third-color counts, horizontal transitions, score, exact
  firmware reconstruction telemetry, and optional reference-modal fidelity.

Passing `--reference-run <id>` compares the new modal with
`runs/<id>/modal.png`. The result classifies every mismatch as an erased,
filled, or recolored pixel. Robust temporal fields use frames at or below the
larger of 32 changed pixels and four times the median frame mismatch count.
Frames above that threshold remain logged as `coherent_outlier_frames` rather
than silently disappearing from the ordinary instability total.

Every `frames.csv` row records the Pico capture-completion timestamp and host
UTC receipt time, active engine, samples per output pixel, corrected and
ambiguous sample counts, corrections per RGB channel, and whether a capture
deadline was missed. `result.json` also aggregates those values.

Bridge experiments remain non-persistent. Flash is changed only by an explicit
`save` or `factory-reset` command or the ordinary configuration UI. Pico 2
v0.4.1 uses line-sliced `window-late`, phase 0, odd-line +1, and rate trim 0 as
its power-on and factory default when no valid saved record is present. The
engine is available in ordinary Pico 2 release builds after whole-screen and
physical VGA validation. Existing saved settings remain authoritative until an
explicit `factory-reset`; the original Pico retains raw phase-zero defaults.

The transport test can run without hardware: a `status` request still returns
viewer state, while an experiment is explicitly rejected as unavailable when
the Pico or P2000T signal is absent.
