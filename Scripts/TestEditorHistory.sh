#!/usr/bin/env bash
# Typed-control checks of the Editor's undo history (named entries, undo/redo, budget
# eviction, undo barrier). Headless: no window, no GPU, nothing is saved.
#
#   Scripts/TestEditorHistory.sh [editor-binary]
#
# 0. --editor-history-smoke and --editor-history-benchmark drive the real Inspector code
#    through a private headless ImGui context.
# 1. Scripts/TestEditorHistory.py drives three headless Editor sessions through the
#    typed mailbox (InspectHistory, UndoHistory, RedoHistory and the existing edit
#    actions): default limits, a one-byte budget, and a three-entry cap.
# 2. Scripts/TestEditorFabImport.sh proves the undo barrier after a Fab commit with the
#    same typed InspectHistory/UndoHistory actions.
# Persistent project files must stay byte-identical across both.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
editor="${1:-$repo_root/bin/Debug-linux-x86_64-gmake/Editor/Editor}"

if [[ ! -x "$editor" ]]; then
    echo "Editor executable is missing or not executable: $editor" >&2
    exit 1
fi
python3 "$script_dir/EditorMaterialControl.py" --help >/dev/null

tracked_files=(
    "$repo_root/output/projects/default.spiralproject"
    "$repo_root/output/scenes/sample.spiral"
    "$repo_root/output/assets/sample.assets"
    "$repo_root/output/assets/PrototypeDefault.spiralmat"
)
fingerprint() {
    local path
    for path in "${tracked_files[@]}"; do
        if [[ -f "$path" ]]; then
            sha256sum -- "$path"
        else
            printf 'missing  %s\n' "$path"
        fi
    done
}
before_fingerprint="$(fingerprint)"

smoke_root="$(mktemp -d "${TMPDIR:-/tmp}/spiral-history-test.XXXXXX")"
cleanup() {
    rm -rf -- "$smoke_root"
}
trap cleanup EXIT

cd -- "$repo_root"

# 0. The Inspector, the shortcuts, the History panel and the special-entity tracking through
#    a private headless ImGui context (the real widget code, no window, nothing saved).
ui_log="$smoke_root/ui-smoke.log"
if ! timeout 120s "$editor" --headless --editor-history-smoke >"$ui_log" 2>&1; then
    cat "$ui_log" >&2
    echo "The in-process Inspector history smoke failed" >&2
    exit 1
fi
grep -Eq -- "EditorHistorySmokeV1 widgets=[0-9]+ gestures=[0-9]+ .* snapshotsPerGesture=2 .* result=pass" "$ui_log"
grep -E -- "EditorHistorySmokeV1" "$ui_log"

# Per-frame Inspector cost on 1000 and 10000 entities (informational; it fails only when
# the Inspector copies the project while idle or more than twice per gesture).
benchmark_log="$smoke_root/benchmark.log"
if ! timeout 300s "$editor" --headless --editor-history-benchmark >"$benchmark_log" 2>&1; then
    cat "$benchmark_log" >&2
    echo "The Inspector history benchmark failed" >&2
    exit 1
fi
grep -E -- "EditorHistoryBenchmarkV1" "$benchmark_log"

python3 "$script_dir/TestEditorHistory.py" "$repo_root" "$editor"
[[ "$(fingerprint)" == "$before_fingerprint" ]] || {
    echo "Typed history edits changed persistent project files" >&2
    exit 1
}

bash "$script_dir/TestEditorFabImport.sh" "$editor"
[[ "$(fingerprint)" == "$before_fingerprint" ]] || {
    echo "The Fab barrier check changed the default project files" >&2
    exit 1
}
# No Editor of this binary may outlive the test. Compare resolved /proc/<pid>/exe targets;
# a pattern search of the command line would match the probe itself.
editor_real="$(readlink -f -- "$editor")"
for process_exe in /proc/[0-9]*/exe; do
    if [[ "$(readlink -f -- "$process_exe" 2>/dev/null || true)" == "$editor_real" ]]; then
        echo "An Editor from the history test is still running: $process_exe" >&2
        exit 1
    fi
done
echo "EditorHistoryTestV1 typed=named-entries,round-trip,redo-invalidation,cas,eviction,barrier persistentBytes=unchanged input=no-ui-synthesis result=pass"
