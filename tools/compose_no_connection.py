#!/usr/bin/env python3
"""Compose the generated P2000T sprite onto the 640x480 screen canvas."""

from pathlib import Path
import sys

from PIL import Image


CANVAS_SIZE = (640, 480)
SPRITE_SIZE = (320, 320)
SPRITE_TOP = 64


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} SPRITE.png OUTPUT.png", file=sys.stderr)
        return 2

    sprite_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    with Image.open(sprite_path) as source:
        sprite = source.convert("RGBA")

    # Ignore nearly transparent generation residue when finding the subject;
    # otherwise invisible edge pixels make the computer needlessly smaller.
    alpha_bounds = sprite.getchannel("A").point(
        lambda alpha: 255 if alpha >= 16 else 0
    ).getbbox()
    if alpha_bounds is None:
        raise ValueError("sprite is fully transparent")
    sprite = sprite.crop(alpha_bounds)
    sprite.thumbnail((312, 312), Image.Resampling.LANCZOS)

    square = Image.new("RGBA", SPRITE_SIZE, (0, 0, 0, 0))
    square.alpha_composite(
        sprite,
        ((SPRITE_SIZE[0] - sprite.width) // 2,
         (SPRITE_SIZE[1] - sprite.height) // 2),
    )

    canvas = Image.new("RGB", CANVAS_SIZE, "black")
    canvas.paste(
        square,
        ((CANVAS_SIZE[0] - SPRITE_SIZE[0]) // 2, SPRITE_TOP),
        square,
    )
    canvas.save(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
