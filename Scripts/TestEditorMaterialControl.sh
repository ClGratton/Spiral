#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
editor="${1:-$repo_root/bin/Debug-linux-x86_64-gmake/Editor/Editor}"

if [[ ! -x "$editor" ]]; then
    echo "Editor executable is missing or not executable: $editor" >&2
    exit 1
fi

python3 "$script_dir/EditorMaterialControl.py" --help >/dev/null

smoke_root="$(mktemp -d "${TMPDIR:-/tmp}/spiral-editor-control-smoke.XXXXXX")"
control_dir="$smoke_root/mailbox"
log_path="$smoke_root/editor.log"
cleanup() {
    if [[ -n "${live_process:-}" ]] && kill -0 "$live_process" 2>/dev/null; then
        kill "$live_process" 2>/dev/null || true
        wait "$live_process" 2>/dev/null || true
    fi
    rm -rf -- "$smoke_root"
}
trap cleanup EXIT

cd -- "$repo_root"
timeout 20s "$editor" --headless \
    "--editor-control-dir=$control_dir" \
    --editor-control-mailbox-smoke 2>&1 | tee "$log_path"

grep -Fq -- "EditorMaterialControlMailboxV1 interface=private-filesystem" "$log_path"
grep -Fq -- "input=mailbox-no-ui-synthesis pollCadenceMs=16" "$log_path"
grep -Fq -- "result=pass" "$log_path"
test -f "$control_dir/session.info"
test -f "$control_dir/session.closed"
test "$(stat -c '%a' "$control_dir")" = "700"
test "$(stat -c '%a' "$control_dir/requests")" = "700"
test "$(stat -c '%a' "$control_dir/responses")" = "700"
test "$(stat -c '%a' "$control_dir/session.info")" = "600"
test "$(stat -c '%a' "$control_dir/session.closed")" = "600"
grep -Eq '^ProcessId [1-9][0-9]*$' "$control_dir/session.info"
grep -Fq -- "ProjectPath \"$repo_root/output/projects/default.spiralproject\"" \
    "$control_dir/session.info"

inspect_response="$control_dir/responses/smoke-05-inspect.response"
entity_id="$(awk '$1 == "EntityId" { print $2 }' "$inspect_response")"
material_handle="$(awk '$1 == "MaterialHandle" { print $2 }' "$inspect_response")"
python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id smoke-05-inspect \
    inspect --entity-id "$entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$material_handle" >"$smoke_root/helper-inspect.json"
python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id smoke-07-patch \
    set --entity-id "$entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$material_handle" \
    --expected-base 0.62 0.22 0.14 --expected-metallic 0.35 --expected-roughness 0.74 \
    --new-base 0.21 0.43 0.67 --new-metallic 0.77 --new-roughness 0.31 \
    >"$smoke_root/helper-set.json"
set +e
python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id smoke-07-patch \
    set --entity-id "$entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$material_handle" \
    --expected-base 0.62 0.22 0.14 --expected-metallic 0.35 --expected-roughness 0.74 \
    --new-base 0.25 0.45 0.65 --new-metallic 0.75 --new-roughness 0.33 \
    >"$smoke_root/helper-conflict.json" 2>"$smoke_root/helper-conflict.error"
conflict_status=$?
set -e
if [[ "$conflict_status" -ne 2 ]]; then
    cat "$smoke_root/helper-conflict.error" >&2
    echo "Helper did not return a typed request-ID conflict" >&2
    exit 1
fi
python3 - "$smoke_root/helper-conflict.json" <<'PY'
import json
import sys

receipt = json.load(open(sys.argv[1], encoding="utf-8"))
assert receipt["status"] == "Rejected"
assert receipt["reason"] == "request_id_conflict"
assert receipt["requestDigest"] == "not-applicable"
assert receipt["action"] == "Unknown" and receipt["frame"] == 0
assert receipt["effect"] == "None" and receipt["recovery"] == "None"
PY
test ! -e "$control_dir/requests/smoke-07-patch.request"
if python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id .hidden-request \
    inspect --entity-id "$entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$material_handle" \
    >"$smoke_root/hidden-helper.json" 2>"$smoke_root/hidden-helper.error"; then
    echo "Helper unexpectedly accepted a leading-dot request ID" >&2
    exit 1
fi
grep -Fq -- "request ID must not start with '.'" "$smoke_root/hidden-helper.error"

live_control_dir="$smoke_root/live-mailbox"
live_log="$smoke_root/live-editor.log"
timeout 20s "$editor" --headless \
    "--editor-control-dir=$live_control_dir" \
    --editor-control-live-helper-smoke >"$live_log" 2>&1 &
