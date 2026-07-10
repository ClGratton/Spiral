# KTX2/Basis Texture Import Plan

Status: Accepted import plan v1
Date: 2026-07-10

## Decision

Use KTX2 as the portable texture container and Basis Universal as the LDR universal payload. Cook imported texture sources into Spiral-owned, target-specific texture pages and metadata. KTX2/Basis data is an import and delivery format, not a renderer API or the permanent runtime texture representation.

The first implementation will vendor a pinned, audited subset of Khronos KTX-Software (`libktx`) behind an `Engine::TextureImporter` boundary. `libktx` owns container validation and Basis transcoding; Spiral owns asset identity, texture semantics, cooked artifacts, streaming, RHI resource creation, and fallback behavior.

Do not use KTX-Software's OpenGL or Vulkan upload helpers. Texture upload must pass through `Engine::RHI` so D3D12, Vulkan, and future Metal paths share the same engine-owned lifetime and synchronization rules.

## Scope

Initial supported source:

- 2D LDR `.ktx2` textures containing BasisLZ/ETC1S or UASTC payloads.
- Mip chains, alpha metadata, transfer function, and color primaries validated from KTX2 metadata.
- One source texture produces one stable `Texture` asset handle and one or more target-specific cooked artifacts.

Initial exclusions:

- Cube, array, 3D, video, and texture-atlas KTX2 assets.
- HDR Basis payloads, until the RHI format-capability path and lighting consumers are ready.
- Direct GPU upload of source KTX2 payloads.
- Render-thread file I/O or transcoding.
- Treating a source extension as proof of color space or material role.

PNG/JPEG/TGA/EXR decode is a later source-decoder decision. The import contract below applies after any decoder produces normalized source pixels. Existing glTF mesh import remains source/structural only; the later material-asset step will route glTF image references into this importer.

## Texture Contract

Each texture import records this metadata in the future `TextureAsset` and cooked manifest:

```text
Source path and content hash
Importer settings version
Texture role and color-space policy
Dimensions, mip count, channels, alpha mode, and transfer function
Basis mode or native source format
Target profile and selected GPU format
Per-mip byte ranges, payload hash, and resident-tail range
```

The source path remains the registry identity. Changing import settings or source content invalidates cooked artifacts, not the stable asset handle.

Validation rejects malformed containers, dimensions above project limits, missing or invalid mip data, unsupported dimensions, invalid color metadata, and payloads whose byte ranges overflow the source file. Import diagnostics must name the source, role, selected target profile, and failed validation stage.

## Role And Color-Space Policy

Material semantics choose import settings. File names and extensions do not.

| Texture role | Source interpretation | Basis profile | Desktop target | Apple/mobile target | Fallback |
| --- | --- | --- | --- | --- | --- |
| Base color / emissive | sRGB | ETC1S by default; UASTC for hero, alpha-tested, or quality-sensitive content | BC7 sRGB | ASTC 4x4 sRGB | RGBA8 sRGB |
| Normal / bent normal | Linear, normal-aware validation | UASTC with linear metrics | BC5 | ASTC 4x4 UNORM | RGBA8 UNORM |
| ORM / masks / height | Linear | UASTC | BC4/BC5 where channel layout permits; otherwise BC7 UNORM | ASTC 4x4 UNORM | RGBA8 UNORM |
| UI / fonts / pixel-critical images | Explicit color space | Native lossless or carefully reviewed UASTC | Native format selected by UI path | Native format selected by UI path | RGBA8 |
| HDR lighting / environment | Linear HDR | Not Basis in v0 | RGBA16F initially; later BC6H after quality validation | RGBA16F; later ASTC HDR after capability validation | RGBA16F |

`ETC1S` is the size-first default for ordinary color maps. `UASTC` is the quality-first default for normal, packed data, sharp alpha-tested content, and hero textures. Non-color data must use linear encoder metrics and must never silently acquire sRGB sampling.

Alpha-tested material import must preserve the material's alpha cutoff and reserve mip/coverage validation for the later material pipeline. It must not rely on post AA or a shader guess to repair an incorrectly imported mask.

