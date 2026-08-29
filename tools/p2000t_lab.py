#!/usr/bin/env python3
"""Send atomic experiment requests to the viewer's Codex lab bridge."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import time
import uuid


DEFAULT_ROOT = Path(
    os.environ.get("P2000T_LAB_ROOT", "/mnt/d/tmp/p2000t-codex-lab")
)


def request_id(command: str) -> str:
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    return f"{command}-{stamp}-{uuid.uuid4().hex[:8]}"


def submit(root: Path, payload: dict, timeout: float) -> dict:
    requests = root / "requests"
    responses = root / "responses"
    requests.mkdir(parents=True, exist_ok=True)
    responses.mkdir(parents=True, exist_ok=True)
    identifier = payload["id"]
    temporary = requests / f".{identifier}.tmp"
    destination = requests / f"{identifier}.json"
    response = responses / f"{identifier}.json"
    if destination.exists() or response.exists():
        raise FileExistsError(f"request id already exists: {identifier}")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(destination)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if response.exists():
            data = json.loads(response.read_text(encoding="utf-8"))
            print(json.dumps(data, indent=2))
            return data
        time.sleep(0.1)
    raise TimeoutError(
        f"No response within {timeout:g}s. Is the P2000T lab agent running?"
    )


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--timeout", type=float, default=10.0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    status = subparsers.add_parser("status", help="query viewer and Pico state")
    add_common(status)

    save = subparsers.add_parser(
        "save", help="persist the current live settings to Pico flash"
    )
    add_common(save)

    factory_reset = subparsers.add_parser(
        "factory-reset",
        help="restore and persist the firmware's known-good defaults",
    )
    add_common(factory_reset)

    cancel = subparsers.add_parser("cancel", help="cancel the active experiment")
    add_common(cancel)

    shutdown = subparsers.add_parser(
        "shutdown", help="gracefully stop the viewer or headless lab agent"
    )
    add_common(shutdown)

    experiment = subparsers.add_parser(
        "experiment", help="apply live settings and capture scored frames"
    )
    add_common(experiment)
    experiment.set_defaults(timeout=120.0)
    experiment.add_argument("--phase", type=int)
    experiment.add_argument("--odd-phase", type=int)
    experiment.add_argument("--rate-trim", type=int)
    experiment.add_argument(
        "--reconstruction",
        choices=(
            "raw",
            "guarded",
            "sharp",
            "window-center",
            "window-channel",
            "window-early",
            "window-late",
            "window-confidence",
        ),
    )
    experiment.add_argument(
        "--reference-run",
        help="existing run id whose modal image is the spatial-fidelity reference",
    )
    experiment.add_argument("--settle", type=int, default=2)
    experiment.add_argument("--frames", type=int, default=10)
    experiment.add_argument("--tag", default="")
    experiment.add_argument("--id", dest="explicit_id")

    diagnostic = subparsers.add_parser(
        "diagnostic", help="record lossless high-resolution RGBS diagnostics"
    )
    add_common(diagnostic)
    diagnostic.set_defaults(timeout=180.0)
    diagnostic.add_argument("--start-line", type=int, required=True)
    diagnostic.add_argument("--lines", type=int, default=16)
    diagnostic.add_argument("--repetitions", type=int, default=100)
    diagnostic.add_argument("--tag", default="")
    diagnostic.add_argument("--id", dest="explicit_id")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    identifier = getattr(args, "explicit_id", None) or request_id(args.command)
    payload: dict = {
        "protocol": 1,
        "id": identifier,
        "command": args.command,
    }
    if args.command == "experiment":
        settings = {
            name: value
            for name, value in (
                ("phase", args.phase),
                ("odd_line_phase", args.odd_phase),
                ("rate_trim", args.rate_trim),
                ("reconstruction", args.reconstruction),
            )
            if value is not None
        }
        payload.update(
            {
                "tag": args.tag,
                "settings": settings,
                "settle_frames": args.settle,
                "capture_frames": args.frames,
            }
        )
        if args.reference_run:
            payload["reference_run"] = args.reference_run
    elif args.command == "diagnostic":
        payload.update(
            {
                "tag": args.tag,
                "start_line": args.start_line,
                "line_count": args.lines,
                "repetitions": args.repetitions,
            }
        )
    try:
        response = submit(args.root, payload, args.timeout)
    except (OSError, ValueError, TimeoutError) as exc:
        print(f"p2000t_lab: {exc}", file=sys.stderr)
        return 2
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
