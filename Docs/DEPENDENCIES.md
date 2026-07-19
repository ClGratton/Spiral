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
| NVRHI | `Vendor/NVRHI` | Renderer abstraction vendor layer. Common, validation, Windows/MSVC D3D12, and Vulkan backend sources are built by Premake behind the engine RHI/backend boundary. | MIT, see `Vendor/NVRHI/LICENSE.txt`. | NVRHI types stay out of gameplay/editor APIs. Vulkan `Engine::RHI` core resource/clear/readback wrappers use the context-owned NVRHI device; SPIR-V/pipeline/draw and Scene integration remain pending. |
| cgltf 1.15 | `Vendor/cgltf` | glTF 2.0 parse/validation and source-buffer loading for the asset import prototype. | MIT, see `Vendor/cgltf/LICENSE`. | Parser only. Spiral owns source identity, import metadata, and cooked manifests; runtime mesh buffers and material/texture conversion remain engine work. |
| KTX-Software / libktx v4.4.2 | Ignored `Vendor/KTX-Software`, acquired by `Scripts/Setup.*` through verified `Scripts/FetchDependencies.*` | Private KTX2 container validation and BasisLZ/UASTC transcoding for `Engine::TextureImporter`. | The complete fetched source preserves upstream's broader per-file inventory in `LICENSE.md`, `LICENSES/`, and `NOTICE.md`, including the non-open Ericsson terms for optional `etcdec.cxx`; the dependency graph therefore records `NOASSERTION` for the untrimmed source checkout. The compiled subset is Apache-2.0 (KTX-Software, Basis Universal, DFD utilities), BSD-1-Clause (uthash), and the BSD-3-Clause option for zstd. | Exact source commit `4d6fc70eaf62ad0558e63e8d97eb9766118327a6`; the official annotated `v4.4.2` tag resolves to that commit. Setup fetches and verifies source identity/notices before Premake generation; the Engine statically compiles only the private KTX2 read/transcode subset. It excludes KTX1, ETC software unpack, GL/Vulkan upload helpers, ASTC decode, writers/encoders, tools, tests, and `external/etcdec/etcdec.cxx`. Nothing is staged beside executables; libktx remains a build-time static implementation detail, not a runtime or redistribution claim. |
| Vulkan-Headers | `Vendor/Vulkan-Headers` | Platform headers for the NVRHI Vulkan backend and engine-owned Vulkan presentation path. | Apache-2.0 OR MIT, see `Vendor/Vulkan-Headers/LICENSE.md`. | Pinned from NVRHI's expected tag. Vulkan symbols are loaded dynamically, so the SDK is not a build prerequisite; the runtime still requires a Vulkan 1.3 loader and ICD. |
| DirectX-Headers | `Vendor/DirectX-Headers` | Platform headers for the NVRHI D3D12 backend. | MIT, see `Vendor/DirectX-Headers/LICENSE`. | Pinned from NVRHI's expected tag. D3D12 sources are enabled only for Windows/MSVC; MinGW can use the independent Vulkan path or the NVRHI common fallback. |
| Slang v2026.13.1 | `Vendor/Slang/v2026.13.1/<platform>-<architecture>` via `Scripts/FetchSlang.*` | Pinned in-process runtime shader compiler: paired DXIL/SPIR-V on admitted Windows x86_64; SPIR-V-only on Linux/macOS. | Apache-2.0 WITH LLVM-exception; stage the package `LICENSE` and the unresolved binary-component gate in `Vendor/Slang/THIRD_PARTY_NOTICE.md`. | Official hash-verified host package only; no system Slang/DXC fallback. `Engine` links the package's `slang` proxy; generated Editor/Sandbox/EngineTests builds stage only the compiler proxy/implementation, the GLSLang optimizer needed for SPIR-V output, one standard module, and notices beside the executable. Linux executables carry `RUNPATH=$ORIGIN` and macOS executables carry `LC_RPATH @loader_path`, so the staged runtime closure is resolved beside each executable rather than through `LD_LIBRARY_PATH` or a host install. DXIL requests fail explicitly outside the admitted Windows x86_64 DXC path. These are runtime dependencies because initial compilation and hot reload occur in process. Windows/MSVC, Windows/MinGW, Linux, and macOS Slang packages are acquisition targets, not runtime qualification claims. Redistribution remains blocked pending the exact binary component/notice audit. |
| Microsoft DirectX Shader Compiler v1.9.2602 | `Vendor/DXC/v1.9.2602/windows-x86_64` via `Scripts/FetchDXC.*` | Slang's pinned in-process downstream DXIL compiler and validator on Windows x86_64. | Official archive terms are Microsoft DXC binary terms in `LICENSE-MS.txt` (Windows-only use and conditional redistribution), LLVM/NCSA in `LICENSE-LLVM.txt`, and MIT in `LICENSE-MIT.txt`; see `Vendor/DXC/NOTICE.md`. | Exact DXC tag/archive/hash selected by Slang v2026.13.1's official `FetchDXC.cmake`. Stage only `dxcompiler.dll`, `dxil.dll`, and their exact archive notices beside compiler consumers. No system, Visual Studio, or Windows SDK fallback. Generated output is not redistribution clearance; other hosts remain unqualified. |
| Vulkan Loader | Homebrew `vulkan-loader` on macOS | Loads the MoltenVK ICD for the experimental hosted macOS presentation path. | Apache-2.0. | Installed by `Scripts/Setup.sh` on macOS; version 1.4.350.1 is verified in hosted CI. Shipping packaging and pinning remain pending. |
| MoltenVK | Homebrew `molten-vk` on macOS, version 1.3.0 or newer required | Implements the Vulkan portability subset over Metal for the experimental NVRHI macOS presentation path. | Apache-2.0. | Installed by `Scripts/Setup.sh` on macOS; version 1.4.1 is verified for x86_64 editor presentation. The hosted Apple Paravirtual device requires argument buffers and `MTLHeap` allocation disabled. Apple Silicon, application bundling, normal hardware capabilities, and scene-renderer conformance remain pending. |

