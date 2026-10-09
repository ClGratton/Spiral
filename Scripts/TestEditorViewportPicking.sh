#!/usr/bin/env bash
# Drives the schema-5 PickAtViewportPoint and FocusSelection mailbox actions of a
# live Editor through the typed Python helper and checks the receipts numerically.
#
#   Scripts/TestEditorViewportPicking.sh [editor-binary] [--vulkan]
#
# First an in-process headless click smoke (--editor-viewport-click-smoke) covers the event-driven
# gestures: click versus drag, Esc, Home, and the animation. Then headless always runs: the pick uses the deterministic virtual viewport rectangle
# (origin 0,0, 1280x720, 16:9) because no ImGui frame exists. With --vulkan the same
# sequence also runs against a short-lived native window whose real viewport image
# rectangle is used (the points are normalized, so the expectations do not change);
# that window opens on whatever monitor exists and is closed by the Editor itself.
#
# The numeric oracle is written independently of the Editor: it rebuilds the object
# and view matrices from their definitions in Engine/Math/Math.cpp (row vectors,
# left-handed, View = T(-camera) * Ry(-yaw) Rx(-pitch) Rz(-roll)), projects the mesh
# bounds through the receipt's camera pose, field of view, and aspect, and checks
# the 15 percent framing margin, view-direction preservation, and pick hits/misses
# on points derived from that projection. Persistent project files must stay
# byte-identical.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
editor="$repo_root/bin/Debug-linux-x86_64-gmake/Editor/Editor"
run_vulkan=""
for argument in "$@"; do
    case "$argument" in
        --vulkan) run_vulkan="--vulkan" ;;
        *) editor="$argument" ;;
    esac
done
project="$repo_root/output/projects/default.spiralproject"

if [[ ! -x "$editor" ]]; then
    echo "Editor executable is missing or not executable: $editor" >&2
    exit 1
fi
python3 "$script_dir/EditorMaterialControl.py" --help >/dev/null

smoke_root="$(mktemp -d "${TMPDIR:-/tmp}/spiral-viewport-picking.XXXXXX")"
live_process=""
cleanup() {
    if [[ -n "$live_process" ]] && kill -0 "$live_process" 2>/dev/null; then
        kill "$live_process" 2>/dev/null || true
        wait "$live_process" 2>/dev/null || true
    fi
    if [[ "${SPIRAL_KEEP_SMOKE_ARTIFACTS:-0}" == "1" ]]; then
        echo "Preserved viewport-picking smoke artifacts: $smoke_root" >&2
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

# The independent numeric oracle (see the header comment). It never imports Editor code.
cat >"$smoke_root/oracle.py" <<'PY'
import json
import math
import sys
from pathlib import Path


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0], [0, c, s], [0, -s, c]]


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, -s], [0, 1, 0], [s, 0, c]]


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, s, 0], [-s, c, 0], [0, 0, 1]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def row_times(v, m):
    return [sum(v[i] * m[i][j] for i in range(3)) for j in range(3)]


def ypr(yaw, pitch, roll):
    return mul(mul(rot_y(yaw), rot_x(pitch)), rot_z(roll))


def load_target(path):
    target = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        key, _, rest = line.partition(" ")
        target[key] = rest
    return target


def numbers(text):
    return [float(token) for token in text.split()]


def world_corners(target):
    """Eight world-space corners of the prototype mesh bounds (sector 0 assumed and asserted)."""
    transform = numbers(target["PrototypeTransform"])
    assert transform[:3] == [0.0, 0.0, 0.0], "prototype is expected in sector 0"
    position = transform[3:6]
    pitch, yaw, roll = (math.radians(v) for v in transform[6:9])
    scale = transform[9:12]
    low_high = numbers(target["PrototypeLocalBounds"])
    rotation = ypr(yaw, pitch, roll)
    corners = []
    for index in range(8):
        local = [low_high[3 + axis] if index & (1 << axis) else low_high[axis] for axis in range(3)]
        scaled = [local[axis] * scale[axis] for axis in range(3)]
        corners.append([a + b for a, b in zip(row_times(scaled, rotation), position)])
    return corners


