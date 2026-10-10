# Design System

## Theme

Quiet dark desktop tool UI for long authoring sessions. Surfaces use restrained neutral separation; blue is reserved for selection, focus, active tabs, and primary actions. The only exception is the transform gizmo, whose handles may use desaturated per-axis hues (see Interaction Contracts); selection itself stays blue.

## Color

- Window background: `rgba(26, 28, 31, 1)`
- Secondary surface: `rgba(31, 33, 36, 1)`
- Popup/title surface: `rgba(20, 23, 26, 1)`
- Border: `rgba(61, 69, 77, 1)`
- Input surface: `rgba(41, 46, 51, 1)`
- Selection: `rgba(51, 79, 107, 1)`
- Selection hover: `rgba(61, 97, 128, 1)`
- Docking preview: `rgba(69, 133, 179, 0.7)`

## Typography

Use the editor's single default sans/monospace-compatible UI font at a compact, fixed scale. Panel titles, property labels, values, buttons, and diagnostics use weight and spacing rather than display typography.

## Shape And Spacing

- Radius: 3 px for windows, frames, popups, scrollbars, and tabs. A panel hosted in a secondary OS window uses radius 0 and an opaque window background, because OS windows are rectangular.
- Window border: 1 px.
- Window padding: 10 px.
- Standard item spacing: 8 px horizontal, 6 px vertical.
- Controls use stable widths and compact rows appropriate for repeated editing.

## Workspace

- Left upper: Scene Hierarchy with search and entity actions.
- Left lower: Content Browser with import, search, type filters, and drag sources.
- Center: renderer-owned viewport.
- Right: selection-scoped Inspector only.
- Bottom: Console, Profiler, and History as sibling tabs.
- Bottom edge of the main window: a status bar (not a dockable or detachable panel) showing the last action announcement, selection summary, active backend, and busy or import state, without animation.
- Top menu bar: File, Edit (named Undo and Redo, History, Clear History), View, Tools, Settings. Undo and Redo live in Edit, not File.
- The layout above is the stable geography and the default. Users may close panels, reopen them from the Window menu, rearrange and save workspaces, and detach panels into their own OS windows where supported; Reset Layout always restores this geography and every detached panel offers Dock Back. The Scene Viewport and the Fab browser panel are pinned to the main window and cannot be detached.
- Top-bar Settings menu: `Project Settings` owns only serialized project frame pacing and save; `Engine Settings` owns global renderer backend selection plus the workspace-global viewport-navigation preset. Profiler owns non-serialized runtime pacing experiments. `Editor Preferences` owns only workspace-global, non-serialized editor state (UI scale, restoring detached windows, shortcut display, optional history budget) and never duplicates controls owned by another menu or the viewport toolbar. Standalone duplicate settings panels are forbidden.

## Interaction Contracts