## Diagnostic-Only Tools That Are Not Dependencies

The official portable Intel/GameTechDev PresentMon 1.10.0 console executable used for local Windows frame-pacing evidence is not admitted, fetched, vendored, linked, staged, or shipped by this repository. A user-supplied copy may live only under ignored `output/tools/presentmon-diagnostic/`. `Scripts/CapturePresentMonCorrelation.ps1` requires its explicit path and caller-supplied SHA-256, checks that the executable reports exactly `PresentMon 1.10.0`, and records both values in the capture receipt. The locally inspected diagnostic asset has SHA-256 `e57a2f8ee1de1ef1a5516d875f1b115e881943cd729fe9c5a2f88b1dc79a8a3b`; the upstream release publishes no checksum and the binary is not Authenticode-signed, so this digest is provenance for that local evidence only, not a repository dependency pin or redistribution approval. The supervisor never downloads the tool, requests elevation, stops another ETW session, or mutates RTSS/driver state.

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
| meshoptimizer | TBD | Meshlet/cluster/vertex/index optimization. | MIT. | Asset pipeline dependency. |

## NVRHI Pin

Vendored upstream:

```text
Repository: https://github.com/NVIDIA-RTX/NVRHI.git
Commit:     8e8c36e37558acec333204619b95d9d2fcdc4a79
```

The current build compiles NVRHI common, validation, and Vulkan backend sources through `Vendor/NVRHI.premake.lua`, links them into the editor/sandbox, and probes `nvrhi::getFormatInfo` at renderer startup. On Windows/MSVC it also compiles NVRHI's D3D12 backend and the engine creates a native D3D12 device plus graphics/compute/copy queues before wrapping them in an NVRHI device. The default editor presentation path then uses an engine-owned DXGI swapchain, D3D12 render-target/SRV descriptor heaps, ImGui's DX12 backend, and a renderer-owned viewport texture. The explicit Vulkan path creates a Vulkan 1.3 instance, GLFW surface, graphics/present queue, NVRHI device, FIFO swapchain, and ImGui Vulkan presentation without a Vulkan SDK link dependency. This Vulkan editor path is runtime-verified on Windows through MSVC and MinGW, on Linux X11 through WSLg with Mesa llvmpipe, in hosted Ubuntu CI through Xvfb with Mesa lavapipe, and for editor presentation on hosted macOS 15 Intel through MoltenVK. macOS scene rendering and Apple Silicon remain pending.

## Slang Pin And Package Verification

`Scripts/ShaderToolchainPins.env` is the machine-readable pin ledger consumed
by the fetch scripts and Premake. `Scripts/FetchSlang.ps1` and
`Scripts/FetchSlang.sh` accept only the following
official `shader-slang/slang` v2026.13.1 artifacts. They verify SHA-256 before
extracting to the deterministic host directory named below, reject absolute or
parent-escaping archive members and links, and record the digest in
`.spiral-package-manifest`. Later setup accepts an installed package only when
that manifest exactly matches the admitted archive and the platform header,
import/shared libraries, compiler runtime, standard module, and license remain
present. An archive with a mismatched digest is removed and setup fails; no
system Slang or DXC lookup is allowed.

Premake exposes the selected host archive digests to Engine code as the string
macros `GE_SLANG_PACKAGE_SHA256` and `GE_DXC_PACKAGE_SHA256`. The latter is
empty outside the admitted Windows x86_64 DXC path. Runtime cache identity must
use these values instead of retyping a version-only label.

