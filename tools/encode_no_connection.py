#!/usr/bin/env python3
"""Encode no-connection artwork as a 16-color RGB444 indexed image."""

from pathlib import Path
import struct
import sys

from PIL import Image


VGA_SIZE = (640, 480)
RENDER_SIZE = (640, 240)


def rgb444(red: int, green: int, blue: int) -> int:
    """Convert one RGB888 palette entry to the firmware's RGB444 layout."""
    return (red >> 4) | ((green >> 4) << 4) | ((blue >> 4) << 8)


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(
            f"usage: {sys.argv[0]} INPUT.png OUTPUT.idx4 [PREVIEW.png]",
            file=sys.stderr,
        )
        return 2

    source = Path(sys.argv[1])
    destination = Path(sys.argv[2])
    with Image.open(source) as image:
        if image.size != VGA_SIZE:
            raise ValueError(f"expected a 640x480 image, got {image.width}x{image.height}")
        image = image.convert("RGB").resize(RENDER_SIZE, Image.Resampling.LANCZOS)
        # Quantize values to the DAC's native channel depth first. Otherwise
        # distinct RGB888 palette entries can collapse to the same RGB444
        # color when serialized, needlessly reducing the effective palette.
        native_depth = image.point(lambda sample: (sample >> 4) * 17)
        indexed = native_depth.quantize(
            colors=16,
            method=Image.Quantize.MEDIANCUT,
            dither=Image.Dither.FLOYDSTEINBERG,
        )

    palette = indexed.getpalette()[: 16 * 3]
    indices = indexed.tobytes()
    if any(index >= 16 for index in indices):
        raise ValueError("quantizer produced an out-of-range palette index")

    with destination.open("wb") as output:
        for offset in range(0, len(palette), 3):
            output.write(struct.pack("<H", rgb444(*palette[offset : offset + 3])))
        for offset in range(0, len(indices), 2):
            output.write(bytes([(indices[offset] << 4) | indices[offset + 1]]))

    if len(sys.argv) == 4:
        preview = indexed.convert("RGB").resize(VGA_SIZE, Image.Resampling.NEAREST)
        preview.save(sys.argv[3])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
