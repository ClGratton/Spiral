#!/usr/bin/env bash
# Headless end-to-end test of the Editor Fab import workflow through the typed
# control (schema 5): GLB import -> provenance -> confirm -> commit -> place ->
# save, then a relaunch of the same --project that proves save/reopen, identical
# reimport (ExactReuse), hostile and corrupt packages, cancellation, an
# assignment commit, immutable-material protection, and project validation.
# It opens no window, uses no synthetic input, and never touches the default
# project: every project file lives under a temporary directory.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
editor="${1:-$repo_root/bin/Debug-linux-x86_64-gmake/Editor/Editor}"

if [[ ! -x "$editor" ]]; then
    echo "Editor executable is missing or not executable: $editor" >&2
    exit 1
fi
python3 "$script_dir/EditorMaterialControl.py" --help >/dev/null

smoke_root="$(mktemp -d "${TMPDIR:-/tmp}/spiral-fab-import.XXXXXX")"
out="$smoke_root/receipts"
mkdir -p "$out"
live_process=""
control_dir=""
log_path=""
cleanup() {
    if [[ -n "$live_process" ]] && kill -0 "$live_process" 2>/dev/null; then
        kill "$live_process" 2>/dev/null || true
        wait "$live_process" 2>/dev/null || true
    fi
    if [[ "${SPIRAL_KEEP_SMOKE_ARTIFACTS:-0}" == "1" ]]; then
        echo "Preserved Fab import smoke artifacts: $smoke_root" >&2
    else
        rm -rf -- "$smoke_root"
    fi
}
trap cleanup EXIT

fail() {
    echo "FabImportTest failure: $*" >&2
    if [[ -n "$log_path" && -f "$log_path" ]]; then
        echo "---- last Editor log lines ----" >&2
        tail -n 40 "$log_path" >&2 || true
    fi
    exit 1
}

# The default project must be byte-identical afterwards (the test names its own).
default_files=(
    "$repo_root/output/projects/default.spiralproject"
    "$repo_root/output/scenes/sample.spiral"
    "$repo_root/output/assets/sample.assets"
    "$repo_root/output/assets/PrototypeDefault.spiralmat"
)
default_fingerprint() {
    local path
    for path in "${default_files[@]}"; do
        if [[ -f "$path" ]]; then sha256sum -- "$path"; else printf 'missing  %s\n' "$path"; fi
    done
}
default_before="$(default_fingerprint)"

# ---- fixtures (generated, deterministic, no licensed content) ----
fixtures="$smoke_root/fixtures"
python3 - "$fixtures" <<'PY'
import json
import struct
import sys
import zipfile
import zlib
from pathlib import Path


def png(seed: int = 0, corrupt_crc: bool = False) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    rgba = bytes([10 + seed, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255])
    raw = b"".join(b"\x00" + rgba[row * 8:(row + 1) * 8] for row in range(2))
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    if corrupt_crc:
        data = bytearray(data)
        data[-20] ^= 0x55
        data = bytes(data)
    return data


def geometry() -> bytes:
    positions = [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]
    normals = [0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]
    uvs = [0, 0, 1, 0, 1, 1, 0, 1]
    return (struct.pack("<12f", *positions) + struct.pack("<12f", *normals)
            + struct.pack("<8f", *uvs) + struct.pack("<6H", 0, 1, 2, 0, 2, 3))


def glb(seed: int = 0, corrupt_png: bool = False) -> bytes:
    image = png(seed, corrupt_png)
    binary = geometry() + image
    binary += b"\x00" * (-len(binary) % 4)
    document = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                    "indices": 3, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                                "metallicFactor": 0.5, "roughnessFactor": 0.5}}],
        "textures": [{"source": 0}], "images": [{"bufferView": 4, "mimeType": "image/png"}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 48},
            {"buffer": 0, "byteOffset": 48, "byteLength": 48},
            {"buffer": 0, "byteOffset": 96, "byteLength": 32},
            {"buffer": 0, "byteOffset": 128, "byteLength": 12},
            {"buffer": 0, "byteOffset": 140, "byteLength": len(image)}],
        "buffers": [{"byteLength": len(binary)}],
    }
    text = json.dumps(document, separators=(",", ":")).encode("ascii")
    text += b" " * (-len(text) % 4)
    total = 12 + 8 + len(text) + 8 + len(binary)
    return (b"glTF" + struct.pack("<II", 2, total) + struct.pack("<I", len(text)) + b"JSON" + text
            + struct.pack("<I", len(binary)) + b"BIN\x00" + binary)


def write_zip(path: Path, members: list[tuple[str, bytes]], stored: bool = False) -> None:
    method = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    with zipfile.ZipFile(path, "w", method) as archive:
        for name, data in members:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = method
            info.external_attr = 0o644 << 16
            archive.writestr(info, data)


