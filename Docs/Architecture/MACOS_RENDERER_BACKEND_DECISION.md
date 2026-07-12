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

## Action Items

1. [x] Enable Vulkan portability enumeration and the advertised portability-subset device extension.
2. [x] Share the dynamically loaded Vulkan entry point with GLFW.
3. [x] Add a strict swapchain-generation and post-resize-present smoke contract.
4. [x] Pass the hosted x86_64 macOS MoltenVK presentation smoke on macOS 15 Intel with MoltenVK 1.4.1 and Vulkan Loader 1.4.350.1.
5. [ ] Add native Apple Silicon generation, build, and runtime coverage.
6. [ ] Qualify scene resources, commands, shaders, and representative captures before declaring production macOS renderer support.

## Primary References

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK)
- [Vulkan portability enumeration](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_enumeration.html)
- [GLFW Vulkan guide](https://www.glfw.org/docs/3.4/vulkan_guide.html)
- [NVRHI](https://github.com/NVIDIA-RTX/NVRHI)
