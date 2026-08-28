#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Validate and summarize a viewer-produced P2000T diagnostic session."""

import argparse
import csv
import json
import struct
import sys
import zlib
from array import array
from pathlib import Path

MAGIC = b"P2DG"
VERSION = 1
HEADER_SIZE = 72
SESSION, TIMING, RAW_RGBS, COMPLETE = range(1, 5)
TYPE_NAMES = {
    SESSION: "session",
    TIMING: "timing",
    RAW_RGBS: "raw_rgbs",
    COMPLETE: "complete",
}
TIMING_RATE = 63_000_000
RAW_RATE = 126_000_000
SAMPLES_PER_LINE = 8064
TIMING_WORDS = 43320
TIMING_SAMPLES = TIMING_WORDS * 32
TIMING_BYTES = TIMING_WORDS * 4


def u16(header, offset):
    return struct.unpack_from("<H", header, offset)[0]


def i16(header, offset):
    return struct.unpack_from("<h", header, offset)[0]


def u32(header, offset):
    return struct.unpack_from("<I", header, offset)[0]


def read_records(path):
    records = []
    with path.open("rb") as stream:
        expected_sequence = 1
        session_id = None
        while True:
            offset = stream.tell()
            header = stream.read(HEADER_SIZE)
            if not header:
                break
            if len(header) != HEADER_SIZE:
                raise ValueError(f"truncated header at byte {offset}")
            if header[:4] != MAGIC or header[4] != VERSION:
                raise ValueError(f"invalid record magic/version at byte {offset}")
            if u16(header, 6) != HEADER_SIZE:
                raise ValueError(f"invalid header size at byte {offset}")
            record_type = header[5]
            if record_type not in TYPE_NAMES:
                raise ValueError(f"unknown record type {record_type}")
            sequence = u32(header, 12)
            if sequence != expected_sequence:
                raise ValueError(
                    f"record sequence {sequence}, expected {expected_sequence}"
                )
            expected_sequence += 1
            payload_size = u32(header, 16)
            payload = stream.read(payload_size)
            if len(payload) != payload_size:
                raise ValueError(f"truncated payload for record {sequence}")
            actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
            if actual_crc != u32(header, 20):
                raise ValueError(f"CRC-32 failure in record {sequence}")
            current_session = u32(header, 24)
            if session_id is None:
                session_id = current_session
            elif current_session != session_id:
                raise ValueError(f"session id changed in record {sequence}")
            record = {
                "offset": offset,
                "type": record_type,
                "sequence": sequence,
                "flags": u32(header, 8),
                "payload": payload,
                "payload_size": payload_size,
                "session_id": current_session,
                "sample_rate_hz": u32(header, 28),
                "sample_count": u32(header, 32),
                "start_line": u16(header, 36),
                "line_count": u16(header, 38),
                "repetition": u16(header, 40),
                "repetitions": u16(header, 42),
                "samples_per_line": u16(header, 44),
                "bits_per_sample": header[46],
                "sync_channel": header[47],
                "first_visible_line": u16(header, 48),
                "horizontal_offset": u16(header, 50),
                "phase": i16(header, 52),
                "odd_phase": i16(header, 54),
                "rate_trim": i16(header, 56),
                "channel_map": header[58],
                "capture_sequence": u32(header, 60),
                "capture_duration_us": u32(header, 64),
                "expected_duration_us": u32(header, 68),
            }
            if record_type in (SESSION, COMPLETE):
                valid = payload_size == 0 and record["sample_count"] == 0
            elif record_type == TIMING:
                valid = (
                    payload_size == TIMING_BYTES
                    and record["sample_rate_hz"] == TIMING_RATE
                    and record["sample_count"] == TIMING_SAMPLES
                    and record["bits_per_sample"] == 1
                )
            else:
                valid = (
                    1 <= record["line_count"] <= 16
                    and record["sample_rate_hz"] == RAW_RATE
                    and record["sample_count"]
                    == record["line_count"] * SAMPLES_PER_LINE
                    and payload_size == record["sample_count"] // 2
                    and record["bits_per_sample"] == 4
                    and 1 <= record["repetition"] <= record["repetitions"]
                )
            if not valid:
                raise ValueError(f"invalid metadata in record {sequence}")
            records.append(record)
    if not records or records[0]["type"] != SESSION:
        raise ValueError("session record is missing")
    timing_seen = False
    expected_repetition = 1
    for index, record in enumerate(records[1:], 1):
        if record["type"] == TIMING:
            if timing_seen or expected_repetition != 1:
                raise ValueError("timing record is out of order")
            timing_seen = True
        elif record["type"] == RAW_RGBS:
            if not timing_seen or record["repetition"] != expected_repetition:
                raise ValueError("raw diagnostic repetition is out of order")
            expected_repetition += 1
        elif record["type"] == COMPLETE and index != len(records) - 1:
            raise ValueError("complete record is not last")
        elif record["type"] == SESSION:
            raise ValueError("duplicate session record")
    return records