def main() -> None:
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    good = glb()
    (root / "good.glb").write_bytes(good)
    write_zip(root / "good.zip", [("model.glb", good)])
    (root / "badpng.glb").write_bytes(glb(corrupt_png=True))

    # The same asset as a split glTF package folder (external .bin and .png).
    folder = root / "package"
    folder.mkdir()
    image = png()
    (folder / "model.bin").write_bytes(geometry())
    (folder / "base.png").write_bytes(image)
    document = json.loads(glb()[20:20 + struct.unpack("<I", glb()[12:16])[0]].decode("ascii"))
    document["images"] = [{"uri": "base.png"}]
    document["bufferViews"] = document["bufferViews"][:4]
    document["buffers"] = [{"uri": "model.bin", "byteLength": len(geometry())}]
    (folder / "model.gltf").write_text(json.dumps(document))

    # Stored (uncompressed) so a flipped payload byte is a CRC failure at extraction,
    # while the archive structure still classifies as a ZIP.
    write_zip(root / "corrupt.zip", [("model.glb", good)], stored=True)
    data = bytearray((root / "corrupt.zip").read_bytes())
    offset = data.index(good[200:216]) + 4
    data[offset] ^= 0xFF
    (root / "corrupt.zip").write_bytes(bytes(data))

    write_zip(root / "traversal.zip", [("../evil.txt", b"escaped\n"), ("model.glb", good)])
    (root / "truncated.zip").write_bytes((root / "good.zip").read_bytes()[:-40])
    (root / "notes.txt").write_bytes(b"not a package\n")


main()
PY
cat >"$smoke_root/fingerprint.py" <<'PY'
import hashlib
import sys
from pathlib import Path

root = Path(sys.argv[1])
manifest = Path(sys.argv[2])
mode = sys.argv[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "missing"


lines = []
if mode == "all":
    paths = {}
    for line in manifest.read_text().splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0] in ("Scene", "AssetRegistry", "FabReceipts"):
            paths[parts[0]] = parts[1].strip().strip('"')
    lines.append(f"manifest {digest(manifest)}")
    for key in sorted(paths):
        lines.append(f"{key} {paths[key]} {digest(root / paths[key])}")
    for entry in sorted(root.rglob("*")):
        # The cook stages a candidate generation under .fab-staging while a job
        # waits at ReadyToCommit; it is transaction-owned and checked separately.
        if ".fab-staging" not in entry.relative_to(root).parts:
            lines.append(f"tree {entry.relative_to(root)}")
generations = root / "Assets" / "fab"
if generations.is_dir():
    for entry in sorted(generations.rglob("*")):
        if entry.is_file():
            lines.append(f"generation {entry.relative_to(root)} {digest(entry)}")
print("\n".join(lines))
PY

project_dir="$smoke_root/project"
project="$project_dir/fabtest.spiralproject"
export XDG_CACHE_HOME="$smoke_root/cache"
export XDG_DATA_HOME="$smoke_root/data"
staging_root="$XDG_CACHE_HOME/Spiral/FabStaging"

fingerprint() { python3 "$smoke_root/fingerprint.py" "$project_dir" "$project" "$1"; }

# ---- request helpers ----
run() {
    local id="$1"
    shift
    set +e
    python3 "$script_dir/EditorMaterialControl.py" --control-dir "$control_dir" \
        --expected-project "$project" --request-id "$id" "$@" \
        >"$out/$id.json" 2>"$out/$id.err"
    local status=$?
    set -e
    return "$status"
}
val() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$out/$1.json" "$2"; }
ok() {
    local id="$1"
    run "$@" || { cat "$out/$id.err" "$out/$id.json" >&2 || true; fail "request $id was expected to succeed"; }
}
rejected() {
    local id="$1" reason="$2" status=0
    shift 2
    run "$id" "$@" || status=$?
    if [[ "$status" -ne 2 ]]; then
        cat "$out/$id.err" "$out/$id.json" >&2 || true
        fail "request $id was expected to be rejected (exit $status)"
    fi
    [[ "$(val "$id" 'd["reason"]')" == "$reason" ]] \
        || fail "request $id reason was $(val "$id" 'd["reason"]'), expected $reason"
}
client_refused() {
    local id="$1" message="$2" status=0
    shift 2
    run "$id" "$@" || status=$?
    [[ "$status" -eq 1 ]] || fail "helper accepted request $id that it must refuse client-side"
    grep -Fq -- "$message" "$out/$id.err" || fail "helper message for $id lacks: $message"
    [[ ! -e "$control_dir/requests/$id.request" ]] || fail "request $id reached the mailbox"
}
expect() {
    local id="$1" expression="$2" description="$3"
    [[ "$(val "$id" "$expression")" == "True" ]] || fail "$id: $description"
}