- Inspector content follows the selected entity or asset.
- Camera projection and background color live on the selected camera in the Inspector. `Project Settings` owns serialized project frame pacing; `Engine Settings` owns global renderer backend selection and viewport navigation; Profiler owns non-serialized runtime pacing experiments. Neither moves selection-scoped controls out of the Inspector or duplicates that authority in a dockable panel.
- Camera-bearing entities omit the ineffective Transform Scale control; camera zoom uses Field of View or the applicable projection setting. The Inspector does not reset stored scale because an entity may also carry a component for which scale is meaningful.
- Perspective navigation is scoped to the focused viewport image and selected in `Settings > Engine Settings`. Capture is deferred for both presets: a qualifying press records the visible cursor as the restore/zoom-anchor position and enters a pending epoch; after the current GLFW event-poll batch, the next navigation update enters disabled mode, discards any synchronous transition callback while baselining the virtual cursor, then accepts ordinary motion. Release or focus loss before that arm only cancels pending state; after arm it restores the exact recorded visible cursor. This avoids treating a disabled-mode recenter callback as input without using an arbitrary motion clamp. `Fusion` is the default: wheel zoom computes the cursor ray from the real image rectangle, FOV, aspect, and camera basis, intersects that ray with the persistent pivot-depth plane, then moves the camera toward/away from the resulting anchor by a bounded multiplicative scale that cannot cross the near-plane safety distance. MMB is canvas pan: it translates camera and pivot together in camera right/up, scaled by FOV, pivot depth, and viewport height. Shift+MMB preserves pivot distance and roll=0 by expressing the actual camera-to-pivot offset in the old camera basis and reconstructing it in the new basis; it does not assume the camera forward ray still intersects the pivot after an off-center zoom, so a zero-delta orbit is an exact no-op and a reversed small delta restores the prior pose within floating-point tolerance. The documented Fusion intent is wheel zoom/MMB pan/Shift+MMB orbit and a default component-group bounding-box-center orbit pivot with reset/custom-pivot controls: <https://help.autodesk.com/view/fusion360/ENU/index.html?guid=GUID-878489CD-3A23-4303-8450-C2F4F8E410B1> and <https://help.autodesk.com/view/fusion360/ENU/?caas=caas%2Fsfdcarticles%2Fsfdcarticles%2FHow-to-reset-the-orbit-pivot-point-in-Fusion-360.html>. Engine has no mesh geometry bounds or picking yet, so its default approximation is the deterministic AABB center of visible mesh entity transforms, or a camera-forward fallback. The startup-selected non-camera entity, project load/create, and undo/redo restoration establish the persistent pivot from that entity's double-precision transform origin before the first gesture. Choosing another non-camera entity in Scene Hierarchy immediately retargets it without moving the camera; subsequent Inspector transform edits keep the pivot synchronized. F uses the same selected origin and additionally repositions the camera. Plain LMB/RMB, fly keys, and RMB-wheel speed control do not navigate in Fusion. `Unreal` retains plain LMB move/yaw, RMB look, LMB+RMB/MMB pan, wheel movement, RMB-wheel speed, and RMB fly keys with the same disabled-cursor epoch. F is not Autodesk Fit/Zoom-to-Fit or bounds framing, and MMB double-click Fit is intentionally unavailable until real bounds/picking exist. The preset persists transactionally at workspace-global `output/editor/engine-settings.spiralsettings`, never in `.spiralproject`; missing, unreadable, malformed, or unknown-version data fails closed to Fusion and leaves the source file untouched. Selection picking, bounds framing, custom/reset pivot UI, orthographic controls, camera piloting, and configurable bindings remain unavailable until their owning contracts exist. The transform gizmo and snapping are owned by `Docs/Architecture/EDITOR_UI_ARCHITECTURE.md`.
- A qualifying MMB press or wheel event over the hovered viewport may acquire its ImGui focus in the same engine input epoch. Because GLFW events are polled before the next ImGui frame, the Editor latches that focus request and keeps the initiating Fusion event; changing the preset through Settings must not require a hidden preparatory LMB click or discard the first pan/orbit/zoom gesture. Active text/widgets, popups covering the viewport, loss of OS-window focus, and input outside the viewport still reject navigation.
- Unsupported backends are visibly disabled and explain why. The same applies to detaching panels on Direct3D 12, native Wayland, OpenGL2, or when the present-mode guard fails, and to snapping modes (surface, vertex, bounds) and pivots that lack their prerequisites.
- Menu items and buttons that would imply behavior the engine does not have are removed or disabled with a reason; test and demo affordances do not appear in product UI.
- Undo and redo operate on named entries (verb plus target, for example `Move Cube`). One continuous edit (an Inspector drag, a text-field focus session, or a gizmo drag) is one entry. Selection changes are not entries and each entry restores its recorded selection. Undo never moves the viewport navigation camera unless the entry edited the camera. A committed Fab import is an undo barrier shown in the Edit menu and History panel. Ctrl+Z undoes; Ctrl+Y and Ctrl+Shift+Z redo; undo and redo are blocked while typing in a text field, during a drag, with a modal open, or while the Fab web page owns the keyboard. The Edit menu shows `Undo <name>` and `Redo <name>`, the History panel lists entries with a current marker and click-to-jump, and every undo, redo, and edit is announced in the status bar and the Console.
- The transform gizmo acts on the Hierarchy selection and is not shown for the main camera entity or for scale on camera-bearing entities. Q selects no tool, W translates, E rotates, R scales, and X toggles World and Local while the viewport has focus and is not navigating. Snapping is off by default (translate step 1.0 world unit, rotate 15 degrees, scale step 0.1); holding Ctrl while dragging inverts the snap toggle, and the snap state is shown with text, not color alone. Axis hues appear on gizmo handles only, each handle also carries an X, Y, or Z letter, and hover and active states use the selection tokens. One drag is one history entry and Esc cancels it, restoring the start transform exactly. Translate snapping respects the canonical sector-local world grid.
- The Scene Viewport takes its input from main-window engine events and cursor capture, so it stays in the main window; the navigation contract above is specified against that window.
- The outliner is flat until entity parenting has its own contract. Lock is session-only editor state labelled as such, and visibility is the MeshRenderer visibility flag shown only where it applies.
- Destructive hierarchy actions are contextual, undoable, and protect the primary camera.
- New projects reject path collisions instead of silently overwriting existing manifests.

## Amendments 2026-10-09

Made to record the Editor UI project-owner decisions in `Docs/Architecture/EDITOR_UI_ARCHITECTURE.md`; unrelated text is unchanged.

- Theme: gizmo axis hues are the single allowed exception to blue-only accents; selection stays blue.
- Shape: radius 0 and an opaque background for panels hosted in secondary OS windows; 3 px elsewhere.
- Workspace: History joins Console and Profiler as a bottom tab; a status bar is added at the bottom edge; the Edit menu is added and Undo and Redo leave File; the default layout is declared the stable geography with user-initiated closing, workspaces, and detaching on top of it; the Scene Viewport and Fab browser panel are pinned to the main window.
- Settings: `Editor Preferences` is the third Settings entry for workspace-global, non-serialized editor state.
- Interaction Contracts: the Fusion paragraph no longer lists gizmos as unavailable (picking, bounds framing, custom pivot UI, orthographic controls, camera piloting, and configurable bindings still are); added contracts for disabled-with-reason detaching and snapping modes, removal of placeholder controls, named undo and redo with shortcut blocking and the Fab undo barrier, the transform gizmo and snapping, main-window-only Scene Viewport input, and the flat outliner with session-only lock.
