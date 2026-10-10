# Editor UI Architecture

Status: Accepted architecture contract
Date: 2026-10-09

This contract records the project owner's 2026-10-09 decisions for turning the Editor from a single-translation-unit prototype into a full authoring tool: named and bounded undo/redo, a command surface, an in-house transform gizmo with world-grid-aware snapping, a flat-scene outliner, saved layouts and detachable panels, and the typed-control additions that let repository agents verify all of it without a mouse. It is the owning contract that `DESIGN.md` previously said gizmos, picking, and configurable bindings were waiting for. It does not implement anything: `PLAN.md` owns order and completion, and every behavior below is unchecked there until it is implemented, integrated, and verified.

## Authority And Scope

- `PLAN.md` owns slice order and checkboxes. Nothing here is an implementation order.
- `DESIGN.md`, `PRODUCT.md`, and `Docs/EDITOR_UI_REVIEW.md` were amended in the same change wherever a decision below conflicted with them; each file ends with an "Amendments 2026-10-09" note listing exactly what changed.
- `Editor/OWNERSHIP.md` owns the directory-level dependency rules that realize the module structure here. `AI_AUTOMATION_ARCHITECTURE.md` carries the matching amendment for the control mailbox.
- `FAB_ASSET_INTEGRATION.md` still owns the Fab panel, import controller, and project transaction. This contract only states how those interact with history, input, and detaching.
- The Editor remains a client of Engine: nothing here adds an Engine dependency on Editor code, and Engine additions are named as separate prerequisite slices.

## Evidence And Decision Levels

AGENTS.md requires four levels to stay separate. Each claim below carries one of these tags when its level is not obvious.

- **S - source claim.** Asserted by a source this contract did not re-run: the vendored Dear ImGui backend headers and changelogs, ImGuizmo's published license, and four read-only 2026-10-09 research packets (UI inventory, UI gap plan, history/gizmo design, multi-viewport feasibility spike). The packets' measurements and the spike were performed in a throwaway session scratchpad outside the repository; their raw artifacts are not durable, so the numbers that matter are transcribed below and must be treated as claims until a slice re-measures them in the repository.
- **I - inference.** This contract's reasoning from source claims or repository facts.
- **A - accepted decision.** A project-owner decision of 2026-10-09 (the owner said "do it fully" and delegated the details; the decisions are final for this work).
- **V - verified in repository.** Observed by this contract's author by reading current source or running a command in `/home/claudio/Spiral-Linux-Worktree` on 2026-10-09. Reading source is verification of what the code says, not of runtime behavior.

No runtime behavior in this document is verified at the time of writing: no Editor was launched, built, or driven, and no test was run. The only command run for this change is `Scripts/CheckCodeStyle.sh`.

### Verified current facts (V)

These repository facts motivate the decisions. Function names are the stable anchors; line numbers drift while other authors edit `EditorLayer.*`.

1. History is whole-document snapshots: `HistoryState` copies the Scene, AssetRegistry, MaterialLibrary, selection, five camera values, and the color-pipeline settings; `HistoryEntry` stores a full Before and After. The entry cap is the hard-coded integer 128, which also appears in many typed-control and smoke postconditions (`std::min<std::size_t>(UndoHistory.size() + 1, 128)`).
2. Inspector edits record the literal label `"Inspector edit"` from `DrawInspectorPanel`, and the Inspector captures history state before knowing whether anything changed.
3. Undo and redo are read as `ImGuiKey_Z` and `ImGuiKey_Y` polls in `OnUiRender`; there is no Ctrl+Shift+Z redo and no check for a Fab page owning the keyboard (`Fab::BrowserInputRouter` already exposes `OwnsKeyboard()` and an `EditorShortcuts` route flag for exactly this).
4. `ImGuiLayer::OnAttach` sets only `NavEnableKeyboard` and `DockingEnable`; `ViewportsEnable` is set nowhere in Engine or Editor. The vendored Vulkan backend picks secondary present modes in the order MAILBOX, IMMEDIATE, FIFO.
5. `Scene` is flat (`SceneEntity` holds a name, transform, and optional camera, light, and mesh renderer; there is no parent field), `SceneEntity` is a copyable value, and the only validated full-transform write is `Scene::SetEntityTransform(entity, SectorLocalPosition, rotationDegrees, scale)`. The default sector extent is 4096 world units with centered half-open double locals (`Engine/Math/WorldGrid.h`, `TryNormalizeSectorLocal`).
6. Special entities are re-found by name after a history restore (`FindEntityByName("Prototype Mesh")`, `"Directional Light"`, `"Player Start"` in the post-restore and project-load paths), so renaming or duplicating them would silently break the Editor's cached handles.
7. The Tools menu items Compile Shaders and Build Motion Pack only log "not implemented yet" messages, the Console has an "Add Test Message" button, and View has an "ImGui Demo" toggle: product UI that implies behavior that does not exist or is a development affordance. (Validate Project was in this class in the packets but is now wired to the Fab validator in the working tree, so it is not listed.)

### Packet measurements and spike results (S)

- Snapshot cost (`-O2` throwaway harness, Release-like; Debug unmeasured): about 410 bytes per entity for Scene plus registry plus materials at 0.5 registry assets per entity; one state is about 0.4 MB at 1,000 entities, 4 MB at 10,000, 20 MB at 50,000; the old 128-entry stack (256 states) is therefore about 100 MB, 1 GB, and 5 GB. A full copy costs about 0.07 ms, 0.6 ms, and 3 ms at those sizes. At a 256 MiB budget with two states per entry the packet estimated about 320, 32, and 6 retained entries.
- Multi-viewport spike (own Vulkan device, not NVRHI; vendored ImGui 1.92.9 docking and unmodified backends; RTX 3080 Ti; Hyprland 0.56.2; GLFW 3.4.0 built X11-only so it runs under XWayland; 200 Hz DP-3): secondary OS windows created, rendered a shared texture descriptor, resized, re-docked, and shut down cleanly with zero Vulkan error results; secondary windows are undecorated and the GLFW backend marks them `_NET_WM_WINDOW_TYPE_DIALOG` so Hyprland floats them; their X11 class is the placeholder "No Title Yet"; requested positions were clamped into the monitor; new windows mapped on the focused workspace rather than the parent's; the main X11 surface offered FIFO and IMMEDIATE only (no MAILBOX), so every secondary used IMMEDIATE; an IMMEDIATE secondary did not throttle the main loop; a FIFO secondary capped the whole application to its vsync (about 7,100 fps to 200 fps with an IMMEDIATE main) and, with its window on a workspace that was not displayed, blocked the whole process (killed by `timeout`, three attempts; process state sleeping, wait channel `drm_syncobj_array_wait`, zero CPU time over 1.5 s) while an IMMEDIATE hidden secondary ran 33,465 frames; 60 consecutive per-frame secondary resizes cost an average 3.1 ms and at most 3.9 ms per frame against 0.21 ms baseline on an idle GPU; window creation cost 30 to 46 ms on the main thread; secondary update plus render averaged 0.02 to 0.06 ms; submitting secondaries after the main present (the engine's order) worked. The blocking call was inferred (`vkAcquireNextImageKHR` with an unbounded timeout), not observed. No Vulkan validation layer is installed on this workstation, so the spike is not validation-clean evidence. One spike run briefly left its windows on DP-1 because a placement helper matched the wrong PID; its data are valid and it was not rerun.