launch() {
    local label="$1" flag="$2" budget="$3"
    control_dir="$smoke_root/$label-mailbox"
    log_path="$smoke_root/$label-editor.log"
    timeout "${budget}s" "$editor" --headless "--project=$project" \
        "--editor-control-dir=$control_dir" "$flag" >"$log_path" 2>&1 &
    live_process=$!
    local target="$control_dir/fab-control-target.info"
    for _ in $(seq 1 600); do
        [[ -f "$target" ]] && return 0
        kill -0 "$live_process" 2>/dev/null || fail "Editor exited before publishing the Fab target ($label)"
        sleep 0.02
    done
    fail "timed out waiting for the Fab target ($label)"
}
finish() {
    local marker="$1"
    if ! wait "$live_process"; then
        live_process=""
        fail "Editor process failed"
    fi
    live_process=""
    grep -Fq -- "$marker" "$log_path" || fail "Editor log lacks: $marker"
    grep -Fq -- "result=pass" "$log_path" || fail "Editor smoke did not report result=pass"
    test -f "$control_dir/session.closed" || fail "session was not closed"
}
target_value() { awk -v key="$1" '$1 == key { print $2 }' "$control_dir/fab-control-target.info"; }
inbox_put() { install -m 600 -- "$fixtures/$1" "$control_dir/fab-inbox/${2:-$1}"; }
staging_clean() {
    [[ -z "$(find "$staging_root" -mindepth 1 2>/dev/null)" ]] || fail "package staging is not clean: $(find "$staging_root" -mindepth 1)"
    [[ ! -e "$project_dir/.fab-staging" ]] || fail "cook staging was left in the project"
}

# Import flow shared by both phases. Leaves the job at ReadyToCommit.
provenance_args=(
    --product-identity "https://www.fab.com/listings/0b4e2f6a-5c1d-4e7a-9a3b-1f2e3d4c5b6a"
    --product-name "Test Quad" --publisher "Test Publisher" --version-label "v1"
    --license-family FabStandard --license-tier Personal
    --no-ai No --generated-with-ai No --raw-source-policy ExcludedFromProject
)
to_ready() { # prefix leaf kind -> job id in $job
    local prefix="$1" leaf="$2" kind="$3"
    inbox_put "$leaf"
    ok "$prefix-select" select-fab-package --inbox-name "$leaf" --kind "$kind"
    job="$(val "$prefix-select" 'd["fab"]["jobId"]')"
    ok "$prefix-await" wait-fab-state --state AwaitingProvenance Failed --expected-job-id "$job"
    expect "$prefix-await" 'd["fab"]["state"] == "AwaitingProvenance"' "the package did not reach AwaitingProvenance"
    ok "$prefix-provenance" set-fab-provenance --expected-job-id "$job" "${provenance_args[@]}"
    digest="$(val "$prefix-provenance" 'd["fab"]["provenanceDigest"]')"
    ok "$prefix-confirm" confirm-fab-provenance --expected-job-id "$job" --expected-provenance-digest "$digest"
    ok "$prefix-ready" wait-fab-state --state ReadyToCommit Failed --expected-job-id "$job"
    expect "$prefix-ready" 'd["fab"]["state"] == "ReadyToCommit"' "the import did not reach ReadyToCommit"
    generation="$(val "$prefix-ready" 'd["fab"]["generationId"]')"
    relation="$(val "$prefix-ready" 'd["fab"]["relation"]')"
}
expect_failed() { # id code
    ok "$1" wait-fab-state --state Failed --expected-job-id "$job"
    expect "$1" "d['fab']['errorCode'] == '$2'" "expected failure code $2, got $(val "$1" 'd["fab"]["errorCode"]')"
}

cd -- "$repo_root"

# =====================================================================
# Phase A: import into a fresh --project, place, save.
# =====================================================================
launch import --editor-control-fab-helper-smoke 120
proto_id="$(target_value PrototypeEntityId)"
proto_name="Prototype Mesh"
proto_mesh="$(target_value PrototypeMeshAsset)"
proto_material="$(target_value PrototypeMaterialAsset)"
initial_selected="$(target_value InitialSelectedEntityId)"
[[ "$(target_value Phase)" == "import" ]] || fail "phase A target phase"

ok fa-01-inspect inspect-fab
expect fa-01-inspect 'd["fab"]["state"] == "Idle" and d["fab"]["projectReceiptCount"] == 0 and d["fab"]["projectValidation"] == "none"' "baseline"
ok fa-02-save save-project-state
before_import="$(fingerprint all)"

inbox_put good.glb
ok fa-03-select select-fab-package --inbox-name good.glb --kind glb
job="$(val fa-03-select 'd["fab"]["jobId"]')"
ok fa-04-await wait-fab-state --state AwaitingProvenance Failed --expected-job-id "$job"
expect fa-04-await 'd["fab"]["state"] == "AwaitingProvenance" and d["fab"]["summary"]["triangles"] == 2' "summary"
ok fa-05-provenance set-fab-provenance --expected-job-id "$job" "${provenance_args[@]}"
digest="$(val fa-05-provenance 'd["fab"]["provenanceDigest"]')"
rejected fa-06-confirm-wrong fab_provenance_digest_mismatch confirm-fab-provenance \
    --expected-job-id "$job" --expected-provenance-digest "$(printf '0%.0s' $(seq 64))"
