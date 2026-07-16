# Design System

## Theme

Quiet dark desktop tool UI for long authoring sessions. Surfaces use restrained neutral separation; blue is reserved for selection, focus, active tabs, and primary actions.

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

- Radius: 3 px for windows, frames, popups, scrollbars, and tabs.
- Window border: 1 px.
- Window padding: 10 px.
- Standard item spacing: 8 px horizontal, 6 px vertical.
- Controls use stable widths and compact rows appropriate for repeated editing.

## Workspace

- Left upper: Scene Hierarchy with search and entity actions.
- Left lower: Content Browser with import, search, type filters, and drag sources.
- Center: renderer-owned viewport.
- Right: selection-scoped Inspector only.
- Bottom: Console and Profiler as sibling tabs.
- Top-bar Settings menu: `Project Settings` owns only serialized project frame pacing and save; `Engine Settings` owns global renderer backend selection plus the workspace-global viewport-navigation preset. Profiler owns non-serialized runtime pacing experiments. Standalone duplicate settings panels are forbidden.

## Interaction Contracts

- Inspector content follows the selected entity or asset.
- Camera projection and background color live on the selected camera in the Inspector. `Project Settings` owns serialized project frame pacing; `Engine Settings` owns global renderer backend selection and viewport navigation; Profiler owns non-serialized runtime pacing experiments. Neither moves selection-scoped controls out of the Inspector or duplicates that authority in a dockable panel.
- Camera-bearing entities omit the ineffective Transform Scale control; camera zoom uses Field of View or the applicable projection setting. The Inspector does not reset stored scale because an entity may also carry a component for which scale is meaningful.
- Perspective navigation is scoped to the focused viewport image and selected in `Settings > Engine Settings`. `Fusion` is the default: wheel zoom computes the cursor ray from the real image rectangle, FOV, aspect, and camera basis, intersects that ray with the persistent pivot-depth plane, then moves the camera toward/away from the resulting anchor by a bounded multiplicative scale that cannot cross the near-plane safety distance. MMB is canvas pan: it translates camera and pivot together in camera right/up, scaled by FOV, pivot depth, and viewport height. Shift+MMB preserves pivot distance and roll=0 by expressing the actual camera-to-pivot offset in the old camera basis and reconstructing it in the new basis; it does not assume the camera forward ray still intersects the pivot after an off-center zoom, so a zero-delta orbit is an exact no-op and a reversed small delta restores the prior pose within floating-point tolerance. The documented Fusion intent is wheel zoom/MMB pan/Shift+MMB orbit and a default component-group bounding-box-center orbit pivot with reset/custom-pivot controls: <https://help.autodesk.com/view/fusion360/ENU/index.html?guid=GUID-878489CD-3A23-4303-8450-C2F4F8E410B1> and <https://help.autodesk.com/view/fusion360/ENU/?caas=caas%2Fsfdcarticles%2Fsfdcarticles%2FHow-to-reset-the-orbit-pivot-point-in-Fusion-360.html>. Engine has no mesh geometry bounds or picking yet, so its verified approximation is instead the deterministic AABB center of visible mesh entity transforms, or a camera-forward fallback. That pivot persists across gestures; F is the only current explicit selected-origin replacement. Plain LMB/RMB, fly keys, and RMB-wheel speed control do not navigate in Fusion. `Unreal` retains plain LMB move/yaw, RMB look, LMB+RMB/MMB pan, wheel movement, RMB-wheel speed, and RMB fly keys. F is not Autodesk Fit/Zoom-to-Fit or bounds framing, and MMB double-click Fit is intentionally unavailable until real bounds/picking exist. The preset persists transactionally at workspace-global `output/editor/engine-settings.spiralsettings`, never in `.spiralproject`; missing, unreadable, malformed, or unknown-version data fails closed to Fusion and leaves the source file untouched. Capture restores the exact cursor position on release or focus loss and never steals an active ImGui widget/text interaction. Selection picking, bounds framing, custom/reset pivot UI, gizmos, orthographic controls, camera piloting, and configurable bindings remain unavailable until their owning contracts exist.
- Unsupported backends are visibly disabled and explain why.
- Destructive hierarchy actions are contextual, undoable, and protect the primary camera.
- New projects reject path collisions instead of silently overwriting existing manifests.