## Accepted Decisions

| # | Decision | Supersedes |
| --- | --- | --- |
| D1 | History keeps whole-document snapshots as the one mechanism and layers names, gesture coalescing, nestable transactions, a byte budget, redo invalidation, and a Fab-commit barrier on it. | The hard-coded 128 count cap and the per-frame "Inspector edit" entries. |
| D2 | An in-house transform gizmo with world-grid-aware snapping; no ImGuizmo. | `DESIGN.md` "gizmos ... remain unavailable". |
| D3 | Detachable panels on Linux/Vulkan/X11 first, behind a present-mode guard, with the Scene Viewport and the Fab browser pinned to the main window. | The single-OS-window assumption of the navigation and Fab input contracts. |
| D4 | The Editor is split into purpose-named directories behind `IEditorPanel`/`PanelRegistry` and a single `CommandRegistry` action entry. | The single `EditorLayer.cpp` translation unit. |
| D5 | The flat outliner gets its authoring features now; entity parenting is a separate contract-first later item. | `EDITOR_UI_REVIEW.md` "hierarchy" follow-ups left open. |
| D6 | The new UI actions are admitted to the control mailbox as new schema versions after the in-flight Fab schema 4 lands; preferences live under `Settings > Editor Preferences`. | `Editor/OWNERSHIP.md` "not entity/component lifecycle authority" and the two-entry Settings split. |
| D7 | `DESIGN.md` and `PRODUCT.md` are amended where they conflict (listed under Product-Rule Resolutions). | The conflicting sentences, edited in place. |

## Module Structure

The target layout of `Editor/src` (A). The existing `Fab/` directory and `EditorMaterialControl.*` keep their locations and owners; this contract does not move them.

| Directory | Tier | Contents | May depend on |
| --- | --- | --- | --- |
| `Core/` | pure | `EditorSession` (document model: Scene, AssetRegistry, MaterialLibrary, project paths), `SelectionModel`, `HierarchyModel`, `LogBuffer`, notification/status model, `UiScale` arithmetic | standard C++, Engine headers |
| `Commands/` | pure | `CommandRegistry`, command ids and typed argument variants, `ShortcutMap` and `ResolveShortcut`, fuzzy matching for the palette | `Core/`, Engine headers |
| `History/` | pure | `HistoryStore<State>`, `HistoryLabel`, `EditGestureTracker`, the state-size estimator interface | standard C++, `Core/` |
| `Layout/` | pure codec plus one ImGui adapter | schema-versioned workspace/layout file codec, panel visibility and detach-policy tables; `LayoutManager` (ImGui ini blob, dock builder) is the only adapter file | `Core/`, panel descriptors |
| `Panels/` | ImGui | `PanelDescriptor` (a pure header), `IEditorPanel`, `PanelRegistry`, `PanelHost`, one file per panel, status bar, toast layer, command palette, shortcuts window, preferences window | everything below it |
| `Viewport/` | ImGui plus pure helpers | `ViewportNavigation` (extracted, behavior unchanged), toolbar, overlays, view modes; pure `ViewportToolState` and the absolute-to-viewport-relative coordinate helper | `Core/`, `Gizmo/` |
| `Gizmo/` | pure plus one adapter file | `GizmoVec`, `GizmoMath`, `SnapMath`, `TransformGizmo` state machine; `GizmoDraw.cpp` is the ImGui adapter | Engine `Math`/`Scene` headers |
| `Smoke/` | Editor-only | the `Run*Smoke` and control-smoke definitions moved out of `EditorLayer.cpp` | everything |
| `EditorLayer.{h,cpp}` | composition root | owns the session, history, command registry, panel registry, layout manager; forwards attach/update/UI/event hooks | everything |

Rules, enforced by `Editor/OWNERSHIP.md` and by the pure-tier include discipline already used for `Editor/src/Fab`:

- **Pure tier** files include only standard C++, `Engine/Core/Base.h`, and Engine value-type headers (`Math`, `Scene`, `Assets` data types). No ImGui, GLFW, CEF, RHI, or native-graphics include, no engine thread, no global state. They are compiled into `EngineTests` (`Tests/premake5.lua`) and exercised without a window. A file added to the pure tier is added to that list in the same change.
- **Adapters** are the only files that include ImGui or GLFW. They are not compiled into `EngineTests`.
- Panels never include `EditorLayer.h` and never include one another; they receive a narrow `PanelContext` of references (session, selection, history and command registry, notifications, UI scale, a read-only renderer-diagnostics view).
- `Smoke/` code is never reachable from the pure tier. `EditorLayer.cpp` carries hooks and composition only, following the pattern the Fab integration already adopted with `EditorLayerFab.cpp`.
- One C++ namespace (`SpiralEditor`) is used for the new code instead of the current global namespace.

## Panel And Command Contracts

### IEditorPanel and PanelRegistry

- `PanelDescriptor` (pure data): permanent ASCII `Id` (for example `scene.hierarchy`), `Title`, `Category`, `DefaultDock` (LeftTop, LeftBottom, Center, Right, Bottom, Floating), `DefaultVisible`, `Closable`, `DetachPolicy` (`Allowed` or `Pinned`), `OwnsWindow` (true for legacy self-windowed panels such as the Fab panels), and a toggle command id. The ImGui window name is `"Title###id"`, so titles can change without invalidating saved layouts.
- `IEditorPanel` (small on purpose): `Descriptor()`, `OnAttach`, `OnDetach`, `OnUpdate` (every frame, even hidden), `OnDraw` (contents only), `WantsKeyboard()`, and `OnEvent(Engine::Event&)` for panels that consume main-window events. `PanelHost` supplies `Begin`/`End`, the close control, the window class, and the header menu (Close, Dock Back, Detach to window, Reset Position). Only `PanelHost` calls `ImGui::Begin` for a registered panel.
- `PanelRegistry` owns panels by id, drives updates and draws, stores visibility, and enumerates panels for the Window menu, the command palette, workspaces, and the typed `DetachPanel`/`DockPanel`/`ListViewports` actions. A `LegacyDrawPanel` adapter wraps the Fab panels, which draw their own windows, so they join the registry without being rewritten.
- `DetachPolicy = Pinned` is set for the **Scene Viewport** and the **Fab browser panel** (A). Reason (V/S): both consume raw main-window `Engine::Event`s, window-relative cursor coordinates, and (for the viewport) main-window cursor capture, and the GLFW backend never chains engine callbacks for secondary windows. A pinned panel must not be hosted by a secondary OS window by any route: mouse drag, saved layout, or typed `DetachPanel`. The mechanism that enforces this (a window class, dock-node flag, or equivalent) is unverified (S: whether `ImGuiDockNodeFlags_NoUndocking` pins as intended was not exercised) and is the first deliverable of the multi-viewport slice, spiked before it is relied on.

### CommandRegistry

One registry is the single action entry (A). Every user-visible action is a command `{Id, Title, Category, DefaultChord, IsEnabled(context) -> {enabled, reason}, Execute(typed args) -> Result}`. Menus, context menus, toolbars, keyboard shortcuts, the command palette, panel detach and dock, workspaces, and the typed control actions all call `Execute`; nothing else contains action logic. Consequences:

