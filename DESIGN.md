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
- Top menu: global renderer backend selection.

## Interaction Contracts

- Inspector content follows the selected entity or asset.
- Camera projection and background color live on the selected camera in the Inspector. The top-bar Settings menu is limited to global renderer backend selection.
- Unsupported backends are visibly disabled and explain why.
- Destructive hierarchy actions are contextual, undoable, and protect the primary camera.
- New projects reject path collisions instead of silently overwriting existing manifests.
