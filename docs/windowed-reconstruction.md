# Windowed 126 MHz reconstruction

Normal capture runs its PIO state machine at 126 MHz but retains two samples
per nominal 6 MHz source dot. The Pico 2 windowed engine retains three adjacent
126 MHz observations around each of those two positions. It therefore captures
six raw RGBS nibbles per source dot and reconstructs them into the existing
480x240 packed framebuffer.

Line synchronization remains clock-accurate in this path. The PIO uses a
hardware `WAIT` for the horizontal edge and then independently qualifies that
edge for 33 capture clocks. Do not replace this with a delayed polling jump:
that quantizes edge detection and manifests as temporal horizontal jitter.

The engine uses two 720-byte DMA scanline buffers. While one buffer is decoded,
the other receives the following source line. This avoids allocating another
172.8 KB full-frame buffer and leaves the existing triple-buffered display path
unchanged. The legacy two-tap PIO program remains available and is restored at
a source-frame boundary whenever a non-window mode is selected.

## Policies

- `window-center` retains the middle sample from every three-tick window. It is
  the window-engine control and should resemble raw capture.
- `window-channel` takes the binary majority independently for the R, G, B,
  and sync inputs.
- `window-early` performs atomic color voting. Repeated colors win; when all
  three samples differ, it chooses the early endpoint.
- `window-late` uses the same atomic vote but chooses the late endpoint for an
  all-distinct transition.
- `window-confidence` uses `window-late` unchanged on one logical row parity.
  On the other parity it copies a paired half-dot only when that half's three
  samples are unanimous and the other half's samples are not. Two unanimous
  but different colors remain an edge, so the mode does not globally duplicate
  dots or erase stable geometry.

The early/late pair is deliberate. A measured blue-cyan-white sequence has no
three-sample mode, but cyan is known to be a one-tick intermediate color. Both
atomic policies suppress cyan while exposing whether choosing the pre-edge or
post-edge state gives the cleaner spatial result.

`sharp` does not use the window engine. It preserves the first normal tap and
only guards the second, unlike the legacy guarded mode which duplicates one
selected sample across both output pixels.

## Evidence and safety

Each streamed frame includes its reconstruction mode, capture engine, samples
per output, changed output samples, all-distinct windows, per-channel changes,
and a line-deadline flag. The Codex lab bridge stores these fields beside every
PNG and aggregates them in `result.json`.

The confidence guard is deliberately parity-aware. It applies on logical odd
rows, which correspond to physical even P2000T lines at the default first
visible line 57. The ordinary late policy remains active on logical even rows;
changing the first visible line also shifts this physical mapping.

Version 0.4.0 introduced the complete reconstruction-mode flash record while
continuing to read v0.3.x records. Version 0.4.1 promotes the validated
`window-confidence`, phase 0, odd-line correction +1, and rate trim 0 tuple to
the Pico 2 factory default. Existing saved settings remain authoritative. The
original Pico retains raw reconstruction and phase-zero defaults. A mode switch
is adopted only after the current source frame completes, so no captured frame
is split between PIO programs or policies. When confidence is the power-on
mode, Pico 2 starts the same six-tap engine with the lightweight center policy
and adopts confidence at a frame boundary after VGA startup; the DMA engine is
not reconfigured during this transition.
