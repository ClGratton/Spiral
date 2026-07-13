# Technical Architecture To Roadmap Coverage

**Status:** Required traceability contract
**Date:** 2026-07-13

## Purpose

This document proves that the renderer and terrain roadmap covers the accepted technical architecture in dependency order. It maps the BRDF, hybrid render path, frame-stability research, geometry/texture/terrain pipeline, lighting/GI, reflection/shadow stack, ray residuals, multithreading, and profiling requirements into `PLAN.md`.

`PLAN.md` remains the only execution-order authority. When a technical contract changes, update this matrix and the consuming roadmap items together.

## Phase Balance Decision

Phase 3 is larger by design because it creates shared infrastructure used by every later renderer phase. It is not one implementation slice. Its ordered sub-milestones are:

1. **3A Backend Bootstrap And Qualification** - native/NVRHI bootstrap, repeated presentation evidence, capabilities, Apple Silicon.
2. **3B Frame And RHI Infrastructure** - D3D12 queue/command/upload foundation, large-world coordinates, CPU task graph, immutable render snapshots.
3. **3C Portable GPU Execution** - portable shaders, Vulkan scene RHI, executable render graph, GPU retirement, multithreaded recording, pacing, hot reload, timing.
4. **3D Scene Resources And Asset Inputs** - scene buffers, texture upload/mips, KTX2/Basis cooking, descriptor/bindless policy.
5. **3E Conventional Renderer Baseline** - clustered Forward+, calibrated HDR/exposure, photometric lights, PBR/material IDs, shadow maps, sky, debug/capture.
6. **3F Platform Qualification Gate** - representative production macOS scene/capture/package/profiling evidence or a measured Metal decision.

Splitting these into new numbered phases would force renumbering every established research phase without changing dependencies. Sub-milestones preserve the stable roadmap while making “next step” bounded and unambiguous.

Phases 4–9 are smaller because they layer algorithms onto that foundation, but they are not allowed to omit their own data, fallback, or verification requirements. This audit expanded them where earlier wording named only the headline technique.

## Foundation Ordering

| Required foundation | Must exist before | Roadmap location |
| --- | --- | --- |
| Adapter capability/qualification states and fallbacks | Portable resources, descriptors, AA, visibility, meshlets, volumes, RT | Phase 3A |
| Camera-relative/large-world coordinate contract | Scene snapshot, culling, raster, debug, RT transforms | Phase 3B |
| CPU frame task graph and immutable publication | Render extraction, parallel preparation, animation/physics integration | Phase 3B |
| Backend-neutral render snapshot | Any scene renderer or render graph pass | Phase 3B |
| Shared asynchronous DXIL/SPIR-V shader contract | Vulkan scene parity and all shared passes | Phase 3C |
| Executable render graph and GPU retirement | Multi-pass renderer, transient reuse, AA, visibility, GI, rays | Phase 3C |
| KTX2/Basis target cook and basic texture upload/mips | Phase 4 mip/filter validation and Phase 5 calibrated materials | Phase 3D |
| Descriptor/bindless model with bounded fallback | Material tables, visibility resolve, streamed textures/geometry | Phase 3D |
| Scene-referred HDR, exposure, photometric units, neutral tone map | PBR lighting, Callisto calibration, daylight and GI | Phase 3E |
| Conventional clustered/PBR/shadow/sky renderer | Frame-stability validation and later visibility/ray corrections | Phase 3E |
| Large-world coordinates, CPU task graph, immutable publication, and asset provenance | Terrain source queries, tile generation, streaming, render/collision readiness, and regeneration | Phase 7 terrain foundation |

## Accepted Technical Feature Coverage

