#!/usr/bin/env python3
"""Typed-control checks of the Editor's undo history, driven through the real mailbox.

Spawned by Scripts/TestEditorHistory.sh. Three headless Editor sessions run against the
default project (nothing is saved):

  A  default limits: a named entry for every typed action kind, undo/redo round trips
     that restore the project state byte-exactly, redo invalidation, compare-and-swap,
     and that undo keeps the navigation camera unless the entry edited it;
  B  a one-byte budget: the eviction notice, the base row, and the receipt counters;
  C  a three-entry cap: eviction by entry count.

The expected labels, strings and numbers below are written by hand from the owner's
wording ("Undo: Move Cube", ...), not read back from the Editor.
"""

from __future__ import annotations

import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

REPO = Path(sys.argv[1])
EDITOR = Path(sys.argv[2])
HELPER = REPO / "Scripts" / "EditorMaterialControl.py"
PROJECT = REPO / "output" / "projects" / "default.spiralproject"
SCENE = REPO / "output" / "scenes" / "sample.spiral"
ASSETS = REPO / "output" / "assets" / "sample.assets"

DEFAULT_BUDGET = 256 * 1024 * 1024
DEFAULT_ENTRIES = 512


class Failure(AssertionError):
    pass


def expect(condition: object, message: str) -> None:
    if not condition:
        raise Failure(message)


def parse_scene() -> dict[str, int]:
    ids: dict[str, int] = {}
    for line in SCENE.read_text(encoding="utf-8").splitlines():
        if line.startswith("Entity "):
            _, entity_id, name = line.split(" ", 2)
            ids[name.strip('"')] = int(entity_id)
    return ids


def prototype_material_handle() -> int:
    for line in ASSETS.read_text(encoding="utf-8").splitlines():
        if line.startswith("Asset ") and " Material " in line and '"Prototype Default"' in line:
            return int(line.split(" ")[1])
    raise Failure("the default project has no Prototype Default material")


