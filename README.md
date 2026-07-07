# Spiral

Spiral is the first buildable spine of the engine described in the architecture documents.

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
- `Editor`: Hazel-style client application with an editor layer.
- `Sandbox`: public API smoke-test app.
- `Scripts`: setup, project generation, build, and run helpers.
- `Vendor/premake`: auto-bootstrapped Premake tool location.
- `Vendor/GLFW`: vendored cross-platform window/input library built from source by Premake.
- `Vendor/ImGui`: vendored Dear ImGui docking UI built from source by Premake.
- `Vendor/NVRHI`: vendored NVRHI common core and validation library built from source by Premake.
- `Vendor/Vulkan-Headers` and `Vendor/DirectX-Headers`: pinned platform headers for the upcoming NVRHI backend work.
- `Engine/Shaders`: engine-owned shader assets loaded by renderer code during development.

The current renderer initializes through an NVRHI backend boundary. On Windows/MSVC it creates a native NVRHI D3D12 device, DXGI swapchain, renderer-owned viewport texture, ImGui DX12 presentation path, and a first native D3D12 indexed prototype mesh pass using a disk-backed HLSL shader asset. The `gmake`/MinGW path currently keeps the common NVRHI probe and OpenGL2 ImGui fallback until the Vulkan backend lands.

To produce a deterministic viewport capture from the native D3D12 editor path:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --capture-viewport --smoke-test
```

The capture is written to `output/captures/editor-viewport.bmp`.

To run the automated Windows render smoke test, including build, capture, BMP validation, and non-blank pixel checks:

```powershell
.\Scripts\TestRender.ps1 -Configuration Debug -Action vs2022
```

CI is scaffolded in `.github/workflows/ci.yml`. The Windows job runs the D3D12 render smoke and uploads the viewport BMP; Linux and macOS run portable gmake builds plus headless editor/sandbox smoke tests. The D3D12 device path falls back to WARP when no hardware adapter is available, which keeps hosted Windows runners usable.

## Platform Direction

The current code slice is OS-neutral C++20 plus GLFW for native windows/input. It is structured to build on Windows, Linux, and macOS from the same Premake workspace.

Current limitation: the viewport mesh is still a bootstrap test pass, not an actual scene draw through the full `Engine::RHI` render graph. The next major platform step is moving shader compilation, resource creation, and drawing into the RHI/render-graph path, then adding the Vulkan backend.

## Architecture

Start with:

- [PLAN.md](PLAN.md)
- [Docs/Architecture/README.md](Docs/Architecture/README.md)
- [Docs/DEPENDENCIES.md](Docs/DEPENDENCIES.md)
- [Docs/Architecture/ENGINE_INSTRUCTIONS.md](Docs/Architecture/ENGINE_INSTRUCTIONS.md)
- [Docs/Architecture/HAZEL_INSPIRED_SKELETON.md](Docs/Architecture/HAZEL_INSPIRED_SKELETON.md)
- [Docs/Architecture/LOW_LEVEL_ARCHITECTURE.md](Docs/Architecture/LOW_LEVEL_ARCHITECTURE.md)
- [Docs/Architecture/RENDERER_IMPLEMENTATION_CONTRACTS.md](Docs/Architecture/RENDERER_IMPLEMENTATION_CONTRACTS.md)

## Run Existing Executables

After a Debug build on Windows, the generated executables are:

```text
bin/Debug-windows-x86_64/Editor/Editor.exe
bin/Debug-windows-x86_64/Sandbox/Sandbox.exe
```

The editor and sandbox open native windows by default and stay open until closed. The Visual Studio editor build contains the dockable UI, renderer backend selector, and native D3D12 viewport prototype mesh pass.

For automated validation without opening a window:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --smoke-test
.\bin\Debug-windows-x86_64\Sandbox\Sandbox.exe --headless --smoke-test
```
