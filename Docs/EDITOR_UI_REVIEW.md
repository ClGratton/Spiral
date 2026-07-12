# Editor UI Review

Date: 2026-07-12

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
- Inspector on the right contains only the selected entity's name, transform, and attached components. Camera controls appear only when a camera entity is selected.
- Renderer backend and clear-color controls live in the top-bar Settings menu rather than a dock.
- Console and Profiler share the bottom diagnostics region.
- Hierarchy filtering, create/delete actions, editable entity names, and Add Component are available where users expect them.

## Follow-up

- Add viewport selection and transform gizmos when scene entities replace the prototype render path.
- Add component removal and duplication with the same undo contract.
- Add asset-specific Inspector views and Inspector locking when asset editing expands.
- Persist named workspace layouts after the editor settings format exists.
- Split `EditorLayer` into panel-focused implementation units as selection and asset inspectors grow; the current single translation unit is already too large for unrelated UI work to scale cleanly.