ok fa-07-confirm confirm-fab-provenance --expected-job-id "$job" --expected-provenance-digest "$digest"
ok fa-08-ready wait-fab-state --state ReadyToCommit Failed --expected-job-id "$job"
generation="$(val fa-08-ready 'd["fab"]["generationId"]')"
expect fa-08-ready 'd["fab"]["relation"] == "AddNewStream"' "relation"
rejected fa-09-commit-wrong-generation fab_generation_mismatch commit-fab-import \
    --expected-job-id "$job" --expected-generation-id "$(printf 'a%.0s' $(seq 64))" --expected-relation AddNewStream
[[ "$(fingerprint all)" == "$before_import" ]] || fail "a refused commit changed the project"
ok fa-10-commit commit-fab-import --expected-job-id "$job" --expected-generation-id "$generation" --expected-relation AddNewStream
fab_mesh="$(val fa-10-commit 'd["fab"]["meshAsset"]')"
fab_material="$(val fa-10-commit 'd["fab"]["materialAsset"]')"
expect fa-10-commit 'd["fab"]["manifestRevision"] == 1 and d["fab"]["projectChanged"] and d["undoDepthAfter"] == 0' "commit"
# The commit is an undo barrier: no entries, the base row says so, and undo explains itself.
ok fa-10b-history inspect-history
expect fa-10b-history 'd["history"]["baseIsBarrier"] and d["history"]["entryCount"] == 0 and not d["history"]["undoEnabled"] and d["history"]["undoLabel"] == "Undo (blocked: Import Fab Asset)" and d["history"]["undoReason"] == "Undo unavailable: Fab import changed the project" and d["history"]["rows"][0]["display"] == "Import Fab Asset (barrier)" and d["history"]["rows"][0]["barrier"] and d["history"]["rows"][0]["base"] and d["history"]["rows"][0]["source"] == "system" and d["history"]["announcement"] == "Undo history cleared: Fab import changed the project"' "the Fab commit must install a named undo barrier"
rejected fa-10c-undo-barrier undo_barrier undo-history --expected-history-revision "$(val fa-10b-history 'd["history"]["revisionBefore"]')"
ok fa-11-inspect inspect-fab
expect fa-11-inspect "d['fab']['state'] == 'Done' and d['fab']['projectReceiptCount'] == 1 and d['fab']['projectMeshAssets'] == [$fab_mesh]" "handles after commit"
ok fa-12-dismiss dismiss-fab-import --expected-job-id "$job"
ok fa-13-place place-mesh-asset --mesh-asset "$fab_mesh" --expected-selected-entity-id "$initial_selected"
placed_id="$(val fa-13-place 'd["entityId"]')"
placed_name="$(val fa-13-place 'd["entityName"]')"
expect fa-13-place "d['afterMeshRenderer'][:2] == [$fab_mesh, $fab_material]" "placed mesh and receipt-associated material"
ok fa-13b-history inspect-history
expect fa-13b-history 'd["history"]["baseIsBarrier"] and d["history"]["entryCount"] == 1 and d["history"]["rows"][0]["display"].startswith("Place ") and d["history"]["rows"][0]["source"] == "agent" and d["history"]["undoEnabled"] and d["history"]["rows"][1]["barrier"]' "the placement is one named Place entry above the barrier"
rejected fa-14-place-forced-rollback injected_postcondition_failure_rolled_back place-mesh-asset \
    --mesh-asset "$fab_mesh" --expected-selected-entity-id "$placed_id"
expect fa-14-place-forced-rollback 'd["rollbackVerified"] and d["effect"] == "RolledBack" and d["undoDepthAfter"] == d["undoDepthBefore"]' "rollback"
ok fa-15-save save-project-state
manifest_after_a="$(val fa-15-save 'd["fab"]["manifestSha256"]')"
ok fa-99-final inspect-fab
finish "EditorFabControlV5 phase=import"
a_fingerprint="$(fingerprint all)"
generations_a="$(fingerprint generations)"
grep -Fq 'ProjectRevision 1' "$project" || fail "manifest revision after phase A"
[[ -z "$(find "$project_dir" "$repo_root" -maxdepth 2 -name 'fab:*' 2>/dev/null)" ]] \
    || fail "an immutable material was written to its logical path"
staging_clean

# =====================================================================
# Phase B: relaunch the same project and prove everything survived.
# =====================================================================
launch reopen --editor-control-fab-reopen-smoke 240
[[ "$(target_value Phase)" == "reopen" ]] || fail "phase B target phase"
[[ "$(target_value PrototypeMeshAsset)" == "$proto_mesh" ]] || fail "prototype changed across reopen"

