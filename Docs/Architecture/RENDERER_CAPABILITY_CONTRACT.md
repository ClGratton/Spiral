# Renderer Capability And Qualification Contract

**Status:** Required design contract
**Date:** 2026-07-14

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

The current implementation provides a backend-neutral capability lifecycle and pure profile evaluator. Synthetic EngineTests cover lifecycle invariants, required API/queue/presentation/format/limit rejection, explicit graphics-queue fallbacks, invalid preferred-adapter fallback, strict preferred-adapter failure, format-usage validation, and deterministic candidate tie-breaking.

Before native device creation, D3D12 and Vulkan now enumerate all visible adapters into the shared model and apply versioned Phase 3 Bootstrap profiles. The profiles gate minimum API level, maximum 2D texture dimension, graphics/presentation, timeline/fence synchronization, compatible compute/copy execution, `RGBA8_UNORM` sampled/color/copy use as required by the backend profile, and `D32_FLOAT` depth attachment use. Every candidate evaluation and selected fallback is retained in `DeviceCapabilities`, not only printed. Adapter identity includes a DXGI LUID or Vulkan device UUID so equal display names remain distinguishable on multi-GPU systems.

The selected devices publish conservative Bootstrap reports with adapter identity, queue mapping, queried formats, feature lifecycle state, qualification, and fallback diagnostics. D3D12 no longer reports timestamps as implemented while query recording/resolve is a stub. D3D12 heap-direct indexing and enhanced barriers are disabled because the Bootstrap profile has no implemented consumer. Vulkan records buffer-device-address advertisement separately and leaves it disabled for the same reason. Optional ray, mesh, work-graph, and neural paths are not presented as usable renderer features merely because NVRHI advertises support.

The active Bootstrap report is copied into a renderer-owned read-only snapshot and presented in the editor Profiler. The diagnostics keep profile, adapter identity, qualification, queue/format decisions, feature lifecycle stages, selected fallbacks, every retained candidate rejection, and versioned consumer groups visible without leaking native or NVRHI types into the editor. D3D12 presentation support is necessarily finalized by real HWND swapchain creation after device selection; the pre-device profile can prove a direct queue but DXGI has no Vulkan-style per-surface physical-device query.

The first consumer group is `Phase3FrameTimingV1`. It prefers GPU timestamp queries only when their advertised/enabled/implemented lifecycle is usable. Current D3D12 and Vulkan timestamp recording/resolve paths are unimplemented, so the group selects the portable CPU steady-clock timing already integrated into the frame and pass workflow, retains the native timestamp detail as its unavailable-path reason, and publishes the fallback. Headed presentation smokes mark that selected path exercised and qualify the group at Presentation independently from the device's Bootstrap level. This does not complete GPU timing, Scene qualification, later consumer groups, dedicated Vulkan queue enablement, physical-device breadth, or Production qualification.

The former single Vulkan scene checklist item is deliberately split into three ordered claims. First, an NVRHI-backed Vulkan `Engine::RHI::Device` core owns buffers, RGBA8/depth textures, clear, explicit transitions, command submission, uploads, staging readback, markers, and GPU-safe retirement, proven by deterministic offscreen clear/readback after the existing context creates its one native device and queue. Second, SPIR-V consumption, reflected binding/input layout, graphics pipeline/framebuffer state, constant update, and indexed offscreen draw/readback use that core. Third, the Scene snapshot and completed-output-to-ImGui bridge integrate it into the viewport. The adapter must never create a second `VkDevice` or submit raw Vulkan scene commands; raw Vulkan remains bootstrap/WSI/ImGui-only. A clear/readback result is Core evidence, not graphics or Scene qualification.

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

Use `--renderer-adapter=<exact name or stable ID>` (or the split-value form) to prefer a device. A missing or unqualified preference falls back to the best qualified device and records the reason. Add `--renderer-adapter-strict` to reject every nonmatching device and terminate initialization instead of falling back. Stable IDs are the unambiguous choice when multiple devices expose the same display name; they are runtime identifiers, not content or save-file identifiers.

## Required Capability Groups

The capability report grows with roadmap phases. Before a consumer is implemented, its required group must exist in `Engine::RHI` and have a deterministic fallback or an explicit unsupported result. Open-ended “all future groups” checkboxes are not completion gates: each versioned group belongs immediately before its real roadmap consumer and carries its own implemented, exercised, and qualification state.

