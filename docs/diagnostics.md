# High-resolution diagnostic captures

The Pico 2 diagnostic recorder is a lossless logic-analyzer path intended for
studying horizontal timing, odd/even-line displacement, and unstable RGB
transitions. It runs alongside the normal capture engine on the RP2350's PIO2.

Use **Adapter > Record high-resolution diagnostics** in the viewer. Select a
physical source line, one to sixteen contiguous lines, and the number of
repeated bursts. Keep the P2000T image unchanged until recording completes.

Each session contains:

- `session.json`: requested acquisition and active alignment settings;
- `capture.p2td`: authoritative, append-only binary records;
- `manifest.csv`: byte offsets and metadata for every validated record.

The first payload is a roughly 22 ms, 63 MHz one-bit CSYNC trace. It is followed
by repeated RGBS bursts sampled at 126 MHz. Each nominal scanline contributes
8064 four-bit observations, so a sixteen-line burst contains 129024 samples and
64512 payload bytes. RGB and CSYNC are conditioned, active-low/active-high input
values exactly as seen at the GPIO pins; no reconstruction or palette mapping
is applied.

The firmware timestamps the sampler trigger and DMA completion independently.
Both measured and expected capture durations are stored in every data header;
an overrun flag is set when the measured interval exceeds the expected interval
by more than 50 microseconds. This makes memory-bus stalls visible in the saved
evidence rather than silently accepting a distorted time axis.

A second DMA channel is chained to each acquisition and disables both PIO2
state machines at the exact requested word count. Because the sampler cannot
then stall merely from continuing past the end of its buffer, the PIO RX-stall
flag in a data record identifies a genuine acquisition-time FIFO stall. Treat
any record carrying either timing-integrity flag as suspect during analysis.

## Binary format

Every record starts with a 72-byte little-endian header and the magic `P2DG`.
Payload words are little-endian `uint32_t` values. Samples within each word are
chronological from its most significant bits: 32 one-bit CSYNC observations or
eight four-bit RGBS nibbles. The payload has a standard reflected CRC-32.
Header offsets and record identifiers are defined in
`src/p2000t_diagnostic_protocol.h`.

The stream contains, in order, a session record, one timing record, one raw
record per requested repetition, and a complete record. A cancelled session
still ends with a CRC-protected complete record carrying the cancelled flag;
already written data remains valid.

## Initial analysis

Run:

```sh
python3 tools/analyze_diagnostics.py path/to/p2000t-diagnostics-*/
```

The script validates every header, sequence number, session identifier, payload
length, and CRC before producing `analysis/timing-edges.csv`,
`analysis/raw-sync-edges.csv`, `analysis/unstable-samples.csv`, and
`analysis/summary.json`. Preserve `capture.p2td`; derived CSV files can always
be regenerated.