ok fb-01-inspect inspect-fab
expect fb-01-inspect "d['fab']['state'] == 'Idle' and d['fab']['projectReceiptCount'] == 1 and d['fab']['projectMeshAssets'] == [$fab_mesh] and d['fab']['projectMaterialAssets'] == [$fab_material] and d['fab']['projectStructural'] == 'passed' and d['fab']['manifestRevision'] == 1 and d['fab']['manifestSha256'] == '$manifest_after_a'" "reopened project state"
ok fb-02-entity-placed inspect-entity --entity-id "$placed_id" --expected-name "$placed_name"
expect fb-02-entity-placed "d['afterMeshRendererPresent'] and d['afterMeshRenderer'][:2] == [$fab_mesh, $fab_material]" "placed entity survived"
ok fb-03-entity-prototype inspect-entity --entity-id "$proto_id" --expected-name "$proto_name"
expect fb-03-entity-prototype "d['afterMeshRenderer'][:2] == [$proto_mesh, $proto_material]" "prototype untouched by the import"
ok fb-04-material-inspect inspect --entity-id "$placed_id" --expected-name "$placed_name" --material-handle "$fab_material"
expect fb-04-material-inspect 'd["rendererReadbackVerified"]' "published Fab material"
rejected fb-05-material-patch-immutable immutable_material set --entity-id "$placed_id" \
    --expected-name "$placed_name" --material-handle "$fab_material" \
    --expected-base 1 1 1 --expected-metallic 0.5 --expected-roughness 0.5 \
    --new-base 0.2 0.2 0.2 --new-metallic 0.1 --new-roughness 0.9
rejected fb-06-save-stale compare_and_swap_state_mismatch save-project-state \
    --expected-manifest-sha256 "$(printf 'b%.0s' $(seq 64))"
ok fb-07-save save-project-state --expected-manifest-sha256 "$manifest_after_a"
[[ "$(fingerprint generations)" == "$generations_a" ]] || fail "a project save rewrote an immutable generation"
[[ -z "$(find "$project_dir" "$repo_root" -maxdepth 2 -name 'fab:*' 2>/dev/null)" ]] \
    || fail "a save wrote an immutable material to its logical path"

# Identical reimport: ExactReuse, no new revision, no file changes.
reuse_before="$(fingerprint all)"
to_ready fb-10 good.glb glb
[[ "$relation" == "ExactReuse" && "$generation" == "$(val fa-10-commit 'd["fab"]["generationId"]')" ]] \
    || fail "identical reimport was classified $relation"
ok fb-13-commit-reuse commit-fab-import --expected-job-id "$job" --expected-generation-id "$generation" --expected-relation ExactReuse
expect fb-13-commit-reuse 'd["effect"] == "FabImportReused" and not d["fab"]["projectChanged"] and d["fab"]["manifestRevision"] == 1 and d["persistence"] == "SessionOnly"' "reuse"
ok fb-14-dismiss dismiss-fab-import --expected-job-id "$job"
[[ "$(fingerprint all)" == "$reuse_before" ]] || fail "an identical reimport changed project files"
staging_clean

# Hostile and corrupt packages are rejected without touching the project.
reject_fingerprint="$(fingerprint all)"
inbox_put corrupt.zip
ok fb-20-select-corrupt select-fab-package --inbox-name corrupt.zip --kind zip
job="$(val fb-20-select-corrupt 'd["fab"]["jobId"]')"
expect_failed fb-21-corrupt-failed ArchiveRejected
ok fb-22-dismiss dismiss-fab-import --expected-job-id "$job"

inbox_put traversal.zip
if run fb-30-select-traversal select-fab-package --inbox-name traversal.zip --kind zip; then
    job="$(val fb-30-select-traversal 'd["fab"]["jobId"]')"
    expect_failed fb-31-traversal-failed ArchiveRejected
    ok fb-32-dismiss dismiss-fab-import --expected-job-id "$job"
else
    [[ "$(val fb-30-select-traversal 'd["reason"]')" == intake_rejected_* ]] || fail "traversal zip rejection reason"
fi
[[ -z "$(find "$smoke_root" -name 'evil.txt' 2>/dev/null)" ]] || fail "a traversal member escaped extraction"

inbox_put badpng.glb
ok fb-33-select-badpng select-fab-package --inbox-name badpng.glb --kind glb
job="$(val fb-33-select-badpng 'd["fab"]["jobId"]')"
expect_failed fb-34-badpng-failed PackageRejected
ok fb-35-dismiss dismiss-fab-import --expected-job-id "$job"

inbox_put truncated.zip
rejected fb-36-select-truncated intake_rejected_incomplete_zip select-fab-package --inbox-name truncated.zip --kind zip
inbox_put notes.txt
rejected fb-37-select-notes intake_rejected_unrecognized_content select-fab-package --inbox-name notes.txt --kind glb
ln -s /etc/passwd "$control_dir/fab-inbox/link.glb"
rejected fb-38-select-symlink intake_rejected_not_regular_object select-fab-package --inbox-name link.glb --kind glb
inbox_put good.glb
rejected fb-39-select-wrong-kind source_kind_mismatch select-fab-package --inbox-name good.glb --kind zip
[[ "$(fingerprint all)" == "$reject_fingerprint" ]] || fail "a rejected package changed project files"
staging_clean

