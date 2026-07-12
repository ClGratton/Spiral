# Architecture Coherence Audit

Status: Living audit
Date: 2026-07-12

Purpose: audit the current engine decision set after reviewing the Fox Engine optimization video and cross-checking the existing engine documents for contradictions.

Post-audit research additions live in [MISSING_RESEARCH_AUDIT_2026.md](MISSING_RESEARCH_AUDIT_2026.md).

## Short Verdict

The architectural direction remains coherent, but the 2026-07-12 integrity pass found missing execution prerequisites, ambiguous roadmap wording, and documentation-authority gaps. Those gaps are now explicit contracts or unchecked roadmap items; no engine behavior was implemented by the documentation pass.

The core shape should remain:

```text
C++ engine core
  + Engine-owned RHI facade
  + NVRHI first backend
  + visibility-buffer opaque path
  + compact material resolve/G-buffer
  + clustered/deferred opaque lighting
  + Forward+ carve-outs for transparency/special materials
  + adaptive probe/light-field indirect lighting
  + selective screen-space, planar, and ray residual correction
  + coverage-first spatial antialiasing
  + automated asset/LOD/material tooling
  + C# gameplay + visual graphs + DOTS-like data runtime
```

No major system is currently fighting another major system once the state/barrier authority and fallback contracts below are observed.

## 2026-07-12 Integrity Pass

This pass read every workspace-authored Markdown file and compared the roadmap claims with current source/test call sites.

### Corrected Completion Claims

- The Phase 1 render-graph declaration compiler is an unused scaffold with no focused tests. It remains documented in current-state prose but is no longer a checked runtime behavior.
- The Phase 1 timestamp query pool and command operations are stubs/no-ops, and renderer GPU status remains pending. They remain documented in current-state prose and the real D3D12 timestamp item stays unchecked in Phase 3.
- The experimental x86_64 macOS presentation item remains correctly checked because strict hosted CI completed device/NVRHI creation, swapchain presentation, resize/recreation, and a successful post-resize present. Apple Silicon, scene rendering, packaging, profiling, and production qualification remain unchecked.
- A later documentation-only run and its retry reproduced a hosted timing failure after swapchain recreation but before a successful post-resize present within the four-frame smoke window; the following final-head run passed. The functional proof remains checked, while this demonstrated intermittency is an adjacent unchecked Phase 3 reliability item.

### Missing Prerequisites Added To The Roadmap

| Gap | Why it is required |
| --- | --- |
| Render-graph execution/integration | Dependency/lifetime compilation alone cannot bind resources, execute callbacks, record barriers/commands, submit queues, retire frames, or drive the scene viewport. |
| Renderer capability/qualification system | Platform names and advertised extensions cannot safely choose formats, features, queues, shader paths, or fallbacks across vendors/devices. |
| Portable scene-shader path | Vulkan scene rendering cannot follow from a D3D12-only compiler; shared DXIL/SPIR-V outputs, reflection/layout validation, conventions, caching, and diagnostics are prerequisites. |
| Scene render snapshot | The renderer needs immutable backend-neutral camera/mesh/material/light input rather than editor state or backend-native types. |
| Descriptor/bindless model | Material, draw-cluster, texture, and structured-buffer tables require capacity, update, retirement, error-resource, writable-resource, and bounded-fallback rules. |
| Virtual-geometry page format and streamer | Phase 7's streaming exit criterion requires deterministic cooked pages, dependency metadata, feedback, upload, eviction, GPU retirement, and coarse fallbacks. |
| Baked-lighting base data/bake path | Time-keyed lighting cannot exist before a versioned single-time probe/lightmap artifact and preview/final bake or validated import path. |
| Ray-tracing RHI contracts | BLAS/TLAS policy assumes acceleration-structure resources, build/update/compaction, ray pipeline/table binding, synchronization, capability reporting, and raster fallback. |

### Ownership And Ordering Corrections

- `PLAN.md` is now explicitly the only implementation-order authority. “First implementation order,” prototype lists, and proposed trees in research/reference documents do not override it.
- Render-graph logical state/dependency policy is authoritative above `Engine::RHI`. A graph-owned command list uses either validated NVRHI automatic barriers or explicit graph-derived RHI barriers for its whole recording lifetime; it cannot mix both ambiguously.
- Production macOS scene qualification is a late Phase 3 gate after the shared shader, Vulkan `Engine::RHI` scene, and render-graph paths. It is not implied by presentation bootstrap.
- The Phase 13 guided project wizard now explicitly extends the basic Phase 2 project creator rather than duplicating it.
- Phase 16 packaged-player verification and production crash reporting now explicitly extend Phase 0 build/smoke and local crash-file foundations.
- Phase 12 animation/material graph compiler wording now states which Phase 10 animation and Phase 3/5 shader/material foundations it consumes.

