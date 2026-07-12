# Dependency Ledger

Status: Living document
Date: 2026-07-12

This file records third-party dependencies, why they exist, how they enter the repo, and what their licensing obligations are.

## Rules

- Prefer vendored source or pinned fetch scripts over "install this random library manually."
- Do not add a dependency without a license entry.
- Do not let temporary bootstrap dependencies define the final architecture.
- Keep backend-specific APIs behind engine interfaces.
- When changing current dependency versions, update `.github/dependency-graph/vendor-dependencies.json` so GitHub's dependency graph snapshot stays aligned with this ledger.

## Current Dependencies

| Dependency | Location | Use | License | Policy |
| --- | --- | --- | --- | --- |
| Premake | `Vendor/premake/bin` via `Scripts/Setup.*` | Project generation. | BSD 3-Clause, see Premake release package. | Tool bootstrap only; not linked into runtime. |
| GLFW 3.4 | `Vendor/GLFW` | Cross-platform native window/input. Uses `GLFW_NO_API` for the D3D12 and explicit Vulkan editor paths and an OpenGL context for fallback builds. | zlib/libpng, see `Vendor/GLFW/LICENSE.md`. | Required for current editor/player shell. Must stay behind `Engine::Window`. |
| Dear ImGui docking | `Vendor/ImGui` | Editor docking UI and panels. Uses DX12 on the default Windows/MSVC renderer, Vulkan for the explicit native Vulkan path, and OpenGL2 as the portable fallback. | MIT, see `Vendor/ImGui/LICENSE.txt`. | Required for current editor UI. Runtime game UI is a future separate decision. |
| NVRHI | `Vendor/NVRHI` | Renderer abstraction vendor layer. Common, validation, Windows/MSVC D3D12, and Vulkan backend sources are built by Premake behind the engine RHI/backend boundary. | MIT, see `Vendor/NVRHI/LICENSE.txt`. | NVRHI types stay out of gameplay/editor APIs. The Vulkan device is wrapped now; Vulkan scene-resource and command implementations through `Engine::RHI::Device` remain pending. |
| cgltf 1.15 | `Vendor/cgltf` | glTF 2.0 parse/validation and source-buffer loading for the asset import prototype. | MIT, see `Vendor/cgltf/LICENSE`. | Parser only. Spiral owns source identity, import metadata, and cooked manifests; runtime mesh buffers and material/texture conversion remain engine work. |
| Vulkan-Headers | `Vendor/Vulkan-Headers` | Platform headers for the NVRHI Vulkan backend and engine-owned Vulkan presentation path. | Apache-2.0 OR MIT, see `Vendor/Vulkan-Headers/LICENSE.md`. | Pinned from NVRHI's expected tag. Vulkan symbols are loaded dynamically, so the SDK is not a build prerequisite; the runtime still requires a Vulkan 1.3 loader and ICD. |
| DirectX-Headers | `Vendor/DirectX-Headers` | Platform headers for the NVRHI D3D12 backend. | MIT, see `Vendor/DirectX-Headers/LICENSE`. | Pinned from NVRHI's expected tag. D3D12 sources are enabled only for Windows/MSVC; MinGW can use the independent Vulkan path or the NVRHI common fallback. |
| Vulkan Loader | Homebrew `vulkan-loader` on macOS | Loads the MoltenVK ICD for the experimental hosted macOS presentation path. | Apache-2.0. | Installed by `Scripts/Setup.sh` on macOS. CI records the resolved formula version; shipping packaging and pinning remain pending. |
| MoltenVK | Homebrew `molten-vk` on macOS, version 1.3.0 or newer required | Implements the Vulkan portability subset over Metal for the experimental NVRHI macOS presentation path. | Apache-2.0. | Installed by `Scripts/Setup.sh` on macOS. Current scope is x86_64 editor presentation; Apple Silicon, application bundling, and scene-renderer conformance remain pending. |

## GitHub Dependency Graph And SBOM

GitHub's dependency graph is driven by supported package manifests, lockfiles, and dependency submissions. Premake files, vendored C++ source trees, and this prose ledger are not enough for GitHub to infer dependencies by themselves.