live_process=$!
for _ in $(seq 1 250); do
    if [[ -f "$live_control_dir/live-target.info" ]]; then
        break
    fi
    if ! kill -0 "$live_process" 2>/dev/null; then
        cat "$live_log" >&2
        exit 1
    fi
    sleep 0.02
done
target="$live_control_dir/live-target.info"
if [[ ! -f "$target" ]]; then
    cat "$live_log" >&2
    echo "Timed out waiting for live Editor material-control target" >&2
    exit 1
fi
live_entity_id="$(awk '$1 == "EntityId" { print $2 }' "$target")"
live_material_handle="$(awk '$1 == "MaterialHandle" { print $2 }' "$target")"
read -r _ before_r before_g before_b before_metallic before_roughness \
    < <(grep '^BeforeSurface ' "$target")
read -r _ after_r after_g after_b after_metallic after_roughness \
    < <(grep '^AfterSurface ' "$target")
python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$live_control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id live-helper-inspect \
    inspect --entity-id "$live_entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$live_material_handle" >"$smoke_root/live-inspect.json"
python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$live_control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id live-helper-set \
    set --entity-id "$live_entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$live_material_handle" \
    --expected-base "$before_r" "$before_g" "$before_b" \
    --expected-metallic "$before_metallic" --expected-roughness "$before_roughness" \
    --new-base "$after_r" "$after_g" "$after_b" \
    --new-metallic "$after_metallic" --new-roughness "$after_roughness" \
    >"$smoke_root/live-set.json"
wait "$live_process"
live_process=""
grep -Fq -- "EditorMaterialControlLiveHelperV1 producer=external-python requests=fresh" "$live_log"
grep -Fq -- "result=pass" "$live_log"
python3 - "$smoke_root/live-inspect.json" "$smoke_root/live-set.json" \
    "$live_entity_id" "$live_material_handle" \
    "$before_r" "$before_g" "$before_b" "$before_metallic" "$before_roughness" \
    "$after_r" "$after_g" "$after_b" "$after_metallic" "$after_roughness" \
    "$repo_root/output/projects/default.spiralproject" <<'PY'
import json
import struct
import sys

inspect = json.load(open(sys.argv[1], encoding="utf-8"))
patch = json.load(open(sys.argv[2], encoding="utf-8"))
entity_id = int(sys.argv[3])
material_handle = int(sys.argv[4])
f32 = lambda value: struct.unpack("=f", struct.pack("=f", float(value)))[0]
before = [f32(value) for value in sys.argv[5:10]]
after = [f32(value) for value in sys.argv[10:15]]
project_path = sys.argv[15]
assert inspect["status"] == "Succeeded" and inspect["effect"] == "ReadOnly"
assert inspect["entityId"] == entity_id and inspect["materialHandle"] == material_handle
assert inspect["entityName"] == "Prototype Mesh"
assert inspect["beforeSurface"] == before and inspect["afterSurface"] == before
assert not inspect["selectionCommitted"] and not inspect["pivotRetargeted"]
assert inspect["rendererReadbackVerified"]
assert inspect["recovery"] == "None"
assert inspect["undoDepthAfter"] == inspect["undoDepthBefore"]
assert inspect["redoDepthAfter"] == inspect["redoDepthBefore"]
assert inspect["affectedEntityCount"] == 2 and entity_id in inspect["affectedEntityIds"]
assert patch["status"] == "Succeeded" and patch["effect"] == "SharedMaterialSurfacePatched"
assert patch["entityId"] == entity_id and patch["materialHandle"] == material_handle
assert patch["entityName"] == "Prototype Mesh"
assert patch["beforeSurface"] == before and patch["afterSurface"] == after
assert patch["recovery"] == "UndoRedo" and patch["selectionCommitted"]
assert patch["pivotRetargeted"] and patch["rendererReadbackVerified"]
assert entity_id in patch["affectedEntityIds"]
assert patch["affectedEntityCount"] == 2 and not patch["affectedEntityIdsTruncated"]
assert patch["undoDepthAfter"] == min(patch["undoDepthBefore"] + 1, 128)
assert patch["redoDepthAfter"] == 0
assert patch["rendererGeneration"] == inspect["rendererGeneration"] + 1
assert inspect["editorProcessId"] == patch["editorProcessId"] > 0
assert inspect["projectPath"] == patch["projectPath"] == project_path
PY

