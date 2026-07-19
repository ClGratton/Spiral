# KTX2/Basis Texture Import Plan

Status: Accepted import plan v2
Date: 2026-07-19

## Decision

Use KTX2 as the portable texture container and Basis Universal as the LDR universal payload. Cook imported texture sources into Spiral-owned, target-specific texture pages and metadata. KTX2/Basis data is an import and delivery format, not a renderer API or the permanent runtime texture representation.

The first implementation will vendor a pinned, audited subset of Khronos KTX-Software (`libktx`) behind an `Engine::TextureImporter` boundary. `libktx` owns container validation and Basis transcoding; Spiral owns asset identity, texture semantics, cooked artifacts, streaming, RHI resource creation, and fallback behavior.

Do not use KTX-Software's OpenGL or Vulkan upload helpers. Texture upload must pass through `Engine::RHI` so D3D12, Vulkan, and future Metal paths share the same engine-owned lifetime and synchronization rules.

## Current Implementation Boundary

`TextureImporter::CookNormalizedRgba8` remains the decoder-to-importer fallback boundary. `TextureImporter::CookKtx2Basis` is the private libktx-backed KTX2/Basis boundary: it accepts only caller-owned bytes and explicit role/color-space settings, validates 2D LDR single-face BasisLZ/ETC1S or UASTC containers and their transfer/primaries metadata, then copies every transcoded mip into a schema-2 Spiral artifact before destroying the libktx object. No ktx type crosses a public Engine API. Schema 2 adds `HasAlpha` and block-aware mip validation; it writes only engine-owned `DesktopBC`, `Astc`, or qualified `RGBAFallback` pages. Schema-1 RGBA fallback artifacts remain readable and conservatively report `HasAlpha=true`, because that schema did not retain the source alpha state. A rejected source does not register a new asset, replace a cooked file, or mutate the caller artifact.

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
| ORM / masks / height | Linear | UASTC | BC7 UNORM in the current artifact contract | ASTC 4x4 UNORM | RGBA8 UNORM |
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

Normal `Scripts/Setup.*` fetches the exact ignored source checkout and verifies its commit/notices before generation, but no libktx binary is staged at runtime. The private static subset compiles only KTX2 read/container, DFD, Basis transcoding, and bundled zstd sources. Its explicit compile-time exclusions are KTX1, GL/Vulkan upload helpers, writers/encoders, tools, upstream tests, `external/etcdec/etcdec.cxx`, software ETC unpack, and ASTC decode. KTX-Software v4.4.2's common texture object retains one link reference to its KTX1 constructor even when `texture1.c` is excluded; the pin-specific private `Ktx2OnlyLinkShim.cpp` satisfies only that symbol by returning `KTX_UNSUPPORTED_TEXTURE_TYPE`, rather than compiling KTX1 support. Basis output targets are deterministic: BaseColor/Emissive select BC7 sRGB or ASTC 4x4 sRGB; Normal selects BC5 UNORM or ASTC 4x4 UNORM; ORM/Mask select BC7 UNORM or ASTC 4x4 UNORM; `RGBAFallback` remains RGBA8 matching the explicit color space. The first implementation deliberately does not infer role or color space from a path, extension, or container name.

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

The headless integration test carries two fixed Base64 KTX2 fixtures from the pinned v4.4.2 source (`cyan_rgb_reference_basis.ktx2`, SHA-256 `2b2ceb5627e3f70969c7d99d5c75d462d156c0df0a431c747e43c9307d6cb131`; `rg_reference_uastc.ktx2`, SHA-256 `5716c6042bb629894bb23705555d8f002691057b376c57d8e8088506ac9e67f4`) and does not depend on upstream test fixture paths. It proves ETC1S DesktopBC/RGBAFallback, UASTC normal DesktopBC/Astc, malformed-input replacement rollback, invalid enum rejection, and schema-1 fallback readability. This is headless CPU artifact evidence only; it does not qualify a backend, device, Apple runner, runtime upload, or redistribution closure.

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
