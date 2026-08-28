# Capture optimization progress log

This log records measured checkpoints and single-variable experiments made
during v0.4.1 confidence-guard validation. Candidate firmware is kept only when
it improves temporal stability without modal-image damage, capture deadline
failures, or host-test regressions.

## 2026-08-28

### Checkpoint C0 — v0.4.1 factory tuple

- Run: `experiment-20260828-210630-9db12c91`
- Settings: `window-confidence`, phase 0, odd-line phase +1, rate trim 0.
- Reference fidelity: exact; 0 erased, 0 filled, 0 recolored pixels.
- Robust temporal instability: 50.0878 ppm over 87/120 frames; median 7
  changed source pixels; 502 robust changed-pixel observations.
- Capture safety: 0 deadline-miss frames.
- Spatial audit: every frame is best aligned at horizontal shift 0. Remaining
  ordinary changes occupy 67 fixed edge sites and toggle only between adjacent
  colors. The 33 coherent outliers are a periodic 133–143-pixel source change
  concentrated in seven bottom rows and are treated as source animation, not
  lateral capture jitter.

### Experiments

| ID | Single change | Robust ppm | Modal mismatch | Deadline frames | Decision |
|---|---|---:|---:|---:|---|
| C1 `experiment-20260828-211036-58e97823` | phase -1 | 701.2442 | 116 | 0 | Reject: 14.0× more jitter and 102 erased pixels |
| C2 `experiment-20260828-211057-a9da6fc7` | phase +1 | 503.4722 | 66 | 0 | Reject: 10.1× more jitter and 62 filled pixels |
| C3 `experiment-20260828-211117-3b75ef3c` | odd phase 0 | 107.8318 | 19 | 0 | Reject: 2.2× more jitter and 19 erased pixels |
| C4 `experiment-20260828-211137-d3f28b0f` | odd phase +2 | 216.0704 | 33 | 0 | Reject: 4.3× more jitter and 33 filled pixels |
| C5 `experiment-20260828-211157-6595f22b` | rate trim -1 | 2691.3339 | 4558 | 0 | Reject: severe horizontal drift and modal damage |
| C6 `experiment-20260828-211217-b946bc51` | rate trim +1 | 2397.4971 | 3207 | 0 | Reject: severe horizontal drift and modal damage |
| C7 `experiment-20260828-211238-f63362ff` | baseline repeat | 50.0801 | 1 | 0 | Keep C0: confirms approximately 0.01 ppm run-to-run variance |
| C8 `experiment-20260828-212231-4bf062e0` | physical-even fixed late endpoint | 36.7757 | 4 | 0 | Reject: 26.6% less jitter, but four repeatable boundary recolors |
| C9 `experiment-20260828-213327-e98ae5db`, `experiment-20260828-213357-809da1bb` | two-frame confirmation on guarded parity | 32.31–34.43 | 1–2 | 0 | Reject: repeatable boundary recolor; priority 0x80 variants also stalled |
| C9b `experiment-20260828-213623-15b305d1` | confirm only unresolved pairs | 48.5364 | 0 | 0 | Reject: only 3% gain for 14.4 KiB SRAM and ambiguous-edge latency |
| C10 `experiment-20260828-214354-bcb385d1` | capture DMA IRQ priority 0x40 only | 50.9259 | 0 | 0 | Keep: exact image fidelity and prevents reproducible startup scanline stalls; later 0x80 diagnostics failed CRC too, so CRC was not caused by priority |

### Raw checkpoint R0 — dominant bottom edges

- Diagnostic: `diagnostic-20260828-211726-13e2ee1b`, physical lines 280–295.
- 56 CRC-valid bursts were preserved; record 59 failed transport CRC and the
  remaining requested bursts were intentionally rejected.
- No capture overruns or PIO RX stalls occurred.
- Aligning every line independently to its measured CSYNC edge located the
  exact window start at raw tick 2153. Replaying the production confidence
  policy over physical lines 287–293 produced 198 temporal disagreements and
  three modal mismatches. Replacing only the physical-even confidence guard
  with a fixed late-window endpoint predicted 106 disagreements and four modal
  mismatches. Live C8 confirmed lower jitter but also confirmed the damage.

An exhaustive replay of every three-position aperture within ±3 raw ticks
found the production `[-1, 0, +1]` aperture to be the only candidate with zero
modal changes. The nearest lower-jitter aperture, `[-2, 0, +1]`, reduced the
subset to 106 disagreements but changed two modal pixels. A symmetric
`[-2, 0, +2]` aperture increased the subset to 414 disagreements and changed
14 modal pixels, so wider 126 MHz sampling was rejected without a live flash.

### Raw checkpoint R1 — full-clock diagnostics

- Diagnostic: `diagnostic-20260828-215147-4bebab42`, physical lines 287–293.
- The raw diagnostic sampler ran at 252 MHz while production capture remained
  at 126 MHz. Twenty-four CRC-valid bursts were analyzed.
- Production sampling aligns at diagnostic tick 4307 with the equivalent
  `[-2, 0, +2]` half-tick aperture. It produced 28 temporal disagreements and
  one mismatch against the reference subset.
- A three-adjacent-sample 252 MHz aperture produced 74 disagreements and two
  reference mismatches. Exhaustive half-tick replay found no wider or denser
  aperture with better fidelity, so changing the production sampler to
  adjacent 252 MHz samples was rejected.

### Diagnostic transport checkpoint

- Stress run: `diagnostic-20260828-222207-81c9ed91`, physical lines 121–136.
- All 23 records, including twenty maximum-size raw bursts, reached the
  analyzer with valid CRCs and no capture overrun or PIO RX stall.
- A deliberately observed rejected attempt was preserved and retransmitted by
  the guarded ACK/retry protocol, confirming recovery rather than silent loss.
