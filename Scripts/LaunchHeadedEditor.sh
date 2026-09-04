#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
editor="$repo_root/bin/Debug-linux-x86_64-gmake/Editor/Editor"
target_monitor="${SPIRAL_HEADED_MONITOR:-DP-3}"
target_workspace="${SPIRAL_HEADED_WORKSPACE:-2}"
build_jobs="${SPIRAL_EDITOR_BUILD_JOBS:-2}"
canonical_linux_worktree="/home/claudio/Spiral-Linux-Worktree"
premake="$repo_root/Vendor/premake/bin/premake5"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "LaunchHeadedEditor.sh is Linux-only" >&2
    exit 1
fi
if [[ ! "$target_workspace" =~ ^[1-9][0-9]*$ ]]; then
    echo "SPIRAL_HEADED_WORKSPACE must be a positive integer" >&2
    exit 1
fi
if [[ ! "$build_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "SPIRAL_EDITOR_BUILD_JOBS must be a positive integer" >&2
    exit 1
fi
for command_name in git hyprctl jq make realpath setsid sha256sum; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "Required command is unavailable: $command_name" >&2
        exit 1
    fi
done

resolved_repo_root="$(realpath "$repo_root")"
git_repo_root="$(git -C "$repo_root" rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$git_repo_root" || "$(realpath "$git_repo_root")" != "$resolved_repo_root" ]]; then
    echo "Launch helper is not rooted at the resolved Git worktree: helper=$resolved_repo_root git=${git_repo_root:-unavailable}" >&2
    exit 1
fi
if [[ -e "$canonical_linux_worktree/.git" ]] \
    && [[ "$resolved_repo_root" != "$(realpath "$canonical_linux_worktree")" ]]; then
    echo "Refusing a non-canonical Linux checkout: expected $canonical_linux_worktree, got $resolved_repo_root" >&2
    exit 1
fi
if [[ -e "$canonical_linux_worktree/.git" ]] \
    && [[ "$target_monitor" != "DP-3" || "$target_workspace" != "2" ]]; then
    echo "Refusing a headed-target override on this workstation: required DP-3/workspace 2, got $target_monitor/workspace $target_workspace" >&2
    exit 1
fi
if [[ ! -x "$premake" ]]; then
    echo "Pinned Premake executable is unavailable: $premake" >&2
    exit 1
fi

