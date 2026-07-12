# ADR-001: Use MoltenVK Through NVRHI For The First macOS Renderer Path

**Status:** Accepted
**Date:** 2026-07-12
**Deciders:** Spiral renderer maintainers

## Context

Spiral needs a macOS renderer without replacing the engine-owned `Engine::RHI` boundary or creating a second scene renderer. The current production-prototype backend is NVRHI, which supports D3D12 and Vulkan but has no Metal backend. The repository already builds NVRHI's Vulkan sources on macOS and GLFW already creates a `VK_EXT_metal_surface` backed by a `CAMetalLayer`.

NVRHI does not list macOS as an upstream-supported platform. MoltenVK implements the Vulkan portability subset over Metal, so it can validate the existing editor presentation bridge but does not by itself prove full scene-renderer or feature parity.

## Decision

Use MoltenVK as the first macOS renderer bridge through the existing NVRHI Vulkan backend. The engine still creates the native Vulkan bootstrap and presentation objects, wraps the selected device with `nvrhi::vulkan::createDevice`, and keeps raw Vulkan confined to WSI and Dear ImGui integration.

The first completion gate is a strict hosted x86_64 macOS editor presentation smoke. It must prove:

- portability enumeration and the portability-subset device extension are enabled;
- device creation returns the NVRHI-backed Vulkan renderer;
- the native ImGui swapchain presents;
- a window resize creates a later swapchain generation;
- a present succeeds on that post-resize generation.

Apple Silicon and production scene-renderer conformance remain separate unchecked work. Native Metal remains a future option only if measured MoltenVK capability, correctness, performance, packaging, or tooling gaps justify the cost of another `Engine::RHI` backend.

## Options Considered

### MoltenVK Through NVRHI

| Dimension | Assessment |
| --- | --- |
| Implementation complexity | Low for presentation; medium/high for production qualification |
| Architecture fit | Strong; reuses the Vulkan/NVRHI ownership path |
| Upstream support | Experimental; NVRHI officially targets Windows and Linux |
| Portability | Vulkan portability subset over Metal |
| Verification cost | Hosted macOS GUI smoke plus later scene conformance |

Pros:

- Preserves the existing renderer boundary and NVRHI device contract.
- Reuses GLFW surface creation, Vulkan presentation, and ImGui integration.
- Avoids a parallel macOS-only scene renderer during Renderer V1.

Cons:

- NVRHI does not officially support macOS.
- MoltenVK exposes a Vulkan subset and needs capability-specific qualification.
- Shipping requires pinned runtime packaging, codesigning, and architecture coverage beyond Homebrew-based CI.

### Native Metal `Engine::RHI` Backend

| Dimension | Assessment |
| --- | --- |
| Implementation complexity | High |
| Architecture fit | Valid only as a complete second RHI backend |
| Upstream support | Native Apple API and tools |
| Portability | macOS-specific |
| Verification cost | Full resource, command, shader, presentation, and scene conformance |

Pros:

- Direct Apple platform support, profiling, and feature access.
- No Vulkan portability-subset translation constraints.

Cons:

- NVRHI has no Metal backend.
- Requires a new device/resource/command/pipeline implementation, Objective-C++ integration, a Metal shader path, CAMetalLayer presentation, and ImGui Metal wiring.
- Duplicates substantial renderer-backend work before the shared scene renderer is mature.

## Consequences

- macOS presentation can share the same strict smoke contract as Windows and Linux Vulkan.
- Portability extensions are selected by capability on every platform, not by vendor identity.
- CI uses an architecture-matching x86_64 macOS runner until Premake and artifact paths support Apple Silicon natively.
- The hosted Apple Paravirtual device requires Metal argument buffers and `MTLHeap` allocation to be disabled for the presentation smoke. These are CI-device constraints, not production renderer defaults.
- Passing presentation smoke is not evidence of ray tracing, mesh shaders, shared-resource interop, full Vulkan parity, or production macOS scene rendering.
- A future Metal decision must be based on captured MoltenVK gaps and measured costs.

## Portability-Subset Qualification

The engine queries `VkPhysicalDevicePortabilitySubsetFeaturesKHR` on the selected physical device before logical-device creation and logs every unsupported field. The device result is authoritative because several fields vary with the Metal device and MoltenVK configuration; source defaults alone are not a qualification result. Querying support also does not enable a feature: any later renderer path that uses one of these behaviors must explicitly request the supported feature during device creation.