durability_control_dir="$smoke_root/durability-mailbox"
durability_log="$smoke_root/durability-editor.log"
timeout 20s "$editor" --headless \
    "--editor-control-dir=$durability_control_dir" \
    --editor-control-durability-smoke >"$durability_log" 2>&1
grep -Fq -- "EditorMaterialControlDurabilityV1 visibility=rename-authoritative parentSync=injected-failure committed=preserved rollback=no acceptance=closed crashDurability=degraded result=pass" "$durability_log"
grep -Fq -- "publication is visible but not confirmed crash-durable" "$durability_log"
test -f "$durability_control_dir/session.closed"
durability_response="$durability_control_dir/responses/durability-visible-success.response"
durability_entity_id="$(awk '$1 == "EntityId" { print $2 }' "$durability_response")"
durability_material_handle="$(awk '$1 == "MaterialHandle" { print $2 }' "$durability_response")"
python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$durability_control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id durability-visible-success \
    set --entity-id "$durability_entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$durability_material_handle" \
    --expected-base 0.62 0.22 0.14 --expected-metallic 0.35 --expected-roughness 0.74 \
    --new-base 0.21 0.43 0.67 --new-metallic 0.77 --new-roughness 0.31 \
    >"$smoke_root/durability-replay.json"
python3 - "$smoke_root/durability-replay.json" <<'PY'
import json
import sys

receipt = json.load(open(sys.argv[1], encoding="utf-8"))
assert receipt["status"] == "Succeeded"
assert receipt["effect"] == "SharedMaterialSurfacePatched"
assert receipt["recovery"] == "UndoRedo"
assert receipt["rendererReadbackVerified"]
assert receipt["selectionCommitted"] and receipt["pivotRetargeted"]
PY

capacity_control_dir="$smoke_root/capacity-mailbox"
capacity_log="$smoke_root/capacity-editor.log"
timeout 20s "$editor" --headless \
    "--editor-control-dir=$capacity_control_dir" \
    --editor-control-capacity-smoke >"$capacity_log" 2>&1
grep -Fq -- "EditorMaterialControlCapacityV1 retained=256 accepting=no pendingUnclaimed=1 response=absent session=closed affectedTotal=42 affectedSample=32 truncated=yes result=pass" "$capacity_log"
test -f "$capacity_control_dir/requests/capacity-0256.request"
test ! -e "$capacity_control_dir/responses/capacity-0256.response"
capacity_response="$capacity_control_dir/responses/capacity-0000.response"
capacity_entity_id="$(awk '$1 == "EntityId" { print $2 }' "$capacity_response")"
capacity_material_handle="$(awk '$1 == "MaterialHandle" { print $2 }' "$capacity_response")"
python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$capacity_control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id capacity-0000 \
    inspect --entity-id "$capacity_entity_id" --expected-name "Prototype Mesh" \
    --material-handle "$capacity_material_handle" >"$smoke_root/capacity-replay.json"
python3 - "$smoke_root/capacity-replay.json" "$capacity_entity_id" <<'PY'
import json
import sys

receipt = json.load(open(sys.argv[1], encoding="utf-8"))
entity_id = int(sys.argv[2])
assert receipt["status"] == "Succeeded" and receipt["effect"] == "ReadOnly"
assert receipt["recovery"] == "None" and receipt["rendererReadbackVerified"]
assert receipt["beforeSurface"] == receipt["afterSurface"]
assert receipt["affectedEntityCount"] == 42
assert len(receipt["affectedEntityIds"]) == 32
assert receipt["affectedEntityIds"] == sorted(set(receipt["affectedEntityIds"]))
assert receipt["affectedEntityIdsTruncated"] and entity_id in receipt["affectedEntityIds"]
PY
if python3 "$script_dir/EditorMaterialControl.py" \
    --control-dir "$capacity_control_dir" \
    --expected-project "$repo_root/output/projects/default.spiralproject" \
    --request-id capacity-after-close \
    inspect --entity-id 1 --expected-name "Main Camera" --material-handle 1 \
    >"$smoke_root/capacity-helper.json" 2>"$smoke_root/capacity-helper.error"; then
    echo "Helper unexpectedly accepted a fresh request for a closed session" >&2
    exit 1
fi
grep -Fq -- "editor-control session is closed" "$smoke_root/capacity-helper.error"

echo "EditorMaterialControlTestV1 internal=pass conflicts=A-B-C-consumed liveHelper=fresh-inspect-set durability=visible-success-preserved capacity=retained private=pass cleanup=bounded result=pass"