| Consumer | Capability group |
| --- | --- |
| Phase 3 frame timing | Prefer usable GPU timestamps; portable CPU steady-clock frame/pass timing is the functional fallback. Group qualification remains separate from device qualification. |
| Phase 3 scene renderer | Resource formats/usages, sampled/storage/attachment support, queue submission/synchronization, shader target/reflection compatibility, descriptors/samplers, timestamps when timing is claimed. |
| Phase 3 transient resources | Heap placement/aliasing and alias barriers when available, plus a correct non-aliased committed/pooled fallback and GPU-retired reuse. |
| Phase 4 image quality | Sample counts, multisampled attachment/storage behavior, alpha-to-coverage, sample-rate interpolation where used, anisotropy, sampler LOD behavior, resolve formats. |
| Phase 6 visibility/material resolve | `R32_UINT` attachment/storage support, fragment barycentrics or a functional reconstruction alternative, descriptor indexing/material-table limits, subgroup behavior, indirect dispatch/draw capabilities, compact G-buffer formats. |
| Phase 7 virtual geometry | Indirect draw count or bounded fallback, buffer device address only if the implementation needs it, vertex layout/stride limits, subgroup/culling support, optional mesh shaders with indexed-indirect fallback. |
| Phase 8 probes/volumetrics | 3D images, view compatibility needed by the chosen froxel/volume layout, compressed/HDR formats, sampling/storage limits. |
| Phase 9 ray residuals | Acceleration structures, ray tracing/ray queries, shader binding/table requirements, update/compaction support, queue/build limits, and a raster/probe fallback when unavailable. |

Do not enable every advertised feature. Enable only features required by the selected renderer profile and optional paths that have an implemented, exercised consumer.

## Shader Portability

The shared shader contract uses Slang/HLSL-style authoring and produces validated per-stage packages for the current D3D12 viewport. The admitted target set is host-specific and exact: Windows x86_64 with pinned Slang v2026.13.1 plus pinned DXC v1.9.2602 produces paired DXIL+SPIR-V; Linux and macOS with their staged Slang package produce SPIR-V-only. A Linux/macOS DXIL request fails before compilation with an explicit unavailable-target diagnostic: no non-Windows DXC package or system compiler is admitted. The D3D12 RHI consumes the Windows package's DXIL; SPIR-V is presently compiled, reflected, and convention-validated artifact evidence rather than Vulkan scene execution. Normal runtime requests use asynchronous job-system fire-and-poll with atomic last-valid-package publication; deterministic-inline execution exists for focused smoke tests. Version-2 disk packages and the in-process completed-package cache use canonical SHA-256 keys over source identity/content, entry/stage, compiler identity/version and admitted package hashes, exact ordered targets, sorted defines/options/dependencies, layout/reflection versions, expected interface, and convention schema.

The implemented contract requires:

- deterministic output for every requested, admitted host target (paired DXIL+SPIR-V on Windows x86_64; SPIR-V-only on Linux/macOS);
- reflected resource layouts matched against C++ structures and RHI pipeline layouts;
- explicit coordinate, depth, clip-space, matrix-layout, and binding conventions;
- cache keys that include source, compiler, target, defines, and layout version;
- diagnostics that identify source, entry point, target, and backend;
- focused rendering through every backend claimed to consume a target.

The Windows x86_64/MSVC D3D12 viewport and its DXIL consumption are exercised. The Vulkan RHI graphics path consumes the package's SPIR-V in a deterministic indexed offscreen draw, but Scene viewport and ImGui-output integration remain separate. Linux/macOS hosted tests must compile, reflect, convention-validate, cache, and diagnose a real SPIR-V-only package; they are not permitted to skip shader compilation. Linux, macOS, MinGW, Windows ARM64, and broader device classes remain unqualified until their matching runtime evidence is recorded; redistribution remains blocked pending the exact binary-component/notice audit recorded in [DEPENDENCIES.md](../DEPENDENCIES.md). Live source-change pipeline rebuild is also separate and remains pending.

NVRHI's Vulkan graphics implementation requires Vulkan 1.3 dynamic rendering and synchronization2. The Vulkan profile queries, rejects when absent, enables, and reports both before device creation. For the current constant-buffer-only shader layout, NVRHI's constant-buffer binding offset is explicitly zero so reflected HLSL `b0` matches SPIR-V descriptor set 0/binding 0; later sampled-resource/sampler tables must define non-colliding offsets as part of their own descriptor contract.

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
