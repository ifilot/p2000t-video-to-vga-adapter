#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Sign and verify a 16 KiB P2000 SLOT1 cartridge image."""

from __future__ import annotations

import argparse
from pathlib import Path


ROM_SIZE = 0x4000
BANK_SIZE = 0x2000
HEADER_SIZE = 5
SIGNATURE_MASK = 0xF5
SIGNATURE_VALUE = 0x54


def validate_layout(image: bytes) -> None:
    if len(image) != ROM_SIZE:
        raise ValueError(
            f"expected a {ROM_SIZE}-byte cartridge image, got {len(image)} bytes"
        )

    signature = image[0]
    if signature & SIGNATURE_MASK != SIGNATURE_VALUE or signature & 0x01:
        raise ValueError(f"invalid P2000 cartridge signature 0x{signature:02x}")
    if not signature & 0x08:
        raise ValueError("signature does not select the 16 KiB SLOT1 layout")


def sign_cartridge(image: bytes) -> tuple[bytes, int, int]:
    """Return a signed image, its covered byte count, and checksum."""
    validate_layout(image)
    signed = bytearray(image)
    byte_count = BANK_SIZE - HEADER_SIZE
    signed[1:3] = byte_count.to_bytes(2, "little")
    signed[3:5] = b"\x00\x00"
    checksum = (-sum(signed[HEADER_SIZE:BANK_SIZE])) & 0xFFFF
    signed[3:5] = checksum.to_bytes(2, "little")
    return bytes(signed), byte_count, checksum


def verify_cartridge(image: bytes) -> tuple[int, int]:
    """Validate the SLOT1 header and return its count and checksum."""
    validate_layout(image)
    byte_count = int.from_bytes(image[1:3], "little")
    checksum = int.from_bytes(image[3:5], "little")
    if byte_count != BANK_SIZE - HEADER_SIZE:
        raise ValueError(f"unexpected checksum byte count 0x{byte_count:04x}")
    if (checksum + sum(image[HEADER_SIZE:BANK_SIZE])) & 0xFFFF:
        raise ValueError("invalid first-bank checksum")
    return byte_count, checksum


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", help="verify an image")
    parser.add_argument("input", type=Path, help="input cartridge image")
    parser.add_argument("output", nargs="?", type=Path, help="signed output image")
    args = parser.parse_args()

    image = args.input.read_bytes()
    if args.verify:
        if args.output is not None:
            parser.error("--verify accepts only one image")
        count, checksum = verify_cartridge(image)
        print(
            f"Verified {args.input}: count=0x{count:04x}, "
            f"checksum=0x{checksum:04x}"
        )
        return

    if args.output is None:
        parser.error("signing requires an output image")
    signed, count, checksum = sign_cartridge(image)
    args.output.write_bytes(signed)
    print(
        f"Signed {args.output}: count=0x{count:04x}, "
        f"checksum=0x{checksum:04x}"
    )


if __name__ == "__main__":
    main()

