#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Evaluate confidence-guard variants over several raw diagnostic blocks."""

import argparse
import json
import statistics
from collections import Counter
from pathlib import Path

from PIL import Image

from analyze_diagnostics import RAW_RGBS, read_records, unpack_msb_samples
from analyze_sampling_aperture import color_late, modal_frame, rising_edges


PALETTE = {
    (0, 0, 0): 0,
    (255, 0, 0): 1,
    (0, 255, 0): 2,
    (255, 255, 0): 3,
    (0, 0, 255): 4,
    (255, 0, 255): 5,
    (0, 255, 255): 6,
    (255, 255, 255): 7,
}


def decoded_color(value, channels):
    return (
        (((value >> channels[0]) & 1) == 0)
        | ((((value >> channels[1]) & 1) == 0) << 1)
        | ((((value >> channels[2]) & 1) == 0) << 2)
    )


def guarded_pair(pair, uncertainty):
    first, second = pair
    first_uncertain, second_uncertain = uncertainty
    if first == second or first_uncertain == second_uncertain:
        return pair
    if first_uncertain:
        return second, second
    return first, first


def load_reference(path):
    image = Image.open(path).convert("RGB")
    if image.size != (480, 480):
        raise ValueError("reference image must be 480x480")
    return [[PALETTE[image.getpixel((x, y * 2))] for x in range(480)]
            for y in range(240)]


