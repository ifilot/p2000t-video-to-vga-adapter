# Engineering-screen autotune

The viewer can run a complete sampling experiment without manual setting
changes. Load `screentest/screentest.bin` on the P2000T, leave that static page
visible, connect the viewer, and choose **Adapter > Run engineering-screen
autotune**.

## What the run changes

The experiment is deliberately staged instead of taking a Cartesian product
of every control:

1. It forces raw reconstruction and tests all 21 sample phases against all 17
   horizontal rate trims. Physical even source lines select the shared base
   phase and rate.
2. It holds that winner fixed and tests all 21 odd-line phase corrections.
   Only the physical odd-line score selects this correction. The viewer maps
   physical parity through the configured first visible source line, so an odd
   starting line does not accidentally reverse the comparison.
3. It captures a longer validation run using the combined winner.

Raw reconstruction is mandatory during this experiment. A reconstruction mode
which duplicates samples can reduce temporal differences simply by discarding
horizontal information; such a blurred image must not win a sampling test.

The successful settings remain active but are not saved to flash. Canceling,
disconnecting, or encountering an error restores phase, rate trim, odd-line
phase, and reconstruction mode to their original live values.

## Score

For each setting, the viewer builds a per-pixel modal image from the retained
frames. It evaluates one scanline from each vertically duplicated viewer pair
and records:

- samples that differ from the per-pixel temporal mode, split by logical
  odd/even row;
- the median per-frame mismatch count and a separate robust instability value
  which excludes coherent high-change frames while retaining their count;
- one-pixel islands whose two horizontal neighbors agree;
- one-pixel third colors which match neither side of a color transition;
- the modal image's horizontal transition count as a sharpness diagnostic.

The primary score is:

```text
instability_ppm + 4 * one_dot_artifact_ppm
```

All raw measurements and the weighting are logged. The algorithm selects the
lowest physical-even score for base phase/rate and the lowest physical-odd
score for odd-line phase. An exact tie prefers the smallest total absolute
correction. The longer validation score is reported rather than used to search
again.

This is an objective engineering heuristic, not a claim that every visible
defect has one cause. The preserved captures and modal images allow the score
and winner to be challenged or reanalyzed later.

The ordinary instability total remains authoritative for the autotune score.
The robust fields are additional diagnostics: they prevent a periodic source
animation, such as a flashing SAA5050 test region, from being mistaken for a
sampling regression while ensuring those frames remain visible in the log.

## Output directory

Each run creates `p2000t-autotune-YYYYMMDD-HHMMSS` beneath the selected parent:

- `session.json`: viewer version, run parameters, original settings, source
  line parity mapping, reconstruction constraint, and score formula;
- `frames.csv`: exact setting, stage, source-frame sequence, Pico capture
  timestamp, host UTC receipt time, and filename for every retained PNG;
- `candidates.csv`: raw and normalized temporal/spatial metrics for every
  candidate plus its modal PNG filename;
- `modal_*.png`: temporal modal image for every candidate;
- `result.json`: phase/rate winner, final recommended settings, validation
  metrics, selection rule, and explicit `written_to_flash: false` state;
- labeled `*.png` files: all retained source frames, including validation.

The default run evaluates 378 search candidates with eight retained frames
each, followed by 100 validation frames. At a 25 FPS viewer stream it takes a
little over two minutes plus PNG and USB overhead and writes 3,124 retained
frames. Increasing frames per candidate improves sensitivity to rare glitches
at the cost of a proportionally longer run.