| Architecture contract | Planned behavior | Phase and dependency notes |
| --- | --- | --- |
| Hybrid renderer | Conventional clustered/PBR baseline, then opaque visibility buffer + compact G-buffer, with clustered Forward+ for transparent/special materials | Phase 3E -> Phase 6. Special/transparent carve-outs are explicit Phase 6 work, not an implied exception. |
| Callisto/Proxima/GGX | Proxima diffuse, diffuse Fresnel, retroreflection, smooth terminator, GGX, specular falloff, optional dual lobe, compact BRDF tables, material classes, reference tests | Phase 5, after Phase 3 HDR/photometric/material-ID foundation and Phase 4 filtering/stability. |
| Non-temporal frame stability | Pass-level MSAA/coverage, alpha-to-coverage, CMAA/SMAA, specular AA, roughness/normal/mip filtering, stable LOD transitions, native captures and multi-refresh-rate motion suite | Phase 4. Phase 7 extends the already-proven transition manager to virtual clusters. |
| Visibility buffer | `R32_UINT` ID, exact 25/7 decode, `DrawClusterBuffer`, visible attribute/barycentric fallback reconstruction, explicit gradients, worst-case worklists, compact G-buffer, selected prepass, two-pass HZB | Phase 6, after portable shaders, descriptors, render graph, frame stability, and material model. |
| Coverage carve-outs | Foliage/hair/masked coverage path plus Forward+ glass/eyes/hair/particles | Phase 4 establishes coverage; Phase 6 integrates visibility exclusions with the shared light grid/material model. |
| Virtual geometry | Meshlet build, optimized/quantized topology, cluster/page format, coarse fallback, async residency, compute/indirect baseline, stable transitions, asset-class policies, optional DGF/CLAS | Phase 7. Mesh shaders remain optional; MoltenVK uses compute plus indirect indexed draws. |
| Project-shaped terrain | Selectable bounded/streamed/unbounded/hybrid profiles, deterministic source queries, versioned canonical tiles and provenance, finite authored baseline, quadtree heightfield LOD, bounded asynchronous residency, collision readiness, hybrid geometry routing, authored constraints/edits, and Terrain Diffusion keep/defer/reject evaluation | Phase 7 after Phase 3B large-world/task/snapshot foundations and the Phase 7 page/residency primitives. Phase 13 adds the authoring workflow, Phase 15 diagnostics, and Phase 16 shipping cook/provenance. No learned or infinite-world path is mandatory. |
| Instancing and merging | GPU-driven instancing plus selective static assembly/material consolidation without destroying culling, streaming, lighting-zone, LOD, or material-quality boundaries | Phase 7 asset/runtime optimization after portable indirect execution exists. |
| Texture pipeline | KTX2/Basis validation and target cook, role/color rules, upload/mips, then virtual-texture pages, mip tails, feedback, async residency/eviction/fallback | Phase 3D basic import/cook; Phase 7 virtual streaming after Phase 6 emits feedback. |
| Asset interchange | glTF foundation, KTX2/Basis textures, OpenPBR/MaterialX materials, USD/OpenAssetIO editor/studio resolution and variants, engine-cooked shipping data | Phase 2 foundation, Phase 3D textures, Phase 5 materials, Phase 13 studio/editor workflow, Phase 16 shipping cooker. |
| Probe/lightmap GI | Adaptive probes, unified static/dynamic `IndirectLightingSample`, leak/confidence, zones/portals, directional lightmaps, versioned bake data, adaptive time keys, reflection probes | Phase 8, after calibrated Phase 3 lighting and stable Phase 5 materials. |
| Spatial ambient correction | GTAO/XeGTAO with bent normals/specular occlusion and current-frame SSGI/contact bounce with validity/fallback | Phase 8 before ray AO/GI residuals. Temporal convergence is not a baseline. |
| Volumetrics | Same-frame spatially resolved fog/froxel lighting using sky/probe/zone data, with a non-2D-view-on-3D fallback | Phase 8; explicitly respects the measured MoltenVK portability constraint. |
| Reflection stack | Reflection probes -> current-frame stochastic SSR candidate/confidence -> bounded planar reflections for hero cases -> sparse RT miss fill -> special mirror/eye routing | Phase 8 probe fallback, then Phase 9 screen/planar/ray layers. No cubemap-only close mirror path. |
| Shadow stack | Stable shadow-map base, explicit caster modes, receiver-aware exclusions/proxies, sparse RT visibility residuals | Phase 3E base then Phase 9 measured optimization/correction. |
| Sparse ray residuals | Capability-gated RHI RT, object-class BLAS/TLAS policy, classifier, shadow/reflection/AO/GI residuals, same-frame reconstruction, discontinuity densification, special routing, fallback/debug | Phase 9 after raster/probe/screen-space bases exist. Unsupported RT retains functional raster/probe rendering. |
| Color pipeline | Scene-referred linear HDR, calibrated exposure, neutral baseline tone map, grading after tone map, then GT/AgX/ACES/PBR Neutral comparison | Phase 3E establishes order; Phase 5 selects/calibrates profiles. |
| Profiling and captures | Early timing/capture labels and deterministic modes, followed by complete CPU/GPU/memory/residency/overdraw/ray/shadow tooling and golden scenes | Phase 3 foundations, phase-local debug views, full product tooling in Phase 15. |
| Presentation evidence | Separate simulation/app-present/GPU/display signals, waitable/timing capability paths, and external PresentMon/platform-equivalent validation | Phase 3C instrumentation and Phase 15 full validation. |
| Optional GPU accelerators | Upscalers/denoisers, Work Graphs/device-generated commands, SER, Cooperative Vector, neural/vendor paths, DGF/CLAS | Phase 7 geometry evaluations and Phase 16 optional integrations; every path retains ordinary portable execution/content fallback. |
| Backend evolution | NVRHI remains backend 1 behind `Engine::RHI`; NRI/custom backend 2 is evaluated only after render-graph conformance tests exist | Phase 16 evaluation, not an early renderer rewrite. |