def line_windows(samples, origin, start_tick, tick_scale, aperture,
                 clock_ppm):
    outputs = []
    uncertain_any = []
    uncertain_late = []
    patterns = []
    for x in range(480):
        progress = (
            (x // 2) * 21 * tick_scale
            + (0 if x % 2 == 0 else 10 * tick_scale)
            + tick_scale
        )
        progress = round(progress * 1_000_000 / (1_000_000 + clock_ppm))
        center = origin + start_tick + progress
        window = tuple(samples[center + offset] for offset in aperture)
        outputs.append(color_late(*window))
        uncertain_any.append(window[0] != window[1] or window[1] != window[2])
        uncertain_late.append(window[1] != window[2])
        patterns.append("".join(f"{value:x}" for value in window))
    return outputs, uncertain_any, uncertain_late, patterns


def load_block(session, reference, start_tick, aperture, clock_ppm):
    capture = session / "capture.p2td" if session.is_dir() else session
    records = [record for record in read_records(capture)
               if record["type"] == RAW_RGBS]
    if not records:
        raise ValueError(f"no raw records in {capture}")
    first = records[0]
    tick_scale = first["sample_rate_hz"] // 126_000_000
    if tick_scale not in (1, 2):
        raise ValueError(f"unsupported raw rate in {capture}")
    channels = ((first["channel_map"] >> 2) & 3,
                (first["channel_map"] >> 4) & 3,
                (first["channel_map"] >> 6) & 3)
    first_visible = first["first_visible_line"]
    frames = []
    evidence = Counter()
    sites_by_key = {}
    patterns_by_key = {}

    for record in records:
        samples = list(unpack_msb_samples(record["payload"], 4,
                                          record["sample_count"]))
        edges = rising_edges(samples, record["sync_channel"])
        periods = [end - begin for begin, end in zip(edges, edges[1:])]
        if not periods:
            raise ValueError(f"too few sync edges in {capture}")
        period = round(statistics.median(periods))
        origins = [edges[0] - period] + edges
        current_frame = []
        late_frame = []
        key_frame = []
        reference_frame = []
        locations = []

        for line_offset in range(record["line_count"]):
            physical = record["start_line"] + line_offset
            logical = physical - first_visible
            if not 0 <= logical < 240:
                continue
            line_start = start_tick + (physical & 1) * tick_scale
            outputs, any_uncertain, late_uncertain, patterns = line_windows(
                samples, origins[line_offset], line_start, tick_scale,
                aperture, clock_ppm
            )
            for x in range(0, 480, 2):
                pair = outputs[x], outputs[x + 1]
                if logical & 1:
                    current = guarded_pair(
                        pair, (any_uncertain[x], any_uncertain[x + 1])
                    )
                    late = guarded_pair(
                        pair, (late_uncertain[x], late_uncertain[x + 1])
                    )
                else:
                    current = pair
                    late = pair
                key = None
                if current != late:
                    key = f"{pair[0]:x}>{pair[1]:x}"
                    evidence[(key, "events")] += 1
                    evidence[(key, "current_reference_errors")] += sum(
                        decoded_color(value, channels) != expected
                        for value, expected in zip(
                            current, reference[logical][x:x + 2]
                        )
                    )
                    evidence[(key, "late_reference_errors")] += sum(
                        decoded_color(value, channels) != expected
                        for value, expected in zip(
                            late, reference[logical][x:x + 2]
                        )
                    )
                    sites_by_key.setdefault(key, set()).add((physical, x))
                    patterns_by_key.setdefault(key, Counter())[
                        f"{patterns[x]}/{patterns[x + 1]}"
                    ] += 1
                current_frame.extend(current)
                late_frame.extend(late)
                key_frame.extend((key, key))
                reference_frame.extend(reference[logical][x:x + 2])
                locations.extend(((physical, x), (physical, x + 1)))
        frames.append({
            "current": current_frame,
            "late": late_frame,
            "keys": key_frame,
            "reference": reference_frame,
            "locations": locations,
            "channels": channels,
        })

    return {
        "session": session.name,
        "frames": frames,
        "evidence": evidence,
        "sites_by_key": sites_by_key,
        "patterns_by_key": patterns_by_key,
    }


def candidate_frames(block, selected_keys=None, all_late=False,
                     confirmed_keys=None):
    selected_keys = selected_keys or set()
    confirmed_keys = confirmed_keys or set()
    result = []
    previous = None
    for frame in block["frames"]:
        if all_late:
            output = frame["late"]
            result.append(output)
            previous = output
            continue
        output = [
            late if key in selected_keys else current
            for current, late, key in zip(
                frame["current"], frame["late"], frame["keys"]
            )
        ]
        if confirmed_keys:
            for index in range(0, len(output), 2):
                key = frame["keys"][index]
                if key not in confirmed_keys:
                    continue
                desired = frame["late"][index:index + 2]
                if (previous is not None and
                        previous[index:index + 2] == desired):
                    output[index:index + 2] = desired
                else:
                    output[index:index + 2] = frame["current"][
                        index:index + 2
                    ]
        result.append(output)
        previous = output
    return result


def evaluate(blocks, selected_keys=None, all_late=False, confirmed_keys=None):
    temporal = 0
    observations = 0
    mismatches = set()
    modal_by_block = {}
    per_block = {}
    for block in blocks:
        frames = candidate_frames(block, selected_keys, all_late,
                                  confirmed_keys)
        modal = modal_frame(frames)
        modal_by_block[block["session"]] = modal
        block_temporal = sum(value != expected for frame in frames
                             for value, expected in zip(frame, modal))
        block_mismatches = set()
        metadata = block["frames"][0]
        for index, (value, expected) in enumerate(
            zip(modal, metadata["reference"])
        ):
            if decoded_color(value, metadata["channels"]) != expected:
                physical, x = metadata["locations"][index]
                block_mismatches.add((block["session"], physical, x))
        temporal += block_temporal
        observations += len(modal) * len(frames)
        mismatches.update(block_mismatches)
        per_block[block["session"]] = {
            "frames": len(frames),
            "pixels": len(modal),
            "temporal_disagreements": block_temporal,
            "reference_mismatches": len(block_mismatches),
        }
    return {
        "temporal_disagreements": temporal,
        "observations": observations,
        "temporal_ppm": temporal * 1_000_000 / observations,
        "reference_mismatches": len(mismatches),
        "mismatch_sites": mismatches,
        "modal_by_block": modal_by_block,
        "per_block": per_block,
    }


def public_metrics(metrics, baseline=None):
    result = {
        "temporal_disagreements": metrics["temporal_disagreements"],
        "observations": metrics["observations"],
        "temporal_ppm": round(metrics["temporal_ppm"], 4),
        "reference_mismatches": metrics["reference_mismatches"],
        "mismatch_sites": [list(site) for site in
                           sorted(metrics["mismatch_sites"])],
    }
    if baseline is not None:
        result["temporal_change"] = (
            metrics["temporal_disagreements"]
            - baseline["temporal_disagreements"]
        )
        result["new_reference_mismatches"] = len(
            metrics["mismatch_sites"] - baseline["mismatch_sites"]
        )
        result["resolved_reference_mismatches"] = len(
            baseline["mismatch_sites"] - metrics["mismatch_sites"]
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path,
                        help="480x480 viewer modal reference PNG")
    parser.add_argument("sessions", type=Path, nargs="+")
    parser.add_argument("--start-tick", type=int, default=4307)
    parser.add_argument("--aperture", type=int, nargs=3, default=(-2, 0, 2))
    parser.add_argument("--clock-ppm", type=float, default=0.0,
                        help="faster candidate sampling clock in ppm; "
                             "--start-tick independently sets the fixed "
                             "sync-to-first-sample phase")
    arguments = parser.parse_args()
    if arguments.clock_ppm <= -1_000_000.0:
        parser.error("--clock-ppm must be greater than -1000000")

    reference = load_reference(arguments.reference)
    blocks = []
    for session in arguments.sessions:
        try:
            blocks.append(load_block(session, reference, arguments.start_tick,
                                     tuple(arguments.aperture),
                                     arguments.clock_ppm))
        except (OSError, ValueError) as error:
            raise ValueError(f"{session}: {error}") from error
    keys = sorted({key for block in blocks
                   for key in block["sites_by_key"]})
    baseline = evaluate(blocks)
    fixed_late = evaluate(blocks, all_late=True)
    confirmed_c1 = evaluate(blocks, {"c>1"}, confirmed_keys={"c>1"})

    individual = {}
    for key in keys:
        individual[key] = evaluate(blocks, {key})

    selected = set()
    greedy_steps = []
    current = baseline
    while True:
        choices = []
        for key in keys:
            if key in selected:
                continue
            metrics = evaluate(blocks, selected | {key})
            if metrics["mismatch_sites"] - baseline["mismatch_sites"]:
                continue
            if metrics["temporal_disagreements"] >= current[
                "temporal_disagreements"
            ]:
                continue
            choices.append((metrics["temporal_disagreements"], key, metrics))
        if not choices:
            break
        _, key, current = min(choices)
        selected.add(key)
        greedy_steps.append({
            "added_key": key,
            **public_metrics(current, baseline),
        })

    evidence = {}
    for key in keys:
        events = sum(block["evidence"][(key, "events")] for block in blocks)
        current_errors = sum(
            block["evidence"][(key, "current_reference_errors")]
            for block in blocks
        )
        late_errors = sum(
            block["evidence"][(key, "late_reference_errors")]
            for block in blocks
        )
        sites = set().union(*(block["sites_by_key"].get(key, set())
                            for block in blocks))
        patterns = Counter()
        for block in blocks:
            patterns.update(block["patterns_by_key"].get(key, Counter()))
        evidence[key] = {
            "events": events,
            "sites": len(sites),
            "current_reference_error_observations": current_errors,
            "late_reference_error_observations": late_errors,
            "individual_candidate": public_metrics(individual[key], baseline),
            "top_triplets": dict(patterns.most_common(6)),
        }

    output = {
        "sessions": len(blocks),
        "raw_bursts": sum(len(block["frames"]) for block in blocks),
        "baseline": public_metrics(baseline),
        "fixed_late": public_metrics(fixed_late, baseline),
        "confirmed_c1": public_metrics(confirmed_c1, baseline),
        "greedy_zero-new-error_policy": {
            "late_transition_keys": sorted(selected),
            "steps": greedy_steps,
            "result": public_metrics(current, baseline),
        },
        "transition_evidence": evidence,
        "per_block_baseline": baseline["per_block"],
    }
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
