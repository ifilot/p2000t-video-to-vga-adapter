# SAA5050 screen-test cartridge

This directory builds a signed 16 KiB SLOT1 cartridge image for the P2000T.
It renders the static 24-by-40 SAA5050 test page recovered from the `DATA`
statements in the tokenized BASIC cassette
[`SAA5050 Test Page.cas`](https://github.com/p2000t/software/blob/main/cassettes/Grafische%20demo%27s/SAA5050%20Test%20Page.cas).
The control bytes are described in the
[P2000T SAA5050 overview](https://www.retrospace.nl/Philips_P2000T_homecomputer.html).

Build and validate it with:

```sh
make
make verify
```

The final flashable image is `screentest.bin`. The build requires `z80asm`
and Python 3. The signer covers the first 8 KiB bank using the count and
16-bit checksum fields in the P2000 cartridge header.
