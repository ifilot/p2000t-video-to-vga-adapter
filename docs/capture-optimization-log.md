# Capture optimization progress log

This log records measured checkpoints and single-variable experiments made
during the withdrawn confidence-guard candidate validation. Candidate firmware
is kept only when it improves temporal stability without modal-image damage,
capture deadline failures, or host-test regressions.

## 2026-08-29 — stable v0.4.0 recovery

Live hardware testing separated two independent symptoms. The six-tap window
engine could retain a correct, locked USB framebuffer while the VGA source
image was completely black. Switching only reconstruction to raw immediately
restored the monitor. Removing its 64 KiB lookup tables and calculating the
same result directly still overloaded the line-rate IRQ severely enough to
stop USB service. Continuous window reconstruction is therefore unavailable
in stable v0.4.0 pending a frame-batched or otherwise contention-free design.

The current static screen was then optimized only across settings which retain
the proven two-tap capture engine. Rate trim remained zero because adjacent
steps had already shown severe horizontal drift.

| Run | Tuple | Robust ppm | Coherent outliers | Modal mismatch | Decision |
|---|---|---:|---:|---:|---|
| `experiment-20260829-053012-63116af9` | raw, phase 0, odd +1 | 707.47 | 0/30 | reference | Baseline |
| `experiment-20260829-053025-28f511d5` | raw, phase -1, odd +1 | 137.15 | 10/30 | 79 | Best raw timing |
| `experiment-20260829-053109-bec08b96` | raw, phase -2, odd +1 | 325.33 | 7/30 | 128 | Reject |
| `experiment-20260829-053118-b5e45f42` | raw, phase -1, odd 0 | 184.10 | 6/30 | 90 | Reject |
| `experiment-20260829-053131-5a84b7f4` | raw, phase -1, odd +2 | 771.12 | 0/30 | 78 | Reject |
| `experiment-20260829-053043-c415dcb3` | sharp, phase 0, odd +1 | 572.62 | 1/30 | 214 | Reject: spatial changes |
| `experiment-20260829-053056-f09a0a8a` | sharp, phase -1, odd +1 | 141.32 | 5/30 | 284 | Reject: spatial changes |
| `experiment-20260829-053140-4b289fb6` | guarded duplicate, phase -1, odd +1 | 20.98 | 6/30 | 1,035 | Reject: blurred geometry |
| `experiment-20260829-053156-8ff7d195` | raw, phase -1, odd +1 | 143.66 | 20/100 | 81 | Keep and persist |

The stable v0.4.0 Pico 2 factory tuple is consequently raw/two-tap, phase -1,
physical odd-line phase +1, and rate trim 0. The long validation retained
8,013 horizontal transitions and had no capture deadline failures.

## 2026-08-28

### Checkpoint C0 — withdrawn confidence-candidate tuple

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

## 2026-08-29

### Raw checkpoint R2 — full-screen transition map

- Fifteen validated 252 MHz blocks cover physical lines 57–296 with 132 raw
  bursts and 1,013,760 reconstructed pixel observations.
- Replaying the confidence candidate produced 2,609 temporal disagreements and one modal-reference
  mismatch. Most disagreements were confined to the animated top block.
- Applying the late-confidence rule globally reduced only fourteen
  disagreements and introduced three new modal errors, confirming that the
  previous fixed-late experiment is not a safe general solution.
- A raw `c->1` pair with an early outlier and two equal late samples was the
  only repeated transition-specific exception: eight events at three sites,
  all matching the late output. The other apparently safe keys had at most
  seven observations and were not promoted to live tests.

### Experiments C11–C12 — targeted `c->1` exception

