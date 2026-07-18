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

The selected devices publish conservative Bootstrap reports with adapter identity, queue mapping, queried formats, feature lifecycle state, qualification, and fallback diagnostics. D3D12 reports timestamps enabled/implemented only when the selected graphics queue exposes a nonzero frequency and the complete private RHI heap/resolve/readback/retirement path exists. Vulkan does so only with nonzero graphics-family `timestampValidBits`, positive physical-device `timestampPeriod`, and its complete private query-pool/record/non-waiting-readback/retirement path. D3D12 heap-direct indexing and enhanced barriers are disabled because the Bootstrap profile has no implemented consumer. Vulkan records buffer-device-address advertisement separately and leaves it disabled for the same reason. Optional ray, mesh, work-graph, and neural paths are not presented as usable renderer features merely because NVRHI advertises support.

The active Bootstrap report is copied into a renderer-owned read-only snapshot and presented in the editor Profiler. The diagnostics keep profile, adapter identity, qualification, queue/format decisions, feature lifecycle stages, selected fallbacks, every retained candidate rejection, and versioned consumer groups visible without leaking native or NVRHI types into the editor. D3D12 presentation support is necessarily finalized by real HWND swapchain creation after device selection; the pre-device profile can prove a direct queue but DXGI has no Vulkan-style per-surface physical-device query.

The first consumer group is `Phase3FrameTimingV1`. Device timestamp usability and renderer timing-consumer readiness are separate gates: P2B/P2C make both native RHI features usable, P3A associates raw endpoints with exact whole-frame/RenderGraph-pass identities, and P3B now publishes real asynchronous named-pass and whole-frame durations. The group begins each backend initialization on portable CPU steady-clock timing and promotes to exercised `GpuTimestamps` only after a real ready exact-frame publication. A diagnostic rendered before that publication may still show the CPU fallback; the publication marker and subsequent capability snapshot are authoritative for promotion. Unsupported devices, pending/disjoint results, and results whose exact renderer frame has already been evicted retain CPU/unavailable behavior rather than being relabeled or attached to the current frame. This does not complete benchmark headroom, Scene qualification, later consumer groups, physical-device breadth, or Production qualification.

GPU timestamp implementation is an explicit prerequisite for production frame-pacing selection and Phase 3E lighting/shading, not a late diagnostics-only enhancement. Before a pacing comparison may claim GPU headroom—and before Forward+/clustered lighting, PBR, lights, shadows, or sky/atmosphere begins—the shared RHI must allocate and record timestamp scopes, resolve them without a per-frame CPU stall, preserve frame/pass identity through readback, and publish valid whole-frame plus named-pass GPU durations. D3D12 uses timestamp query heaps; Vulkan uses timestamp query pools with timestamp-period/valid-bit handling and truthful unsupported/disjoint results. RenderGraph pass labels are the profiling scope authority. Pacing artifacts may derive target-budget headroom only from a valid duration associated with the same frame ID. CPU steady-clock timing remains a functional fallback on devices without usable timestamps, but that fallback cannot qualify a claim about GPU cost or headroom.

Delivery is split at real authority boundaries. P1 establishes the backend-neutral query-pool lifecycle and deterministic failure/reuse semantics without claiming native execution. P2A1 adds logical-validation-before-callback, exact-token publication, and bounded post-submit native-state retirement. P2A2 retains the logical pool through public-wrapper destruction before submit and adds non-mutating publication preflight. P2B is locally qualified on D3D12 through private heaps/readback/frequency. P2C is locally qualified on Vulkan through a private pool, the live NVRHI command buffer, valid-bit masking, physical period, and non-waiting exact-token collection. NVRHI's paired timer-query API is not substituted for the indexed P1 contract; raw calls remain inside RHI escape hatches and reuse the NVRHI-created device/queues/commands. `GetTimestampPeriodNanoseconds` gives P3 one portable scale.

P2D is the production asynchronous-lifetime boundary exposed before P3. Both viewport renderers submit their real clear/raster/output-handoff RenderGraph. `SubmittedRenderGraphFrameOwner` now retains up to four heap-owned graphs, authoritative frame IDs, accepted-prefix pass labels, all accepted exact tokens, and shared payloads for pass-captured resources that live outside the graph, including D3D12 mapped frame-slot constants and Vulkan per-frame constant buffers. It polls without waiting and releases a frame only when every token is complete. Same-queue final Graphics handoff preserves ordering into native presentation; any cross-queue producer remains governed by compiled dependencies. Pending constant-slot reuse, Vulkan output replacement, capacity exhaustion, duplicate identity, and invalid/failed completion are explicit failures rather than hidden waits or dropped work. Device idle is used only at shutdown before final owner release. P2D adds no timestamp measurement. P3 consumes this owner to associate query generations with frame/pass identity and publish Profiler durations/headroom. A checked foundation never makes a later stage implicitly complete.