# The schema carries no filesystem path: a crafted leaf or extra path field is refused by the Editor.
client_refused fb-40-leaf-traversal "inbox name must be one leaf" select-fab-package --inbox-name "../good.glb" --kind glb
python3 - "$script_dir" "$control_dir" "$project" "$out" <<'PY'
import json
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
import EditorMaterialControl as helper

control_dir, project, out = Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
session = helper._parse_session(control_dir)


def raw(identifier, action, extra, expected_reason):
    lines = [helper.REQUEST_HEADER, f'RequestId "{identifier}"', f'SessionId "{session["session_id"]}"',
             f'ProjectPath "{session["project_path"]}"', f"Action {action}", *extra]
    # A request the parser refuses never reaches an action, so its receipt says Unknown.
    parser_level = expected_reason in ("unknown_field", "invalid_duplicate_or_oversized_attribution_text")
    receipt = helper._submit_request(control_dir, session, identifier,
                                     "Unknown" if parser_level else action, lines, 10.0)
    assert receipt["status"] == "Rejected" and receipt["reason"] == expected_reason, (identifier, receipt["reason"])
    (out / f"{identifier}.json").write_text(json.dumps(receipt))


raw("fb-41-leaf-slash", "SelectFabPackage", ['InboxName "sub/good.glb"', "ExpectedSourceKind glb"], "invalid_inbox_name")
raw("fb-42-leaf-dotdot", "SelectFabPackage", ['InboxName "..good.glb"', "ExpectedSourceKind glb"], "invalid_inbox_name")
raw("fb-43-path-field", "SelectFabPackage", ['InboxName "good.glb"', "ExpectedSourceKind glb", 'SourcePath "/etc/passwd"'], "unknown_field")
raw("fb-44-url-field", "SelectFabPackage", ['InboxName "good.glb"', "ExpectedSourceKind glb", 'DownloadUrl "https://example.invalid/x.zip"'], "unknown_field")
raw("fb-45-attribution-cap", "SetFabProvenance",
    ["ExpectedFabJobId 1", 'ProductIdentity "https://www.fab.com/listings/x"', 'ProductName "n"', 'Publisher "p"',
     'VersionOrDownloadLabel "v"', "LicenseFamily CC-BY", "LicenseTier NotApplicable",
     'AttributionText "' + "a" * 2049 + '"', "NoAI Unknown", "GeneratedWithAI Unknown",
     "RawSourcePolicy ExcludedFromProject"], "invalid_duplicate_or_oversized_attribution_text")
PY
client_refused fb-46-attribution-client "attribution text exceeds the 2048-byte mailbox cap" set-fab-provenance \
    --expected-job-id 1 "${provenance_args[@]}" --attribution-text "$(python3 -c 'print("a" * 2049)')"

# A package folder is a valid intake kind too (copied into the inbox as a directory).
cp -r -- "$fixtures/package" "$control_dir/fab-inbox/package"
ok fb-47-select-directory select-fab-package --inbox-name package --kind directory
job="$(val fb-47-select-directory 'd["fab"]["jobId"]')"
ok fb-48-directory-await wait-fab-state --state AwaitingProvenance Failed --expected-job-id "$job"
expect fb-48-directory-await 'd["fab"]["state"] == "AwaitingProvenance" and d["fab"]["sourceKind"] == "directory" and d["fab"]["summary"]["triangles"] == 2 and d["fab"]["sourceSha256"] == d["fab"]["expandedTreeSha256"]' "directory package summary"
ok fb-49-directory-cancel cancel-fab-import --expected-job-id "$job"
ok fb-49-directory-cancelled wait-fab-state --state Cancelled --expected-job-id "$job"
ok fb-49-directory-dismiss dismiss-fab-import --expected-job-id "$job"

# Cancellation: an expected-source mismatch, a cancel while awaiting provenance, and a cancel after confirming.
inbox_put good.glb
ok fb-50-select-mismatch select-fab-package --inbox-name good.glb --kind glb --source-sha256 "$(printf 'a%.0s' $(seq 64))"
job="$(val fb-50-select-mismatch 'd["fab"]["jobId"]')"
ok fb-51-mismatch-cancelled wait-fab-state --state Cancelled Failed --expected-job-id "$job"
expect fb-51-mismatch-cancelled 'd["fab"]["state"] == "Cancelled" and "source_sha256_mismatch" in d["fab"]["note"]' "source SHA-256 expectation"
ok fb-52-dismiss dismiss-fab-import --expected-job-id "$job"

ok fb-53-select-cancel select-fab-package --inbox-name good.glb --kind glb
job="$(val fb-53-select-cancel 'd["fab"]["jobId"]')"
ok fb-54-await wait-fab-state --state AwaitingProvenance Failed --expected-job-id "$job"
ok fb-55-cancel cancel-fab-import --expected-job-id "$job"
ok fb-56-cancelled wait-fab-state --state Cancelled --expected-job-id "$job"
ok fb-57-dismiss dismiss-fab-import --expected-job-id "$job"
staging_clean