| ID | Change | Robust ppm | Modal mismatch | Decision |
|---|---|---:|---:|---|
| C11 `experiment-20260828-223932-1a66cb8b` | accept settled-late `c->1` | 49.3580 | 0 | Repeat required |
| C11r `experiment-20260828-223949-da3a14ea` | same, repeat | 48.2148 | 0 | Repeat looked better than the old 50.08 ppm checkpoint |
| C11l `experiment-20260828-225417-ceed4697` | same, 300 frames | 46.7355 | 0 | Reject: disputed pixel changed in 213/299 frame transitions |
| control `experiment-20260828-224307-b6d1f6d6` | contemporaneous confidence guard | 39.8915 | 1 filled | Reject as control: low score came from freezing the disputed pixel to the wrong modal color |
| C12 `experiment-20260828-224825-959f30b9` | require same accepted pair in prior frame | 44.9633 | 0 | Reject: disputed pixel still changed in 64/119 transitions and requires frame history |

The single disputed source pixel was viewer coordinate `(321, 472)`. C11
showed its reference color in 74/120 frames and C12 in 70/120, while the first
control showed it in only 30/120 and therefore appeared steadier by freezing
to the wrong color. A later exact confidence-candidate control showed 71/49 observations and
68 changes over 119 transitions. The 300-frame C11 run showed 187/113
observations but 213 changes over 299 transitions (71.2%). This proves a
threshold tradeoff rather than a universally correct endpoint: accepting the
late half-dot repairs modal geometry but exposes the unstable analog boundary.
Both firmware candidates were removed.

### Raw checkpoint R3 — measured source line rate

- Nineteen valid 252 MHz diagnostic sessions supplied 2,214 complete line
  periods. The combined mean was about 16,125.175 diagnostic ticks versus the
  nominal 16,128 ticks, so this P2000T source is approximately 175.19 ppm
  faster than nominal.
- Physical-even lines averaged 16,125.1105 ticks and physical-odd lines
  16,125.2399 ticks. The 0.1293-tick difference is about 0.5 ns and rules out
  materially different odd/even line lengths.
- The ordinary PIO rate-trim step is 1/256 of divider 2, approximately 1,953
  ppm. It is therefore roughly eleven times too coarse to track this measured
  source-rate error.

### Experiment C13 — fine system-clock rate lock

RP2350 can express a much finer rate correction in its fractional system-clock
divider. Offline replay modeled +169 ppm and phase +1. Preserving only the
repeated settled `d->c` and `d->9` transitions removed all modal-reference
errors across the 1,013,760-observation corpus. This justified a live trial,
but did not justify retaining it without live confirmation.

An initial PLL trial was deliberately discarded after a raw clock check found
16,064.7 samples per line. The requested 1,530 MHz VCO was not an integer
multiple of the 12 MHz crystal and had been realized as 1,524 MHz. The corrected
build used an exact 1,536 MHz VCO and measured 16,127.92 samples per line
(median 16,128), confirming that the intended rate lock was physically present.

| Run | Live tuple | Robust ppm | Modal mismatch | Decision |
|---|---|---:|---:|---|
| `experiment-20260828-231239-0472237f` | phase 0, odd +1 | 762.2975 | 69 | Reject |
| `experiment-20260828-231255-3e5cc6d3` | phase +1, odd +1 | 84.2821 | 2 | Best rate-locked tuple, still worse than the confidence candidate |
| `experiment-20260828-231316-fefbe374` | phase +1, odd 0 | 130.6801 | 18 | Reject |
| `experiment-20260828-231324-fa6375df` | phase +1, odd +2 | 479.9934 | 56 | Reject |
| `experiment-20260828-231545-dbef6eed` | restored confidence candidate | 49.8400 | 0 | Keep |

All C13 runs had zero deadline-miss frames. The experiment shows that matching
the average source line rate does not improve this capture: it shifts which
analog boundaries enter the three-sample aperture and creates more unstable or
incorrect decisions. The Pico and firmware source were restored to exact
the withdrawn confidence-candidate behavior. Further temporal filtering would suppress visible glitter
only by adding stale pixels or motion blur, failure modes already demonstrated
by C9/C12, so no narrower firmware correction was retained.