P3 is split at its three publication authorities. P3A records one independent two-query transaction per accepted effective-Graphics RenderGraph pass because a P1/P2 transaction belongs to one command list and exact submission token; the three separately submitted viewport passes cannot share one logical transaction. Scope order encloses graph-derived barriers/acquires, named pass work, and releases, while unsupported independent queues reject before recording. Four retained three-pass frames permit 12 reserved or unresolved native query states. P2D owns each pool/generation until its exact token is terminal, then emits only raw frame/pass/token/generation endpoints; failed tokens remain retained but expose disjoint terminal observations rather than fabricated values. P3B validates exact frame/label/token/generation/period/Graphics-clock identity, converts each ready scope with the pool's nanoseconds-per-tick scale, publishes named-pass durations, and derives whole-frame time only as final-pass end minus first-pass start. It amends a bounded renderer history by exact frame ID, ignores a missing/evicted record as unavailable, exposes the latest completed result to Profiler, and promotes the capability only after ready native evidence. Summing pass durations, subtracting across queues, or attaching a late result to the current frame is forbidden. The native retirement queue serializes concurrent worker reservation/publication/retirement because graph pass recording is parallel. P3C independently retains benchmark frames and accepts an amendment only for the same retained frame ID; schema 3 publishes GPU status/duration and computes headroom only from that frame's positive effective Smooth target and ready interval. Schema 4 adds cadence/limiter source identity; schema 5 adds exact-source input-to-simulation/submit/Present engine intervals while input-to-display and click-to-photon remain unavailable. Responsive, missing-target, invalid, stale, evicted, pending, unavailable, and disjoint cases do not invent headroom. CPU fallback remains truthful; none of these slices supplies display or input-to-photon evidence.

`Phase3TransientResourcesV1` is the ordered capability prerequisite for graph transient allocation. It prefers `PlacedAliasedTransient` only when RHI reports both placed-resource and alias-barrier translation usable; otherwise it selects `NonAliasedGpuRetiredPool`. That fallback means committed or non-aliased pooled resources, with future cross-frame reuse gated only by RHI completion-token retirement. It does not mean CPU-frame-number reuse, and it does not relax graph lifetime compatibility. Current D3D12 and Vulkan/NVRHI reports select the fallback because neither translation exists in the backend-neutral RHI. The local RTX 3080 Ti headed smokes exercise selected-path diagnostics and presentation publication on both backends. This is not a native placement/alias-barrier claim, an allocator implementation, a memory-cost measurement, or hosted fallback-regression evidence.

The former single Vulkan scene checklist item is deliberately split into four ordered claims. First, an NVRHI-backed Vulkan `Engine::RHI::Device` core owns buffers, RGBA8/depth textures, clear, explicit transitions, command submission, uploads, staging readback, markers, and GPU-safe retirement, proven by deterministic offscreen clear/readback after the existing context creates its one native device and queue. Second, SPIR-V consumption, reflected binding/input layout, graphics pipeline/framebuffer state, constant update, and indexed offscreen draw/readback use that core. Third, the `NVRHIVulkanViewportSceneRenderer` acquires the published immutable `SceneRenderSnapshot`, applies the shared camera-relative raster preparation and `EditorViewport.hlsl` SPIR-V package, and records only `Engine::RHI` commands into its RGBA8/depth outputs. Fourth, the real editor viewport exports only the completed NVRHI-owned image/view to the existing native Vulkan ImGui bridge. The output finishes in shader-read-only layout; Presentation owns descriptor/sampler registration/removal, waits for GPU retirement before output replacement, and owns WSI/ImGui submission. `VulkanSceneOutputCaptureV1` distinguishes output production from `VulkanSceneOutputHandoffV1`, which requires descriptor registration, Editor `ImGui::Image` queuing, and successful post-resize swapchain presentation. The adapter creates neither a second `VkDevice` nor raw Vulkan Scene commands; raw Vulkan remains bootstrap/WSI/ImGui-only. This is Presentation-plus-current-Scene-output evidence on the named device class, not broad production-device qualification.

## Vulkan Queue Admission

Vulkan exposes Compute/Copy only when logical-device creation returned a selected concrete handle; otherwise each resolves truthfully to Graphics. Local Windows RTX 3080 Ti qualification selected Graphics family 0/index 0, Copy family 1/index 0, and Compute family 2/index 0. Hosted Vulkan jobs are fallback/regression evidence unless their logs show the admitted topology.

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

The Windows x86_64/MSVC D3D12 viewport and its DXIL consumption are exercised. The Vulkan RHI graphics path consumes the package's SPIR-V in both deterministic indexed offscreen draw and immutable-Scene-snapshot raster paths; native ImGui-output integration remains separate. Linux/macOS hosted tests compile, reflect, convention-validate, cache, diagnose, and execute a real SPIR-V-only Scene raster package rather than skipping shader compilation. MinGW Scene raster, Windows ARM64, broader physical-device classes, and production qualification remain unqualified until their matching runtime evidence is recorded; redistribution remains blocked pending the exact binary-component/notice audit recorded in [DEPENDENCIES.md](../DEPENDENCIES.md). Live source-change pipeline rebuild is also separate and remains pending.

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