def unpack_msb_samples(payload, bits_per_sample):
    per_word = 32 // bits_per_sample
    mask = (1 << bits_per_sample) - 1
    for (word,) in struct.iter_unpack("<I", payload):
        for index in range(per_word):
            yield (word >> (32 - bits_per_sample * (index + 1))) & mask


def timing_edges(record):
    bits = unpack_msb_samples(record["payload"], 1)
    previous = next(bits, None)
    rising = []
    falling = []
    for index, value in enumerate(bits, 1):
        if value != previous:
            (rising if value else falling).append(index)
            previous = value
    return rising, falling


def write_timing(records, output):
    path = output / "timing-edges.csv"
    periods = []
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "record_sequence",
                "edge_index",
                "rising_sample",
                "period_samples",
                "period_us",
                "high_samples",
                "high_us",
            ]
        )
        for record in records:
            if record["type"] != TIMING:
                continue
            rising, falling = timing_edges(record)
            fall_index = 0
            for edge_index, start in enumerate(rising):
                while fall_index < len(falling) and falling[fall_index] <= start:
                    fall_index += 1
                high = (
                    falling[fall_index] - start
                    if fall_index < len(falling)
                    else None
                )
                period = start - rising[edge_index - 1] if edge_index else None
                if period is not None:
                    periods.append(period)
                writer.writerow(
                    [
                        record["sequence"],
                        edge_index,
                        start,
                        "" if period is None else period,
                        ""
                        if period is None
                        else f"{period * 1e6 / record['sample_rate_hz']:.6f}",
                        "" if high is None else high,
                        ""
                        if high is None
                        else f"{high * 1e6 / record['sample_rate_hz']:.6f}",
                    ]
                )
    return path, periods


def write_raw_summaries(records, output):
    raw_records = [record for record in records if record["type"] == RAW_RGBS]
    if not raw_records:
        return None, None, 0
    sample_count = raw_records[0]["sample_count"]
    counts = [array("H", [0]) * sample_count for _ in range(16)]
    sync_path = output / "raw-sync-edges.csv"
    with sync_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "record_sequence",
                "repetition",
                "edge_index",
                "rising_sample",
                "period_samples",
                "period_us",
            ]
        )
        for record in raw_records:
            if record["sample_count"] != sample_count:
                raise ValueError("raw sample count changed during the session")
            previous_sync = None
            rising = []
            for index, nibble in enumerate(
                unpack_msb_samples(record["payload"], 4)
            ):
                counts[nibble][index] += 1
                sync = (nibble >> record["sync_channel"]) & 1
                if previous_sync == 0 and sync == 1:
                    rising.append(index)
                previous_sync = sync
            for edge_index, start in enumerate(rising):
                period = start - rising[edge_index - 1] if edge_index else None
                writer.writerow(
                    [
                        record["sequence"],
                        record["repetition"],
                        edge_index,
                        start,
                        "" if period is None else period,
                        ""
                        if period is None
                        else f"{period * 1e6 / record['sample_rate_hz']:.6f}",
                    ]
                )

    unstable_path = output / "unstable-samples.csv"
    unstable = 0
    with unstable_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "sample_index",
                "nominal_line_offset",
                "tick_within_nominal_line",
                "distinct_values",
                "modal_nibble_hex",
                "modal_count",
                "observations",
            ]
            + [f"nibble_{value:x}" for value in range(16)]
        )
        samples_per_line = raw_records[0]["samples_per_line"]
        for index in range(sample_count):
            values = [counts[value][index] for value in range(16)]
            distinct = sum(value != 0 for value in values)
            if distinct <= 1:
                continue
            unstable += 1
            modal = max(range(16), key=lambda value: values[value])
            writer.writerow(
                [
                    index,
                    index // samples_per_line,
                    index % samples_per_line,
                    distinct,
                    f"{modal:x}",
                    values[modal],
                    sum(values),
                ]
                + values
            )
    return sync_path, unstable_path, unstable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "session",
        type=Path,
        help="diagnostic directory or its capture.p2td file",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="output directory (default: SESSION/analysis)",
    )
    arguments = parser.parse_args()
    capture = (
        arguments.session / "capture.p2td"
        if arguments.session.is_dir()
        else arguments.session
    )
    output = arguments.output or capture.parent / "analysis"
    output.mkdir(parents=True, exist_ok=True)
    records = read_records(capture)
    timing_path, periods = write_timing(records, output)
    sync_path, unstable_path, unstable = write_raw_summaries(records, output)
    raw_records = [record for record in records if record["type"] == RAW_RGBS]
    summary = {
        "capture_file": str(capture),
        "session_id": records[0]["session_id"],
        "records": len(records),
        "raw_bursts": len(raw_records),
        "cancelled": bool(records[-1]["flags"] & 2),
        "timing_horizontal_period_samples_median": (
            sorted(periods)[len(periods) // 2] if periods else None
        ),
        "unstable_raw_sample_positions": unstable,
        "capture_overrun_records": [
            record["sequence"] for record in records if record["flags"] & 16
        ],
        "pio_rx_stall_records": [
            record["sequence"] for record in records if record["flags"] & 32
        ],
        "outputs": [
            str(path)
            for path in (timing_path, sync_path, unstable_path)
            if path is not None
        ],
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
