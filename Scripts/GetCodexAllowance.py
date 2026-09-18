#!/usr/bin/env python3
"""Report the current Codex thread's weekly allowance sample without guessing."""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import json
import os
import pathlib
import sys
from typing import NoReturn


def fail(message: str) -> NoReturn:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--thread-id", default=os.environ.get("CODEX_THREAD_ID"))
    parser.add_argument(
        "--codex-root",
        default=os.environ.get("CODEX_HOME", str(pathlib.Path.home() / ".codex")),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.thread_id or not args.thread_id.strip():
        fail("CODEX_THREAD_ID is unavailable; pass --thread-id explicitly instead of guessing from the newest rollout.")

    sessions_root = pathlib.Path(args.codex_root) / "sessions"
    if not sessions_root.is_dir():
        fail(f"Codex sessions directory does not exist: {sessions_root}")

    rollouts = list(sessions_root.rglob(f"*-{args.thread_id}.jsonl"))
    if len(rollouts) != 1:
        fail(
            f"Expected exactly one rollout for thread {args.thread_id} beneath "
            f"{sessions_root}; found {len(rollouts)}."
        )

    with rollouts[0].open("r", encoding="utf-8") as stream:
        recent_lines = collections.deque(stream, maxlen=256)

    sample = None
    for line in reversed(recent_lines):
        try:
            candidate = json.loads(line)
        except json.JSONDecodeError:
            continue
        payload = candidate.get("payload", {})
        if candidate.get("type") == "event_msg" and payload.get("type") == "token_count" and "rate_limits" in payload:
            sample = candidate
            break
    if sample is None:
        fail("No recent rate-limit sample exists in the current thread rollout. Complete one model response and retry.")

    rate_limits = sample["payload"]["rate_limits"] or {}
    weekly = next(
        (
            value
            for key in ("primary", "secondary")
            if isinstance((value := rate_limits.get(key)), dict)
            and int(value.get("window_minutes", -1)) == 10080
        ),
        None,
    )
    if weekly is None:
        fail("The current sample has no 10080-minute weekly rate-limit window; do not reinterpret token totals as allowance.")

    used_percent = float(weekly["used_percent"])
    reset_utc = dt.datetime.fromtimestamp(
        int(weekly["resets_at"]), tz=dt.timezone.utc
    ).isoformat().replace("+00:00", "Z")
    result = {
        "schema": 1,
        "threadId": args.thread_id,
        "sampleTimestamp": str(sample.get("timestamp", "")),
        "usedPercent": used_percent,
        "remainingPercent": 100.0 - used_percent,
        "windowMinutes": int(weekly["window_minutes"]),
        "resetsAtUtc": reset_utc,
        "source": str(rollouts[0]),
    }
    print(json.dumps(result, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
