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

The v0.4.1 engine DMA-captures a complete 172.8 KB high-resolution frame before
decoding it into the existing triple-buffered display path. Reconstruction is
therefore absent from the 64 us source scanline cadence. The DMA IRQ only
quiesces capture and records completion; core 0 then decodes one 720-byte
source line per main-loop service slice. Each slice writes 240 bytes and
returns, giving scanvideo on core 1 regular SRAM arbitration opportunities.
Its raw storage shares the logic analyzer's already-reserved SRAM; window
capture and a diagnostic session are mutually exclusive. The legacy two-tap
PIO program remains available and is restored at a source-frame boundary
whenever a non-window mode is selected.

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
continuing to read v0.3.x records. Hardware validation found that both the
lookup-based and direct line-rate six-tap reconstruction paths can starve the
independent VGA core's SRAM access: capture remains locked and USB frames are
correct, but the monitor receives black source scanlines.

Version 0.4.1 first tested complete-frame DMA followed by one monolithic batch
decode. USB continued to receive correct frames, but the VGA monitor lost
signal. Splitting that same decode into one-line service slices retained the
engineering image throughout 20- and 100-frame USB runs with zero deadline
misses. The window timing validator permits the resulting 60–80 ms cadence;
raw timing validation remains unchanged. The matched 100-frame confidence run
measured 149.19 robust instability ppm versus 142.67 ppm for raw. A second
whole-screen test exposed the more demanding blue-on-white and mosaic
transitions. On that page, `window-late`, phase 0, odd-line correction +1
reduced average changed pixels in every stable screen band while changing only
70 of 115,200 modal pixels. That balanced tuple is the Pico 2 v0.4.1 factory
default. Builds may explicitly disable the engine with
`-DP2000T_WINDOW_CAPTURE_ENABLED=OFF`.
