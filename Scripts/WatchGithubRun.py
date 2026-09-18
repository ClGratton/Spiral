#!/usr/bin/env python3
"""Monitor one GitHub Actions run while emitting only decision-relevant changes."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time


FIELDS = "databaseId,name,status,conclusion,url,headSha,jobs"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_id")
    parser.add_argument("--repo")
    parser.add_argument("--expect-sha")
    parser.add_argument("--interval-seconds", type=float, default=30.0)
    parser.add_argument("--timeout-seconds", type=float, default=0.0)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    if args.interval_seconds < 1.0:
        parser.error("--interval-seconds must be at least 1")
    if args.timeout_seconds < 0.0:
        parser.error("--timeout-seconds cannot be negative")
    return args


def fetch(args: argparse.Namespace) -> dict[str, object]:
    command = ["gh", "run", "view", args.run_id, "--json", FIELDS]
    if args.repo:
        command.extend(("--repo", args.repo))
    environment = os.environ.copy()
    environment["GH_PAGER"] = "cat"
    result = subprocess.run(command, text=True, capture_output=True, env=environment)
    if result.returncode != 0:
        message = result.stderr.strip() or result.stdout.strip() or "gh run view failed"
        raise RuntimeError(message)
    return json.loads(result.stdout)


def compact(raw: dict[str, object]) -> dict[str, object]:
    jobs = []
    for job in raw.get("jobs", []):
        active_step = None
        steps = job.get("steps", [])
        for step in steps:
            if step.get("status") == "in_progress":
                active_step = step.get("name")
                break
        if active_step is None:
            completed = [step.get("name") for step in steps if step.get("status") == "completed"]
            active_step = completed[-1] if completed else None
        jobs.append(
            {
                "name": job.get("name"),
                "status": job.get("status"),
                "conclusion": job.get("conclusion"),
                "step": active_step,
            }
        )
    return {
        "schema": 1,
        "runId": str(raw.get("databaseId", "")) or None,
        "name": raw.get("name"),
        "headSha": raw.get("headSha"),
        "status": raw.get("status"),
        "conclusion": raw.get("conclusion"),
        "url": raw.get("url"),
        "jobs": jobs,
    }


def main() -> int:
    args = parse_args()
    started = time.monotonic()
    prior = None
    while True:
        try:
            raw = fetch(args)
        except (RuntimeError, json.JSONDecodeError) as error:
            print(f"GitHub run query failed: {error}", file=sys.stderr)
            return 2

        if args.expect_sha and raw.get("headSha") != args.expect_sha:
            print(
                f"GitHub run SHA mismatch: expected {args.expect_sha}, got {raw.get('headSha')}",
                file=sys.stderr,
            )
            return 2

        snapshot = compact(raw)
        fingerprint = json.dumps(snapshot, sort_keys=True, separators=(",", ":"))
        if fingerprint != prior:
            print(json.dumps(snapshot, separators=(",", ":")), flush=True)
            prior = fingerprint

        if args.once:
            return 0
        if raw.get("status") == "completed":
            return 0 if raw.get("conclusion") == "success" else 1
        if args.timeout_seconds and time.monotonic() - started >= args.timeout_seconds:
            print("GitHub run monitor timed out before completion.", file=sys.stderr)
            return 124
        time.sleep(args.interval_seconds)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130) from None