ok fb-58-select-cancel-cook select-fab-package --inbox-name good.glb --kind glb
job="$(val fb-58-select-cancel-cook 'd["fab"]["jobId"]')"
ok fb-59-await wait-fab-state --state AwaitingProvenance Failed --expected-job-id "$job"
ok fb-60-provenance set-fab-provenance --expected-job-id "$job" "${provenance_args[@]}"
ok fb-61-confirm confirm-fab-provenance --expected-job-id "$job" --expected-provenance-digest "$(val fb-60-provenance 'd["fab"]["provenanceDigest"]')"
ok fb-62-cancel cancel-fab-import --expected-job-id "$job"
ok fb-63-cancelled wait-fab-state --state Cancelled --expected-job-id "$job"
ok fb-64-dismiss dismiss-fab-import --expected-job-id "$job"
staging_clean
[[ "$(fingerprint all)" == "$reject_fingerprint" ]] || fail "a cancelled import changed project files"

# A changed source (the ZIP of the same listing/version) replaces the stream and
# is assigned to the prototype entity; a stale assignment is refused first.
to_ready fb-70 good.zip zip
[[ "$relation" == "ReplaceSameStreamSource" ]] || fail "zip reimport was classified $relation"
rejected fb-74-commit-stale-assignment compare_and_swap_state_mismatch commit-fab-import \
    --expected-job-id "$job" --expected-generation-id "$generation" --expected-relation "$relation" \
    --assign-entity-id "$proto_id" --assign-expected-name "$proto_name" \
    --assign-expected-mesh-asset "$fab_mesh" --assign-expected-material-asset "$fab_material"
[[ "$(val fb-74-commit-stale-assignment 'd["fab"]["state"]')" == "ReadyToCommit" ]] || fail "stale assignment changed the job state"
ok fb-75-commit-replace commit-fab-import --expected-job-id "$job" --expected-generation-id "$generation" \
    --expected-relation "$relation" --assign-entity-id "$proto_id" --assign-expected-name "$proto_name" \
    --assign-expected-mesh-asset "$proto_mesh" --assign-expected-material-asset "$proto_material"
expect fb-75-commit-replace 'd["fab"]["manifestRevision"] == 2 and d["fab"]["assignmentApplied"] and d["fab"]["projectChanged"] and d["undoDepthAfter"] == 0' "replacement commit"
ok fb-75b-history inspect-history
expect fb-75b-history 'd["history"]["baseIsBarrier"] and d["history"]["entryCount"] == 0 and d["history"]["rows"][0]["display"] == "Import Fab Asset (barrier)" and d["history"]["undoReason"] == "Undo unavailable: Fab import changed the project"' "a replacement commit is a barrier too, including the in-memory adoption fallback"
ok fb-76-dismiss dismiss-fab-import --expected-job-id "$job"
ok fb-77-entity-prototype inspect-entity --entity-id "$proto_id" --expected-name "$proto_name"
expect fb-77-entity-prototype "d['afterMeshRenderer'][:2] == [$fab_mesh, $fab_material]" "prototype assigned to the imported mesh"
grep -Fq 'ProjectRevision 2' "$project" || fail "manifest revision after replacement"
staging_clean

# Typed re-assignment (one history entry) restores the prototype's own assets.
ok fb-80-assign-back set-entity-mesh-renderer-assets --entity-id "$proto_id" --expected-name "$proto_name" \
    --expected-mesh-asset "$fab_mesh" --expected-material-asset "$fab_material" \
    --new-mesh-asset "$proto_mesh" --new-material-asset "$proto_material"
rejected fb-81-assign-stale compare_and_swap_state_mismatch set-entity-mesh-renderer-assets --entity-id "$proto_id" \
    --expected-name "$proto_name" --expected-mesh-asset "$fab_mesh" --expected-material-asset "$fab_material" \
    --new-mesh-asset "$proto_mesh" --new-material-asset "$proto_material"

# Full-hash project validation on a worker.
ok fb-85-validate validate-project
ok fb-86-validated wait-fab-state --validation passed failed
expect fb-86-validated 'd["fab"]["projectValidation"] == "passed"' "project validation: $(val fb-86-validated 'd["fab"]["projectValidationMessage"]')"

# The browser panel actions need no mouse; a headless Editor has no panel to show.
rejected fb-90-panel-show headless_has_no_browser_panel set-fab-panel-visible --visible yes
ok fb-91-panel-hide set-fab-panel-visible --visible no
ok fb-92-panel-inspect inspect-fab-panel
expect fb-92-panel-inspect 'd["fab"]["panel"]["state"] == "NotStarted" and not d["fab"]["panel"]["visible"]' "panel diagnostics"
ok fb-99-final inspect-fab
finish "EditorFabControlV5 phase=reopen"
staging_clean
grep -Fq 'ProjectRevision 2' "$project" || fail "final manifest revision"