- A disabled command always carries a human-readable reason (`PRODUCT.md` principle 4). Menus show the reason instead of silently greying out.
- Arguments are a closed variant per command; there is no string-to-code dispatch. Typed control actions each map to exactly one registered command with typed arguments and never accept a command-id string, so the mailbox gains fixed actions, not arbitrary dispatch.
- Commands run on the Editor main thread, return a result carrying the history revision they produced, and mutate only through the normal Scene, MaterialLibrary, history, and Renderer authorities.
- Commands that mutate an entity enforce the session-only lock (below) in one place, so UI and typed callers agree.

### Shortcuts

`ResolveShortcut(chord, ShortcutContext) -> optional<command>` is a pure decision table. The context is latched once per UI frame, the same latch convention `DESIGN.md` already uses for viewport navigation focus. Context bits: text input wanted, browser owns keyboard (`BrowserInputRouter::OwnsKeyboard()`, that is `route.EditorShortcuts == false`), any item being dragged, history gesture open, modal or popup open, viewport focused, navigation captured, and application focus. Initial default chords (changeable only by amending this table; user-configurable bindings remain a later contract):

| Chord | Command | Requires |
| --- | --- | --- |
| Ctrl+Z | Undo | none of: text input, drag, gesture open, modal, browser owns keyboard |
| Ctrl+Y and Ctrl+Shift+Z | Redo | same as Undo |
| Q, W, E, R | Select (no tool), Translate, Rotate, Scale | viewport focused, not navigation-captured, no Ctrl/Alt |
| X | Toggle World/Local | same as tool keys |
| Esc | Cancel active gizmo drag | a drag in progress |
| F2, Delete, Ctrl+D, Ctrl+C, Ctrl+V | Rename, Delete, Duplicate, Copy, Paste selection | outliner or viewport focus, not typing |
| Ctrl+Shift+P | Command palette | not typing |

Tool keys therefore never fight the Unreal-preset RMB fly keys (which require a held mouse button, a navigation-captured state), and Fusion, which has no fly keys, keeps W/E/R free. Undo and redo do not auto-repeat in v1 (each restore copies and republishes registries). Because `OnEvent` (GLFW phase) and `OnUiRender` (ImGui phase) are different phases, both consult the same latched context; the browser-owns-keyboard flag is part of it, not a separate check.

## History Model

### Delivered API supersedes the sketch (2026-10-10, verified in repo)

`Editor/src/History/HistoryStore.h` is the authority for signatures. Deviations from the sketch below: restore lives on an `IHistoryStateAdapter<State>` (Restore, EstimateBytes, Equal) instead of a per-call function; `Reset` and `Barrier` take no state, and `Barrier` takes the disabled-reason text; marks are `SaveMark`/`LoadMark`; the store adds `JumpToRow`, `UndoAvailability`/`RedoAvailability` for menu text, an announcement string on every result, never-reused revisions (`HeadRevision()` is safe for typed-control compare-and-swap), and gestures and transactions are mutually exclusive in v1. The Editor consumes it through `Editor/src/EditorHistoryState.*`, with names from `Editor/src/History/HistoryNaming.*`.

### Mechanism (A)

Whole-document snapshots stay the storage mechanism; the entry model changes, the storage model does not. Command objects were rejected for now (see Rejected Alternatives).

```
HistoryLabel { Verb, Target, Source in {User, Agent, System} }   // display: "Move Cube"
HistoryStore<State>  (pure template; tests need no Scene)
  Record(label, before, after)                 // one-shot entry
  BeginGesture(key, label, before) / UpdateGesture(refined label)
  EndGesture(after) -> Recorded | NoChange | NoGesture ;  CancelGesture() -> before
  BeginTransaction(label, before) / EndTransaction(after) / AbortTransaction()   // nestable
  Undo / Redo / JumpTo(revision)               // each through a caller RestoreFn; cursor unmoved if it fails
  Reset(baseLabel, base)                       // new or loaded project
  Barrier(label, postState)                    // drops BOTH stacks
  observation: HeadRevision, UndoDepth, RedoDepth, TopUndo, TopRedo, GestureOpen, UsedBytes, Evicted, Rows
  Mark Save() / Load(Mark)                     // pointer copy for typed-control rollback
```

States are immutable `shared_ptr<const State>`, so undo, redo, and jump copy no entries and typed-control rollback copies pointers instead of vectors. The model is a base row (project opened, barrier, or "earlier history dropped") followed by entries with a cursor; restoring target cursor `t` uses `entries[t].Before` when `t < cursor`, else `entries[t-1].After` (the base state when `t == 0`), which is exactly today's semantics.

### Rules (A)

