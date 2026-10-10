# Editor UI Review

Date: 2026-07-13

## Finding

The previous dock layout grew by appending global camera, renderer, and clear-color controls to the entity Inspector. This broke selection context: selecting a mesh exposed settings unrelated to that mesh. The hierarchy also occupied a secondary right-side slot while the asset list consumed the entire left column.

## Reference Patterns

- Unity's Inspector changes with the selected GameObject or asset and displays that selection's components and materials: <https://docs.unity3d.com/cn/current/Manual/UsingTheInspector.html>
- Unreal's Details panel is specific to the current selection, while the Outliner owns scene selection and search: <https://dev.epicgames.com/documentation/unreal-engine/level-editor-details-panel-in-unreal-engine?lang=en-US> and <https://dev.epicgames.com/documentation/unreal-engine/outliner-in-unreal-engine?lang=en-US>
- Unreal treats project configuration as a separate categorized, searchable Project Settings window and assets as a dedicated Content Browser: <https://dev.epicgames.com/documentation/unreal-engine/project-settings-in-unreal-engine> and <https://dev.epicgames.com/documentation/unreal-engine/content-browser-in-unreal-engine?lang=en-US>
- Godot keeps Scene and FileSystem docks beside the viewport, uses a selection-driven Inspector, and places diagnostics in a collapsible bottom panel: <https://docs.godotengine.org/en/stable/getting_started/introduction/first_look_at_the_editor.html> and <https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html>

These engines differ visually, but their information architecture agrees. Spiral should borrow that stable geography rather than imitate one engine's styling.

## Implemented Direction

- Scene Hierarchy above Content Browser on the left.
- Viewport remains the central work surface.
- Inspector on the right contains only the selected entity's name, effective transform controls, and attached components. Camera-bearing entities expose Position and Rotation but omit Transform Scale because scale has no camera-view effect; entities without a Camera component retain Scale. Camera projection and background color appear only when a camera entity is selected.
- Renderer backend selection lives in the top-bar Settings menu rather than a dock.
- Console and Profiler share the bottom diagnostics region.
- The Profiler owns renderer capability diagnostics: active profile, adapter identity, explicit qualification level, queue/format decisions, advertised/enabled/implemented/exercised feature states, fallbacks, and accepted/rejected adapter candidates remain separate from global backend selection.
- Hierarchy filtering, create/delete actions, editable entity names, and Add Component are available where users expect them.

## Ownership Rule

Controls belong with the data that owns their effect. Backend choice is application-level and belongs in the top bar. A camera background color is scene-authored view state, belongs to `CameraComponent`, is serialized with that camera, and is edited in the camera Inspector. Future sky, fog, and global ambient controls belong to a scene environment asset or component, not application Settings; a camera may override that environment when the renderer supports the distinction.

This follows established editor ownership models: [Unity's Camera Inspector](https://docs.unity3d.com/current/Manual/class-Camera.html) exposes background color on the camera, [Unreal's Camera Component](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UCameraComponent) owns view and post-process overrides, and [Godot's Environment](https://docs.godotengine.org/en/stable/tutorials/3d/environment_and_post_processing.html) owns broader scene environment state with an optional camera override.

## Follow-up

- Add viewport selection when real scene geometry and a picking contract replace the current snapshot-driven prototype geometry. Transform gizmos no longer wait for picking: they act on the Hierarchy selection under `Architecture/EDITOR_UI_ARCHITECTURE.md`.
- Add component removal and duplication with the same undo contract, which is now the named, coalesced, byte-budgeted history of `Architecture/EDITOR_UI_ARCHITECTURE.md`.
- Add asset-specific Inspector views and Inspector locking when asset editing expands.
- Persist named workspace layouts. The Editor will own persistence through schema-versioned workspace files beside the other workspace-global editor settings instead of waiting for a shared settings format, with Reset Layout, panel close and reopen, and Dock Back.
- Split `EditorLayer` into panel-focused implementation units as selection and asset inspectors grow; the current single translation unit is already too large for unrelated UI work to scale cleanly. Decided 2026-10-09: the `Core`, `Commands`, `History`, `Layout`, `Panels`, `Viewport`, `Gizmo`, and `Smoke` directories behind `IEditorPanel`, `PanelRegistry`, and `CommandRegistry`.

## Amendments 2026-10-09

Made to record the Editor UI project-owner decisions in `Architecture/EDITOR_UI_ARCHITECTURE.md`; unrelated text is unchanged.

- Follow-up items rewritten in place: gizmos are decoupled from picking, component removal and duplication point at the named history contract, layout persistence is Editor-owned, and the `EditorLayer` split is decided.
- Findings from the 2026-10-09 read-only inventory, now tracked by `PLAN.md` slices: every Inspector drag frame records an entry labelled "Inspector edit" and the Inspector copies the whole document each frame it shows an entity; history is capped by a hard-coded 128 entries with no byte bound; Ctrl+Shift+Z undoes instead of redoing; undo shortcuts ignore a Fab page owning the keyboard; panels have no close button or reopen menu; the default dock layout is rebuilt on every launch; Tools items Compile Shaders and Build Motion Pack, the Console "Add Test Message" button, and View > ImGui Demo imply behavior or affordances that do not belong in product UI and are to be removed or honestly disabled.
- The implemented-direction list above describes the layout as of 2026-07-13; the bottom region gains a History tab and a status bar is added when those slices land, as recorded in `DESIGN.md`.