### Intentional Future Design Work

The following areas are roadmapped but are not yet detailed enough to implement without a later design pass: shipping package/cooker format, Player packaging and clean-machine runtime dependencies, networking/replication, script/plugin/asset trust and sandboxing, save/API migration, marketplace packages, and contribution governance. They are not current Phase 3 blockers, but future agents must not infer designs from the phase titles alone.

## 2026 Research Addendum

The deeper research pass did not overturn the architecture. It added extension points:

- DLSS, XeSS, FSR, frame generation, neural denoisers, ray regeneration, SER, Cooperative Vector, RTXDI, and neural radiance caching are optional accelerators after the native baseline is good. They are not a foundation for image stability or frame time.
- Work Graphs, mesh nodes, Vulkan device-generated commands, and shader enqueue are future GPU-driven backend paths. The first implementation still uses ordinary indirect/compute/meshlet execution.
- AMD DGF/DGFS and RTX Mega Geometry/CLAS are optional geometry backend/cook targets. Content must remain portable and decodable to ordinary meshlets.
- OpenPBR, MaterialX, USD, OpenAssetIO, glTF, and KTX2 are import/authoring/pipeline bridges, not the engine's hot runtime representation.
- idTech 8 and Neural Light Grid are valuable GI references, but their temporal/history-dependent pieces are not baseline-compatible unless bounded, debug-visible, and replaceable by current-frame/spatial paths.

## Fox Engine Takeaways

The linked video is Threat Interactive's "Took For Granted: Why Fox Engine Is So Crazy Optimized." It lines up with Adrian Courreges' MGSV frame study and the public Fox Engine/GDC material: Fox Engine was a classic deferred renderer with a terrain depth prepass, compact G-buffer, material IDs, probe lighting, screen-space reflections, strong packing, practical shadow choices, and a photographic validation culture.

What this engine should learn from Fox:

- Photographic validation matters as much as shader novelty.
- Material IDs are a powerful way to get richer BRDF/material behavior without a huge G-buffer.
- A selected occluder prepass can save expensive downstream shading.
- Probes, SSR, planar reflections, and shadow maps are still useful when they are used with confidence rules.
- Texture packing should consider pass usage, not only memory size.
- Tone mapping/exposure are core rendering systems, not cosmetic post effects.

What this engine should not copy:

- Lambert + normalized Blinn-Phong as the material foundation.
- FXAA as the main antialiasing plan.
- Naive half-resolution upscaling for indirect light.
- Static cubemaps for close glossy/mirror motion.
- Global front-face shadow culling.
- A classic deferred renderer that assumes one hard visibility sample per pixel without coverage handling.

## Conflicts Found And Fixed

| Area | Issue | Resolution |
| --- | --- | --- |
| Geometry wording | The older research doc said "software raster for tiny triangles," which could imply subpixel Nanite density as a goal. | Reworded to keep cluster hierarchy/GPU culling, but clamp runtime density before it becomes subpixel or quad-inefficient. |
| Fox prepass vs visibility buffer | Fox-style terrain prepass could be misread as shaded G-buffer duplication. | Added selected occluder prepass contract: depth/coverage/visibility ID only. |
| Visibility ID vs material ID | "ID buffer" language could blur primitive visibility IDs and material/BRDF IDs. | Split them explicitly: `VisibilityID` decodes draw cluster/local triangle; `MaterialID`/`brdfParamIndex` selects BRDF parameters. |
| Texture packing | Channel packing was specified, but shadow/depth pass usage was under-specified. | Added rule to separate opacity/cutout data when shadow/depth passes do not need base color RGB. |
| Shadow exclusion | The docs allowed shadow exclusion but did not mention culling mode policy. | Added per-object/per-material shadow culling modes and rejected global front-face culling. |
| Tone mapping | The engine mentioned filmic/GT tone mapping but did not make color pipeline a contract. | Added tone mapping/color pipeline contract and profile evaluation list. |

## Confirmed Core Choices

### Renderer Type

The engine is not pure deferred and not pure Forward+.

Correct position:

```text
Opaque: visibility buffer -> material resolve -> compact G-buffer -> clustered/deferred lighting.
Special/transparent: Forward+ using the same light grid and compatible material model.
```

This integrates well with:

- Material-ID BRDF lookup.
- Exact visible-pixel material evaluation.
- Ray residual classification.
- Spatial reconstruction guide buffers.
- MSAA/analytic coverage carve-outs.

### Antialiasing

The AA decision is still correct:

```text
Prevent aliasing at the source, then use spatial cleanup.
```

Required stack:

