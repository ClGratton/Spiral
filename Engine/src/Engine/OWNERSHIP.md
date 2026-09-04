# Engine Module Ownership

The engine library owns reusable runtime systems. It must not depend on `Editor` or `Sandbox`.

Allowed dependencies:

- C++ standard library.
- Explicit vendor libraries added through `Vendor/` and declared in Premake.
- Platform code hidden behind engine interfaces.

Forbidden:

- Editor panels or workflow UI.
- Direct application-specific logic.
- Backend-specific renderer types leaking into public gameplay APIs.

## Current Module Scopes

| Module | Owns | Must not own |
| --- | --- | --- |
| `Core` | Application lifecycle, layers, windows, logging, assertions, arguments, utilities. | Renderer, scene, assets, scripting, editor behavior. |
| `RHI` | Backend-neutral GPU contracts and backend adapters, including the explicit single-slot vertex-layout/stride contract, device-lifetime completion-token query/dependency authority, and backend-private bounded native completion history. | Scene/material policy, editor UI, backend-native vertex-layout types, or backend-native synchronization handles in public contracts. |
| `RenderGraph` | Pass/resource dependency, lifetime, state, scheduling, and transient-resource policy. | Scene traversal or backend presentation. |
| `Renderer` | High-level render passes, presentation bridges, shaders, scene rendering, render diagnostics, exact pipeline strides derived from renderer-owned vertex structures, and frame-local material-row/normal-transform publication from immutable Scene and Assets inputs. | Gameplay entity ownership, persistent material identity, asset import, or backend-native layout policy. |
| `Scene` | Entities/components, authoring facade, serialization, cameras, and backend-neutral immutable render extraction retaining stable mesh/material handles. | Resolved material rows, editor panels, or backend-native GPU objects. |
| `Assets` | Asset identity, import, cooked metadata including versioned mesh geometry/normals, dependencies, reimport, and streaming inputs. | Rendering, frame-local material IDs, or editor widget policy. |
| `Jobs` | Worker scheduling and task dependencies. | Subsystem-specific business logic. |
| `Math` | Backend-neutral numeric types, transforms, and canonical world-grid conversion, normalization, composition, and bounded sector queries. | Scene serialization authority, per-view origin state, terrain/physics partition ownership, or renderer policy. |
| `Terrain` (planned Phase 7) | Terrain topology/profile and source contracts, tile identity/artifacts, generation scheduling, caches, edits, provenance, and diagnostic publication. | Renderer passes, physics-world authority, Scene entities, editor UI, or native GPU types. |
| `Water` (planned Phase 8W) | Water-body/profile/artifact identity, bounded simulation policy, deterministic low-frequency queries, immutable snapshots, recapture state, provenance, and diagnostics. | Terrain generation, renderer passes, physics-world authority, Scene entities, editor UI, or native GPU types. |
| `Platform` | OS/window/headless implementations behind engine interfaces. | Renderer feature policy or editor workflows. |
| `UI` | Engine tool-UI integration and documented native presentation bridges. | Scene rendering through native API escape hatches. |
| `Diagnostics` | Crash reports, profiling contracts, logs, captures, and diagnostic data. | Owning the systems it observes. |
| `Automation` (planned; absent until Phase 13 has a real workflow consumer) | Future model-neutral action, transaction, receipt, and deterministic headless workflow contracts extracted from proven consumers. | Editor/model orchestration, domain commands, hidden project mutation, or provider-specific policy. |

Dependency direction is toward public contracts: editor/client code calls Engine; Renderer consumes Scene extraction and Assets outputs; Renderer uses RenderGraph and RHI; RenderGraph uses RHI descriptions/contracts; RHI must not call upward into Renderer or Scene.

Editor owns future workflow UX, tool registration, model/provider adapters, planning/orchestration, preview/approval, cancellation, and history presentation. Domain modules own their commands, preconditions, validators, and results. The accepted split is [../../../Docs/Architecture/AI_AUTOMATION_ARCHITECTURE.md](../../../Docs/Architecture/AI_AUTOMATION_ARCHITECTURE.md).

## Planned Phase 7 Module

`Engine/src/Engine/Terrain` does not exist yet. When its Phase 7 foundation begins, it will own project-selectable terrain profiles and topology, deterministic spatial source queries, canonical versioned tile artifacts, terrain-specific generation scheduling and caches, edit layers, provenance, and diagnostics publication.

Terrain may consume `Core`, `Jobs`, diagnostics contracts, cooked `Assets` artifacts, and backend-neutral RHI upload services. `Scene` references terrain instances and profiles; `Renderer` consumes immutable render payloads; `Physics` consumes finalized collision payloads; `Editor` owns authoring workflows. Terrain must not depend on `Editor`, own renderer passes or the physics world, or expose native GPU types. The full contract is [../../../Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md](../../../Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md).

## Planned Phase 8W Module

`Engine/src/Engine/Water` does not exist yet. When Phase 8W begins, it will own stable water-body and interaction-zone identities, profiles and cooked artifacts, bounded simulation and recapture policy, deterministic low-frequency CPU surface queries, immutable snapshots, provenance, and diagnostic publication.

Water may consume `Core`, `Math`, `Jobs`, diagnostics contracts, cooked `Assets` and `Terrain` artifacts, and backend-neutral RHI services. `Renderer` owns GPU simulation execution and optical passes; `Physics` consumes fixed-tick query snapshots and may publish delayed visual coupling inputs; `Editor` owns authoring workflows. Water must not depend on `Editor`, own terrain generation, renderer passes, or the physics world, or expose native GPU types. The full contract is [../../../Docs/Architecture/WATER_ARCHITECTURE_AND_RESEARCH.md](../../../Docs/Architecture/WATER_ARCHITECTURE_AND_RESEARCH.md).

## Planned Phase 11 Module

`Engine/src/Engine/Physics` does not exist yet. When Phase 11 begins, it will own the backend-neutral fixed-step world, generation-safe physics handles, staged commands, immutable results/events, collision/query contracts, backend capabilities, state hashes/snapshots, and debug/metric publication.

Physics may consume `Core`, `Jobs`, diagnostics contracts, cooked `Assets` artifacts, and backend-neutral RHI services for optional GPU deformation. It must not depend on `Editor`, `Renderer`, `RenderGraph` policy, or mutable Scene entity storage. Scene exchanges stable IDs and staged command/result snapshots; Renderer consumes finalized transforms and explicitly synchronized visual-deformation resources. The full contract is [../../../Docs/Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md](../../../Docs/Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md).
