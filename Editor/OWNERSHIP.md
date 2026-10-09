# Editor Ownership

The editor is a client of the engine. It owns panels, workflows, inspectors, viewports, guided creation, and AI-assisted tooling.

The editor may ask engine diagnostics APIs for renderer and asset data, but it must not reach around public engine boundaries to mutate private runtime state.

The opt-in control mailbox is Editor-owned verification tooling. Its accepted schema 3 retains fixed material inspect and select-plus-surface-patch actions plus fixed entity inspect/select, complete Transform, typed Light, complete project color-pipeline, and dedicated main-camera viewport-pose actions. It additionally admits only two bounded debug-workflow actions: complete Scene-debug view/selected-bounds publication bound to the expected selected entity, and complete MeshRenderer visibility/shadow-flag mutation bound to stable entity identity. The shared foundation puts document mutations through normal Scene or MaterialLibrary validation and Editor history/restore, while debug state uses the normal immutable Renderer publication and generation authority. Mailbox work executes on the Editor main thread through normal renderer-publication, selection/pivot/camera, and history authorities and publishes bounded terminal receipts with at most 32 sampled affected stable IDs. Every mutation is explicitly session-only/not-saved. It must not become arbitrary dispatch, a network service, implicit save authority, entity/component lifecycle authority, or a substitute for the future model-neutral Automation boundary.

## Fab Browser Module (`Editor/src/Fab`)

`Editor/src/Fab` is the Editor-private home of the integrated Fab browser panel described by `Docs/Architecture/FAB_ASSET_INTEGRATION.md` (2026-10-09 amendment). Engine and Sandbox never include it, and no browser-engine header (CEF or any other) may appear anywhere outside the Editor's adapter files and the separate browser-helper executable.

The directory is split by dependency, and the split is a contract:

- **Pure core** (`BrowserSurface.h`/`.cpp`, `BrowserFrameMirror`, `BrowserNavigationPolicy`, `BrowserDownloadPolicy`, `BrowserInputRouter`, `FabIntake`, `NullBrowserSurface`): standard C++ and `Engine/Core/Base.h` plus the `Engine/Platform/ExternalUrl.h` URL primitive only. No CEF, ImGui, GLFW, RHI, or other native-graphics include is allowed in these files, and they hold no engine thread or global state. Every `IBrowserSurface::Listener` callback is delivered on the main thread from inside `Pump()`. These files are also compiled into `EngineTests` (see `Tests/premake5.lua`) and exercised through `Tests/src/TestSupport/FakeBrowserSurface.h`; adding a file to this tier means adding it to that list, and nothing that needs a browser engine, ImGui, GLFW, or a GPU may be added to it.
- **Adapters** (the future engine-backed `IBrowserSurface`, the ImGui panel, and the helper executable): the only code allowed to include third-party browser or UI headers. They consume the pure core and are not compiled into `EngineTests`.

Authority stays where the Fab contract puts it. The navigation policy composes `Engine::IsAllowedExternalHttpsUrl` rather than re-implementing URL grammar. The panel never reads the browser profile, cookies, or the download URL; a staged download is handed to the Engine-owned import authority as a path, and `FabIntake` is a routing classifier whose results the Engine snapshot and ZIP authorities re-validate. The panel is an acquisition adapter, not a second importer.

## Project Manifest And Browser Helper

The project manifest codec is no longer Editor code: `EditorLayer` reads and writes the manifest through thin wrappers over `Engine::ProjectManifest` (format 7). The Editor owns when to commit and the undo barrier, never the commit mechanism. `Editor/helper/` builds `SpiralBrowserHelper`, the separate browser subprocess executable (built only when `Vendor/CEF` is present); it is staged under the Editor's `cef/` directory, and no other Editor, Engine, Sandbox, or test target links or includes CEF.