# The Fab Import panel's ImGui code runs headlessly through every workflow state
# (a private ImGui context asserts on unbalanced stacks and duplicate IDs).
ui_log="$smoke_root/ui-editor.log"
log_path="$ui_log"
timeout 120s "$editor" --headless "--project=$smoke_root/uiproject/ui.spiralproject" \
    --fab-panel-ui-smoke "--fab-ui-smoke-fixtures=$fixtures" >"$ui_log" 2>&1 \
    || fail "the Fab panel UI smoke failed"
grep -Fq -- "FabPanelUiSmokeV1 states=" "$ui_log" || fail "the Fab panel UI smoke did not report"
grep -Fq -- "result=pass" "$ui_log" || fail "the Fab panel UI smoke did not pass"
grep -Fq -- "ProjectRevision 1" "$smoke_root/uiproject/ui.spiralproject" || fail "the UI smoke commit did not persist"
staging_clean

# The default layout (manifest paths relative to the working directory, no
# --project) takes a commit too and reopens with its receipt. The Editor runs
# with an empty temporary directory as its working directory.
legacy_root="$smoke_root/legacy"
mkdir -p "$legacy_root"
legacy_project="$legacy_root/output/projects/default.spiralproject"
launch_plain() {
    control_dir="$smoke_root/$1-mailbox"
    log_path="$smoke_root/$1-editor.log"
    (cd "$legacy_root" && exec timeout 120s "$editor" --headless "--editor-control-dir=$control_dir") >"$log_path" 2>&1 &
    live_process=$!
    for _ in $(seq 1 600); do
        [[ -f "$control_dir/session.info" ]] && return 0
        kill -0 "$live_process" 2>/dev/null || fail "Editor exited before opening its mailbox ($1)"
        sleep 0.02
    done
    fail "timed out waiting for the mailbox ($1)"
}
stop_plain() {
    kill "$live_process" 2>/dev/null || true
    wait "$live_process" 2>/dev/null || true
    live_process=""
}
saved_project="$project"
project="$legacy_project"
launch_plain legacy-import
ok lc-01-save save-project-state
inbox_put good.glb
ok lc-02-select select-fab-package --inbox-name good.glb --kind glb
job="$(val lc-02-select 'd["fab"]["jobId"]')"
ok lc-03-await wait-fab-state --state AwaitingProvenance Failed --expected-job-id "$job"
ok lc-04-provenance set-fab-provenance --expected-job-id "$job" "${provenance_args[@]}"
ok lc-05-confirm confirm-fab-provenance --expected-job-id "$job" \
    --expected-provenance-digest "$(val lc-04-provenance 'd["fab"]["provenanceDigest"]')"
ok lc-06-ready wait-fab-state --state ReadyToCommit Failed --expected-job-id "$job"
ok lc-07-commit commit-fab-import --expected-job-id "$job" \
    --expected-generation-id "$(val lc-06-ready 'd["fab"]["generationId"]')" --expected-relation AddNewStream
expect lc-07-commit 'd["fab"]["manifestRevision"] == 1 and d["fab"]["projectChanged"]' "legacy-layout commit"
legacy_mesh="$(val lc-07-commit 'd["fab"]["meshAsset"]')"
ok lc-08-save save-project-state
stop_plain
grep -Fq 'Scene "output/scenes/' "$legacy_project" || fail "the legacy layout lost its relative paths"
grep -Fq 'ProjectRevision 1' "$legacy_project" || fail "legacy manifest revision"
launch_plain legacy-reopen
ok lc-10-inspect inspect-fab
expect lc-10-inspect "d['fab']['projectReceiptCount'] == 1 and d['fab']['projectMeshAssets'] == [$legacy_mesh] and d['fab']['projectStructural'] == 'passed' and d['fab']['manifestRevision'] == 1" "legacy-layout reopen"
stop_plain
project="$saved_project"
staging_clean

# Nothing outside the temporary project changed.
default_after="$(default_fingerprint)"
if [[ "$default_after" != "$default_before" ]]; then
    diff -u <(printf '%s\n' "$default_before") <(printf '%s\n' "$default_after") >&2 || true
    fail "the default project changed"
fi

echo "EditorFabImportTestV5 schema=5 project=temporary phases=import-then-reopen import=glb-provenance-confirm-commit place=one-history-entry rollback=verified save-reopen=entity-mesh-material-receipt exact-reuse=no-new-revision intake=glb-zip-directory rejected=corrupt-zip-traversal-bad-png-truncated-notes-symlink-wrong-kind schema-paths=leaf-only cancellation=awaiting-and-cooking panelUi=all-states-headless legacyLayout=commit-and-reopen staging=clean assignment=cas-stale-rejected-then-applied immutable=patch-rejected-no-logical-file validation=full-hash-worker panel=headless-guarded defaultProject=unchanged input=no-ui-synthesis result=pass"