def ray_hits_corners(origin, direction, corners):
    """Nearest hit of a ray on the 12 triangles of the box with the given corners (index bits x=1,y=2,z=4)."""
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    sub = lambda a, b: [a[i] - b[i] for i in range(3)]
    cross = lambda a, b: [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
    dot = lambda a, b: sum(a[i] * b[i] for i in range(3))
    best = None
    for face in faces:
        for tri in ((face[0], face[1], face[2]), (face[0], face[2], face[3])):
            a, b, c = (corners[i] for i in tri)
            normal = cross(sub(b, a), sub(c, a))
            denominator = dot(normal, direction)
            if abs(denominator) < 1e-12:
                continue
            t = dot(normal, sub(a, origin)) / denominator
            if t < 0.0:
                continue
            p = [origin[i] + direction[i] * t for i in range(3)]
            if (dot(cross(sub(b, a), sub(p, a)), normal) >= 0.0 and dot(cross(sub(c, b), sub(p, b)), normal) >= 0.0
                    and dot(cross(sub(a, c), sub(p, c)), normal) >= 0.0):
                best = t if best is None else min(best, t)
    return best


def forward_vector(pose):
    """World forward of the camera: the third column of View = Ry(-yaw) Rx(-pitch) Rz(-roll)."""
    pitch, yaw, roll = (math.radians(v) for v in pose[3:6])
    view = ypr(-yaw, -pitch, -roll)
    return [view[0][2], view[1][2], view[2][2]]


def view_point(pose, point):
    """Row-vector view transform of the renderer: (p - camera) * Ry(-yaw) Rx(-pitch) Rz(-roll)."""
    pitch, yaw, roll = (math.radians(v) for v in pose[3:6])
    view = ypr(-yaw, -pitch, -roll)
    return row_times([point[axis] - pose[axis] for axis in range(3)], view)


def project(pose, fov_degrees, aspect, point):
    x, y, z = view_point(pose, point)
    if z <= 0.0:
        return None
    tangent = math.tan(math.radians(fov_degrees) / 2.0)
    return x / (z * tangent * aspect), y / (z * tangent), z


def receipt(root, label, number):
    return json.loads((Path(root) / f"{label}-{number}.json").read_text(encoding="utf-8"))
PY

run_sequence() {
    local label="$1"
    local expected_backend="$2"
    shift 2
    local control_dir="$smoke_root/$label-mailbox"
    local log_path="$smoke_root/$label-editor.log"
    local target="$control_dir/viewport-picking-target.info"

    timeout 90s "$editor" "$@" \
        "--editor-control-dir=$control_dir" \
        --editor-control-viewport-picking-helper-smoke >"$log_path" 2>&1 &
    live_process=$!
    for _ in $(seq 1 500); do
        if [[ -f "$target" ]]; then
            break
        fi
        if ! kill -0 "$live_process" 2>/dev/null; then
            cat "$log_path" >&2
            echo "Editor exited before publishing the viewport-picking target ($label)" >&2
            exit 1
        fi
        sleep 0.02
    done
    if [[ ! -f "$target" ]]; then
        cat "$log_path" >&2
        echo "Timed out waiting for the viewport-picking target ($label)" >&2
        exit 1
    fi

    grep -Fxq -- "SpiralEditorViewportPickingTarget 1" <(head -n 1 "$target")
    grep -Fq -- "PrototypeEntityName \"Prototype Mesh\"" "$target"
    grep -Fq -- "LightEntityName \"Directional Light\"" "$target"
    grep -Fq -- "ExpectedTypedRequests 16" "$target"
    grep -Fq -- "ExpectedServerRejectedFixtures 2" "$target"

    local camera_id prototype_id light_id
    camera_id="$(awk '$1 == "MainCameraEntityId" { print $2 }' "$target")"
    prototype_id="$(awk '$1 == "PrototypeEntityId" { print $2 }' "$target")"
    light_id="$(awk '$1 == "LightEntityId" { print $2 }' "$target")"
    [[ "$(awk '$1 == "InitialSelectedEntityId" { print $2 }' "$target")" == "$camera_id" ]]

    invoke() {
        local request_id="$1"
        shift
        python3 "$script_dir/EditorMaterialControl.py" \
            --control-dir "$control_dir" --expected-project "$project" \
            --request-id "$request_id" "$@"
    }
    # A request the Editor must reject returns status 2 from the helper.
    invoke_rejected() {
        local request_id="$1"
        shift
        local status=0
        invoke "$request_id" "$@" >"$smoke_root/$label-$request_id.json" || status=$?
        if [[ "$status" -ne 2 ]]; then
            echo "Expected a typed rejection for $request_id ($label), helper exit status $status" >&2
            exit 1
        fi
    }
    pick() { # request-id expected-selected x y -> JSON in $label-<request-id>.json
        local request_id="$1" expected="$2" x="$3" y="$4"
        invoke "$request_id" pick-viewport --expected-selected-entity-id "$expected" \
            --x "$x" --y "$y" >"$smoke_root/$label-$request_id.json"
    }
    focus() { # request-id expected-selected [animate: yes|no, default no]
        invoke "$1" focus-selection --expected-selected-entity-id "$2" --animate "${3:-no}" \
            >"$smoke_root/$label-$1.json"
    }

    # 1. A pick on empty sky (top-left corner) clears the startup selection.
    pick vp-01-pick-sky "$camera_id" 0.03 0.05
    # 2. A pick at the viewport center hits the default cube from the default camera.
    pick vp-02-pick-cube 0 0.5 0.5
    # 3. A stale expected selection is rejected and changes nothing.
    invoke_rejected vp-03-stale-pick pick-viewport \
        --expected-selected-entity-id "$camera_id" --x 0.5 --y 0.5
    # 4. Picking the already selected entity succeeds without a selection change.
    pick vp-04-pick-cube-again "$prototype_id" 0.5 0.5
    # 5-6. F on the cube: jump (animation disabled) so the receipt pose is final; the second request
    # uses the animated path and, starting at the target already, must report the same pose.
    focus vp-05-focus-cube "$prototype_id"
    focus vp-06-focus-again "$prototype_id" yes
    # 7. The pick uses the camera after framing (the cube still fills the center).
    pick vp-07-pick-after-focus "$prototype_id" 0.5 0.5

    # 8-10. Points derived from the post-framing projection of the mesh bounds: 30 percent of the
    # way from the projected center to two corners lie inside the (convex) silhouette and must hit;
    # 115 percent of the way to the farthest corner lies outside it and must miss (clearing the selection).
    python3 - "$smoke_root" "$label" "$target" "$smoke_root/$label-points.txt" <<'PY'
import math
import sys
sys.path.insert(0, sys.argv[1])
import oracle

root, label, target_path, out = sys.argv[1:5]
target = oracle.load_target(target_path)
focus = oracle.receipt(root, label, "vp-05-focus-cube")["viewport"]
pose = focus["focus"]["after"]
fov = focus["rect"]["fovDegrees"]
aspect = focus["rect"]["aspect"]
corners = oracle.world_corners(target)
projected = [oracle.project(pose, fov, aspect, corner) for corner in corners]
assert all(item is not None for item in projected)
center_world = [sum(corner[axis] for corner in corners) / 8.0 for axis in range(3)]
center = oracle.project(pose, fov, aspect, center_world)
to_normalized = lambda ndc: (ndc[0] * 0.5 + 0.5, 0.5 - ndc[1] * 0.5)
cx, cy = to_normalized(center)
assert abs(cx - 0.5) < 1e-3 and abs(cy - 0.5) < 1e-3, "framing centers the bounds"
offsets = sorted(((to_normalized(item)[0] - cx, to_normalized(item)[1] - cy) for item in projected),
                 key=lambda value: math.hypot(*value))
near, far = offsets[0], offsets[-1]
points = [(cx + 0.3 * near[0], cy + 0.3 * near[1]), (cx + 0.3 * far[0], cy + 0.3 * far[1]),
          (cx + 1.15 * far[0], cy + 1.15 * far[1])]
assert all(0.0 <= value <= 1.0 for point in points for value in point), points
with open(out, "w", encoding="utf-8") as stream:
    for x, y in points:
        stream.write(f"{x!r} {y!r}\n")
PY
    local -a inside_a inside_b outside point_lines
    mapfile -t point_lines <"$smoke_root/$label-points.txt"
    read -r -a inside_a <<<"${point_lines[0]}"
    read -r -a inside_b <<<"${point_lines[1]}"
    read -r -a outside <<<"${point_lines[2]}"
    pick vp-08-pick-inside-a "$prototype_id" "${inside_a[0]}" "${inside_a[1]}"
    pick vp-09-pick-inside-b "$prototype_id" "${inside_b[0]}" "${inside_b[1]}"
    pick vp-10-pick-outside "$prototype_id" "${outside[0]}" "${outside[1]}"

    # 11-12. The same framing on an entity without a mesh (the directional light) uses the
    # default radius.
    invoke vp-11-select-light select-entity --entity-id "$light_id" \
        --expected-name "Directional Light" --expected-selected-entity-id 0 \
        >"$smoke_root/$label-vp-11-select-light.json"
    focus vp-12-focus-light "$light_id"
    # 13. Empty sky clears the selection again.
    pick vp-13-pick-clear "$light_id" 0.97 0.04
    # 14-15. Framing with nothing selected does nothing; a stale expectation is rejected first.
    invoke_rejected vp-14-focus-nothing focus-selection --expected-selected-entity-id 0 --animate no
    invoke_rejected vp-15-focus-stale focus-selection \
        --expected-selected-entity-id "$prototype_id" --animate no
    invoke vp-17-final-inspect inspect-entity \
        --entity-id "$camera_id" --expected-name "Main Camera" \
        >"$smoke_root/$label-vp-17-final-inspect.json"

    if ! wait "$live_process"; then
        live_process=""
        cat "$log_path" >&2
        echo "Editor viewport-picking process failed ($label)" >&2
        exit 1
    fi
    live_process=""
    grep -Fq -- "EditorViewportPickingV5 producer=external-python" "$log_path"
    grep -Fq -- "backend=$expected_backend result=pass" "$log_path"
    grep -Fq -- "Renderer initialized with backend: $expected_backend" "$log_path"
    test -f "$control_dir/session.closed"
    grep -Fq -- 'Reason "unsupported_schema_expected_v5"' \
        "$control_dir/responses/vp-schema-stale.response"
    grep -Fq -- 'Reason "invalid_or_duplicate_viewport_point"' \
        "$control_dir/responses/vp-16-pick-out-of-range.response"
    test "$(stat -c '%a' "$control_dir/responses/vp-17-final-inspect.response")" = "600"

    python3 - "$smoke_root" "$label" "$target" "$project" "$prototype_id" "$light_id" "$camera_id" <<'PY'
import math
import sys
sys.path.insert(0, sys.argv[1])
import oracle

root, label, target_path, project = sys.argv[1:5]
prototype, light, camera = (int(value) for value in sys.argv[5:8])
target = oracle.load_target(target_path)
get = lambda name: oracle.receipt(root, label, name)

# Receipt identity: schema 5, this project, a live process.
names = ["vp-01-pick-sky", "vp-02-pick-cube", "vp-03-stale-pick", "vp-04-pick-cube-again",
         "vp-05-focus-cube", "vp-06-focus-again", "vp-07-pick-after-focus", "vp-08-pick-inside-a",
         "vp-09-pick-inside-b", "vp-10-pick-outside", "vp-11-select-light", "vp-12-focus-light",
         "vp-13-pick-clear", "vp-14-focus-nothing", "vp-15-focus-stale", "vp-17-final-inspect"]
receipts = {name: get(name) for name in names}
assert all(value["schema"] == 5 and value["projectPath"] == project and value["editorProcessId"] > 0
           for value in receipts.values())
assert sum(value["status"] == "Succeeded" for value in receipts.values()) == 13
assert sum(value["status"] == "Rejected" for value in receipts.values()) == 3
assert all(value["undoDepthAfter"] == value["undoDepthBefore"] for value in receipts.values()), "no history entries"

# Picks (the virtual or live rectangle is echoed; headless must be the documented virtual one).
sky, cube = receipts["vp-01-pick-sky"], receipts["vp-02-pick-cube"]
assert sky["viewport"]["pick"]["state"] == "miss" and sky["selectedEntityIdBefore"] == camera
assert sky["selectedEntityIdAfter"] == 0 and sky["selectionCommitted"], "sky clears the selection"
assert cube["viewport"]["pick"]["state"] == "hit" and cube["viewport"]["pick"]["entityId"] == prototype
assert cube["selectedEntityIdAfter"] == prototype and cube["selectionCommitted"] and cube["pivotRetargeted"]
assert cube["viewport"]["pick"]["refinement"] == "triangles", "the cube is within the triangle budget"
assert cube["viewport"]["pick"]["trianglesTested"] > 0
rect = cube["viewport"]["rect"]
if label == "headless":
    assert cube["viewport"]["pick"]["virtualRect"]
    assert (rect["x"], rect["y"], rect["width"], rect["height"]) == (0.0, 0.0, 1280.0, 720.0)
    assert abs(rect["aspect"] - 1280.0 / 720.0) < 1e-12
# Expected distance: the center ray of the default camera against the 12 triangles of the rotated cube.
camera_pose = [float(v) for v in target["CameraPose"].split()]
corners = oracle.world_corners(target)
expected_distance = oracle.ray_hits_corners(camera_pose[:3], oracle.forward_vector(camera_pose), corners)
assert expected_distance is not None, "the default camera looks at the cube"
assert abs(cube["viewport"]["pick"]["distance"] - expected_distance) < 1e-4, (
    cube["viewport"]["pick"]["distance"], expected_distance)
assert receipts["vp-03-stale-pick"]["reason"] == "compare_and_swap_state_mismatch"
again = receipts["vp-04-pick-cube-again"]
assert again["viewport"]["pick"]["entityId"] == prototype and not again["selectionCommitted"]

# Framing the cube: bounds inside the viewport with the 15 percent margin, view direction kept.
first, second = receipts["vp-05-focus-cube"], receipts["vp-06-focus-again"]
f = first["viewport"]["focus"]
fov, aspect = first["viewport"]["rect"]["fovDegrees"], first["viewport"]["rect"]["aspect"]
assert f["subject"] == "bounds" and f["state"] == "framed" and not f["animated"]
assert f["before"][:3] == camera_pose[:3] and f["before"][3:] == camera_pose[3:], "starts at the default camera"
assert f["after"][3:] == f["before"][3:], "view direction is kept"
center_world = [sum(corner[axis] for corner in corners) / 8.0 for axis in range(3)]
radius = max(math.dist(corner, center_world) for corner in corners)
tight_half = min(math.radians(fov) / 2.0, math.atan(math.tan(math.radians(fov) / 2.0) * aspect))
expected_framing_distance = radius * 1.15 / math.sin(tight_half)
assert abs(f["radius"] - radius) < 1e-4 * max(1.0, radius), (f["radius"], radius)
assert all(abs(f["center"][axis] - center_world[axis]) < 1e-4 for axis in range(3))
assert abs(f["distance"] - expected_framing_distance) < 1e-4 * expected_framing_distance
extent = 0.0
for corner in corners:
    projected = oracle.project(f["after"], fov, aspect, corner)
    assert projected is not None and abs(projected[0]) < 1.0 and abs(projected[1]) < 1.0, "corner inside the viewport"
    extent = max(extent, abs(projected[0]) if aspect < 1.0 else abs(projected[1]))
assert 0.35 < extent < 1.0, f"tight-axis extent {extent} is neither tiny nor clipped"
center = oracle.project(f["after"], fov, aspect, center_world)
assert abs(center[0]) < 1e-4 and abs(center[1]) < 1e-4, "the bounds center is at the viewport center"
g = second["viewport"]["focus"]
assert all(abs(g["before"][i] - f["after"][i]) < 1e-12 for i in range(6)), "second focus starts where the first ended"
assert all(abs(g["after"][i] - f["after"][i]) < 1e-9 for i in range(6)), "framing an already framed subject is idempotent"
assert g["animated"] and not second["editorCameraSynchronized"], "the second request took the animated path"
assert receipts["vp-07-pick-after-focus"]["viewport"]["pick"]["entityId"] == prototype

# Picks derived from the projection of the framed bounds.
inside_a, inside_b, outside = (receipts[name] for name in ("vp-08-pick-inside-a", "vp-09-pick-inside-b", "vp-10-pick-outside"))
assert inside_a["viewport"]["pick"]["state"] == "hit" and inside_b["viewport"]["pick"]["state"] == "hit"
assert inside_a["viewport"]["pick"]["entityId"] == prototype and inside_b["viewport"]["pick"]["entityId"] == prototype
assert outside["viewport"]["pick"]["state"] == "miss" and outside["selectedEntityIdAfter"] == 0

# The light has no mesh bounds: a 2 unit sphere around its position.
select_light, focus_light = receipts["vp-11-select-light"], receipts["vp-12-focus-light"]
assert select_light["selectedEntityIdBefore"] == 0 and select_light["selectedEntityIdAfter"] == light
h = focus_light["viewport"]["focus"]
light_transform = [float(v) for v in target["LightTransform"].split()]
assert light_transform[:3] == [0.0, 0.0, 0.0]
light_position = light_transform[3:6]
assert h["subject"] == "default-radius" and abs(h["radius"] - 2.0) < 1e-9
assert all(abs(h["center"][axis] - light_position[axis]) < 1e-9 for axis in range(3))
fov2, aspect2 = focus_light["viewport"]["rect"]["fovDegrees"], focus_light["viewport"]["rect"]["aspect"]
tight2 = min(math.radians(fov2) / 2.0, math.atan(math.tan(math.radians(fov2) / 2.0) * aspect2))
assert abs(h["distance"] - 2.0 * 1.15 / math.sin(tight2)) < 1e-9
assert h["after"][3:] == h["before"][3:]
for axis in range(3):
    for sign in (-2.0, 2.0):
        probe = list(light_position)
        probe[axis] += sign
        projected = oracle.project(h["after"], fov2, aspect2, probe)
        assert projected is not None and abs(projected[0]) < 1.0 and abs(projected[1]) < 1.0, "light sphere fits"
clear = receipts["vp-13-pick-clear"]
assert clear["viewport"]["pick"]["state"] == "miss" and clear["selectedEntityIdAfter"] == 0

# F with nothing selected does nothing and says why.
nothing = receipts["vp-14-focus-nothing"]
assert nothing["reason"] == "nothing_selected" and nothing["viewport"]["focus"]["state"] == "none"
assert receipts["vp-15-focus-stale"]["reason"] == "compare_and_swap_state_mismatch"
assert receipts["vp-17-final-inspect"]["entityId"] == camera
PY
}

cd -- "$repo_root"
# Event-driven half of click-to-select, Esc, Home, and the animated F framing: an in-process
# headless smoke (no window, no OS input) that drives the layer's OnEvent with engine events.
click_log="$smoke_root/click-smoke.log"
if ! timeout 90s "$editor" --headless --editor-viewport-click-smoke >"$click_log" 2>&1; then
    cat "$click_log" >&2
    echo "Viewport click smoke failed" >&2
    exit 1
fi
grep -Eq -- "ViewportClickSmokeV1 checks=[0-9]+ .*history=unchanged result=pass" "$click_log"

# The ImGui half that cannot run without a window: every panel, the dockspace, hidden-panel
# states, and an emptied selection are drawn through frames of a private headless ImGui
# context so ImGui's stack, ID, and window assertions run over the new code.
panel_log="$smoke_root/panel-ui-smoke.log"
if ! timeout 90s "$editor" --headless --editor-panel-ui-smoke >"$panel_log" 2>&1; then
    cat "$panel_log" >&2
    echo "Panel UI smoke failed" >&2
    exit 1
fi
grep -Eq -- "PanelUiSmokeV1 frames=14 .*persistence=save-load-corrupt-fails-closed result=pass" "$panel_log"

run_sequence headless Headless --headless
if [[ "$run_vulkan" == "--vulkan" ]]; then
    run_sequence vulkan "NVRHI Vulkan" --renderer-vulkan
fi

after_fingerprint="$(fingerprint)"
if [[ "$after_fingerprint" != "$before_fingerprint" ]]; then
    diff -u <(printf '%s\n' "$before_fingerprint") \
        <(printf '%s\n' "$after_fingerprint") >&2 || true
    echo "Viewport picking changed persistent project bytes" >&2
    exit 1
fi

echo "EditorViewportPickingV5Test clickSmoke=pass panelUiSmoke=pass helper=typed schema=5 requests=16 succeeded=13 rejected=3 serverRejectedFixtures=2 headless=pass vulkan=$([[ "$run_vulkan" == "--vulkan" ]] && echo pass || echo skipped) pick=center-hit,sky-clear,silhouette-inside-hit,outside-miss,stale-rejected focus=bounds-fit-15pct-margin,view-direction-kept,idempotent,light-default-radius,nothing-selected-rejected history=unchanged persistentBytes=unchanged input=no-ui-synthesis result=pass"