## Infrastructure That Must Not Be Deferred

- The CPU frame task graph is distinct from the GPU render graph; both are Phase 3 foundations.
- Large-world/camera-relative coordinates are established before scene buffers and RT transforms, not retrofitted in Phase 9.
- Terrain generation remains a source contract distinct from world partition and residency; finite, unbounded, and learned sources publish the same canonical versioned artifacts and cannot bypass renderer, collision, persistence, or provenance contracts.
- Photometric units and the HDR/exposure order precede material calibration; they are not delayed until probe lighting.
- Basic texture import/cooking precedes mip and material validation; virtual-texture streaming follows visibility feedback but precedes dense streamed-asset exit criteria.
- Visible attribute/gradient reconstruction is part of the visibility renderer itself, not an unspecified shader detail.
- Spatial AO, screen-space GI, SSR, planar reflections, and Forward+ special materials are named roadmap consumers because the accepted architecture depends on them.
- Heap aliasing, bindless descriptors, sample-rate interpolation, mesh shaders, ray tracing, and 2D views of 3D images all retain functional capability-gated fallbacks.

## Later-Phase Granularity

The current technical document set is renderer-heavy. Phase 7 terrain is backed by [TERRAIN_ARCHITECTURE_AND_RESEARCH.md](TERRAIN_ARCHITECTURE_AND_RESEARCH.md), Phase 11 physics is backed by its accepted contract, and Phase 12 is backed by the accepted language/concurrency contract. Phases 10, 13 outside the terrain workflow, 14, and 16 outside terrain cooking/provenance still summarize domains whose detailed ownership/data/fallback designs are not accepted yet. Their brevity is not permission to implement from headings alone.

Before entering those phases:

- Animation needs pose/skeleton/clip formats, compression, graph evaluation, root motion, task scheduling, skinning/morph publication, retarget, and motion-matching contracts.
- Physics now has an accepted planning contract in [PHYSICS_ARCHITECTURE_AND_RESEARCH.md](PHYSICS_ARCHITECTURE_AND_RESEARCH.md) and dependency-ordered Phase 11A-11D coverage for backend boundaries, fixed-step authority, determinism/state capabilities, task-graph publication, collision cooking/queries, CPU/GPU synchronization, hero-solver research, fallbacks, and qualification.
- Terrain now has an accepted planning contract in [TERRAIN_ARCHITECTURE_AND_RESEARCH.md](TERRAIN_ARCHITECTURE_AND_RESEARCH.md) and dependency-ordered Phase 7/13/15/16 coverage for project profiles, source queries, canonical artifacts, spatial LOD, streaming, collision readiness, edits/provenance, workflows, diagnostics, shipping cook policy, and the optional Terrain Diffusion bake-off.
- Automation needs tool permissions, provenance, undo/transaction boundaries, validation, and failure recovery.
- Audio/UI/save/navigation/networking need separate domain contracts; Phase 14 is a product grouping, not one implementation slice.
- Packaging needs target profiles, cooked package/manifest/versioning, runtime dependency bundling, signing/notarization, clean-machine verification, update/migration, and rollback policy.

`PLAN.md` contains an explicit design gate requiring these phases to be expanded before implementation. This is deliberate unresolved design work, not hidden renderer debt.

## Maintenance Rule

Whenever an accepted technical document adds a required runtime feature, update this matrix and insert its infrastructure before the first roadmap consumer. Whenever a roadmap item is removed or deferred, record which accepted contract changed; do not let planned behavior disappear through checklist editing alone.