class Session:
    def __init__(self, label: str, flags: list[str]) -> None:
        self.label = label
        self.directory = Path(tempfile.mkdtemp(prefix=f"spiral-history-{label}-")) / "mailbox"
        self.log_path = self.directory.parent / "editor.log"
        self.counter = 0
        self.log = open(self.log_path, "wb")
        self.process = subprocess.Popen(
            [str(EDITOR), "--headless", f"--editor-control-dir={self.directory}", *flags],
            cwd=REPO, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 30.0
        while not (self.directory / "session.info").exists():
            if self.process.poll() is not None:
                raise Failure(f"Editor exited before opening the mailbox ({label}):\n{self.log_path.read_text()}")
            if time.monotonic() > deadline:
                raise Failure(f"Editor did not open the mailbox in time ({label})")
            time.sleep(0.02)

    def close(self) -> None:
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait()
        self.log.close()

    def invoke(self, *arguments: str, expect_status: int = 0) -> dict:
        self.counter += 1
        request_id = f"h-{self.label}-{self.counter:03d}"
        result = subprocess.run(
            [sys.executable, str(HELPER), "--control-dir", str(self.directory), "--expected-project",
             str(PROJECT), "--request-id", request_id, *arguments],
            capture_output=True, text=True, timeout=60)
        if result.returncode not in (0, 2):
            raise Failure(f"helper failed for {arguments}: {result.stderr.strip()}")
        expect(result.returncode == expect_status,
               f"{' '.join(arguments)} returned {result.returncode}, wanted {expect_status}: {result.stdout[:400]}")
        return json.loads(result.stdout)

    # ---- typed reads and edits ----
    def history(self) -> dict:
        receipt = self.invoke("inspect-history")
        expect(receipt["effect"] == "ReadOnly" and receipt["reason"] == "ok", "InspectHistory succeeds read-only")
        return receipt["history"]

    def entity(self, entity_id: int, name: str) -> dict:
        return self.invoke("inspect-entity", "--entity-id", str(entity_id), "--expected-name", name)


def tokens(values: list) -> list[str]:
    return [("yes" if value is True else "no" if value is False else repr(value) if isinstance(value, float) else str(value))
            for value in values]


def vec12(transform: list, **changes: float) -> list:
    """transform is [sx, sy, sz, lx, ly, lz, rx, ry, rz, cx, cy, cz]; changes by name."""
    names = {"lx": 3, "ly": 4, "lz": 5, "rx": 6, "ry": 7, "rz": 8, "cx": 9, "cy": 10, "cz": 11}
    changed = list(transform)
    for key, value in changes.items():
        changed[names[key]] = value
    return changed


def check_row(history: dict, newest_display: str, verb: str, target: str, source: str, entries: int,
              previous_head: int) -> None:
    rows = history["rows"]
    expect(history["entryCount"] == entries and history["undoDepth"] == entries and history["redoDepth"] == 0,
           f"{newest_display}: entry count {history['entryCount']} (wanted {entries})")
    newest = rows[0]
    expect(newest["display"] == newest_display and newest["verb"] == verb and newest["target"] == target,
           f"newest entry is '{newest['display']}' / {newest['verb']!r} / {newest['target']!r}, wanted '{newest_display}'")
    expect(newest["source"] == source and newest["current"] and newest["applied"] and not newest["base"],
           f"{newest_display}: source/current flags {newest}")
    expect(newest["revision"] == history["revisionBefore"] and newest["revision"] > previous_head,
           f"{newest_display}: head revision must advance past {previous_head}")
    expect(newest["bytes"] > 0 and history["usedBytes"] > 0, f"{newest_display}: the entry has an estimated size")
    expect(history["undoEnabled"] and history["undoLabel"] == f"Undo: {newest_display}" and history["undoReason"] == "",
           f"{newest_display}: Edit menu undo text is '{history['undoLabel']}'")


def phase_a() -> None:
    ids = parse_scene()
    prototype, light, camera = ids["Prototype Mesh"], ids["Directional Light"], ids["Main Camera"]
    material = prototype_material_handle()
    session = Session("a", [])
    try:
        helper = session.invoke
        baseline = session.history()
        expect(baseline["entryCount"] == 0 and len(baseline["rows"]) == 1 and baseline["rowTotal"] == 1,
               "a fresh project has only the base row")
        base = baseline["rows"][0]
        expect(base["base"] and base["current"] and not base["barrier"] and base["display"] == "Open Project default"
               and base["source"] == "system", f"base row is {base}")
        expect(not baseline["undoEnabled"] and baseline["undoLabel"] == "Undo (nothing to undo)"
               and baseline["undoReason"] == "Undo unavailable: nothing to undo", "empty undo text")
        expect(not baseline["redoEnabled"] and baseline["redoLabel"] == "Redo (nothing to redo)"
               and baseline["redoReason"] == "Redo unavailable: nothing to redo", "empty redo text")
        expect(baseline["budgetBytes"] == DEFAULT_BUDGET and baseline["maximumEntries"] == DEFAULT_ENTRIES,
               "default budget 256 MiB and 512 entries")
        expect(baseline["evictedEntries"] == 0 and not baseline["gestureOpen"] and not baseline["baseIsBarrier"]
               and baseline["announcement"] == "" and baseline["evictionNotice"] == "", "fresh counters")

        def surface() -> dict:
            return helper("inspect", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
                          "--material-handle", str(material))

        def state() -> dict:
            proto, lit, cam = (session.entity(prototype, "Prototype Mesh"), session.entity(light, "Directional Light"),
                               session.entity(camera, "Main Camera"))
            material_receipt = surface()
            return {"prototypeTransform": proto["beforeTransform"], "prototypeMesh": proto["beforeMeshRenderer"],
                    "light": lit["beforeLight"], "lightTransform": lit["beforeTransform"],
                    "camera": cam["beforeTransform"], "cameraLens": cam["beforeCamera"],
                    "color": proto["beforeColorPipeline"], "surface": material_receipt["beforeSurface"],
                    "names": (proto["entityName"], lit["entityName"], cam["entityName"])}

        initial = state()
        head = baseline["revisionBefore"]
        entries = 0
        edits: list[str] = []

        def committed(display: str, verb: str, target: str) -> None:
            nonlocal head, entries
            entries += 1
            after = session.history()
            check_row(after, display, verb, target, "agent", entries, head)
            head = after["revisionBefore"]
            edits.append(display)

        proto = session.entity(prototype, "Prototype Mesh")
        base_transform = proto["beforeTransform"]

        def set_transform(label: str, **changes: float) -> None:
            current = session.entity(prototype, "Prototype Mesh")["beforeTransform"]
            receipt = helper("set-transform", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
                             "--expected-transform", *tokens(current), "--new-transform", *tokens(vec12(current, **changes)))
            expect(receipt["effect"] == "EntityTransformSet" and receipt["history"]["revisionBefore"] == head,
                   f"{label}: receipt carries the head revision it was handled at")
            expect(receipt["history"]["revisionAfter"] == 0, f"{label}: a recording action does not predict its head revision")

        set_transform("move", lx=base_transform[3] + 1.0)
        committed("Move Prototype Mesh", "Move", "Prototype Mesh")
        set_transform("rotate", ry=base_transform[7] + 10.0)
        committed("Rotate Prototype Mesh", "Rotate", "Prototype Mesh")
        set_transform("scale", cx=1.5)
        committed("Scale Prototype Mesh", "Scale", "Prototype Mesh")
        set_transform("two parts", lx=base_transform[3] + 2.0, rx=base_transform[6] + 5.0)
        committed("Edit Transform of Prototype Mesh", "Edit Transform of", "Prototype Mesh")

        lit = session.entity(light, "Directional Light")["beforeLight"]
        helper("set-typed-light", "--entity-id", str(light), "--expected-name", "Directional Light",
               "--expected-light", *tokens(lit), "--new-light", *tokens([lit[0], 0.5, lit[2], lit[3], *lit[4:]]))
        committed("Edit Light.Color of Directional Light", "Edit Light.Color of", "Directional Light")
        lit = session.entity(light, "Directional Light")["beforeLight"]
        helper("set-typed-light", "--entity-id", str(light), "--expected-name", "Directional Light",
               "--expected-light", *tokens(lit),
               "--new-light", *tokens([*lit[:4], lit[4] + 100.0, lit[5], lit[6] + 1.0, *lit[7:]]))
        committed("Edit Light of Directional Light", "Edit Light of", "Directional Light")

        color = session.entity(prototype, "Prototype Mesh")["beforeColorPipeline"]
        helper("set-project-color-pipeline", "--expected-color-pipeline", *tokens(color),
               "--new-color-pipeline", *tokens([color[0] + 0.5, *color[1:]]))
        committed("Edit Color Pipeline.Manual EV100 of Project", "Edit Color Pipeline.Manual EV100 of", "Project")

        flags = session.entity(prototype, "Prototype Mesh")["beforeMeshRenderer"]
        helper("set-mesh-renderer-flags", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
               "--expected-visible", "yes" if flags[3] else "no", "--expected-casts-shadows", "yes" if flags[4] else "no",
               "--new-visible", "no" if flags[3] else "yes", "--new-casts-shadows", "yes" if flags[4] else "no")
        committed("Edit Mesh Renderer.Visible of Prototype Mesh", "Edit Mesh Renderer.Visible of", "Prototype Mesh")

        pose = session.entity(camera, "Main Camera")["beforeTransform"]
        helper("set-viewport-main-camera-pose", "--entity-id", str(camera), "--expected-name", "Main Camera",
               "--expected-transform", *tokens(pose), "--new-transform", *tokens(vec12(pose, lx=pose[3] + 0.5)))
        committed("Move Main Camera", "Move", "Main Camera")

        surf = surface()["beforeSurface"]
        new_surface = [min(1.0, surf[0] + 0.1), surf[1], surf[2], surf[3], min(1.0, surf[4] + 0.1)]
        helper("set", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh", "--material-handle", str(material),
               "--expected-base", *tokens(surf[:3]), "--expected-metallic", repr(surf[3]), "--expected-roughness", repr(surf[4]),
               "--new-base", *tokens(new_surface[:3]), "--new-metallic", repr(new_surface[3]),
               "--new-roughness", repr(new_surface[4]))
        committed("Edit Material.Surface of Prototype Default", "Edit Material.Surface of", "Prototype Default")

        # The whole list, newest first, with strictly increasing revisions and one current row.
        listing = session.history()
        rows = listing["rows"]
        expect([row["display"] for row in rows[:-1]] == list(reversed(edits)) and rows[-1]["base"],
               f"rows newest first: {[row['display'] for row in rows]}")
        revisions = [row["revision"] for row in rows]
        expect(revisions == sorted(revisions, reverse=True) and len(set(revisions)) == len(revisions), "revisions strictly increase")
        expect(sum(1 for row in rows if row["current"]) == 1 and rows[0]["current"], "exactly the newest row is current")
        expect(all(row["source"] == "agent" for row in rows[:-1]), "every typed entry is attributed to the agent")

        final = state()
        expect(final != initial, "the edits changed the project")

        # Undo every entry, with the exact announcement; the project returns byte-exactly.
        for display in reversed(edits):
            current = session.history()
            undo = helper("undo-history", "--expected-history-revision", str(current["revisionBefore"]))
            expect(undo["history"]["announcement"] == f"Undo: {display}" and undo["effect"] == "HistoryUndone",
                   f"undo announces 'Undo: {display}', got '{undo['history']['announcement']}'")
            after = session.history()
            expect(after["revisionBefore"] == undo["history"]["revisionAfter"] and after["announcement"] == f"Undo: {display}",
                   "the receipt's resulting head revision and announcement are what InspectHistory reports afterwards")
            expect(after["undoDepth"] == current["undoDepth"] - 1 and after["redoDepth"] == current["redoDepth"] + 1,
                   "undo moves one entry onto the redo side")
            expect(after["redoLabel"] == f"Redo: {display}" and after["redoEnabled"], "redo names the entry just undone")
        expect(state() == initial, "after undoing every entry the project state is byte-identical to the start")
        empty = session.history()
        expect(not empty["undoEnabled"] and empty["undoReason"] == "Undo unavailable: nothing to undo" and empty["undoDepth"] == 0,
               "the stack is empty and says why")
        refused = helper("undo-history", "--expected-history-revision", str(empty["revisionBefore"]), expect_status=2)
        expect(refused["reason"] == "nothing_to_undo", f"undo on an empty stack: {refused['reason']}")

        for display in edits:
            current = session.history()
            redo = helper("redo-history", "--expected-history-revision", str(current["revisionBefore"]))
            expect(redo["history"]["announcement"] == f"Redo: {display}" and redo["effect"] == "HistoryRedone",
                   f"redo announces 'Redo: {display}'")
        expect(state() == final, "after redoing every entry the project state is byte-identical to the edited state")
        top = session.history()
        expect(not top["redoEnabled"] and top["redoReason"] == "Redo unavailable: nothing to redo", "nothing left to redo")
        refused = helper("redo-history", "--expected-history-revision", str(top["revisionBefore"]), expect_status=2)
        expect(refused["reason"] == "nothing_to_redo", f"redo with nothing to redo: {refused['reason']}")

        # Stale compare-and-swap leaves the state unchanged.
        stale = top["revisionBefore"] - 1
        for command in ("undo-history", "redo-history"):
            rejected = helper(command, "--expected-history-revision", str(stale), expect_status=2)
            expect(rejected["reason"] == "history_revision_mismatch" and rejected["effect"] == "None",
                   f"{command} with a stale revision: {rejected['reason']}")
        unchanged = session.history()
        expect(unchanged["revisionBefore"] == top["revisionBefore"] and unchanged["entryCount"] == top["entryCount"]
               and unchanged["cursor"] == top["cursor"], "a rejected undo/redo changes nothing")
        expect(state() == final, "a rejected undo/redo leaves the project untouched")

        # A new edit after undo discards the redo entries and says so.
        for _ in range(2):
            current = session.history()
            helper("undo-history", "--expected-history-revision", str(current["revisionBefore"]))
        before_discard = session.history()
        expect(before_discard["redoDepth"] == 2, "two entries wait on the redo side")
        current_transform = session.entity(prototype, "Prototype Mesh")["beforeTransform"]
        helper("set-transform", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
               "--expected-transform", *tokens(current_transform),
               "--new-transform", *tokens(vec12(current_transform, lx=current_transform[3] + 3.0)))
        after_discard = session.history()
        expect(after_discard["redoDepth"] == 0 and after_discard["entryCount"] == before_discard["undoDepth"] + 1,
               "the new entry empties redo")
        expect(after_discard["announcement"] == "Redo history discarded (2)", f"announcement: {after_discard['announcement']}")
        rejected = helper("redo-history", "--expected-history-revision", str(after_discard["revisionBefore"]), expect_status=2)
        expect(rejected["reason"] == "nothing_to_redo", "redo is unavailable after a new entry")

        # Undo keeps the navigation camera unless the entry edited it.
        def camera_pose() -> list[float]:
            transform = session.entity(camera, "Main Camera")["beforeTransform"]
            return [transform[3], transform[4], transform[5], transform[6], transform[7], transform[8]]

        def close(first: list[float], second: list[float]) -> bool:
            # Entity transforms are printed with nine significant digits, so compare to 1e-6.
            return all(math.isclose(a, b, rel_tol=0, abs_tol=1e-6) for a, b in zip(first, second, strict=True))

        def select_prototype() -> None:
            current_selection = session.entity(prototype, "Prototype Mesh")["selectedEntityIdBefore"]
            if current_selection != prototype:
                helper("select-entity", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
                       "--expected-selected-entity-id", str(current_selection))

        def focus() -> list[float]:
            select_prototype()
            receipt = helper("focus-selection", "--expected-selected-entity-id", str(prototype), "--animate", "no")
            return receipt["viewport"]["focus"]["after"]

        select_prototype()
        pose_start = camera_pose()
        current_transform = session.entity(prototype, "Prototype Mesh")["beforeTransform"]
        helper("set-transform", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
               "--expected-transform", *tokens(current_transform),
               "--new-transform", *tokens(vec12(current_transform, lz=current_transform[5] + 0.25)))
        navigated = focus()
        expect(not close(navigated, pose_start), "focusing moved the navigation camera away from where it was")
        current = session.history()
        helper("undo-history", "--expected-history-revision", str(current["revisionBefore"]))
        expect(close(camera_pose(), navigated),
               f"undoing a non-camera entry leaves the navigation camera where it is: {camera_pose()} != {navigated}")
        current = session.history()
        helper("redo-history", "--expected-history-revision", str(current["revisionBefore"]))
        expect(close(camera_pose(), navigated), "redoing a non-camera entry leaves the navigation camera where it is")

        # A camera entry moves it: undo goes back to where the entry started, redo to where it ended.
        started = camera_pose()
        pose_now = session.entity(camera, "Main Camera")["beforeTransform"]
        edited_pose = vec12(pose_now, lx=pose_now[3] + 1.0, ry=pose_now[7] + 20.0)
        helper("set-viewport-main-camera-pose", "--entity-id", str(camera), "--expected-name", "Main Camera",
               "--expected-transform", *tokens(pose_now), "--new-transform", *tokens(edited_pose))
        ended = camera_pose()
        expect(not close(ended, started), "the camera entry moved the camera")
        moved_away = focus()
        expect(not close(moved_away, ended) and not close(moved_away, started), "focusing after the entry moved it somewhere new")
        current = session.history()
        helper("undo-history", "--expected-history-revision", str(current["revisionBefore"]))
        expect(close(camera_pose(), started), "undoing a camera entry returns the camera to where the entry started")
        current = session.history()
        helper("redo-history", "--expected-history-revision", str(current["revisionBefore"]))
        expect(close(camera_pose(), ended), "redoing a camera entry returns the camera to where the entry left it")
    finally:
        session.close()
    print("EditorHistoryTestV1 phase=default-limits named-entries=10 round-trip=byte-exact redo-invalidation=announced "
          "compare-and-swap=stale-rejected camera=kept-unless-edited result=pass")


def phase_budget() -> None:
    ids = parse_scene()
    prototype = ids["Prototype Mesh"]
    session = Session("b", ["--editor-history-budget-bytes=1"])
    try:
        baseline = session.history()
        expect(baseline["budgetBytes"] == 1, "the budget override is honoured")

        def move(delta: float) -> None:
            current = session.entity(prototype, "Prototype Mesh")["beforeTransform"]
            session.invoke("set-transform", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
                           "--expected-transform", *tokens(current),
                           "--new-transform", *tokens(vec12(current, lx=current[3] + delta)))

        move(1.0)
        first = session.history()
        expect(first["entryCount"] == 1 and first["evictedEntries"] == 0 and first["announcement"] == "",
               "the newest entry is always kept, so one entry over budget evicts nothing")
        move(1.0)
        second = session.history()
        expect(second["entryCount"] == 1 and second["evictedEntries"] == 1 and second["evictionEvents"] == 1
               and second["evictedBytes"] > 0, f"the second entry evicts the first: {second['evictedEntries']}")
        expect(second["announcement"] == "Earlier history dropped", f"eviction announcement: {second['announcement']}")
        expect(second["evictionNotice"] == "1 oldest entry was dropped to stay within 1 B",
               f"History panel notice: {second['evictionNotice']}")
        expect(second["rows"][0]["display"] == "Move Prototype Mesh" and second["rows"][1]["display"] == "Earlier history dropped"
               and second["rows"][1]["base"], f"rows after eviction: {[row['display'] for row in second['rows']]}")
        expect(second["usedBytes"] > second["budgetBytes"], "the newest entry may exceed the budget on its own")
        undo = session.invoke("undo-history", "--expected-history-revision", str(second["revisionBefore"]))
        expect(undo["history"]["announcement"] == "Undo: Move Prototype Mesh", "the surviving entry still undoes")
        after = session.history()
        expect(not after["undoEnabled"] and after["undoReason"].startswith("Undo unavailable: earlier history was dropped"),
               f"undo explains the dropped history: {after['undoReason']}")
        refused = session.invoke("undo-history", "--expected-history-revision", str(after["revisionBefore"]), expect_status=2)
        expect(refused["reason"] == "nothing_to_undo", "undo past the dropped history is refused")
    finally:
        session.close()
    print("EditorHistoryTestV1 phase=tiny-budget eviction-notice='Earlier history dropped' base-row=dropped newest-kept=yes result=pass")


def phase_cap() -> None:
    ids = parse_scene()
    prototype = ids["Prototype Mesh"]
    session = Session("c", ["--editor-history-max-entries=3"])
    try:
        for index in range(5):
            current = session.entity(prototype, "Prototype Mesh")["beforeTransform"]
            session.invoke("set-transform", "--entity-id", str(prototype), "--expected-name", "Prototype Mesh",
                           "--expected-transform", *tokens(current),
                           "--new-transform", *tokens(vec12(current, lx=current[3] + 1.0 + index)))
        listing = session.history()
        expect(listing["maximumEntries"] == 3 and listing["entryCount"] == 3 and listing["evictedEntries"] == 2
               and listing["evictionEvents"] == 2 and listing["announcement"] == "Earlier history dropped"
               and listing["evictionNotice"] == "1 oldest entry was dropped to stay within 3 entries",
               f"entry-cap eviction: {listing['entryCount']} entries, {listing['evictedEntries']} evicted, "
               f"notice '{listing['evictionNotice']}'")
    finally:
        session.close()
    rejected = subprocess.run([str(EDITOR), "--headless", "--editor-history-max-entries=0", "--smoke-test"], cwd=REPO,
                              capture_output=True, text=True, timeout=60)
    expect(rejected.returncode != 0 and "Invalid --editor-history-max-entries" in rejected.stdout + rejected.stderr,
           "an invalid entry cap is refused at start-up")
    print("EditorHistoryTestV1 phase=entry-cap eviction=oldest-dropped invalid-flag=refused result=pass")


def main() -> int:
    try:
        phase_a()
        phase_budget()
        phase_cap()
    except Failure as failure:
        print(f"EditorHistoryTest failed: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
