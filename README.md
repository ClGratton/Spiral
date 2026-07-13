# Spiral

Spiral is a game engine focused on sharp motion, measured materials, transparent performance, and automation. Its baseline image-quality path does not require temporal antialiasing or temporal upscaling.

## Build

Windows PowerShell:

```powershell
.\Scripts\Build.ps1 -Configuration Debug
```

Linux/macOS shell:

```bash
./Scripts/Build.sh Debug
```

The setup script bootstraps a pinned Premake binary into `Vendor/premake/bin` if it is missing. No project files need to be hand-authored.

Default behavior:

- Uses Visual Studio/MSBuild if available.
- Falls back to Premake `gmake` and `mingw32-make`/`make` when Visual Studio is not available.

Run the lightweight repository style check:

```powershell
.\Scripts\CheckCodeStyle.ps1
```

Run the sample app:

```powershell
.\Scripts\RunSandbox.ps1
```

or:

```bash
./Scripts/RunSandbox.sh Debug
```

Run the editor shell:

```powershell
.\Scripts\RunEditor.ps1
```

On Windows, the native D3D12 viewport path is compiled by the Visual Studio/MSBuild action. To force that path explicitly:

```powershell
.\Scripts\RunEditor.ps1 -Action vs2022
```

The `gmake`/MinGW executable is kept as a portable shell fallback for now. It will show `NVRHI Common` and does not have the D3D12 viewport texture.

or:

```bash
./Scripts/RunEditor.sh Debug
```

## Current Slice

- `Engine`: C++20 static library.
- `Editor`: dockable scene-authoring application with hierarchy, content browser, selection Inspector, settings, viewport, and diagnostics panels.
- `Sandbox`: public API smoke-test app.
- `Scripts`: setup, project generation, build, and run helpers.
- `Vendor/premake`: auto-bootstrapped Premake tool location.
- `Vendor/GLFW`: vendored cross-platform window/input library built from source by Premake.
- `Vendor/ImGui`: vendored Dear ImGui docking UI built from source by Premake.
- `Vendor/NVRHI`: vendored NVRHI common core, validation layer, Vulkan backend, and Windows/MSVC D3D12 backend built from source by Premake.
- `Vendor/cgltf`: pinned glTF 2.0 parser used by the asset import prototype.
- `Vendor/Vulkan-Headers` and `Vendor/DirectX-Headers`: pinned platform headers for NVRHI backend builds.
- `Engine/Shaders`: engine-owned shader assets loaded by renderer code during development.

The current renderer initializes through an NVRHI backend boundary. On Windows/MSVC it creates a native NVRHI D3D12 device, DXGI swapchain, renderer-owned viewport texture, ImGui DX12 presentation path, and a first native D3D12 indexed prototype mesh pass using a disk-backed HLSL shader asset. The editor also has an engine-owned Vulkan 1.3 device, GLFW window surface, FIFO swapchain, and native ImGui Vulkan presentation path selected with `--renderer-vulkan`. The Vulkan path is runtime-verified on Windows with MSVC and MinGW and on Linux X11 through WSLg with Mesa llvmpipe; its scene viewport renderer remains pending.

To exercise Vulkan device creation, native ImGui presentation, and swapchain resize through a strict smoke test:

```powershell
.\Scripts\TestVulkan.ps1 -Configuration Debug -Action vs2022
.\Scripts\TestVulkan.ps1 -Configuration Debug -Action gmake
```

Linux CI repeats the strict smoke through Mesa lavapipe and Xvfb:

```bash
bash Scripts/TestVulkan.sh Debug gmake
```

On macOS, `Scripts/Setup.sh` installs Homebrew's Vulkan loader and MoltenVK runtime when missing. The same strict smoke selects MoltenVK through the loader, enables Vulkan portability enumeration, wraps the device with NVRHI, recreates the swapchain after resize, and requires a successful post-resize present:

```bash
bash Scripts/TestVulkan.sh Debug gmake
```

The experimental macOS path is verified on hosted macOS 15 Intel CI with Vulkan Loader 1.4.350.1, MoltenVK 1.4.1, and the Apple Paravirtual device. That hosted device requires Metal argument buffers and `MTLHeap` allocation to be disabled for the correctness smoke. Apple Silicon generation, normal hardware capability coverage, runtime packaging, and production scene-renderer qualification remain pending.

To produce a deterministic viewport capture from the native D3D12 editor path:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --capture-viewport --smoke-test
```

The capture is written to `output/captures/editor-viewport.bmp`.

To save and reload-validate the current sample scene in headless mode:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --smoke-test --save-scene-smoke
```

