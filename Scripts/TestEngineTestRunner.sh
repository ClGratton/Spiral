#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 /absolute/path/to/EngineTests" >&2
    exit 2
fi

ENGINE_TESTS="$1"
if [[ ! -x "$ENGINE_TESTS" ]]; then
    echo "EngineTests executable not found or not executable: $ENGINE_TESTS" >&2
    exit 2
fi

TEMPORARY="$(mktemp -d "${TMPDIR:-/tmp}/spiral-engine-test-runner.XXXXXX")"
trap 'rm -rf "$TEMPORARY"' EXIT

"$ENGINE_TESTS" --list > "$TEMPORARY/list.txt"
TOTAL_COUNT="$(awk -F '\t' '$1 == "Fast" || $1 == "Integration" { count++ } END { print count+0 }' "$TEMPORARY/list.txt")"
FAST_COUNT="$(awk -F '\t' '$1 == "Fast" { count++ } END { print count+0 }' "$TEMPORARY/list.txt")"
INTEGRATION_COUNT="$(awk -F '\t' '$1 == "Integration" { count++ } END { print count+0 }' "$TEMPORARY/list.txt")"
FIRST_FAST="$(awk -F '\t' '$1 == "Fast" { print $2; exit }' "$TEMPORARY/list.txt")"
if [[ "$TOTAL_COUNT" -eq 0 || "$FAST_COUNT" -eq 0 || "$INTEGRATION_COUNT" -eq 0 || -z "$FIRST_FAST" ]]; then
    echo "EngineTests list did not expose both nonempty tiers." >&2
    exit 1
fi

"$ENGINE_TESTS" --test "$FIRST_FAST" --report-json "$TEMPORARY/exact.json"
"$ENGINE_TESTS" --filter "Frame timing capability group" --report-json "$TEMPORARY/filter.json"
"$ENGINE_TESTS" --tier fast --report-json "$TEMPORARY/fast.json"
REPLAY_NAME="Generated world-grid normalization preserves canonical reference results"
"$ENGINE_TESTS" --test "$REPLAY_NAME" --seed 42 --replay-trace 0,0 --report-json "$TEMPORARY/generated-replay.json"
"$ENGINE_TESTS" --report-json "$TEMPORARY/integration-default.json"

expect_exit_two() {
    set +e
    "$ENGINE_TESTS" "$@" > "$TEMPORARY/rejected.out" 2>&1
    local status=$?
    set -e
    if [[ $status -ne 2 ]]; then
        echo "Expected exit 2 for: $*; got $status" >&2
        cat "$TEMPORARY/rejected.out" >&2
        exit 1
    fi
}
expect_exit_two --filter __no_such_spiral_test__
expect_exit_two --tier slow
expect_exit_two --tier fast --test "$FIRST_FAST"
expect_exit_two --unknown
expect_exit_two --tier fast --replay-trace 0,0
expect_exit_two --test "$REPLAY_NAME" --replay-trace 0,,1

python3 - "$TEMPORARY" "$TOTAL_COUNT" "$FAST_COUNT" "$FIRST_FAST" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
total = int(sys.argv[2])
fast_count = int(sys.argv[3])
first_fast = sys.argv[4]
listed = [line.rstrip("\n").split("\t", 1)[1] for line in (root / "list.txt").read_text(encoding="utf-8").splitlines()]

exact = json.loads((root / "exact.json").read_text(encoding="utf-8"))
assert exact["schema"] == 1 and exact["selectedCount"] == 1 and exact["failures"] == 0
assert exact["results"][0]["name"] == first_fast and exact["results"][0]["tier"] == "Fast"

filtered = json.loads((root / "filter.json").read_text(encoding="utf-8"))
assert filtered["selectedCount"] >= 2
assert all("Frame timing capability group" in item["name"] for item in filtered["results"])

fast = json.loads((root / "fast.json").read_text(encoding="utf-8"))
assert fast["selection"] == "tier:fast" and fast["selectedCount"] == fast_count and fast["budgetMs"] == 60000
assert all(item["tier"] == "Fast" for item in fast["results"])

complete = json.loads((root / "integration-default.json").read_text(encoding="utf-8"))
assert complete["selection"] == "tier:integration" and complete["selectedCount"] == total and complete["budgetMs"] == 300000
assert [item["name"] for item in complete["results"]] == listed

replay = json.loads((root / "generated-replay.json").read_text(encoding="utf-8"))
assert replay["seed"] == 42 and replay["selectedCount"] == 1 and replay["failures"] == 0
assert replay["results"][0]["name"] == "Generated world-grid normalization preserves canonical reference results"
PY

echo "EngineTestRunnerV1 result=pass total=$TOTAL_COUNT fast=$FAST_COUNT integration=$INTEGRATION_COUNT"
