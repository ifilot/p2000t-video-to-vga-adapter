# TODO

## Remaining P2000T sampling glitches

Firmware v0.8.6 captures reliably again with the expensive eye-quality
statistics disabled during normal operation. The observed fast-path status is:

- locked at approximately 49.768 Hz with a 20.093 ms source-frame period;
- maximum line decoding time of 13 us, safely below the 64 us line period;
- first visible line 57 and horizontal source-window offset 48;
- five-tap majority sampling at 126 MHz from a 252 MHz RP2040 clock.

Two consecutive 21-phase eye scans selected global phase `+5`. Their minima
were 434 ppm and 472 ppm respectively, so this result is repeatable. The scan
range is `-10` through `+10`, which means `+5` is not actually at the edge.
All three channels independently selected the same phase and centered tap
window (`R+0/G+0/B+0`). This argues against fixed, channel-specific input
delays being the cause of the remaining wrong or black pixels.

The important anomaly is that the preferred phase varies horizontally. The
first scan reported these twelve horizontal eye centres:

```text
+2 +3 +4 +2 +6 +3 +4 +5 +0 +5 +4 -5
```

The repeated scan was nearly identical:

```text
+2 +3 +4 +2 +6 +3 +4 +5 +0 +4 +4 -5
```

Most of the scanline prefers approximately `+2` through `+6`, while its last
region prefers `-5`. This is consistent with sampling phase drifting across a
line, probably due to a small mismatch between the assumed 6 MHz/21-tick dot
period and the real P2000T dot timing. A fixed global phase can therefore move
the glitches but cannot eliminate them everywhere.

### Next experiment

Measure the source line/dot timing more precisely and test horizontal phase
compensation. Possible implementations are:

1. Distribute occasional 20- or 22-tick source dots among the normal 21-tick
   dots to correct accumulated phase error across a scanline.
2. Derive that correction from measured source-line timing instead of assuming
   exactly 21 capture ticks for every dot.
3. Keep the five-tap majority decoder, initially using global phase `+5` and
   centered channel windows as the known-good baseline.

Do not interpret loss of VGA lock during an `a` eye scan as a capture failure:
calibration deliberately enables the slower statistics path for about 28
seconds. Normal capture must return to `locked=yes` afterward.