## Target Selection And Streaming

Target selection is an RHI capability decision, not an OS-name switch:

```text
Texture source + import settings
  -> validated KTX2/Basis payload
  -> asynchronous target transcode/cook
  -> engine texture page + manifest per target profile
  -> async streaming upload through Engine::RHI
  -> resident mip tail or neutral fallback until higher mips arrive
```

The first target profiles are `DesktopBC`, `Astc`, and `RGBAFallback`. D3D12 and desktop Vulkan normally select the BC profile; future Metal and constrained targets select ASTC when supported; every profile has an uncompressed fallback.

Cooked shipping artifacts should normally contain the selected target format already. Runtime Basis transcoding is allowed for editor tools, starter projects, and a missing target cache, but it must execute on asset jobs or worker threads before RHI upload. It never blocks the render thread.

Each cooked artifact stores independent mip offsets and a required resident mip tail. Streaming may upload finer mips only after the tail is resident. Missing or failed pages keep the tail or a neutral role-appropriate texture bound.

## libktx Integration Boundary

When implementation starts, pin KTX-Software `v4.4.2` or a deliberately re-audited successor. Its source license and every linked third-party component must be recorded in the dependency ledger before vendoring.

The adapter responsibilities are limited to:

1. Create a KTX2 object from a named file or memory buffer.
2. Query dimensions, levels, transfer function, color primaries, alpha state, and whether transcoding is required.
3. Transcode BasisLZ/ETC1S or UASTC into the engine-selected target format.
4. Copy validated level data into engine-owned cooked pages.
5. Destroy the KTX object before the result crosses the importer boundary.

No `ktxTexture*`, Vulkan, OpenGL, or Basis implementation types may appear in `TextureAsset`, scene, material, renderer, or RHI public headers.

## Validation And Test Plan

Importer unit/smoke coverage must include:

- Valid ETC1S color texture: selects sRGB target and preserves all mip levels.
- Valid UASTC normal texture: selects a linear two-channel/normal target and never marks it sRGB.
- Valid alpha-tested color texture: retains alpha metadata and material cutoff handoff.
- Valid target fallback: unavailable compressed format cooks RGBA8 without changing texture role.
- Invalid header, invalid level index, truncated payload, oversized dimensions, and unsupported texture shape: fail with actionable diagnostics and no registry entry/cooked file.
- Reimport: source/settings change replaces the cooked artifact while preserving the stable texture handle.
- Async streaming: only the resident tail is initially uploaded; no render-thread disk I/O or transcode occurs.
- glTF material fixture using `KHR_texture_basisu`: validates its KTX2 image and assigns the imported texture handle once the material step exists.

CI will run a headless texture-import smoke for `DesktopBC` and `RGBAFallback`. An Apple runner validates the `Astc` artifact-selection path even before a Metal renderer exists. Reference fixtures must be small, licensed for redistribution, and include source hashes in their manifest.

## Rollout

1. Vendor and license-audit `libktx`; add a narrow wrapper with container-validation tests.
2. Add `TextureAsset` metadata plus versioned cooked manifest/page format.
3. Add target-format capability reporting to `Engine::RHI` and implement `DesktopBC` plus `RGBAFallback` cooking.
4. Add asynchronous asset jobs, resident-tail streaming, and deterministic import smoke fixtures.
5. Connect glTF material/image import and the material asset format to texture handles.
6. Add ASTC profile coverage with the future Metal path, then virtual-texture page generation and higher-end HDR/BC6H evaluation.

## References

- [KTX Registry](https://registry.khronos.org/KTX/)
- [libktx KTX2 reference](https://github.khronos.org/KTX-Software/libktx/structktxTexture2.html)
- [KTX-Software v4.4.2 release](https://github.com/KhronosGroup/KTX-Software/releases/tag/v4.4.2)
- [Basis Universal](https://github.com/BinomialLLC/basis_universal)
- [Basis UASTC specification](https://github.com/BinomialLLC/basis_universal/wiki/UASTC-Texture-Specification)