The repo keeps two records in sync:

- `Docs/DEPENDENCIES.md` is the human-readable policy, license, and architecture ledger.
- `.github/dependency-graph/vendor-dependencies.json` is the machine-readable list used by `Scripts/GenerateDependencySnapshot.py`.

`.github/workflows/dependency-submission.yml` submits that snapshot to GitHub's Dependency Submission API on pushes to `main` and on manual dispatch. This should make the vendored/tool dependencies visible in GitHub's dependency graph and SBOM export. It does not replace the license audit, and Dependabot alert coverage still depends on whether GitHub has advisories for the submitted package ecosystem/package URL.

Dependabot is enabled for GitHub Actions in `.github/dependabot.yml` so workflow actions stay current. It is not expected to update the vendored C++ dependencies; those still require explicit version bumps in this ledger, the fetch scripts, and the dependency graph metadata.

Reference docs:

- <https://docs.github.com/en/code-security/reference/supply-chain-security/dependency-graph-supported-package-ecosystems>
- <https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/use-dependency-submission-api>

## Planned Dependencies

| Dependency | Planned Location | Use | License | Policy |
| --- | --- | --- | --- | --- |
| Slang | TBD | Shader authoring/compilation. | Apache-2.0 with LLVM exceptions for some components; audit before vendoring. | Shader compiler/tooling, not hardcoded into app layer. |
| meshoptimizer | TBD | Meshlet/cluster/vertex/index optimization. | MIT. | Asset pipeline dependency. |
| KTX-Software / libktx | TBD | KTX2 validation and BasisLZ/UASTC transcoding for the texture importer. | Apache-2.0 for repository-unique files, with third-party component audit required; see upstream `LICENSE.md`. | Keep container/transcode types inside the importer. Do not use upstream GPU upload helpers. |

## NVRHI Pin

Vendored upstream:

```text
Repository: https://github.com/NVIDIA-RTX/NVRHI.git
Commit:     8e8c36e37558acec333204619b95d9d2fcdc4a79
```

The current build compiles NVRHI common, validation, and Vulkan backend sources through `Vendor/NVRHI.premake.lua`, links them into the editor/sandbox, and probes `nvrhi::getFormatInfo` at renderer startup. On Windows/MSVC it also compiles NVRHI's D3D12 backend and the engine creates a native D3D12 device plus graphics/compute/copy queues before wrapping them in an NVRHI device. The default editor presentation path then uses an engine-owned DXGI swapchain, D3D12 render-target/SRV descriptor heaps, ImGui's DX12 backend, and a renderer-owned viewport texture. The explicit Vulkan path creates a Vulkan 1.3 instance, GLFW surface, graphics/present queue, NVRHI device, FIFO swapchain, and ImGui Vulkan presentation without a Vulkan SDK link dependency. This Vulkan editor path is runtime-verified on Windows through MSVC and MinGW, on Linux X11 through WSLg with Mesa llvmpipe, and in hosted Ubuntu CI through Xvfb with Mesa lavapipe. The macOS MoltenVK implementation and strict hosted x86_64 smoke are present but remain unverified until the resulting hosted run passes.

## cgltf Pin

Vendored upstream:

```text
Repository: https://github.com/jkuhlmann/cgltf.git
Tag:        v1.15
Commit:     360db1a95480fe102ae9c69b27c5d101167ff5ba
```

`GltfImporter` uses cgltf to parse and validate `.gltf`/`.glb` sources, load buffers (including data URIs), then write Spiral-owned mesh import manifests. It does not make cgltf data structures part of a public engine or renderer contract.

## NVRHI Platform Header Pins

These tags match the expectations recorded in `Vendor/NVRHI/CMakeLists.txt` at the pinned NVRHI commit:

```text
Vulkan-Headers: https://github.com/KhronosGroup/Vulkan-Headers.git v1.4.352
DirectX-Headers: https://github.com/microsoft/DirectX-Headers.git v1.717.0-preview
```