The strict [hosted macOS 15 Intel run](https://github.com/ClGratton/Spiral/actions/runs/29208977496/job/86693298182) queried the Apple Paravirtual device with MoltenVK 1.4.1 and reported:

| Result | Portability-subset features |
| --- | --- |
| Supported | `constantAlphaColorBlendFactors`, `events`, `imageViewFormatReinterpretation`, `imageViewFormatSwizzle`, `multisampleArrayImage`, `mutableComparisonSamplers`, `separateStencilMaskRef`, `triangleFans`, `vertexAttributeAccessBeyondStride` |
| Unsupported | `imageView2DOn3DImage`, `pointPolygons`, `samplerMipLodBias`, `shaderSampleRateInterpolationFunctions`, `tessellationIsolines`, `tessellationPointMode` |

**Hosted CI conclusion: PASS.** The matrix-producing run completed successfully, including NVRHI device creation, native ImGui presentation, swapchain recreation, and a successful post-resize present. The documentation-only follow-up was also verified by the completed, successful [final-head CI run](https://github.com/ClGratton/Spiral/actions/runs/29209189654). This directly corrects the preliminary risk list: events, image-view swizzle, separate stencil mask/reference, and triangle fans are supported on the measured device. That result is specific to the hosted device and its CI configuration; physical Intel and Apple Silicon devices still require their own query.

Impact attribution for the unsupported fields:

| Unsupported feature | Roadmap impact |
| --- | --- |
| `imageView2DOn3DImage` | No current roadmap item requires a 2D view onto a 3D image. If volumetric fog or a froxel-grid pass is added, its common 2D-slice-of-3D-image technique must be capability-checked or implemented with another layout. |
| `pointPolygons` | No current roadmap consumer. Current geometry, visibility, debug, and meshlet plans use triangles/lines or analytic coverage rather than polygon-mode points. |
| `samplerMipLodBias` | Direct constraint on Phase 4's correct mip-selection and anisotropic-filtering item. The MoltenVK path must use explicit LOD/gradients or a capability-gated equivalent instead of fixed sampler mip bias. It is not a Phase 6-only concern. |
| `shaderSampleRateInterpolationFunctions` | Constrains Phase 4's MSAA/analytic coverage and specular-AA work as well as Phase 6's coverage-aware foliage/hair/masked carve-outs. Those paths must not assume sample-rate interpolation functions on this device. |
| `tessellationIsolines` | No current roadmap consumer; the baseline geometry path is meshlet/cluster plus indexed/indirect drawing. |
| `tessellationPointMode` | No current roadmap consumer; tessellation point mode must not become a hidden geometry fallback. |

The measured gaps do not block Phase 6's basic `R32_UINT` visibility buffer, 2D HZB, material worklists, or compact G-buffer.

The measured portability fields do not block Phase 7's offline meshlet builder or a triangle-list, compute-culling, indirect indexed-draw runtime. Phase 7 must keep packed attributes within their declared stride despite `vertexAttributeAccessBeyondStride` being supported, and must obey the still-to-be-queried minimum vertex-input stride alignment. It must not acquire a hidden dependency on point-polygon or tessellation modes. `imageView2DOn3DImage` is more relevant to future volume/probe storage than to the listed geometry path.

The portability-subset struct is not a complete production capability audit. Phase 6 and Phase 7 qualification must also query descriptor indexing, buffer device address, fragment-shader barycentrics, subgroup behavior, indirect draw count, required image formats, and the portability subset's minimum vertex-input stride alignment. The hosted smoke disables MoltenVK argument buffers and `MTLHeap`, so bindless material tables and heap-backed transient aliasing require explicit testing rather than inference.

## Phase 3 Roadmap Completion Call

The checked Phase 3 item is deliberately narrow: **experimental x86_64 macOS editor presentation through MoltenVK and NVRHI Vulkan, including swapchain recreation and successful post-resize present on hosted macOS 15 Intel CI**. It should remain checked because the strict smoke proves the same resize/post-resize presentation gate required by the Linux Vulkan presentation items, and both cited CI runs finished successfully.

This does not check off a generic production macOS renderer. Native Apple Silicon generation/presentation and production scene resources, commands, shaders, representative captures, packaging, profiling, and physical-device qualification remain explicit unchecked work in `PLAN.md`.

There is no broader checked “macOS renderer backend decision and implementation” claim in the current roadmap. If wording like that is reintroduced, it must remain unchecked until it states and proves a bounded presentation or production qualification level under the renderer capability contract.

## Linking And End-User Packaging

The verified developer and CI model is **Vulkan Loader plus the MoltenVK ICD manifest**, not a direct link against `libMoltenVK.dylib`. The build defines `VK_NO_PROTOTYPES`, resolves `vkGetInstanceProcAddr` dynamically, and does not link Vulkan or MoltenVK. The strict smoke sets `DYLD_LIBRARY_PATH` for Homebrew's Vulkan Loader and `VK_DRIVER_FILES` to Homebrew's MoltenVK ICD manifest.

Vulkan-Hpp can fall back to opening a bare MoltenVK dylib/framework name when no loader is found, but that opportunistic search is not a tested or packaged engine contract.

The present arrangement is suitable for development and CI only. A clean end-user Mac currently needs the setup script's Homebrew Vulkan Loader and MoltenVK installations; the app bundle is not self-contained. The production packaging model remains open and must be chosen explicitly between:

- bundling a pinned universal Vulkan Loader, MoltenVK dylib, and bundle-local ICD manifest, preserving loader/layer and multi-ICD behavior; or
- embedding and signing a pinned MoltenVK dynamic framework/XCFramework or dylib and explicitly loading its bundle path, reducing the runtime package but changing the tested bootstrap contract.

A static MoltenVK XCFramework is another direct-link variant, but it requires a linked `vkGetInstanceProcAddr` bootstrap rather than the current dynamic-loader path. Requiring Homebrew or the Vulkan SDK on an end-user machine is not an acceptable shipping solution. Whichever option is selected must be universal (`arm64` and `x86_64`), pinned, codesigned/notarized, license-complete, and tested on a clean Mac with no package manager or Vulkan SDK installed.

## Mesh-Shader Availability

MoltenVK 1.4.1 advertises neither `VK_EXT_mesh_shader` nor `VK_NV_mesh_shader`, and it exposes no equivalent Vulkan mesh/task-shader interface that NVRHI can use. Upstream mesh-shader work remains unreleased and incomplete, so it is not a production capability.

Phase 7 must therefore keep meshlet/cluster data independent of mesh-shader availability and provide a compute-culling plus indirect indexed-draw runtime path on MoltenVK. No Phase 7 implementation is part of this decision. On other Vulkan devices, future mesh-shader enablement also requires extension and feature negotiation before NVRHI can report `Feature::Meshlets`; the current Vulkan logical-device path does not enable it.

## Action Items

1. [x] Enable Vulkan portability enumeration and the advertised portability-subset device extension.
2. [x] Share the dynamically loaded Vulkan entry point with GLFW.
3. [x] Add a strict swapchain-generation and post-resize-present smoke contract.
4. [x] Pass the hosted x86_64 macOS MoltenVK presentation smoke on macOS 15 Intel with MoltenVK 1.4.1 and Vulkan Loader 1.4.350.1.
5. [ ] Add native Apple Silicon generation, build, and runtime coverage.
6. [ ] Qualify scene resources, commands, shaders, and representative captures before declaring production macOS renderer support.
7. [ ] Choose and verify a self-contained macOS runtime packaging model on a clean end-user Mac.
8. [x] Record the hosted Apple Paravirtual portability-subset result and cross-check the Phase 4, Phase 6, and Phase 7 plans.
9. [ ] Repeat the capability record on physical Intel and Apple Silicon qualification devices.

## Primary References

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK)
- [MoltenVK 1.4.1 Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/Docs/MoltenVK_Runtime_UserGuide.md)
- [Vulkan Loader macOS driver discovery](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-discovery-on-macos)
- [Vulkan portability enumeration](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_enumeration.html)
- [GLFW Vulkan guide](https://www.glfw.org/docs/3.4/vulkan_guide.html)
- [NVRHI](https://github.com/NVIDIA-RTX/NVRHI)