- Stable LOD and topology.
- Coverage/MSAA/analytic edge handling.
- Alpha-to-coverage or coverage-aware masked paths.
- Specular AA, normal filtering, mip correctness, roughness remapping.
- CMAA2/SMAA-style spatial cleanup.

TAA remains optional only as a non-baseline quality mode. It must not be required for the image to look stable.

### Geometry And LOD

The correct choice is not "classic LOD only" and not "Nanite at any density."

Correct position:

```text
Use a cluster hierarchy and GPU-driven selection, but target stable 1-8 pixel-ish triangle coverage, avoid subpixel density, and optimize topology for quad utilization.
```

This matches the user goal: scan assets and high detail are welcome, but the importer must bake detail into textures, normals, height/depth/micro-shadow data, and better LODs before geometry becomes a performance and stability problem.

### Indirect Lighting

The probe/GI decision is coherent:

```text
Adaptive probe/light-field volume is the stable indirect backbone.
Lightmaps are high-frequency static detail, not a separate lighting universe.
Static and dynamic objects consume the same `IndirectLightingSample`.
```

Screen-space GI/AO and sparse RT are corrections, not the foundation.

### Reflections

The reflection stack is coherent:

```text
SSR candidate -> planar or RT for high-value/missing data -> probes/cubemaps for rough/distant fallback.
```

Fox Engine reinforces this: SSR and probes are not obsolete, but they need confidence masks and cannot be trusted for close moving mirror/glossy detail.

### Shadows

The shadow decision is coherent:

```text
Optimized shadow maps are the stable base.
Sparse RT visibility residuals correct important or uncertain regions.
Receiver-aware caster culling and proxy shadows control cost.
```

Shadow object exclusion is good only when it is conservative, debuggable, and receiver-aware.

### RHI

The RHI decision is still correct:

```text
Use NVRHI first behind `Engine::RHI`.
Keep NRI as advanced/portable second backend candidate.
Do not use bgfx for the main renderer.
```

NVRHI gives state/lifetime guardrails, parallel command recording, ray tracing, meshlet support, and multi-queue support. NRI is lower-level and broader in API/platform goals, but expects us to own more synchronization and lifetime logic. bgfx is useful for tools/viewers, not for the core renderer.

### Language And Runtime

The language/runtime choice is coherent:

```text
C++ for the engine core.
C#/.NET for user gameplay scripting.
Blueprint-like graphs as a front-end over C#/IR/native jobs.
DOTS-like archetype/chunk ECS and native job system under the approachable scene facade.
Slang-first shader authoring.
```

This supports the accessibility goal without weakening the renderer.

## Remaining Intentional Open Questions

These are not contradictions. They are prototype decisions:

- Exact probe structure for v0: sparse brick grid, octree, cascaded grid, artist-placed probes, or hybrid.
- Exact default tone mapper after validation: GT-style, AgX-style, ACES/filmic, or Khronos PBR Neutral profile.
- Exact per-sample visibility representation for MSAA/coverage-heavy opaque edges.
- Whether NRI or a custom Vulkan/D3D12 backend becomes backend 2.
- Exact ECS implementation: build engine-owned ECS after studying Flecs, EnTT, Bevy ECS, and Unity DOTS.
- Exact SSR denoising/reconstruction budget without temporal history.

## Implementation Guardrails

Before writing renderer code:

1. Start with `Engine::RHI`, not raw NVRHI types leaking through the renderer.
2. Implement visibility ID decode exactly as specified in `RENDERER_IMPLEMENTATION_CONTRACTS.md`.
3. Implement material IDs as a separate BRDF/material table path.
4. Add selected occluder prepass only after counters show it saves work.
5. Add debug views early: visibility ID, material ID, LOD, overdraw, quad waste, shadow casters, reflection source, indirect source, tone mapper, probe confidence.
6. Keep every temporal feature behind an optional quality flag. Baseline validation uses current-frame/spatial results only.

## Sources

- Threat Interactive, Took For Granted: Why Fox Engine Is So Crazy Optimized: https://www.youtube.com/watch?v=aB5qxp6SPPQ
- Metal Gear Solid V Graphics Study: https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/
- Photorealism Through the Eyes of a FOX, GDC Vault: https://www.gdcvault.com/play/1031807/Photorealism-Through-the-Eyes-of
- Game Developer, FOX engine brings Kojima Productions one step closer to photo-reality: https://www.gamedeveloper.com/programming/fox-engine-brings-kojima-productions-one-step-closer-to-photo-reality
- NVRHI: https://github.com/NVIDIA-RTX/NVRHI
- NRI: https://github.com/NVIDIA-RTX/NRI
- Khronos PBR Neutral Tone Mapper: https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md
- Blender AgX color management notes: https://developer.blender.org/docs/release_notes/4.0/color_management/
- ACES project: https://www.oscars.org/science-technology/sci-tech-projects/aces
