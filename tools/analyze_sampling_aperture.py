#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Replay alternative three-sample apertures from a raw P2000T trace."""

import argparse
import json
import statistics
from collections import Counter
from itertools import combinations
from pathlib import Path

from PIL import Image

from analyze_diagnostics import RAW_RGBS, read_records, unpack_msb_samples


def rising_edges(samples, sync_channel):
    result = []
    previous = (samples[0] >> sync_channel) & 1
    for index, sample in enumerate(samples[1:], 1):
        current = (sample >> sync_channel) & 1
        if previous == 0 and current == 1:
            result.append(index)
        previous = current
    return result


def color_late(first, center, last):
    if first == center or first == last:
        return first
    if center == last:
        return center
    return last


def reconstruct_line(samples, origin, start, offsets, tick_scale,
                     confidence_guard, guard_rule):
    output = []
    uncertain = []
    for x in range(480):
        center = (origin + start + (x // 2) * 21 * tick_scale +
                  (0 if x % 2 == 0 else 10 * tick_scale) + tick_scale)
        window = [samples[center + offset] for offset in offsets]
        output.append(color_late(*window))
        uncertain.append(
            len(set(window)) == 3 if guard_rule == "ambiguous"
            else window[1] != window[2] if guard_rule == "late"
            else window[0] != window[1] or window[1] != window[2]
        )
    if confidence_guard:
        for x in range(0, 480, 2):
            if output[x] == output[x + 1] or uncertain[x] == uncertain[x + 1]:
                continue
            if uncertain[x]:
                output[x] = output[x + 1]
            else:
                output[x + 1] = output[x]
    return output


def modal_frame(frames):
    return [Counter(values).most_common(1)[0][0] for values in zip(*frames)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--start-tick", type=int, default=2152,
                        help="first sample tick on physical-even lines")
    parser.add_argument("--first-line", type=int, default=287)
    parser.add_argument("--last-line", type=int, default=293)
    parser.add_argument("--spacings", type=int, nargs="+")
    parser.add_argument("--search-radius", type=int,
                        help="also exhaustively test triplets in -R..+R")
    parser.add_argument("--reference-png", type=Path,
                        help="480x480 viewer modal used for spatial fidelity")
    parser.add_argument("--details", type=int, default=0,
                        help="report the N most unstable baseline pixels")
    parser.add_argument("--guard-rule", choices=("any", "ambiguous", "late"),
                        default="any")
    arguments = parser.parse_args()

    capture = arguments.session / "capture.p2td" if arguments.session.is_dir() else arguments.session
    records = [record for record in read_records(capture)
               if record["type"] == RAW_RGBS]
    if not records:
        raise ValueError("no raw records")
    first_physical = records[0]["start_line"]
    first_visible = records[0]["first_visible_line"]
    tick_scale = records[0]["sample_rate_hz"] // 126_000_000
    if tick_scale not in (1, 2):
        raise ValueError("raw rate must be 126 or 252 MHz")
    spacings = arguments.spacings or [tick_scale * value for value in (1, 2, 3, 4)]
    selected = range(arguments.first_line, arguments.last_line + 1)
    apertures = {f"spacing-{spacing}": (-spacing, 0, spacing)
                 for spacing in spacings}
    if arguments.search_radius is not None:
        apertures.update({
            ",".join(str(offset) for offset in offsets): offsets
            for offsets in combinations(range(-arguments.search_radius,
                                               arguments.search_radius + 1), 3)
        })
    candidates = {name: [] for name in apertures}

    reference = None
    if arguments.reference_png is not None:
        image = Image.open(arguments.reference_png).convert("RGB")
        palette = {
            (0, 0, 0): 0, (255, 0, 0): 1, (0, 255, 0): 2,
            (255, 255, 0): 3, (0, 0, 255): 4, (255, 0, 255): 5,
            (0, 255, 255): 6, (255, 255, 255): 7,
        }
        reference = []
        for physical in selected:
            y = (physical - first_visible) * 2
            reference.extend(palette[image.getpixel((x, y))] for x in range(480))

    channel_map = records[0]["channel_map"]
    channels = ((channel_map >> 2) & 3, (channel_map >> 4) & 3,
                (channel_map >> 6) & 3)

    for record in records:
        samples = list(unpack_msb_samples(record["payload"], 4,
                                          record["sample_count"]))
        edges = rising_edges(samples, record["sync_channel"])
        periods = [b - a for a, b in zip(edges, edges[1:])]
        period = round(statistics.median(periods))
        origins = [edges[0] - period] + edges
        for name, offsets in apertures.items():
            frame = []
            for physical in selected:
                offset = physical - first_physical
                logical = physical - first_visible
                start = arguments.start_tick + (physical & 1) * tick_scale
                frame.extend(reconstruct_line(samples, origins[offset], start,
                                              offsets, tick_scale,
                                              bool(logical & 1),
                                              arguments.guard_rule))
            candidates[name].append(frame)

    baseline_modal = modal_frame(candidates[f"spacing-{spacings[0]}"])
    summary = {}
    for name, frames in candidates.items():
        modal = modal_frame(frames)
        disagreements = sum(value != mode for frame in frames
                            for value, mode in zip(frame, modal))
        summary[name] = {
            "frames": len(frames),
            "pixels_per_frame": len(modal),
            "temporal_disagreements": disagreements,
            "modal_mismatches_from_first_spacing": sum(
                value != reference for value, reference in zip(modal, baseline_modal)
            ),
        }
        if reference is not None:
            decoded = [
                (((value >> channels[0]) & 1) == 0) |
                ((((value >> channels[1]) & 1) == 0) << 1) |
                ((((value >> channels[2]) & 1) == 0) << 2)
                for value in modal
            ]
            summary[name]["reference_png_mismatches"] = sum(
                value != expected for value, expected in zip(decoded, reference)
            )
    ordered = dict(sorted(summary.items(), key=lambda item: (
        item[1].get("reference_png_mismatches",
                    item[1]["modal_mismatches_from_first_spacing"]),
        item[1]["modal_mismatches_from_first_spacing"],
        item[1]["temporal_disagreements"], item[0])))
    if arguments.details:
        baseline_name = f"spacing-{spacings[0]}"
        frames = candidates[baseline_name]
        modal = modal_frame(frames)
        sites = []
        for index, expected in enumerate(modal):
            counts = Counter(frame[index] for frame in frames)
            disagreements = len(frames) - counts[expected]
            if disagreements == 0:
                continue
            sites.append({
                "physical_line": arguments.first_line + index // 480,
                "x": index % 480,
                "disagreements": disagreements,
                "raw_nibbles": {f"{value:x}": count
                                for value, count in counts.most_common()},
            })
        sites.sort(key=lambda site: (-site["disagreements"],
                                     site["physical_line"], site["x"]))
        detail_sites = sites[:arguments.details]
        aperture = apertures[baseline_name]
        def attach_patterns(target_sites):
            patterns = [Counter() for _ in target_sites]
            for record in records:
                samples = list(unpack_msb_samples(record["payload"], 4,
                                                  record["sample_count"]))
                edges = rising_edges(samples, record["sync_channel"])
                periods = [b - a for a, b in zip(edges, edges[1:])]
                origins = [edges[0] - round(statistics.median(periods))] + edges
                for site_index, site in enumerate(target_sites):
                    physical = site["physical_line"]
                    start = arguments.start_tick + (physical & 1) * tick_scale
                    origin = origins[physical - first_physical]
                    parts = []
                    for x in (site["x"] & ~1, site["x"] | 1):
                        center = (origin + start + (x // 2) * 21 * tick_scale +
                                  (0 if x % 2 == 0 else 10 * tick_scale) +
                                  tick_scale)
                        triplet = tuple(samples[center + value]
                                        for value in aperture)
                        parts.append("".join(f"{value:x}" for value in triplet))
                    patterns[site_index]["/".join(parts)] += 1
            for site, counts in zip(target_sites, patterns):
                site["paired_triplets"] = dict(counts.most_common())

        attach_patterns(detail_sites)
        output = {"candidates": ordered,
                  "top_unstable": detail_sites}
        if reference is not None:
            decoded = [
                (((value >> channels[0]) & 1) == 0) |
                ((((value >> channels[1]) & 1) == 0) << 1) |
                ((((value >> channels[2]) & 1) == 0) << 2)
                for value in modal
            ]
            mismatch_sites = [
                {
                    "physical_line": arguments.first_line + index // 480,
                    "x": index % 480,
                    "candidate": decoded[index],
                    "reference": reference[index],
                }
                for index in range(len(reference))
                if decoded[index] != reference[index]
            ]
            attach_patterns(mismatch_sites)
            output["reference_mismatches"] = mismatch_sites
    else:
        output = ordered
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
