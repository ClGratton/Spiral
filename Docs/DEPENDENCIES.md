# Dependency Ledger

Status: Living document
Date: 2026-07-06

This file records third-party dependencies, why they exist, how they enter the repo, and what their licensing obligations are.

## Rules

- Prefer vendored source or pinned fetch scripts over "install this random library manually."
- Do not add a dependency without a license entry.
- Do not let temporary bootstrap dependencies define the final architecture.
- Keep backend-specific APIs behind engine interfaces.

## Current Dependencies

| Dependency | Location | Use | License | Policy |
| --- | --- | --- | --- | --- |
| Premake | `Vendor/premake/bin` via `Scripts/Setup.*` | Project generation. | BSD 3-Clause, see Premake release package. | Tool bootstrap only; not linked into runtime. |
| GLFW 3.4 | `Vendor/GLFW` | Cross-platform native window/input. Uses `GLFW_NO_API` for the Windows/MSVC D3D12 editor path and an OpenGL context for fallback builds. | zlib/libpng, see `Vendor/GLFW/LICENSE.md`. | Required for current editor/player shell. Must stay behind `Engine::Window`. |
| Dear ImGui docking | `Vendor/ImGui` | Editor docking UI and panels. Uses DX12 renderer backend on Windows/MSVC and OpenGL2 fallback elsewhere. | MIT, see `Vendor/ImGui/LICENSE.txt`. | Required for current editor UI. Runtime game UI is a future separate decision. |
| NVRHI | `Vendor/NVRHI` | Renderer abstraction vendor layer. Common, validation, Windows/MSVC D3D12, and Vulkan backend sources are built by Premake and used through `Engine::RHI`. | MIT, see `Vendor/NVRHI/LICENSE.txt`. | NVRHI types stay out of gameplay/editor APIs. D3D12/Vulkan details remain behind `Engine::RHI`. |
| Vulkan-Headers | `Vendor/Vulkan-Headers` | Platform headers for the NVRHI Vulkan backend. | Apache-2.0 OR MIT, see `Vendor/Vulkan-Headers/LICENSE.md`. | Pinned from NVRHI's expected tag. Enables vendor Vulkan backend compilation; engine Vulkan device/swapchain creation is still pending. |
| DirectX-Headers | `Vendor/DirectX-Headers` | Platform headers for the NVRHI D3D12 backend. | MIT, see `Vendor/DirectX-Headers/LICENSE`. | Pinned from NVRHI's expected tag. D3D12 sources are enabled only for Windows/MSVC; MinGW builds keep the NVRHI common fallback. |

## Planned Dependencies

| Dependency | Planned Location | Use | License | Policy |
| --- | --- | --- | --- | --- |
| Slang | TBD | Shader authoring/compilation. | Apache-2.0 with LLVM exceptions for some components; audit before vendoring. | Shader compiler/tooling, not hardcoded into app layer. |
| meshoptimizer | TBD | Meshlet/cluster/vertex/index optimization. | MIT. | Asset pipeline dependency. |

## NVRHI Pin

Vendored upstream:

```text
Repository: https://github.com/NVIDIA-RTX/NVRHI.git
Commit:     8e8c36e37558acec333204619b95d9d2fcdc4a79
```

The current build compiles NVRHI common, validation, and Vulkan backend sources through `Vendor/NVRHI.premake.lua`, links them into the editor/sandbox, and probes `nvrhi::getFormatInfo` at renderer startup. On Windows/MSVC it also compiles NVRHI's D3D12 backend and the engine creates a native D3D12 device plus graphics/compute/copy queues before wrapping them in an NVRHI device. The editor presentation path then uses an engine-owned DXGI swapchain, D3D12 render-target/SRV descriptor heaps, ImGui's DX12 backend, and a renderer-owned viewport texture. MinGW/GMake intentionally keeps the NVRHI common fallback because DirectX-Headers and NVRHI D3D12 sources do not currently form a reliable MinGW ABI path in this repo. Vulkan device/swapchain creation is still an engine implementation task; this ledger entry only means the vendored NVRHI Vulkan backend now compiles against the pinned Vulkan-Headers.

## NVRHI Platform Header Pins

These tags match the expectations recorded in `Vendor/NVRHI/CMakeLists.txt` at the pinned NVRHI commit:

```text
Vulkan-Headers: https://github.com/KhronosGroup/Vulkan-Headers.git v1.4.352
DirectX-Headers: https://github.com/microsoft/DirectX-Headers.git v1.717.0-preview
```
