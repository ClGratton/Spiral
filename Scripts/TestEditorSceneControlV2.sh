#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
editor="${1:-$repo_root/bin/Debug-linux-x86_64-gmake/Editor/Editor}"
run_vulkan="${2:-}"
project="$repo_root/output/projects/default.spiralproject"
headed_monitor="${SPIRAL_HEADED_MONITOR:-}"
headed_workspace="${SPIRAL_HEADED_WORKSPACE:-}"
headed_observation_seconds="${SPIRAL_SCENE_CONTROL_HEADED_OBSERVATION_SECONDS:-0}"
step_observation_seconds="${SPIRAL_SCENE_CONTROL_STEP_OBSERVATION_SECONDS:-0}"
capture_dir="${SPIRAL_SCENE_CONTROL_CAPTURE_DIR:-}"

for seconds in "$headed_observation_seconds" "$step_observation_seconds"; do
    if [[ ! "$seconds" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo "Scene-control observation durations must be nonnegative numbers" >&2
        exit 1
    fi
done
if [[ -n "$headed_monitor" || -n "$headed_workspace" ]]; then
    if [[ -z "$headed_monitor" || ! "$headed_workspace" =~ ^[1-9][0-9]*$ ]]; then
        echo "SPIRAL_HEADED_MONITOR and a positive SPIRAL_HEADED_WORKSPACE are required together" >&2
        exit 1
    fi
    command -v hyprctl >/dev/null
    command -v jq >/dev/null
fi
if [[ -n "$capture_dir" ]]; then
    if [[ "$capture_dir" != /* || -z "$headed_monitor" ]]; then
        echo "SPIRAL_SCENE_CONTROL_CAPTURE_DIR must be absolute and requires headed placement" >&2
        exit 1
    fi
    command -v grim >/dev/null
    install -d -m 700 -- "$capture_dir"
fi
editor_timeout_seconds="$(awk -v initial="$headed_observation_seconds" \
    -v step="$step_observation_seconds" 'BEGIN { print 60 + initial + step * 7 }')"

if [[ ! -x "$editor" ]]; then
    echo "Editor executable is missing or not executable: $editor" >&2
    exit 1
fi
python3 "$script_dir/EditorMaterialControl.py" --help >/dev/null

smoke_root="$(mktemp -d "${TMPDIR:-/tmp}/spiral-scene-control-v2.XXXXXX")"
live_process=""
cleanup() {
    if [[ -n "$live_process" ]] && kill -0 "$live_process" 2>/dev/null; then
        kill "$live_process" 2>/dev/null || true
        wait "$live_process" 2>/dev/null || true
    fi
    if [[ "${SPIRAL_KEEP_SMOKE_ARTIFACTS:-0}" == "1" ]]; then
        echo "Preserved scene-control V3 smoke artifacts: $smoke_root" >&2
    else
        rm -rf -- "$smoke_root"
    fi
}
trap cleanup EXIT

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

run_sequence() {
    local label="$1"
    local expected_backend="$2"
    shift 2
    local control_dir="$smoke_root/$label-mailbox"
    local log_path="$smoke_root/$label-editor.log"
    local target="$control_dir/scene-control-target.info"

    timeout "${editor_timeout_seconds}s" "$editor" "$@" \
        "--editor-control-dir=$control_dir" \
        --editor-control-scene-v3-helper-smoke >"$log_path" 2>&1 &
    live_process=$!
    for _ in $(seq 1 500); do
        if [[ -f "$target" ]]; then
            break
        fi
        if ! kill -0 "$live_process" 2>/dev/null; then
            cat "$log_path" >&2
            echo "Editor exited before publishing the V3 target ($label)" >&2
            exit 1
        fi
        sleep 0.02
    done
    if [[ ! -f "$target" ]]; then
        cat "$log_path" >&2
        echo "Timed out waiting for the V3 target ($label)" >&2
        exit 1
    fi

    if [[ "$label" == "vulkan" && -n "$headed_monitor" ]]; then
        local address=""
        for _ in $(seq 1 200); do
            mapfile -t addresses < <(hyprctl clients -j | jq -r \
                '.[] | select(.class == "Spiral Editor" and .title == "Spiral Editor") | .address')
            if [[ "${#addresses[@]}" -eq 1 ]]; then
                address="${addresses[0]}"
                break
            fi
            if [[ "${#addresses[@]}" -gt 1 ]]; then
                echo "Cannot place an ambiguous set of Spiral Editor windows" >&2
                exit 1
            fi
            sleep 0.025
        done
        if [[ -z "$address" ]]; then
            echo "Timed out resolving the headed Spiral Editor window" >&2
            exit 1
        fi
        hyprctl dispatch \
            "hl.dsp.window.move({ workspace = $headed_workspace, follow = false, window = 'address:$address' })" \
            | grep -Fxq ok
        hyprctl dispatch "hl.dsp.focus({ window = 'address:$address' })" | grep -Fxq ok
        sleep 0.1
        local actual_monitor_id actual_monitor actual_workspace
        actual_monitor_id="$(hyprctl clients -j | jq -r --arg address "$address" \
            '.[] | select(.address == $address) | .monitor')"
        actual_workspace="$(hyprctl clients -j | jq -r --arg address "$address" \
            '.[] | select(.address == $address) | .workspace.id')"
        actual_monitor="$(hyprctl monitors -j | jq -r --argjson id "$actual_monitor_id" \
            '.[] | select(.id == $id) | .name')"
        if [[ "$actual_monitor" != "$headed_monitor"
            || "$actual_workspace" != "$headed_workspace" ]]; then
            echo "Headed Editor placement mismatch: expected $headed_monitor/workspace $headed_workspace, got $actual_monitor/workspace $actual_workspace" >&2
            exit 1
        fi
        echo "EditorSceneControlV3Placement address=$address monitor=$actual_monitor workspace=$actual_workspace result=pass"
        if [[ -n "$capture_dir" ]]; then
            sleep 0.1
            grim -o "$headed_monitor" "$capture_dir/$label-initial.png"
        fi
    fi
    if [[ "$label" == "vulkan" && "$headed_observation_seconds" != "0" ]]; then
        sleep "$headed_observation_seconds"
    fi

    grep -Fxq -- "SpiralEditorSceneControlTarget 3" <(head -n 1 "$target")
    grep -Fq -- "PrototypeEntityName \"Prototype Mesh\"" "$target"
    grep -Fq -- "LightEntityName \"Directional Light\"" "$target"
    grep -Fq -- "MainCameraEntityName \"Main Camera\"" "$target"
    grep -Fq -- "ExpectedDocumentMutations 10" "$target"
    grep -Fq -- "ExpectedTypedRequests 30" "$target"
    grep -Fq -- "ExpectedServerRejectedFixtures 4" "$target"

    local initial_selected prototype_id light_id camera_id
    initial_selected="$(awk '$1 == "InitialSelectedEntityId" { print $2 }' "$target")"
    prototype_id="$(awk '$1 == "PrototypeEntityId" { print $2 }' "$target")"
    light_id="$(awk '$1 == "LightEntityId" { print $2 }' "$target")"
    camera_id="$(awk '$1 == "MainCameraEntityId" { print $2 }' "$target")"
    [[ "$initial_selected" == "$camera_id" ]]

    local -a prototype_before prototype_after light_before light_after
    local -a camera_before camera_after color_before color_after
    local -a debug_before debug_after mesh_before mesh_after
    read -r -a prototype_before <<<"$(sed -n 's/^PrototypeTransformBefore //p' "$target")"
    read -r -a prototype_after <<<"$(sed -n 's/^PrototypeTransformAfter //p' "$target")"
    read -r -a light_before <<<"$(sed -n 's/^LightBefore //p' "$target")"
    read -r -a light_after <<<"$(sed -n 's/^LightAfter //p' "$target")"
    read -r -a camera_before <<<"$(sed -n 's/^MainCameraTransformBefore //p' "$target")"
    read -r -a camera_after <<<"$(sed -n 's/^MainCameraTransformAfter //p' "$target")"
    read -r -a color_before <<<"$(sed -n 's/^ColorPipelineBefore //p' "$target")"
    read -r -a color_after <<<"$(sed -n 's/^ColorPipelineAfter //p' "$target")"
    read -r -a debug_before <<<"$(sed -n 's/^DebugVisualizationBefore //p' "$target")"
    read -r -a debug_after <<<"$(sed -n 's/^DebugVisualizationAfter //p' "$target")"
    read -r -a mesh_before <<<"$(sed -n 's/^PrototypeMeshRendererFlagsBefore //p' "$target")"
    read -r -a mesh_after <<<"$(sed -n 's/^PrototypeMeshRendererFlagsAfter //p' "$target")"
    [[ "${#prototype_before[@]}" -eq 12 && "${#prototype_after[@]}" -eq 12 ]]
    [[ "${#light_before[@]}" -eq 10 && "${#light_after[@]}" -eq 10 ]]
    [[ "${#camera_before[@]}" -eq 12 && "${#camera_after[@]}" -eq 12 ]]
    [[ "${#color_before[@]}" -eq 7 && "${#color_after[@]}" -eq 7 ]]
    [[ "${#debug_before[@]}" -eq 2 && "${#debug_after[@]}" -eq 2 ]]
    [[ "${#mesh_before[@]}" -eq 2 && "${#mesh_after[@]}" -eq 2 ]]

    invoke() {
        local request_id="$1"
        shift
        python3 "$script_dir/EditorMaterialControl.py" \
            --control-dir "$control_dir" --expected-project "$project" \
            --request-id "$request_id" "$@"
    }
    observe_step() {
        if [[ "$label" == "vulkan" && -n "$capture_dir" ]]; then
            sleep 0.1
            grim -o "$headed_monitor" "$capture_dir/$label-$1.png"
        fi
        if [[ "$label" == "vulkan" && "$step_observation_seconds" != "0" ]]; then
            echo "EditorSceneControlV3Observation step=$1 seconds=$step_observation_seconds"
            sleep "$step_observation_seconds"
        fi
    }

    invoke v2-01-inspect inspect-entity \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        >"$smoke_root/$label-01.json"
    invoke v2-02-select-light select-entity \
        --entity-id "$light_id" --expected-name "Directional Light" \
        --expected-selected-entity-id "$camera_id" >"$smoke_root/$label-02.json"

    set +e
    invoke v2-03-stale-selection select-entity \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-selected-entity-id "$camera_id" >"$smoke_root/$label-03.json"
    local stale_selection_status=$?
    set -e
    [[ "$stale_selection_status" -eq 2 ]]

    invoke v2-04-light-set set-typed-light \
        --entity-id "$light_id" --expected-name "Directional Light" \
        --expected-light "${light_before[@]}" \
        --new-light "${light_after[@]}" >"$smoke_root/$label-04.json"
    observe_step typed-light-set

    set +e
    invoke v2-05-stale-light set-typed-light \
        --entity-id "$light_id" --expected-name "Directional Light" \
        --expected-light "${light_before[@]}" \
        --new-light "${light_after[@]}" >"$smoke_root/$label-05.json"
    local stale_light_status=$?
    set -e
    [[ "$stale_light_status" -eq 2 ]]

    invoke v2-06-light-restore set-typed-light \
        --entity-id "$light_id" --expected-name "Directional Light" \
        --expected-light "${light_after[@]}" \
        --new-light "${light_before[@]}" >"$smoke_root/$label-06.json"
    invoke v2-07-select-prototype select-entity \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-selected-entity-id "$light_id" >"$smoke_root/$label-07.json"
    invoke v2-08-transform-set set-transform \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-transform "${prototype_before[@]}" \
        --new-transform "${prototype_after[@]}" >"$smoke_root/$label-08.json"
    observe_step transform-set

    set +e
    invoke v2-09-transform-forced-rollback set-transform \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-transform "${prototype_after[@]}" \
        --new-transform "${prototype_before[@]}" >"$smoke_root/$label-09.json"
    local forced_rollback_status=$?
    set -e
    [[ "$forced_rollback_status" -eq 2 ]]

    invoke v2-10-transform-restore set-transform \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-transform "${prototype_after[@]}" \
        --new-transform "${prototype_before[@]}" >"$smoke_root/$label-10.json"
    invoke v2-11-color-set set-project-color-pipeline \
        --expected-color-pipeline "${color_before[@]}" \
        --new-color-pipeline "${color_after[@]}" >"$smoke_root/$label-11.json"
    observe_step color-pipeline-set

    set +e
    invoke v2-12-stale-color set-project-color-pipeline \
        --expected-color-pipeline "${color_before[@]}" \
        --new-color-pipeline "${color_after[@]}" >"$smoke_root/$label-12.json"
    local stale_color_status=$?
    set -e
    [[ "$stale_color_status" -eq 2 ]]

    invoke v2-13-color-restore set-project-color-pipeline \
        --expected-color-pipeline "${color_after[@]}" \
        --new-color-pipeline "${color_before[@]}" >"$smoke_root/$label-13.json"
    invoke v2-14-select-camera select-entity \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        --expected-selected-entity-id "$prototype_id" >"$smoke_root/$label-14.json"

    set +e
    invoke v2-15-stale-camera set-viewport-main-camera-pose \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        --expected-transform "${camera_after[@]}" \
        --new-transform "${camera_before[@]}" >"$smoke_root/$label-15.json"
    local stale_camera_status=$?
    set -e
    [[ "$stale_camera_status" -eq 2 ]]

    set +e
    invoke v2-16-generic-camera-transform set-transform \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        --expected-transform "${camera_before[@]}" \
        --new-transform "${camera_after[@]}" >"$smoke_root/$label-16.json"
    local generic_camera_status=$?
    set -e
    [[ "$generic_camera_status" -eq 2 ]]

    invoke v2-17-camera-set set-viewport-main-camera-pose \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        --expected-transform "${camera_before[@]}" \
        --new-transform "${camera_after[@]}" >"$smoke_root/$label-17.json"
    observe_step main-camera-pose-set
    invoke v2-18-camera-restore set-viewport-main-camera-pose \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        --expected-transform "${camera_after[@]}" \
        --new-transform "${camera_before[@]}" >"$smoke_root/$label-18.json"

    set +e
    invoke v2-19-stale-transform set-transform \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-transform "${prototype_after[@]}" \
        --new-transform "${prototype_before[@]}" \
        >"$smoke_root/$label-19.json" 2>"$smoke_root/$label-19.error"
    local stale_transform_status=$?
    set -e
    if [[ "$stale_transform_status" -ne 2 ]]; then
        cat "$smoke_root/$label-19.error" >&2
        echo "Stale transform did not produce a typed rejection ($label)" >&2
        exit 1
    fi

    invoke v2-20-select-prototype-debug select-entity \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-selected-entity-id "$camera_id" >"$smoke_root/$label-20.json"
    invoke v2-21-debug-set set-scene-debug-visualization \
        --expected-selected-entity-id "$prototype_id" \
        --expected-view "${debug_before[0]}" --expected-selected-bounds "${debug_before[1]}" \
        --new-view "${debug_after[0]}" --new-selected-bounds "${debug_after[1]}" \
        >"$smoke_root/$label-21.json"
    observe_step debug-visualization-set

    set +e
    invoke v2-22-stale-debug-state set-scene-debug-visualization \
        --expected-selected-entity-id "$prototype_id" \
        --expected-view "${debug_before[0]}" --expected-selected-bounds "${debug_before[1]}" \
        --new-view "${debug_after[0]}" --new-selected-bounds "${debug_after[1]}" \
        >"$smoke_root/$label-22.json"
    local stale_debug_state_status=$?
    set -e
    [[ "$stale_debug_state_status" -eq 2 ]]

    set +e
    invoke v2-23-stale-debug-selection set-scene-debug-visualization \
        --expected-selected-entity-id "$camera_id" \
        --expected-view "${debug_after[0]}" --expected-selected-bounds "${debug_after[1]}" \
        --new-view "${debug_before[0]}" --new-selected-bounds "${debug_before[1]}" \
        >"$smoke_root/$label-23.json"
    local stale_debug_selection_status=$?
    set -e
    [[ "$stale_debug_selection_status" -eq 2 ]]

    set +e
    invoke v2-24-debug-forced-rollback set-scene-debug-visualization \
        --expected-selected-entity-id "$prototype_id" \
        --expected-view "${debug_after[0]}" --expected-selected-bounds "${debug_after[1]}" \
        --new-view "${debug_before[0]}" --new-selected-bounds "${debug_before[1]}" \
        >"$smoke_root/$label-24.json"
    local debug_rollback_status=$?
    set -e
    [[ "$debug_rollback_status" -eq 2 ]]

    invoke v2-25-debug-restore set-scene-debug-visualization \
        --expected-selected-entity-id "$prototype_id" \
        --expected-view "${debug_after[0]}" --expected-selected-bounds "${debug_after[1]}" \
        --new-view "${debug_before[0]}" --new-selected-bounds "${debug_before[1]}" \
        >"$smoke_root/$label-25.json"
    invoke v2-26-mesh-set set-mesh-renderer-flags \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-visible "${mesh_before[0]}" --expected-casts-shadows "${mesh_before[1]}" \
        --new-visible "${mesh_after[0]}" --new-casts-shadows "${mesh_after[1]}" \
        >"$smoke_root/$label-26.json"
    observe_step mesh-flags-set

    set +e
    invoke v2-27-stale-mesh set-mesh-renderer-flags \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-visible "${mesh_before[0]}" --expected-casts-shadows "${mesh_before[1]}" \
        --new-visible "${mesh_after[0]}" --new-casts-shadows "${mesh_after[1]}" \
        >"$smoke_root/$label-27.json"
    local stale_mesh_status=$?
    set -e
    [[ "$stale_mesh_status" -eq 2 ]]

    invoke v2-28-mesh-restore set-mesh-renderer-flags \
        --entity-id "$prototype_id" --expected-name "Prototype Mesh" \
        --expected-visible "${mesh_after[0]}" --expected-casts-shadows "${mesh_after[1]}" \
        --new-visible "${mesh_before[0]}" --new-casts-shadows "${mesh_before[1]}" \
        >"$smoke_root/$label-28.json"
    invoke v2-29-select-camera-restore select-entity \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        --expected-selected-entity-id "$prototype_id" >"$smoke_root/$label-29.json"

    python3 - "$smoke_root" "$label" <<'PY'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
label = sys.argv[2]
expected = {
    3: ("compare_and_swap_state_mismatch", "None", False),
    5: ("compare_and_swap_state_mismatch", "None", False),
    9: ("injected_postcondition_failure_rolled_back", "RolledBack", True),
    12: ("compare_and_swap_state_mismatch", "None", False),
    15: ("compare_and_swap_state_mismatch", "None", False),
    16: ("main_camera_pose_action_required", "None", False),
    19: ("compare_and_swap_state_mismatch", "None", False),
    22: ("compare_and_swap_state_mismatch", "None", False),
    23: ("compare_and_swap_state_mismatch", "None", False),
    24: ("injected_postcondition_failure_rolled_back", "RolledBack", True),
    27: ("compare_and_swap_state_mismatch", "None", False),
}
for index, (reason, effect, rolled_back) in expected.items():
    receipt = json.load((root / f"{label}-{index:02}.json").open(encoding="utf-8"))
    assert receipt["schema"] == 3 and receipt["status"] == "Rejected"
    assert receipt["reason"] == reason and receipt["effect"] == effect
    assert receipt["recovery"] == "None" and not receipt["postconditionVerified"]
    assert receipt["rollbackVerified"] == rolled_back
    assert receipt["undoDepthAfter"] == receipt["undoDepthBefore"]
    assert not receipt["selectionCommitted"] and not receipt["pivotRetargeted"]
PY

    local -a invalid_camera=("${camera_before[@]}")
    invalid_camera[9]=2
    set +e
    invoke v2-invalid-camera-scale set-viewport-main-camera-pose \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        --expected-transform "${camera_before[@]}" \
        --new-transform "${invalid_camera[@]}" \
        >"$smoke_root/$label-invalid-camera.json" \
        2>"$smoke_root/$label-invalid-camera.error"
    local invalid_camera_status=$?
    set -e
    if [[ "$invalid_camera_status" -ne 1 ]]; then
        echo "Helper accepted non-unit main-camera scale ($label)" >&2
        exit 1
    fi
    grep -Fq -- "main-camera scale must be exactly 1 1 1" \
        "$smoke_root/$label-invalid-camera.error"
    test ! -e "$control_dir/requests/v2-invalid-camera-scale.request"

    invoke v2-30-final-inspect inspect-entity \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        >"$smoke_root/$label-30.json"
    observe_step final-restored

    if ! wait "$live_process"; then
        live_process=""
        cat "$log_path" >&2
        echo "Editor scene-control V3 process failed ($label)" >&2
        exit 1
    fi
    live_process=""
    grep -Fq -- "EditorSceneControlV3 producer=external-python" "$log_path"
    grep -Fq -- "backend=$expected_backend result=pass" "$log_path"
    grep -Fq -- "Renderer initialized with backend: $expected_backend" "$log_path"
    test -f "$control_dir/session.closed"
    test "$(stat -c '%a' "$control_dir/session.info")" = "600"
    test "$(stat -c '%a' "$control_dir/responses/v2-30-final-inspect.response")" = "600"
    grep -Fq -- 'Reason "wrong_project"' \
        "$control_dir/responses/v2-project-wrong.response"
    grep -Fq -- 'Reason "missing_or_unexpected_action_field"' \
        "$control_dir/responses/v2-mask-unexpected.response"
    grep -Fq -- 'Reason "invalid_or_duplicate_entity_id"' \
        "$control_dir/responses/v2-mask-duplicate.response"
    grep -Fq -- 'Reason "unsupported_schema_expected_v3"' \
        "$control_dir/responses/v2-schema-stale.response"

    python3 - "$smoke_root" "$label" "$project" <<'PY'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
label = sys.argv[2]
project = sys.argv[3]
receipts = [json.load(path.open(encoding="utf-8"))
            for path in sorted(root.glob(f"{label}-[0-9][0-9].json"))]
assert len(receipts) == 30
assert all(value["schema"] == 3 for value in receipts)
assert all(value["projectPath"] == project for value in receipts)
assert all(value["editorProcessId"] > 0 for value in receipts)
assert sum(value["status"] == "Succeeded" for value in receipts) == 19
assert sum(value["status"] == "Rejected" for value in receipts) == 11
assert sum(bool(value["rollbackVerified"]) for value in receipts) == 2
debug_set, debug_rollback, debug_restore = receipts[20], receipts[23], receipts[24]
assert debug_set["selectedEntityIdBefore"] == debug_set["selectedEntityIdAfter"]
assert debug_rollback["beforeDebugVisualization"] == debug_set["afterDebugVisualization"]
assert debug_rollback["afterDebugVisualization"] == debug_set["afterDebugVisualization"]
assert debug_rollback["selectedEntityIdBefore"] == debug_set["selectedEntityIdAfter"]
assert debug_rollback["selectedEntityIdAfter"] == debug_set["selectedEntityIdAfter"]
assert debug_rollback["rendererReadbackVerified"] and debug_rollback["rollbackVerified"]
assert debug_restore["afterDebugVisualization"] == debug_set["beforeDebugVisualization"]
mesh_set, mesh_restore = receipts[25], receipts[27]
assert mesh_set["undoDepthAfter"] == mesh_set["undoDepthBefore"] + 1
assert mesh_restore["undoDepthAfter"] == mesh_restore["undoDepthBefore"] + 1
assert mesh_restore["afterMeshRenderer"] == mesh_set["beforeMeshRenderer"]
PY
}

cd -- "$repo_root"
run_sequence headless Headless --headless
if [[ "$run_vulkan" == "--vulkan" ]]; then
    run_sequence vulkan "NVRHI Vulkan" --renderer-vulkan
fi

after_fingerprint="$(fingerprint)"
if [[ "$after_fingerprint" != "$before_fingerprint" ]]; then
    diff -u <(printf '%s\n' "$before_fingerprint") \
        <(printf '%s\n' "$after_fingerprint") >&2 || true
    echo "Scene-control V3 changed persistent project bytes" >&2
    exit 1
fi

echo "EditorSceneControlV3Test helper=typed schema=3 requests=30 succeeded=19 rejected=11 headless=pass vulkan=$([[ "$run_vulkan" == "--vulkan" ]] && echo pass || echo skipped) security=project-mask-duplicate-rejected cas=selection-transform-light-color-camera-debug-mesh stale=rejected selectedIdentity=bound mainCameraAuthority=dedicated invalid-camera=client-rejected rollbacks=2-verified history=one-per-document-action restore=exact save=not-invoked persistentBytes=unchanged input=no-ui-synthesis result=pass"
