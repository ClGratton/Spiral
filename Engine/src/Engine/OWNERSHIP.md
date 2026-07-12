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
| `RHI` | Backend-neutral GPU contracts and backend adapters. | Scene/material policy or editor UI. |
| `RenderGraph` | Pass/resource dependency, lifetime, state, scheduling, and transient-resource policy. | Scene traversal or backend presentation. |
| `Renderer` | High-level render passes, presentation bridges, shaders, scene rendering, and render diagnostics. | Gameplay entity ownership or asset import. |
| `Scene` | Entities/components, authoring facade, serialization, cameras, and render extraction inputs. | Editor panels or backend-native GPU objects. |
| `Assets` | Asset identity, import, cooked metadata, dependencies, reimport, and streaming inputs. | Rendering or editor widget policy. |
| `Jobs` | Worker scheduling and task dependencies. | Subsystem-specific business logic. |
| `Platform` | OS/window/headless implementations behind engine interfaces. | Renderer feature policy or editor workflows. |
| `UI` | Engine tool-UI integration and documented native presentation bridges. | Scene rendering through native API escape hatches. |
| `Diagnostics` | Crash reports, profiling contracts, logs, captures, and diagnostic data. | Owning the systems it observes. |
| `Automation` | Deterministic workflow contracts through public engine/editor APIs. | Hidden direct mutation of private subsystem state. |

Dependency direction is toward public contracts: editor/client code calls Engine; Renderer consumes Scene extraction and Assets outputs; Renderer uses RenderGraph and RHI; RenderGraph uses RHI descriptions/contracts; RHI must not call upward into Renderer or Scene.
