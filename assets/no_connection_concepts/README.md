# No-connection screen concepts

These ten alternatives were generated as deliberately different retro visual
directions and then processed through the firmware's real display model.

## What to review

- `contact_sheet.png` provides an overview of all ten concepts.
- `device_previews/` contains the final 640×480 images as they will appear
  after the 640×240 logical scanline conversion and vertical line doubling.
- `firmware_assets/` contains matching 4-bpp indexed assets. Each file has a
  16-entry RGB444 palette followed by 320 packed bytes for each of 240 logical
  scanlines, for a total of 76,832 bytes.
- `sources/` preserves the full-resolution imagegen output for later editing.
- `PROMPTS.md` records the exact built-in imagegen prompts.

Every device preview has exactly 16 colors, every channel is on an RGB444
step, and every matching firmware asset was decoded and compared with its
preview.

| Number | Direction |
| --- | --- |
| 01 | Isometric 8-bit grid |
| 02 | Cyan technical blueprint |
| 03 | Teletext mosaic |
| 04 | Green phosphor terminal |
| 05 | Synthwave arcade |
| 06 | Dutch modernist geometry |
| 07 | Pop-art comic screenprint |
| 08 | Amber circuit schematic |
| 09 | 1980s paper collage |
| 10 | One-bit desktop alert |

Regenerate all device previews and firmware assets with:

```sh
python3 tools/prepare_no_connection_concepts.py \
  assets/no_connection_concepts/sources \
  assets/no_connection_concepts/device_previews \
  assets/no_connection_concepts/firmware_assets
```