| Host package directory | Official archive | SHA-256 |
| --- | --- | --- |
| `windows-x86_64` | `slang-2026.13.1-windows-x86_64.zip` | `fa1c9bcab2cdcd3626f7a1e250dd35d606c1b84745b64627f1dd63fca3746a70` |
| `windows-aarch64` | `slang-2026.13.1-windows-aarch64.zip` | `d34469404f092b8ac9fd6b11fb6e1bd653ab03b9a8e5cbfb694707b3f08e7f75` |
| `linux-x86_64` | `slang-2026.13.1-linux-x86_64-glibc-2.27.tar.gz` | `f6db08763e38c398086d2b1d785ab7fc190bad27f29992e1be6ca3cc187884d0` |
| `linux-aarch64` | `slang-2026.13.1-linux-aarch64-glibc-2.28.tar.gz` | `dea124c2f1633b7461245f157c697fb86bd5e692680b7a1c6e14ddeb488f7b69` |
| `macos-x86_64` | `slang-2026.13.1-macos-x86_64.tar.gz` | `986fdccfb0a2f4ed811666b378df7d88978e932eba6764fc63138316e7338acf` |
| `macos-aarch64` | `slang-2026.13.1-macos-aarch64.tar.gz` | `cf58b42ba87f66f58e0de297da57f4a5c92d00b7e7f38a708d2a4244abd8d003` |

Release base URL: <https://github.com/shader-slang/slang/releases/download/v2026.13.1/>.
The standalone fetchers retain the ARM64 pins for package audit, but
`Scripts/Setup.*` fails before downloading on an ARM64 host because the current
Premake workspace and admitted DXC path are x86_64-only. ARM64 acquisition is
not a usable build or runtime qualification claim.
The complete extracted package remains in the ignored staging directory.
`Engine` uses its header/import library privately. Generated Editor, Sandbox,
and EngineTests builds stage only the `slang` proxy, `slang-compiler`
implementation, the GLSLang downstream optimizer required for SPIR-V output,
one version-matched standard-module directory, and notices; gfx, LLVM, runtime,
and GLSL-source plugins are not copied without a demonstrated consumer.
The generated executable itself owns the platform loader lookup: ELF targets use
`RUNPATH=$ORIGIN`; Mach-O targets use `LC_RPATH @loader_path`. This closes the
staged proxy-to-versioned-compiler lookup without environment-variable or
system-library fallback. Hosted Actions run `29350139365` proved the previous
Linux output lacked that lookup when `EngineTests` failed to load
`libslang-compiler.so.0.2026.13.1`; a disposable WSL probe verified the ELF
repair. Final-head hosted run `29354068102` then passed all Ubuntu and macOS
build, `EngineTests`, and platform presentation steps without an environment or
system-library fallback.
`Vendor/Slang/THIRD_PARTY_NOTICE.md` records that the official binary
archive carries `LICENSE` but no exact binary component/notice manifest. The
runtime output includes that gate and records `distribution_status=blocked` in
its toolchain manifest; it is not redistribution-cleared.

The runtime shader target contract follows the admitted downstream closure, not
the source language's theoretical targets: Windows x86_64 emits a paired
DXIL+SPIR-V package and has a nonempty `GE_DXC_PACKAGE_SHA256`; Linux/macOS
emit a SPIR-V-only package and have an empty DXC hash. A non-Windows DXIL
request fails explicitly rather than consulting PATH, a system SDK, or an
unreviewed downstream compiler. This is intentional future Vulkan package
evidence, not Vulkan scene-rendering qualification.

Pinned upstream provenance used for the unresolved component audit:

- <https://github.com/shader-slang/slang/blob/v2026.13.1/LICENSE>
- <https://github.com/shader-slang/slang/blob/v2026.13.1/.gitmodules>
- <https://github.com/shader-slang/slang/blob/v2026.13.1/.github/workflows/release.yml>
- <https://github.com/shader-slang/slang/blob/v2026.13.1/source/standard-modules/CMakeLists.txt>

## DXC Pin And Package Verification

Slang v2026.13.1's official `cmake/FetchDXC.cmake` selects Microsoft DXC
`v1.9.2602`, source commit `21d28f727ad395b59394815ef76012e432f7e4e5`,
and Windows archive `dxc_2026_02_20.zip`. Spiral uses that same compatibility
pin rather than a newer independently selected compiler patch.

```text
Release: https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2602
Archive: https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602/dxc_2026_02_20.zip
SHA-256: a1e89031421cf3c1fca6627766ab3020ca4f962ac7e2caa7fab2b33a8436151e
```

`Scripts/FetchDXC.ps1` and the Windows/MSYS path in `FetchDXC.sh` verify the
archive and its member paths before extracting it to
`Vendor/DXC/v1.9.2602/windows-x86_64`. They record and subsequently revalidate
the same installed manifest/digest rather than accepting a version-only path.
Setup fails if `bin/x64/dxcompiler.dll`, `bin/x64/dxil.dll`, or any of the
archive's `LICENSE-LLVM.txt`, `LICENSE-MIT.txt`, and `LICENSE-MS.txt` notices is
absent. Generated Windows Editor, Sandbox, and
EngineTests builds stage the compiler, validator, and renamed archive notices
beside the executable so Slang can find a known DXIL backend without consulting
PATH, Visual Studio, or the Windows SDK. This admission and execution evidence
are Windows x86_64
only. The official Slang source-build policy uses the corresponding Linux
archive only after checking both shared libraries' GLIBC requirements and
builds DXC from source on macOS; those paths are not admitted or qualified by
this change.

License sources:

- <https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2602/LICENSE.TXT>
- <https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2602/ThirdPartyNotices.txt>

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