The scene is written to `output/scenes/sample.spiral`.

To parse, validate, register, and cook a self-contained glTF triangle import smoke:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --gltf-import-smoke
```

The source and cooked mesh manifest are written under `output/assets/gltf-smoke` and `output/imports/gltf`.

To save and reload-validate the versioned material asset format:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --material-asset-smoke
```

The smoke material is written to `output/assets/material-smoke.spiralmat`.

To run the automated Windows render smoke test, including build, capture, BMP validation, and non-blank pixel checks:

```powershell
.\Scripts\TestRender.ps1 -Configuration Debug -Action vs2022
```

CI runs from `.github/workflows/ci.yml`. The Windows job runs the D3D12 render smoke and uploads the viewport BMP; Linux and macOS run portable gmake builds plus engine contract tests and headless editor/sandbox workflow smokes. Linux also runs the strict X11 Vulkan presentation smoke through Mesa lavapipe and Xvfb. The x86_64 macOS job runs the equivalent strict presentation smoke through the Homebrew Vulkan loader and MoltenVK. The D3D12 device path falls back to WARP when no hardware adapter is available, which keeps hosted Windows runners usable.

To validate project creation, entity authoring, component assignment, undo/redo, and save/reopen as one workflow:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --scene-authoring-smoke
```

## Platform Direction

The current code slice is OS-neutral C++20 plus GLFW for native windows/input. It is structured to build on Windows, Linux, and macOS from the same Premake workspace.

Current limitation: the viewport mesh is still a D3D12 bootstrap test pass, not an actual scene draw through the full render graph. Its buffers, shaders, pipeline, and indexed draw pass through `Engine::RHI`; descriptor binding, scene submission, and a backend-neutral viewport renderer are the remaining bridge work before Vulkan can display the same scene target.

## Architecture

Start with:

- [AGENTS.md](AGENTS.md)
- [PLAN.md](PLAN.md)
- [Docs/README.md](Docs/README.md)
- [Docs/VERIFICATION.md](Docs/VERIFICATION.md)
- [PRODUCT.md](PRODUCT.md)
- [DESIGN.md](DESIGN.md)
- [Docs/ROADMAP_GOVERNANCE.md](Docs/ROADMAP_GOVERNANCE.md)
- [Docs/EDITOR_UI_REVIEW.md](Docs/EDITOR_UI_REVIEW.md)
- [Docs/Architecture/README.md](Docs/Architecture/README.md)
- [Docs/DEPENDENCIES.md](Docs/DEPENDENCIES.md)
- [Docs/Architecture/ENGINE_INSTRUCTIONS.md](Docs/Architecture/ENGINE_INSTRUCTIONS.md)
- [Docs/Architecture/HAZEL_INSPIRED_SKELETON.md](Docs/Architecture/HAZEL_INSPIRED_SKELETON.md)
- [Docs/Architecture/LOW_LEVEL_ARCHITECTURE.md](Docs/Architecture/LOW_LEVEL_ARCHITECTURE.md)
- [Docs/Architecture/RENDERER_IMPLEMENTATION_CONTRACTS.md](Docs/Architecture/RENDERER_IMPLEMENTATION_CONTRACTS.md)
- [Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md](Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md)
- [Docs/Architecture/RENDERER_CAPABILITY_CONTRACT.md](Docs/Architecture/RENDERER_CAPABILITY_CONTRACT.md)
- [Docs/Architecture/TECHNICAL_ROADMAP_COVERAGE.md](Docs/Architecture/TECHNICAL_ROADMAP_COVERAGE.md)
- [Docs/Architecture/MACOS_RENDERER_BACKEND_DECISION.md](Docs/Architecture/MACOS_RENDERER_BACKEND_DECISION.md)
- [Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md](Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md)

## Run Existing Executables

After a Debug build on Windows, the generated executables are:

```text
bin/Debug-windows-x86_64/Editor/Editor.exe
bin/Debug-windows-x86_64/Sandbox/Sandbox.exe
```

After a Debug gmake/MinGW build on Windows, Premake appends the action name:

```text
bin/Debug-windows-x86_64-gmake/Editor/Editor.exe
bin/Debug-windows-x86_64-gmake/Sandbox/Sandbox.exe
```

The editor and sandbox open native windows by default and stay open until closed. The Visual Studio editor build contains the dockable UI, renderer backend selector, and native D3D12 viewport prototype mesh pass.

For automated validation without opening a window:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --smoke-test
.\bin\Debug-windows-x86_64\Sandbox\Sandbox.exe --headless --smoke-test
```
