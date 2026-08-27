#!/usr/bin/env python3
"""Prepare concept art exactly as the compact scanvideo pipeline sees it."""

from pathlib import Path
import struct
import sys

from PIL import Image, ImageDraw, ImageFont


VGA_SIZE = (640, 480)
LOGICAL_SIZE = (640, 240)
PALETTE_COLORS = 16
ASSET_BYTES = PALETTE_COLORS * 2 + LOGICAL_SIZE[0] * LOGICAL_SIZE[1] // 2


def rgb444(red: int, green: int, blue: int) -> int:
    """Convert one native-depth RGB entry to the firmware's RGB444 layout."""
    return (red >> 4) | ((green >> 4) << 4) | ((blue >> 4) << 8)


def prepare(source_path: Path, preview_path: Path, asset_path: Path) -> int:
    """Generate a device preview and matching 4-bpp firmware asset."""
    with Image.open(source_path) as source:
        source = source.convert("RGB").resize(VGA_SIZE, Image.Resampling.LANCZOS)
        logical = source.resize(LOGICAL_SIZE, Image.Resampling.LANCZOS)

    native_depth = logical.point(lambda sample: (sample >> 4) * 17)
    indexed = native_depth.quantize(
        colors=PALETTE_COLORS,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.FLOYDSTEINBERG,
    )

    palette = indexed.getpalette()[: PALETTE_COLORS * 3]
    indices = indexed.tobytes()
    if any(index >= PALETTE_COLORS for index in indices):
        raise ValueError(f"{source_path}: palette index exceeds four bits")

    packed_palette = []
    device_palette = []
    for offset in range(0, len(palette), 3):
        packed = rgb444(*palette[offset : offset + 3])
        packed_palette.append(packed)
        device_palette.extend(
            [
                (packed & 0x0F) * 17,
                ((packed >> 4) & 0x0F) * 17,
                ((packed >> 8) & 0x0F) * 17,
            ]
        )

    with asset_path.open("wb") as output:
        for packed in packed_palette:
            output.write(struct.pack("<H", packed))
        for offset in range(0, len(indices), 2):
            output.write(bytes([(indices[offset] << 4) | indices[offset + 1]]))

    device_indexed = indexed.copy()
    device_indexed.putpalette(device_palette + [0] * (768 - len(device_palette)))
    preview = device_indexed.convert("RGB").resize(
        VGA_SIZE, Image.Resampling.NEAREST
    )
    preview.save(preview_path)
    colors = preview.getcolors(maxcolors=PALETTE_COLORS + 1)
    if colors is None or len(colors) > PALETTE_COLORS:
        raise ValueError(f"{preview_path}: preview exceeds {PALETTE_COLORS} colors")
    if any(channel % 17 for _, color in colors for channel in color):
        raise ValueError(f"{preview_path}: preview contains a non-RGB444 color")

    # Decode the serialized asset independently and require a byte-identical
    # match with the preview. This exercises palette byte order and nibble
    # order in addition to checking dimensions and color count.
    payload = asset_path.read_bytes()
    if len(payload) != ASSET_BYTES:
        raise ValueError(f"{asset_path}: expected {ASSET_BYTES} bytes")
    decoded_palette = []
    for packed in struct.unpack_from("<16H", payload):
        decoded_palette.extend(
            [
                (packed & 0x0F) * 17,
                ((packed >> 4) & 0x0F) * 17,
                ((packed >> 8) & 0x0F) * 17,
            ]
        )
    decoded_indices = []
    for packed in payload[PALETTE_COLORS * 2 :]:
        decoded_indices.extend([packed >> 4, packed & 0x0F])
    decoded = Image.new("P", LOGICAL_SIZE)
    decoded.putpalette(decoded_palette + [0] * (768 - len(decoded_palette)))
    decoded.putdata(decoded_indices)
    decoded = decoded.convert("RGB").resize(VGA_SIZE, Image.Resampling.NEAREST)
    if decoded.tobytes() != preview.tobytes():
        raise ValueError(f"{asset_path}: decoded pixels do not match preview")
    return len(colors)


def make_contact_sheet(previews: list[Path], destination: Path) -> None:
    """Create a compact overview while leaving full previews untouched."""
    columns = 5
    thumb_size = (320, 240)
    label_height = 24
    rows = (len(previews) + columns - 1) // columns
    sheet = Image.new(
        "RGB", (columns * thumb_size[0], rows * (thumb_size[1] + label_height)), "black"
    )
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()
    for index, preview_path in enumerate(previews):
        with Image.open(preview_path) as preview:
            thumbnail = preview.convert("RGB").resize(thumb_size, Image.Resampling.NEAREST)
        left = (index % columns) * thumb_size[0]
        top = (index // columns) * (thumb_size[1] + label_height)
        sheet.paste(thumbnail, (left, top))
        draw.text((left + 6, top + thumb_size[1] + 5), preview_path.stem,
                  fill="white", font=font)
    sheet.save(destination)


def main() -> int:
    if len(sys.argv) != 4:
        print(
            f"usage: {sys.argv[0]} SOURCE_DIR PREVIEW_DIR ASSET_DIR",
            file=sys.stderr,
        )
        return 2

    source_dir, preview_dir, asset_dir = map(Path, sys.argv[1:])
    preview_dir.mkdir(parents=True, exist_ok=True)
    asset_dir.mkdir(parents=True, exist_ok=True)
    sources = sorted(source_dir.glob("*.png"))
    if not sources:
        raise ValueError(f"no PNG concepts found in {source_dir}")

    previews = []
    for source_path in sources:
        preview_path = preview_dir / source_path.name
        asset_path = asset_dir / f"{source_path.stem}.idx4"
        color_count = prepare(source_path, preview_path, asset_path)
        previews.append(preview_path)
        print(
            f"{source_path.stem}: {color_count} RGB444 colors, "
            f"{asset_path.stat().st_size} bytes"
        )

    make_contact_sheet(previews, preview_dir.parent / "contact_sheet.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