mapfile -t existing_windows < <(
    hyprctl -j clients | jq -r '.[] | select(.class == "Spiral Editor" or .title == "Spiral Editor") | "\(.address) pid=\(.pid) monitor=\(.monitor) workspace=\(.workspace.id)"'
)
if (( ${#existing_windows[@]} != 0 )); then
    printf 'Refusing an ambiguous second Editor launch; close the existing window first:\n' >&2
    printf '  %s\n' "${existing_windows[@]}" >&2
    exit 1
fi

target_monitor_id="$(hyprctl -j monitors | jq -r --arg name "$target_monitor" '.[] | select(.name == $name) | .id')"
if [[ ! "$target_monitor_id" =~ ^[0-9]+$ ]]; then
    echo "Target monitor is unavailable: $target_monitor" >&2
    exit 1
fi
target_monitor_x="$(hyprctl -j monitors | jq -r --arg name "$target_monitor" '.[] | select(.name == $name) | .x')"
left_monitor_x="$(hyprctl -j monitors | jq -r '.[] | select(.name == "DP-1") | .x')"
if [[ ! "$target_monitor_x" =~ ^-?[0-9]+$ \
    || ! "$left_monitor_x" =~ ^-?[0-9]+$ \
    || "$target_monitor_x" -le "$left_monitor_x" ]]; then
    echo "Physical display mapping is not the required DP-1-left/DP-3-right layout: DP-1 x=$left_monitor_x, $target_monitor x=$target_monitor_x" >&2
    exit 1
fi

cd "$repo_root"
source_head="$(git rev-parse --verify HEAD)"
if [[ -n "$(git status --porcelain=v1 --untracked-files=normal)" ]]; then
    source_state="dirty"
else
    source_state="clean"
fi
source_fingerprint="$({
    git diff --binary HEAD --
    while IFS= read -r -d '' untracked_path; do
        printf '%s\0' "$untracked_path"
        sha256sum -- "$untracked_path"
    done < <(git ls-files --others --exclude-standard -z | sort -z)
} | sha256sum | awk '{print $1}')"
echo "Headed Editor project refresh: $premake --file=$repo_root/premake5.lua gmake"
"$premake" --file="$repo_root/premake5.lua" gmake
echo "Headed Editor build gate: $editor"
make config=debug Editor -j"$build_jobs"
if [[ ! -x "$editor" ]]; then
    echo "Debug Editor was not produced at the canonical path: $editor" >&2
    exit 1
fi
expected_editor="$(realpath "$editor")"
expected_cwd="$(realpath "$repo_root")"

hyprctl dispatch "hl.dsp.focus({ workspace = '$target_workspace' })" | grep -Fxq ok
focused_monitor="$(hyprctl -j monitors | jq -r '.[] | select(.focused) | .name')"
active_target_workspace="$(hyprctl -j monitors | jq -r --arg name "$target_monitor" '.[] | select(.name == $name) | .activeWorkspace.id')"
if [[ "$focused_monitor" != "$target_monitor" || "$active_target_workspace" != "$target_workspace" ]]; then
    echo "Pre-launch display focus failed: expected $target_monitor/workspace $target_workspace, got $focused_monitor/workspace $active_target_workspace" >&2
    exit 1
fi

mkdir -p "$repo_root/output/live"
run_dir="$(mktemp -d "$repo_root/output/live/headed-editor-XXXXXXXX")"
control_dir="$run_dir/mailbox"
log_path="$run_dir/editor.log"

setsid "$expected_editor" --renderer-vulkan "--editor-control-dir=$control_dir" "$@" >"$log_path" 2>&1 </dev/null &
editor_pid=$!
keep_editor=false
cleanup_failed_launch() {
    if [[ "$keep_editor" != true ]] && kill -0 "$editor_pid" 2>/dev/null; then
        kill -TERM "$editor_pid" 2>/dev/null || true
    fi
}
trap cleanup_failed_launch EXIT

editor_address=""
for _ in $(seq 1 300); do
    if ! kill -0 "$editor_pid" 2>/dev/null; then
        tail -n 120 "$log_path" >&2 || true
        echo "Debug Editor exited before publishing a window" >&2
        exit 1
    fi
    editor_address="$(hyprctl -j clients | jq -r --argjson pid "$editor_pid" '.[] | select(.pid == $pid and .class == "Spiral Editor" and .title == "Spiral Editor") | .address')"
    if [[ "$editor_address" =~ ^0x[0-9a-fA-F]+$ ]]; then
        break
    fi
    sleep 0.05
done
if [[ ! "$editor_address" =~ ^0x[0-9a-fA-F]+$ ]]; then
    tail -n 120 "$log_path" >&2 || true
    echo "Timed out resolving the exact new Spiral Editor window" >&2
    exit 1
fi

actual_editor="$(realpath "/proc/$editor_pid/exe")"
actual_cwd="$(realpath "/proc/$editor_pid/cwd")"
if [[ "$actual_editor" != "$expected_editor" || "$actual_cwd" != "$expected_cwd" ]]; then
    echo "Editor provenance mismatch: executable=$actual_editor cwd=$actual_cwd" >&2
    exit 1
fi

hyprctl dispatch "hl.dsp.window.move({ workspace = $target_workspace, follow = false, window = 'address:$editor_address' })" | grep -Fxq ok
hyprctl dispatch "hl.dsp.focus({ window = 'address:$editor_address' })" | grep -Fxq ok

client_state="$(hyprctl -j clients | jq -c --arg address "$editor_address" '.[] | select(.address == $address) | {monitor, workspace: .workspace.id}')"
actual_monitor_id="$(jq -r '.monitor' <<<"$client_state")"
actual_workspace="$(jq -r '.workspace' <<<"$client_state")"
actual_monitor="$(hyprctl -j monitors | jq -r --argjson id "$actual_monitor_id" '.[] | select(.id == $id) | .name')"
if [[ "$actual_monitor_id" != "$target_monitor_id" || "$actual_monitor" != "$target_monitor" || "$actual_workspace" != "$target_workspace" ]]; then
    echo "Headed Editor placement mismatch: expected $target_monitor/$target_monitor_id/workspace $target_workspace, got $actual_monitor/$actual_monitor_id/workspace $actual_workspace" >&2
    exit 1
fi

handoff_ready=false
for _ in $(seq 1 600); do
    if ! kill -0 "$editor_pid" 2>/dev/null; then
        tail -n 120 "$log_path" >&2 || true
        echo "Debug Editor exited before native Scene handoff" >&2
        exit 1
    fi
    if grep -Fq 'Renderer initialized with backend: NVRHI Vulkan' "$log_path" \
        && grep -Eq 'VulkanSceneOutputHandoffV1 .*producer=pass .*imgui=queued present=pass' "$log_path"; then
        handoff_ready=true
        break
    fi
    sleep 0.05
done
if [[ "$handoff_ready" != true ]]; then
    tail -n 120 "$log_path" >&2 || true
    echo "Timed out waiting for the native Vulkan Scene-output handoff" >&2
    exit 1
fi
if grep -Eiq 'native viewport unavailable|renderer initialization failed|failed to initialize renderer' "$log_path"; then
    tail -n 120 "$log_path" >&2 || true
    echo "Editor log contains a renderer/viewport initialization failure" >&2
    exit 1
fi

# Startup can publish/recreate Scene output after the first compositor placement.
# Reassert the exact address only after native handoff and require it to remain on
# the physical right-hand display for a stability interval before showing it.
stable_samples=0
for _ in $(seq 1 80); do
    client_state="$(hyprctl -j clients | jq -c --arg address "$editor_address" '.[] | select(.address == $address) | {monitor, workspace: .workspace.id, at}')"
    actual_monitor_id="$(jq -r '.monitor // -1' <<<"$client_state")"
    actual_workspace="$(jq -r '.workspace // -1' <<<"$client_state")"
    actual_x="$(jq -r '.at[0] // -1' <<<"$client_state")"
    target_geometry="$(hyprctl -j monitors | jq -c --arg name "$target_monitor" '.[] | select(.name == $name) | {id, x, width, activeWorkspace: .activeWorkspace.id}')"
    target_monitor_id="$(jq -r '.id // -1' <<<"$target_geometry")"
    target_x="$(jq -r '.x // -1' <<<"$target_geometry")"
    target_width="$(jq -r '.width // 0' <<<"$target_geometry")"
    active_target_workspace="$(jq -r '.activeWorkspace // -1' <<<"$target_geometry")"

    if [[ "$actual_monitor_id" == "$target_monitor_id" \
        && "$actual_workspace" == "$target_workspace" \
        && "$active_target_workspace" == "$target_workspace" \
        && "$actual_x" -ge "$target_x" \
        && "$actual_x" -lt $((target_x + target_width)) ]]; then
        stable_samples=$((stable_samples + 1))
        if (( stable_samples >= 20 )); then
            break
        fi
    else
        stable_samples=0
        hyprctl dispatch "hl.dsp.focus({ workspace = '$target_workspace' })" | grep -Fxq ok
        hyprctl dispatch "hl.dsp.window.move({ workspace = $target_workspace, follow = false, window = 'address:$editor_address' })" | grep -Fxq ok
        hyprctl dispatch "hl.dsp.focus({ window = 'address:$editor_address' })" | grep -Fxq ok
    fi
    sleep 0.05
done
if (( stable_samples < 20 )); then
    echo "Headed Editor final placement did not remain stable on physical $target_monitor/workspace $target_workspace: $client_state" >&2
    exit 1
fi
actual_monitor="$target_monitor"

keep_editor=true
trap - EXIT
receipt="HeadedEditorLaunchV2 sourceRoot=$expected_cwd sourceHead=$source_head sourceState=$source_state sourceFingerprint=$source_fingerprint build=Debug-gmake-editor-only executable=$expected_editor pid=$editor_pid address=$editor_address backend=NVRHI-Vulkan monitor=$actual_monitor monitorId=$actual_monitor_id workspace=$actual_workspace stableSamples=$stable_samples sceneHandoff=pass controlDir=$control_dir log=$log_path result=pass"
printf '%s\n' "$receipt" | tee "$run_dir/launch-receipt.txt"