1. **Names.** Every entry has a verb plus target, never "Inspector edit". Verbs: Move, Rotate, Scale, Rename, Create, Delete, Duplicate, Add `<Component>`, Remove `<Component>`, Edit Light, Edit Camera, Edit Mesh Renderer, Edit Material, Edit Color Pipeline, Import Fab Asset. The target is the entity name at edit time truncated to 32 characters, "N Entities" for a multi-selection, the asset name for materials, and "Project" for project-level state. Agent-originated entries use the same verbs with `Source = Agent`, shown as text (never color alone).
2. **Gestures.** A continuous edit is Begin (state captured before the first mutation), N in-place updates, End (one entry). If an injected equality predicate says before equals after, End records nothing. Inspector widgets use ImGui `IsItemActivated`/`IsItemDeactivatedAfterEdit`, which already act on whole composite widgets; the existing project color-pipeline gesture is the in-repo precedent being generalized. Discrete widgets (checkbox, combo, Add Component) that report an edit with no open gesture are Begin+End in one frame. A text field is one gesture per focus session. Because a drag widget has already written its value on its activation frame, the Inspector must edit a local copy and apply it to the Scene after the Before state is captured. A gesture whose item stops being submitted (panel closed, selection changed, entity deleted, focus lost, project switch) is ended at the end of the UI frame when no item is active, never left open across a project switch. A gizmo drag is a gesture (see Transform Gizmo).
3. **Transactions.** Multi-step actions (create plus assign mesh, Fab placement, group transforms) open a nestable transaction; exactly one entry is recorded at the outermost close with the outermost label, and abort restores the captured Before state.
4. **Redo invalidation.** Recording at a cursor below the top truncates the redo tail and announces "Redo history discarded (N)".
5. **Selection** changes are not undo entries. Each entry restores the selection recorded with it.
6. **Camera.** Undo and redo never move the viewport navigation camera unless the entry itself edited the camera (a typed or Inspector edit of the main camera). The main camera is an entity inside the Scene and navigation writes its transform without history today; the restore path must therefore overlay the current navigation pose on the restored Scene for every entry that did not edit the camera. The overlay mechanism is chosen in the history-integration slice; the invariant is fixed here.
7. **Budget.** Default 256 MiB over the sum of distinct state bytes (shared states counted once) and an entry cap of 512 (the cap only bounds the History panel list). After each record the oldest entries are evicted until under budget, always keeping the newest entry; the oldest survivor's Before becomes the new base row, labelled "Earlier history dropped". Eviction raises one visible notice per burst (toast, Console line, status bar, and a banner in the History panel: "37 oldest entries were dropped to stay within 256 MiB"). The budget is a named constant; surfacing it in `Editor Preferences` is allowed without amending this contract if it is range-clamped.
8. **Barriers.** A successful Fab commit, and a post-commit recovery-required reload, is an **undo barrier** that clears both stacks: the on-disk manifest, cooked generation, and receipts have moved forward and an older registry snapshot must never be restorable. The base row reads "Import Fab Asset (barrier)" and Undo shows "Cannot undo past: Import Fab Asset - project changes were committed to disk". If the commit included a scene assignment it is inside the same barrier; a later user placement is an ordinary entry. A commit that fails before the manifest pointer, or is cancelled, changes no history. Commit and import controls are disabled in the UI, and rejected `history_gesture_open` over typed control, while a gesture is open. `LoadProject` and `CreateNewProject` use `Reset` (the current `LoadProject` does not clear history, so Ctrl+Z after opening another project restores the previous project's snapshot: a defect this contract fixes). The legacy glTF drop import is recorded as a barrier until the Fab intake replaces it. Save is not a barrier.
9. **Notifications.** Every undo, redo, jump, and record is announced in the status bar and the Console ("Undo: Move Cube", "Redo: Move Cube", "Jumped to: Create Entity 2 (3 steps back)", "Nothing to undo", "Redo history discarded (3)"). The last announcement is also part of the typed history receipt.
10. **Delta payloads are designed for, not built.** A typed delta entry kind (entity transform, name, property) behind the same `HistoryEntry` interface is admitted only when a measurement shows the 256 MiB budget retaining too few entries at realistic scene sizes (the packet's estimate: about 32 at 10,000 entities). De-duplicating `After(k)` and `Before(k+1)` by content fingerprint, or sharing unchanged domains, are likewise measurement-gated optimizations.

### UI (A)

- **Edit menu** (new top-level menu between File and View; Undo/Redo leave File): `Undo <name>  Ctrl+Z`, `Redo <name>  Ctrl+Y`, History panel toggle, `Clear History...` (confirmation modal). A disabled item states why: "Undo (nothing to undo)", "Undo (finish the current edit)", "Undo (blocked: Import Fab Asset)".
- **History panel** (dockable, default in the bottom tab group with Console and Profiler): columns marker, name, source text, approximate size, revision; a base row, applied entries, a current-state marker (selection styling plus a leading `>`), then undone entries dimmed with an "(undone)" suffix. Click jumps (one restore, not N). Disabled while a gesture is open. The header shows entry count and budget use and the eviction banner.
- **Status bar** (new): a strip at the bottom edge of the main window, not a dockable panel and not detachable. It shows the last announcement, selection summary, active backend, and busy or import state, with no animation.

### Typed parity

See Typed-Control Additions: `InspectHistory`, `UndoHistory`, `RedoHistory`. Postconditions use `HeadRevision` and cursor instead of the hard-coded 128.

## Selection, Outliner, And Hierarchy

The scene is flat today (V). Decisions (A):

- Outliner features come first: multi-select (click, Ctrl, Shift-range over the visible filtered order, select-all) with a primary entity that the Inspector and gizmo use, in-place rename (F2 and double-click), duplicate, delete (the main camera stays protected), copy/cut/paste within the scene, visibility, lock, search/filter, create submenu (Empty, Camera, Light, Mesh), and context menus. Multi-select primary versus secondary differs by outline or marker, not hue alone.
- **Visibility** is the existing `MeshRendererComponent::Visible` flag, shown only for entities that have a MeshRenderer. No per-entity enabled flag is added, because that is a Scene format change.
- **Lock** is session-only editor state: not in the Scene, not in history, not saved, cleared on project load, labelled "Lock (this session)". Locked entities refuse Inspector, gizmo, rename, duplicate-over, and delete commands with the reason `entity_locked`; they can still be selected.
- `SelectionModel` (ordered set, primary, anchor, stale-ID pruning) becomes part of history state. `HierarchyModel` owns the naming policy ("Entity", "Entity (2)", copy suffixes, collisions) and the protection rules.
- Before rename or duplicate, the name-based special-entity lookup after restore (V fact 6) is replaced by stable entity identity.
- Typed delete/duplicate undo and paste need a public `Scene` API to re-insert an entity with its original `EntityId` and to clone with a new id; this is a separate Engine prerequisite slice. Until it lands those operations are recorded as snapshot entries.
- **Entity parenting** is a separate, later, contract-first item: scene format version 6 with a deterministic migration from 5, cycle rejection, delete-with-children policy, transform composition at world-grid precision, render-snapshot extraction from world matrices, and effects on copy/paste, picking, and the outliner tree. It needs its own architecture contract and a `PLAN.md` entry before any code and is not started by this work.

## Transform Gizmo And Snapping

### Facts that constrain it

- Transforms are not floats. `TransformComponent` keeps a canonical `SectorLocalPosition` (signed 64-bit sector plus a double local in a centered half-open range of the sector extent, default 4096), `RotationDegrees` (Euler), and `Scale` (V). `Scene::SetEntityTransform` requires a canonical position, finite rotation, strictly positive scale, and unit scale for camera-bearing entities (S: packet-read). The Inspector's existing position path is lossy at large sector indices (`ApproximateWorldPosition` plus per-axis set) and the gizmo must not copy it.
- `Engine::Math` has no inverse, dot/cross, quaternion, or Euler extraction (S: packet-read); matrices are row-major, row-vector, left-handed, depth 0..1. The gizmo therefore carries its own tiny vector helpers.
- There is no viewport picking and no mesh-bounds API, so the gizmo acts on the **selection** (outliner), not on a clicked object.

### Decision: in-house, not ImGuizmo (A)

ImGuizmo is MIT-licensed (S) but works on one float 4x4 object matrix, so absolute positions would be fed as floats (wrong far from the origin) or faked with camera-relative matrices and decomposed back; sector-local carry and snapping would remain ours either way; its global ImGui-bound state cannot be unit-tested under `TESTING_STRATEGY.md`; and it would add a pinned dependency to `Docs/DEPENDENCIES.md`. The in-house gizmo is a pure core (picking, drag solve, snapping, Euler recomposition) plus an ImGui draw-list adapter that is the only throwaway part; if schedule pressure ever favors ImGuizmo it could replace only the draw and pointer layer. The renderer's debug overlay was rejected as the gizmo surface: it is post-tone-map, capped at 12 segments, published per settings generation, and has no hover or active state.

### Behavior (A)

- **Tools and keys:** Q none, W translate, E rotate, R scale, X toggles World/Local (flat scene: Local means the entity's own rotated axes, there is no parent space). No gizmo is shown for the main camera entity (it is the viewport eye) or when nothing is selected; the scale tool is hidden for camera-bearing entities (`DESIGN.md` omits their Scale control). Pivot v1: Origin and Selection Centre; Bounds Centre is shown disabled with its reason until a bounds API exists.
- **Snapping is OFF by default.** Defaults when no saved state exists: translate step 1.0 world unit, rotate 15 degrees, scale step 0.1. Holding Ctrl during a drag **inverts** the toggle (on: Ctrl suspends snapping; off: Ctrl enables it). The state is shown as icon plus text ("Snap: On"), never color alone. Presets: translate 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10; rotate 1, 5, 10, 15, 30, 45, 90.
- **Application of a drag:** always recompute from the drag-start transform plus the total delta (never accumulate), solve in camera-relative floats, apply as a double delta to the sector-local position, snap, normalize with `TryNormalizeSectorLocal`, then `Scene::SetEntityTransform`. A rejected update keeps the last valid transform. Gizmo origin uses `TryGetSectorLocalRelativePosition` against the view's canonical translation origin when present, so precision holds at huge sector indices.
- **Grid snapping is aware of the world-grid authority:** the translate lattice is anchored at each sector's center and only constrained axes snap (`local' = floor(local/step + 0.5) * step`, ties toward positive infinity; unconstrained axes are bit-identical). It coincides with the absolute world lattice exactly when the sector extent divided by the step is an integer (all power-of-two steps; the decimal presets within a 1e-9 relative tolerance for extent 4096); otherwise the snap popover says "grid restarts at sector boundaries" instead of claiming absolute alignment. An absolute-phase variant would need exact modular arithmetic on the sector index and is deferred. A snapped local equal to the positive half-extent is not canonical and carries into the next sector; near the signed 64-bit sector limits normalization fails and the gizmo rejects the update. Local-space translation snaps the axis distance as a relative increment from the drag start (the UI says so).
- **Rotation snap** rounds the drag angle relative to the drag start (not the resulting Euler triple). With row-vector composition, rotating about a world axis is `R1 = R0 * Rw(theta)` and about a local axis `R1 = Rl(theta) * R0`, then Euler is extracted choosing the branch nearest the previous Euler (no 360-degree jumps), with a documented gimbal rule at 90 degrees pitch. Correctness is checked by recomposing and comparing matrices, not Euler triples (I: the composition identity is to be proven by the oracle test). **Scale snap** is a relative factor `1 + round((factor-1)/step)*step` clamped to the Inspector's `[0.01, 100]` range.
- **Constant screen size:** world scale `viewDepth * 2*tan(fovY/2) * (targetPixels / viewportHeightPx)` with a target near 96 px scaled by font size, depth clamped to the near plane. **Hit testing is 2D** (distance to the projected segment or polyline within about 6 px, 8 px for caps, point-in-quad for plane handles; ties by smaller view depth), independent of mesh geometry. Initial pixel constants are tunable by the slice. The overlay draws into the viewport window's draw list clipped to the image rectangle after `ImGui::Image`; the displayed image can lag the UI by the renderer's frames in flight (unmeasured).
- **Colors:** axis hues are allowed on gizmo handles only, desaturated and distinct from the selection blues, each with a dark outline and an X/Y/Z letter at the tip; hover uses the selection-hover token and the active handle the docking-preview token with a numeric readout ("+1.50", "+15 deg", "x1.20"). The contrast of the dark hover blue on dark scenes must be checked on a real capture.
- **History:** press on a hovered handle begins a gesture with the verb and target; release ends it as exactly one entry; Esc, or losing OS-window focus mid-drag, cancels the gesture and re-applies the start transform exactly with no entry; right-click is not a cancel (it is Unreal navigation). Undo, redo, and jump are disabled during a drag.
- **Input arbitration:** the UI phase latches hover and active state; the next frame's `OnEvent` skips Unreal-preset LMB navigation capture while the pointer was over a handle. Fusion has no LMB conflict.
- **Explicit later items, shown disabled with their reason until their prerequisites exist:** surface, vertex, and bounds snapping, viewport ray picking, mesh-bounds picking, and bounds-centre pivot. Multi-selection group transforms (translate the same snapped delta; rotate and scale members about the selection centre; one "Move 3 Entities" entry) follow single-selection gizmo acceptance.
- **Persistence:** `ViewportToolState` (tool, space, pivot, snap toggle, three steps) is workspace-global viewport-authoring state kept in the Editor preferences file, never in `.spiralproject`. Typed `SetTransformToolState` changes are session-only and do not rewrite that file.

## Viewport Contract Changes

- The image rectangle must be stored relative to the owning ImGui viewport's position (`ImGui::GetWindowViewport()->Pos`), because enabling ImGui viewports turns all ImGui screen coordinates into desktop-absolute values while engine cursor events stay window-relative. Without this, Fusion zoom-to-cursor breaks even when the Viewport is docked in the main window (S: packet-read, I). This fix lands before multi-viewport is enabled.
- A viewport toolbar overlay hosts tool buttons, space and pivot toggles, the snap toggle with its popover, view modes (moved from the View menu), camera speed (disabled with its reason in Fusion, where fly keys and RMB-wheel speed do not navigate), and a stats toggle. It is a view of `ViewportToolState` and the existing navigation preset, not a second authority; the navigation preset stays in `Settings > Engine Settings`. Widgets over the image clear the viewport-hover state used for navigation and gizmo picking.
- Picking, bounds-based framing, custom or reset pivot UI, orthographic controls, camera piloting, and configurable bindings stay unavailable until their own contracts exist. A viewport ray-picking contract (Engine ray/AABB math, CPU mesh-bounds query, nearest-hit tie-break; AABB picking selects by box, not triangle) is an explicit later item and does not block the gizmo.

## Layout, Workspaces, And Detachable Panels

### Layout and workspaces (A)

- Today `m_ResetDockLayout` is initialized true (V), so the default dock tree is rebuilt on every launch and ImGui's `imgui.ini` autosave is effectively defeated (I). The Editor takes over persistence: `io.IniFilename = nullptr`, layouts saved as schema-versioned workspace files beside the other workspace-global editor settings, written atomically, failing closed to the DESIGN default layout when missing, corrupt, or from an unknown version (and saying so in the status bar).
- Workspaces are layout files plus **Reset Layout**, which always returns the `DESIGN.md` default geography. Every panel gets a close button, a Window menu entry that reopens it, and a Dock Back control.
- **Detached OS windows are not restored on launch** unless the saved workspace explicitly opts in; otherwise a panel saved as detached returns docked or floating inside the main window. Reason: the headed-launch acceptance contract assumes one stable `Spiral Editor` window on DP-3/workspace 2.

### Multi-viewport platform and backend matrix (A, with S/I evidence)

| Platform / backend | v1 status | Basis |
| --- | --- | --- |
| Linux, GLFW X11 (XWayland), Vulkan | first target; enabled only when every guard below passes | spike on this workstation (S) |
| Linux, native Wayland GLFW | visibly disabled with the reason | the ImGui GLFW backend turns viewports off under Wayland (S); this repository's GLFW is X11-only today (V: `Vendor/GLFW` defines only the X11 platform) |
| Windows, D3D12 | visibly disabled; later gated item | source-feasible (S), no Windows host, unverified |
| OpenGL2 fallback | visibly disabled | would need GL context backup/restore around platform-window rendering |
| macOS / MoltenVK, other | disabled | not assessed |

Enablement happens in `ImGuiLayer::OnAttach` and only when all hold: the native renderer is Vulkan, `glfwGetPlatform()` reports X11, the **main surface advertises MAILBOX or IMMEDIATE**, and the run is not headless. When any condition fails the feature is off and the panel header menu, Window menu, and Engine Settings state the reason ("Detach unavailable: Direct3D 12 backend", "... native Wayland", "... surface offers only FIFO"). After the main render and present, the main thread calls `UpdatePlatformWindows` and `RenderPlatformWindowsDefault` (the engine presents the main window inside `RenderImGuiDrawData`, so secondaries run after it; the spike used that order successfully). All ImGui submits stay on the one thread that drives rendering because ImGui calls `vkQueueSubmit`/`vkQueuePresentKHR` on the NVRHI-owned graphics queue without NVRHI's queue mutex (S; whether any NVRHI submit can run off the main thread was not audited).

### Hazards and mitigations

| ID | Hazard | Evidence | Mitigation (A unless marked) |
| --- | --- | --- | --- |
| H1 | A FIFO secondary swapchain blocks the whole process in an unbounded `vkAcquireNextImageKHR` when its window is not visible, and with an IMMEDIATE main it caps the whole app at the secondary's vsync. | spike (S): hung three times, killed by timeout; 7,100 to 200 fps | Never enable multi-viewport unless the surface offers MAILBOX or IMMEDIATE; the vendored backend then selects MAILBOX before IMMEDIATE and FIFO only as a last resort. Record the actual secondary present mode per viewport as a diagnostic, and (I) close or dock any secondary that reports FIFO and raise a notice. MAILBOX is not offered on this workstation's X11 surfaces; the upstream backend carries a FIXME that MAILBOX halves frame rate with a second window on some drivers, so any MAILBOX run needs its own measurement. An engine-owned bounded-acquire replacement of the backend's secondary render function is the documented fallback and is not built speculatively. |
| H2 | UI-texture retirement ignores secondary submissions: `PollCompletedPresentationSerial` knows only the main swapchain's fences, so a texture sampled by a detached window can be freed while that window's GPU work still reads it. | source (S) | After platform-window rendering, read each secondary viewport's frame fence (`ImGui_ImplVulkanH_GetWindowDataFromViewport`), assign serials from the same submitted-serial counter, and poll them like the main fences. D3D12 has no public accessor (its viewport data is file-local) and needs a wrapper or a conservative whole-queue fence in its own later slice. |
| H3 | The Scene Viewport and the Fab panel consume engine events and window-relative cursor coordinates, but under `ViewportsEnable` ImGui positions are absolute and secondary windows never feed the engine event dispatcher (the engine callbacks are chained only for the main window and `CallbacksChainForAllWindows` must stay false, because the engine's lambdas dereference a window-data pointer secondary windows do not have). `m_WindowFocused` also goes false when a detached panel takes OS focus. | source (S) | Pin both panels to the main window (decision D3). Make the image rectangle viewport-relative before enabling. Read shortcuts from ImGui key state so they work in any OS window. Redefine `m_WindowFocused` as "an engine-owned OS window has focus" using ImGui's application-focus state. File drops are main-window only, so dropping files on a detached Content Browser is not delivered. |
| H4 | Compositor-negotiated placement: Hyprland clamps floating windows into a monitor, opens new windows on the focused workspace rather than the parent's, and the X11 class of secondary windows is a placeholder, so class-based window rules cannot select them. | spike (S) | Typed `DetachPanel` returns the compositor-reported geometry, never assumes the request. The headed-launch helper verifies and corrects every window of the Editor PID, and during an agent-driven headed gate a detached panel must stay on DP-3/workspace 2 (AGENTS.md headed-launch rule; the helper change is a separate slice). Restoring detached windows is opt-in for the same reason. |
| H5 | Frame-pacing and latency contracts assume one swapchain. | source (S) | See Pacing And Latency Non-Claim. |
| H6 | `PlatformRequestClose` only closes windows that pass `p_open` to `Begin`, and every Editor panel currently calls `ImGui::Begin("Name")` without it, so closing a detached window does nothing; detached windows are undecorated. | source (S) | `PanelHost` passes `p_open`, every panel has a close button, a Window menu entry, and a Dock Back control, and Reset Layout recovers a lost panel. |

Further invariants the multi-viewport slice must preserve (S, each to be re-checked there): the vendored Vulkan backend's `SetMinImageCount` asserts on a changed value, so the engine's constant minimum image count must stay constant; the descriptor pool needs no growth for viewports (secondaries allocate no descriptors); shutdown order is `ShutdownImGui`, device idle, release viewport output, `ImGui_ImplVulkan_Shutdown`, then `ImGui_ImplGlfw_Shutdown`; the GLFW backend sets visible/focused/focus-on-show window hints for secondaries and never resets them, so any future second engine-created window inherits them; every secondary create, resize, and destroy calls `vkDeviceWaitIdle`; the Application skips ImGui begin/end while minimized, which would freeze detached panels, so viewports must keep ticking while any secondary is visible (not verified; Hyprland has no iconify); mixed-DPI behavior is unverified.

### Window style (A)

Panels hosted in secondary OS windows use a window radius of 0 and an opaque window background (ImGui's guidance for platform windows, which are rectangular); in-application panels keep the `DESIGN.md` 3 px radius. Applied per window by `PanelHost`. Visual quality (corner artifacts, flicker on creating an IMMEDIATE secondary) is a human gate.

## Pacing And Latency Non-Claim

Frame pacing, present cadence, and latency evidence are valid **only with zero secondary viewports** (A). The renderer's timing structures hold one swapchain generation, one present duration, and one frame index; secondary acquire, submit, and present happen after the recorded `RenderSubmission`/`PresentBegin`/`PresentEnd` phases and are active CPU work inside the frame that is never classified as pacing, acquire, or present (S). Any benchmark, headed pacing run, or latency capture that has a secondary viewport open is not attributable and must not be cited as pacing evidence. A mixed run must publish diagnostics (`SecondaryViewportCount`, secondary render and swap CPU milliseconds, per-secondary present mode, per-secondary acquire milliseconds) so its cost is visible, and the AGENTS.md rule that one frame ID is carried through start, input, submission, `Present`, GPU completion, and display feedback is unchanged. The first demo-scene benchmark runs with detach disabled or no panel detached.

## Typed-Control Additions

Repository agents may not inject mouse or keyboard input to mutate parameters (AGENTS.md); every new UI behavior therefore needs a typed route (A). The fixed set below is the maximum admitted by this contract; any addition requires amending this section. The additions land as new control-mailbox schema versions **after the in-flight Fab schema 4 lands**, and are delivered in two groups so that history, gizmo, and outliner verification does not wait for multi-viewport work: group 1 (history, transform-tool, outliner lifecycle) and group 2 (viewports). Each action keeps the established pattern: exact session and project identity, exact field masks, complete-value compare-and-swap preconditions, domain validation, main-thread commit through the normal authorities, one history entry (`Source = Agent`) per successful document mutation, rollback, bounded terminal receipts (at most 64 KiB, at most 32 sampled affected stable IDs), and an explicit session-only/not-saved result.

| Action | Group | Purpose and notable fields |
| --- | --- | --- |
| `InspectHistory` | 1 | Read-only. Receipt block: `HeadRevision`, cursor, `UndoDepth`, `RedoDepth`, `TopUndoDisplay`, `TopRedoDisplay` (exactly what the Edit menu shows), `BaseDisplay`, `BaseIsBarrier`, `BudgetBytes`, `UsedBytes`, evicted entries and bytes, `GestureOpen`, `LastAnnouncement`, and the newest 16 rows `{Revision, Display, Source, Barrier, Bytes, Applied}`. |
| `UndoHistory`, `RedoHistory` | 1 | Fields `ExpectedHistoryRevision`, `ExpectedTopDisplay`, `Steps` (1 to 32). Same `Undo`/`Redo`/`JumpTo` path as the UI; they record no entry. Reject codes: `history_revision_mismatch`, `top_entry_mismatch`, `nothing_to_undo`, `nothing_to_redo`, `undo_barrier`, `gesture_in_progress`, `restore_failed`. Rollback after a failed response publish is `Load(Mark)`. |
| `SetTransformToolState` | 1 | Session-only `Tool`, `Space`, `Pivot`, `SnapEnabled`, `TranslateStep`, `RotateStepDegrees`, `ScaleStep` with `Expected...` compare-and-swap and an echo of the effective state. Establishes verification state without UI injection. |
| `ApplyGizmoOperation` | 1 | Records exactly one history entry through the same solver, snapper, `SetEntityTransform`, and gesture code the mouse uses: `EntityId`/`ExpectedEntityName`, `Mode`, `Space`, axis mask (X, Y, Z, XY, YZ, XZ, Uniform), `Amount`, `SnapEnabled`, `ExpectedTransform`. Receipt echoes the effective snapped amount, the resulting sector and local position, and the history revision and display name. It exercises everything below the pointer-to-ray step. |
| `CreateEntity`, `DuplicateEntities`, `DeleteEntities`, `RenameEntity`, `SetSelection`, `SetEntityLock` | 1 | The outliner lifecycle actions, each mapped to its registered command and enforcing the same protections (main camera cannot be deleted, `entity_locked`). They are fixed named actions, not a generic entity/component dispatcher; add/remove component and asset assignment stay on the existing and Inspector paths until a consumer justifies more. |
| `ListViewports` | 2 | Read-only: for every viewport its id, hosted panel ids, ImGui geometry, platform-reported geometry, monitor and workspace when the platform can report them, whether it is the main viewport, and its secondary present mode. |
| `DetachPanel`, `DockPanel` | 2 | `DetachPanel{PanelId, X, Y, Width, Height}` is the typed equivalent of the drag-out (`SetNextWindowDockID(0)` plus position and size, the sequence the spike exercised); `DockPanel{PanelId}` returns it to its dock home. Receipts carry the **actual** compositor-reported geometry. Reject codes include `panel_unknown`, `panel_pinned` (Scene Viewport, Fab browser), `viewports_unavailable` (with the reason string), and `panel_not_detached`. |

Because the physical drag-out is itself the behavior under test for detaching, it is a user-performed manual gate; `DetachPanel`, `DockPanel`, and `ListViewports` verify everything underneath it.

`Editor/OWNERSHIP.md` and `AI_AUTOMATION_ARCHITECTURE.md` carry the matching scope amendments. The mailbox stays opt-in, Editor-owned verification tooling: no arbitrary dispatch, no command-id strings, no network service, no implicit save authority, and no substitute for the future model-neutral Automation boundary.

## Editor Preferences And Persistence

`Settings > Editor Preferences` is a third entry beside Project Settings (serialized project frame pacing) and Engine Settings (global renderer backend and the viewport-navigation preset). It owns workspace-global, non-serialized, non-project editor state: UI scale, the opt-in to restore detached windows, shortcut display and (later) bindings, autosave when it exists, and optionally the history budget. It does not duplicate controls owned elsewhere: the viewport toolbar edits `ViewportToolState`, and Engine Settings keeps the navigation preset. These files live beside `engine-settings.spiralsettings`, are schema-versioned, written atomically through the shared atomic-file helper, fail closed to defaults on any unknown, malformed, or unreadable content without modifying the source file, and never enter `.spiralproject`. New state goes into new sibling files rather than migrating the existing positional `SpiralEditorSettings` v1 format, so the navigation-preset file is untouched.

## Product-Rule Resolutions

| Conflict | Resolution (A) |
| --- | --- |
| `DESIGN.md`: gizmos and bindings "remain unavailable until their owning contracts exist" | This contract is the owning contract for the transform gizmo and snapping; picking, bounds framing, custom pivot UI, orthographic controls, and configurable bindings remain unavailable. `DESIGN.md` edited in place. |
| `DESIGN.md`: blue reserved for selection, focus, active tabs, and primary actions | Axis colors are allowed on gizmo handles only; selection stays blue; gizmo hover and active use the selection tokens. The current theme also tints every `ImGuiCol_Button*` blue, an existing violation fixed by the theme slice. |
| `PRODUCT.md` principle 2 stable geography | The default layout is the stable geography; user-initiated detaching is allowed; Reset Layout and Dock Back always restore it. The bottom tab group becomes Console, Profiler, and History, and the status bar is added. |
| `DESIGN.md`: Settings split and no duplicate settings panels | Add `Editor Preferences` as the third Settings entry; no standalone settings panel is created. |
| `DESIGN.md`: window radius 3 px | In-app panels keep 3 px; panels in OS windows use 0. |
| `PRODUCT.md` principle 4 and the "controls that imply unsupported behavior" anti-reference | Unsupported detach is visibly disabled with a reason; the Tools items that only log "not implemented yet", the Console "Add Test Message" button, and the "ImGui Demo" toggle are removed or honestly disabled with a reason. |
| `PRODUCT.md` principle 7: project-local mutations are undoable | The committed Fab import is an explicit undo barrier, shown in the UI and the typed receipt rather than hidden; ordinary edits stay undoable. |
| `PRODUCT.md` accessibility: non-color-only, no decorative motion | Snap state, source badges, and multi-select primary use text or outline; toasts and the status bar do not animate, and a toast auto-dismisses only with an equivalent persistent Console record. |
| `AGENTS.md`: synthetic UI input forbidden for parameter mutation | Typed actions cover every automatable behavior; the physical gizmo drag, drag-out, and mouse outliner gestures are user-performed manual gates. |
| `DESIGN.md` typography: single default font at a compact fixed scale | Interpreted as one font family at a fixed relative scale uniformly multiplied by one UI-scale factor. Crisp HiDPI text needs a scalable font; whether to admit a TTF (and its `DEPENDENCIES.md` row) is open and belongs to the HiDPI slice. |
| `Editor/OWNERSHIP.md`: mailbox is not entity/component lifecycle authority | Amended to admit only the fixed outliner lifecycle actions listed above. |

## Rejected Alternatives

- **Command objects (apply/revert) for all edits.** Cheaper per entry, but correct only if every mutation across roughly 25 call sites and the typed rollback machinery is a command; a missed mutation is an undo that silently does not undo. Snapshots are correct today and `RestoreHistoryState` already encodes the post-restore invariants. Delta payloads remain a measurement-gated option.
- **ImGuizmo.** See the gizmo decision.
- **Renderer debug overlay as the gizmo surface.** See the gizmo decision.
- **Detaching the Scene Viewport or the Fab panel in v1.** Their input source (main-window engine events) and cursor capture target (the main GLFW window) would both change, which is a contract change to the Fusion navigation and Fab input contracts, not an implementation detail.
- **Letting the backend pick FIFO for secondaries, or wrapping present policy for them now.** FIFO secondaries hang the process when hidden; an engine-owned secondary render path is deferred behind a concrete FIFO-only requirement.
- **Absolute-phase grid snapping now.** Needs exact modular arithmetic over sector indices; the sector-centre lattice plus an honest popover note is the smaller mechanism.
- **A per-entity enabled flag and persisted lock.** Both are Scene format changes with no current consumer beyond the outliner; visibility reuses `MeshRendererComponent::Visible` and lock is session-only.
- **Restoring detached windows by default.** Breaks the single-window headed acceptance assumption.
- **An ImGui null-platform synthetic-input harness for gesture tests.** A grey area against the synthetic-input rule; not built, not relied on.

## Verification Strategy

Every slice follows `Docs/TESTING_STRATEGY.md` (tier, oracle, replay) and `Docs/VERIFICATION.md` (commands, evidence). Pure-tier tests go in the Fast tier with seeds from `GeneratedTest.h`, failure traces to ignored `output/`, and ASan/UBSan lane coverage.

| Seam | Independent oracle | Boundaries and generators |
| --- | --- | --- |
| `HistoryStore` | Trivial reference model (`vector<int>` states, integer cursor, naive eviction loop); invariants: undo-redo identity, strictly increasing revisions, barrier unreachable, `UsedBytes` equals the sum of distinct state bytes, budget respected unless one entry, a gesture of N updates yields one entry, `Load(Save())` round trip | random operation sequences with shrinking; empty, one, cap-1/cap/cap+1, budget equal to one state, failed `RestoreFn`, abort in a nested transaction, gesture open during undo |
| State-size estimator | glibc `mallinfo2().uordblks` delta around a copy, 15 percent tolerance (Linux/glibc only) | scenes of 0, 1, 100, and 10,000 entities with long and short names |
| `EditGestureTracker` | A differently written reference scan over the event stream | activation and edit in the same frame, edit without activation, deactivation without edit, item id change mid-gesture |
| `ResolveShortcut` | Exhaustive context bits times the chord set against a plain table | includes browser-owns-keyboard, text input, gesture open |
| `CommandRegistry`, `SelectionModel`, `HierarchyModel`, `LogBuffer` | Set/ordered-model oracles in visible-filtered order; naming-policy tables; ring-capacity B-1/B/B+1 and filter oracles | click/Ctrl/Shift sequences, stale-id pruning, very long and Unicode names |
| Layout codec | Round trip; schema-version table; structure-aware truncation, mutation, and oversize corpus; atomic-write failure injection; unique fixture paths (never the default project's artifacts) | fail-closed to the DESIGN default |
| `SnapMath` | Exact `__int128` fixed-point reference (sectors within 1e6, locals as multiples of 2^-20, power-of-two and decimal steps); idempotence, `snap(p + k*step) = snap(p) + k*step`, `abs(snap(p) - p) <= step/2`, canonical output, bit-identical unconstrained axes, documented tie rule | half-extent rollover B-1/B/B+1, negatives, extreme sector indices, steps that do not divide the extent |
| `GizmoMath` | Project `origin + s*axis` through the real camera matrices and measure the pixel length; inverse oracle (project a true 3D displacement to pointer positions and require recovery); Rodrigues axis-angle on probe vectors for rotation; matrix recomposition for Euler | near-parallel rays, edge-on rings, hit thresholds at the pixel limits, behind-camera rejection, pitch at plus and minus 90 degrees |
| `TransformGizmo` | Scripted pointer traces replayed on a real `Engine::Scene`: transform equals start plus delta (not accumulated), cancel restores bit-exactly | cancel mid-drag, rejected write, entity deleted mid-drag |

Beyond the Fast tier: an in-process, headless command smoke executes every registered command through `CommandRegistry` and asserts document, history, and status results (no ImGui); the control-mailbox smokes assert `InspectHistory`/`UndoHistory`/`RedoHistory`/`ApplyGizmoOperation` against `HeadRevision` and display names; one headed Debug Vulkan run through `Scripts/LaunchHeadedEditor.sh` on DP-3/workspace 2 with the Editor otherwise idle (no overlapping native graphics harness, per the GPU-headroom rule); and, for multi-viewport, a bounded Vulkan smoke that creates a secondary viewport through `DetachPanel`, asserts it rendered, destroyed cleanly, and shut down in the documented order, run with the Editor closed. Because no validation layer is installed on the workstation, synchronization correctness for multi-viewport must either run with the layer installed or be recorded as unvalidated. Unit tests for the H2 serial bridge use fake fences. Hosted CI is not relied on (`PLAN.md` records the hosted-run restriction); matching local evidence is required, and none of this verification is claimed yet.

### Human-only gates

These are the smallest set the user must perform; agents must not inject this input. Each states what the agent resumes automatically afterward.

1. **History (Inspector).** Drag Position X in the Inspector for about two seconds and release. Success: the History panel shows exactly one new entry named "Move <name>"; Ctrl+Z restores the value; Ctrl+Y and Ctrl+Shift+Z redo; the Edit menu labels match; typing in a text field and pressing Ctrl+Z does not undo the Scene.
2. **Gizmo.** Select an entity, press W, drag the X arrow with snap off, then on, then with Ctrl held to invert; press E, R, and X to switch tool and space; press Esc during a drag. Success: the position lands on the grid when snapping is effective, each completed drag is one entry, Esc restores exactly and adds no entry, navigation is unaffected.
3. **Fab keyboard ownership.** With the Fab panel focused, type in a web text box and press Ctrl+Z. Success: the Scene does not change.
4. **Detach.** With the Editor launched through the helper on DP-3, drag the Console tab outside the main window, observe it become a separate window that follows the cursor without stutter, drag it back into the dock, and record pass or fail and any stutter. Also confirm the Scene Viewport and Fab tabs refuse to leave.
5. **Visual quality.** Look for flicker when a secondary opens, corner artifacts at radius 0, and the gizmo's hover and axis contrast on a real capture.
6. **Hidden-workspace recovery.** Move a detached panel to a workspace that is not displayed, switch away and back, and confirm the Editor recovers.
7. **Mixed DPI and monitor reconfiguration**, if the user's setup has them.
8. **Outliner and content-browser mouse gestures:** click, Ctrl-click, Shift-click, F2, Delete, context menus, and dragging an asset onto an Inspector slot or the viewport.

## Non-Claims And Open Items

- Nothing here is implemented or verified at runtime. D3D12, Windows, macOS, native Wayland, and OpenGL2 behavior of every feature is unverified; the gizmo and history are platform-neutral but their headed evidence is Linux/Vulkan only until a later gate.
- Not verified: NVRHI-owned queue behavior with ImGui secondary submissions, dynamic textures rewritten each frame inside a secondary window, hidden-secondary recovery, the third advertised present mode, mixed DPI, whether `NoUndocking` (or the chosen mechanism) pins the two panels, the physical drag, and Debug-build cost of Begin/End snapshots at 50,000 entities.
- Open decisions deliberately left to their slices: the pinning mechanism, the camera-pose overlay mechanism for history restore, whether to admit a scalable font, the exact keyboard defaults beyond the table above, configurable bindings, a dirty-document indicator and unsaved-changes prompt (not scheduled), autosave and crash recovery (not scheduled), selection-as-undo (rejected for now), and a history budget setting.
- A scene-parenting contract is required before any hierarchy code. A viewport ray-picking contract is required before click-to-select, surface or vertex snapping, or a bounds-centre pivot.
- Raw spike artifacts were kept only in a session scratchpad. A slice that depends on a spike number re-measures it in the repository and records the result in `PLAN.md` or `HANDOFF.md`.
