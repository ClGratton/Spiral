# Renderer Capability And Qualification Contract

**Status:** Required design contract
**Date:** 2026-07-13

## Purpose

The renderer targets multiple vendors, adapters, operating systems, and API implementations through `Engine::RHI` and NVRHI. Platform names are not capability checks. This contract defines how adapters are selected, which features are enabled, how fallbacks remain functional, and what evidence is required before a roadmap item claims backend/device coverage.

## Capability Terms

Keep these states distinct:

- **Advertised:** the adapter/API reports a feature, extension, format, or limit.
- **Enabled:** device creation explicitly enabled the feature when required.
- **Implemented:** Engine/RHI/renderer code has a path that uses it correctly.
- **Exercised:** a focused runtime test ran that path on the named device/backend.
- **Qualified:** representative workloads, captures, performance, and failure/fallback behavior meet the roadmap wording.

An advertised extension is not an enabled or qualified renderer feature. Logs and UI must not collapse these states into one “supported” flag.

## Current Bootstrap Foundation

The current implementation provides a backend-neutral capability lifecycle and pure profile evaluator. Synthetic EngineTests cover lifecycle invariants, required API/queue/presentation/format/limit rejection, explicit graphics-queue fallbacks, invalid preferred-adapter fallback, format-usage validation, and deterministic candidate tie-breaking.

The selected D3D12 and Vulkan devices publish conservative Bootstrap reports with adapter identity, queue mapping, feature lifecycle state, qualification, and fallback diagnostics. D3D12 no longer reports timestamps as implemented while query recording/resolve is a stub. Vulkan records buffer-device-address advertisement separately and leaves it disabled because the Bootstrap profile has no implemented consumer. Optional ray, mesh, work-graph, and neural paths are not presented as usable renderer features merely because NVRHI advertises support.

This is not full completion of this contract. Native D3D12/Vulkan enumeration still performs backend-local selection rather than feeding all queried candidates through the shared evaluator before device creation. Required format usages and limits are not yet validated per candidate, rejection/fallback reports are not editor-visible, and consumer-specific Scene/Phase 4-9 groups and physical-device qualification remain pending.

## Adapter Selection

The engine must enumerate adapters and select by required capabilities, limits, presentation support, and user preference rather than vendor identity. Selection must record:

- backend/API and API version;
- adapter name, vendor/device identifiers, type, and driver/runtime version when available;
- required capabilities that passed;
- optional capabilities that were enabled or rejected;
- selected queue families/classes and presentation support;
- selected format and resource-limit decisions;
- the reason a candidate was rejected or a fallback backend/device was selected.

A strict backend request fails clearly if its minimum contract is not met. An ordinary launch may use a documented fallback selected before native device creation. The reported active backend must match the device actually running.

## Required Capability Groups

The capability report grows with roadmap phases. Before a consumer is implemented, its required group must exist in `Engine::RHI` and have a deterministic fallback or an explicit unsupported result.

| Consumer | Capability group |
| --- | --- |
| Phase 3 scene renderer | Resource formats/usages, sampled/storage/attachment support, queue submission/synchronization, shader target/reflection compatibility, descriptors/samplers, timestamps when timing is claimed. |
| Phase 3 transient resources | Heap placement/aliasing and alias barriers when available, plus a correct non-aliased committed/pooled fallback and GPU-retired reuse. |
| Phase 4 image quality | Sample counts, multisampled attachment/storage behavior, alpha-to-coverage, sample-rate interpolation where used, anisotropy, sampler LOD behavior, resolve formats. |
| Phase 6 visibility/material resolve | `R32_UINT` attachment/storage support, fragment barycentrics or a functional reconstruction alternative, descriptor indexing/material-table limits, subgroup behavior, indirect dispatch/draw capabilities, compact G-buffer formats. |
| Phase 7 virtual geometry | Indirect draw count or bounded fallback, buffer device address only if the implementation needs it, vertex layout/stride limits, subgroup/culling support, optional mesh shaders with indexed-indirect fallback. |
| Phase 8 probes/volumetrics | 3D images, view compatibility needed by the chosen froxel/volume layout, compressed/HDR formats, sampling/storage limits. |
| Phase 9 ray residuals | Acceleration structures, ray tracing/ray queries, shader binding/table requirements, update/compaction support, queue/build limits, and a raster/probe fallback when unavailable. |

Do not enable every advertised feature. Enable only features required by the selected renderer profile and optional paths that have an implemented, exercised consumer.

## Shader Portability

Before Vulkan scene rendering can claim parity with D3D12 scene rendering, the engine needs a shared shader contract that produces and validates the targets used by both backends. The planned direction is Slang/HLSL-style authoring, but the completion requirement is behavioral:

- deterministic DXIL and SPIR-V outputs for the shared scene passes;
- reflected resource layouts matched against C++ structures and RHI pipeline layouts;
- explicit coordinate, depth, clip-space, matrix-layout, and binding conventions;
- cache keys that include source, compiler, target, defines, and layout version;
- diagnostics that identify source, entry point, target, and backend;
- focused rendering of the same representative scene through each backend claimed.

A D3D12-only HLSL compiler does not complete the portable shader path.

## Descriptor And Binding Model

The renderer must define capacities, update/retirement rules, error descriptors, and non-bindless fallback behavior for:

- sampled textures and samplers;
- read-only structured/geometry buffers;
- explicit writable resources;
- per-frame material, instance, light, and draw-cluster tables.

Descriptor indexing/bindless use is capability-gated. If a device cannot support the selected table shape, it must choose a documented bounded-table/batched path or report the renderer profile unavailable. Streaming updates and descriptor reuse must be GPU-retired; CPU frame number alone is insufficient.

## Fallback Rules

- Optional mesh shading falls back to compute culling plus indirect indexed draws.
- Unsupported fixed sampler mip bias uses explicit LOD/gradients or another verified filter path.
- Unsupported sample-rate interpolation selects a coverage/AA path that does not require it.
- Unsupported ray tracing preserves the stable raster/probe renderer and marks ray residuals unavailable.
- Missing compressed formats select an engine-cooked fallback format without changing texture semantics.
- Missing async/secondary queues run on a compatible queue with correct synchronization; performance may change, correctness may not.

Fallbacks must be visible in diagnostics and exercised. Silently disabling a feature while leaving its UI or backend report enabled is a failure.

## Qualification Levels

| Level | Evidence | Valid claim |
| --- | --- | --- |
| Build | Project generation and compilation. | Builds for named toolchain/platform. |
| Bootstrap | Instance/device/NVRHI creation and capability log. | Device initializes. |
| Presentation | Native ImGui/swapchain present plus resize/post-resize present. | Editor presentation works. |
| Scene | Representative Engine::RHI resources, shaders, commands, and capture. | Scene renderer works on named backend/device. |
| Production | Representative content, performance/profiling, packaging, clean-machine launch, and fallback/error coverage. | Production renderer qualified for named platform/device class. |

Roadmap wording must name the achieved level and device class. Hosted software/virtual devices do not imply physical hardware qualification.

## Measured macOS Profile

The MoltenVK decision, exact hosted portability-subset matrix, unsupported-feature impact, loader/packaging model, and mesh-shader result live in [MACOS_RENDERER_BACKEND_DECISION.md](MACOS_RENDERER_BACKEND_DECISION.md). That ADR is the authoritative macOS profile; this document defines the cross-backend method.

## Verification

Capability work requires deterministic tests for selection and fallback logic plus runtime logs/tests on each claimed backend. Format and limit queries must be tested with the actual resource creation/use they protect. See [../VERIFICATION.md](../VERIFICATION.md).
