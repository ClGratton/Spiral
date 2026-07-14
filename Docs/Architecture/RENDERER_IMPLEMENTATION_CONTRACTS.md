# Renderer Implementation Contracts

Status: Draft v0.1
Date: 2026-07-06

Purpose: close low-level ambiguities before culling, visibility, streaming, ray tracing, and resolve code are written.

Adapter/feature negotiation, portable shader targets, descriptor fallback, and qualification levels are specified in [RENDERER_CAPABILITY_CONTRACT.md](RENDERER_CAPABILITY_CONTRACT.md). Frame scheduling, execution, barrier authority, and transient lifetime are specified in [RENDER_GRAPH_ARCHITECTURE.md](RENDER_GRAPH_ARCHITECTURE.md). `PLAN.md` remains the implementation-order authority.

## Summary Decisions

| Topic | Decision |
| --- | --- |
| Visibility ID | `R32_UINT` packs `drawClusterId:25` + `localTriangleId:7`. No global flat primitive table. |
| Occlusion culling | Use two-pass HZB occlusion for GPU-driven cluster rendering. |
| BLAS/TLAS | Per-object-class policy: static build/compact, rigid TLAS-only, deforming refit plus scheduled rebuild. |
| Foliage/masked coverage | Do not send alpha-to-coverage foliage through a single-ID per-pixel opaque visibility path. |
| Material resolve bins | Pixel worklist is sized to full screen/sample count, not average material distribution. |
| Streaming | Render thread never blocks on mesh/texture I/O; resident fallback is mandatory. |
| Large worlds | CPU world state can be large-coordinate; GPU rendering and RT use camera-relative translated space. |
| Antialiasing | Geometry coverage first, then CMAA2/SMAA cleanup. Post AA does not replace coverage. |
| Topology/LOD | Auto import and LOD optimize projected triangle area, shape, and quad utilization, not just triangle count. |
| LOD transitions | Stable ordered/complementary dither or morph; no TAA-dependent temporal noise baseline. |
| Multithreading | Job-system native task graph and multithreaded command recording are required architecture, not optional optimization. |
| Indirect lighting | Static and dynamic objects use one unified indirect-lighting sample API; no separate ambient model for dynamic objects. |
| Selected occluder prepass | Allowed and encouraged for large proven occluders, but it writes depth/coverage/visibility only, not shaded G-buffer outputs. |
| Material IDs | Material/BRDF IDs enrich lighting through structured tables; they are distinct from visibility IDs. |
| Color pipeline | Tone mapping is a calibrated color-science stage before grading, with explicit profiles and validation scenes. |

## 1. Visibility ID Contract

The main opaque visibility buffer uses:

```text
VisibilityID : R32_UINT

bits 31..7 : drawClusterId      25 bits
bits  6..0 : localTriangleId     7 bits
```

Decode:

```cpp
constexpr uint32_t kLocalTriangleBits = 7;
constexpr uint32_t kLocalTriangleMask = (1u << kLocalTriangleBits) - 1u;

uint32_t localTriangleId = visibilityId & kLocalTriangleMask;
uint32_t drawClusterId   = visibilityId >> kLocalTriangleBits;
```

Rules:

- `0xFFFFFFFF` is reserved as invalid/background.
- A renderable cluster/meshlet must contain at most 128 triangles.
- Prefer 64 to 124 triangles per cluster to leave room for platform quirks, mesh-shader output limits, and future flags.
- The high 25 bits index a **per-frame DrawClusterBuffer**, not a permanent global primitive table.
- `DrawClusterBuffer[drawClusterId]` stores instance ID, meshlet/cluster ID, material ID, vertex/index page references, transform reference, flags, and geometry class.
- The renderer must not allocate one flat table row for every possible `(instance, meshlet, triangle)` combination in the resident scene.
- IDs are frame-local. Persistent asset identity lives in asset/mesh/cluster tables, not in the visibility buffer.

Recommended record:

```cpp
struct DrawClusterRecord
{
    uint instanceId;
    uint meshId;
    uint clusterId;
    uint materialId;

    uint vertexPage;
    uint indexPage;
    uint transformIndex;
    uint flags; // opaque, skinned, two-sided, coverage path, etc.
};
```

Why this layout:

- 7 local triangle bits match the meshlet/cluster target.
- 25 draw-cluster bits allow about 33 million frame-visible cluster records before invalid reservation.
- Streaming stays sane because only frame-emitted draw clusters need records.
- Material resolve does one bounded record lookup plus local triangle decode.

Rejected alternatives:

- **Flat PrimitiveTable per global primitive**: too large, painful under streaming, and easy to invalidate.
- **Direct instance bits + triangle bits only**: too little structure for meshlet pages, cluster LOD, skinned buffers, and material indirection.
- **`RG32_UINT` everywhere**: more flexible, but doubles visibility bandwidth. Keep it as a debug/overflow path, not the default.

## 2. Occlusion Culling Contract

Do not rely on a single culling pass against only the previous frame HZB.

Baseline GPU culling for virtual geometry is two-pass:

```text
previous frame HZB
  -> main cull pass
      visible/uncertain clusters -> main draw list
      occluded clusters          -> occluded candidate list
  -> raster main draw list
  -> build current frame HZB
  -> post cull pass on occluded candidate list
  -> raster newly visible post draw list
```

Rules:

- Previous-frame HZB may only reject into an occluded candidate list, not permanently delete work.
- Current-frame HZB decides the post pass for previously occluded candidates.
- Camera cuts, teleports, very large camera rotations, or invalid history must disable previous-HZB rejection for that frame.
- If bounds, depth, or page residency are uncertain, classify visible/uncertain rather than culled.
- Main pass and post pass both write compatible visibility IDs.
- Post-pass geometry may be less optimal as an occluder for the current frame; correctness is more important than perfect occlusion efficiency.

This prevents fast-turn disocclusion pop-in caused by trusting stale depth.

## 3. Ray Tracing Acceleration Structure Policy

This policy consumes an `Engine::RHI` ray-tracing implementation. Before it can execute, the RHI must expose capability-gated acceleration structures, build/update/compaction commands, ray pipelines/shader-table binding, synchronization, diagnostics, and the stable raster/probe fallback defined by the roadmap and capability contract.

Acceleration structures are not updated uniformly. The policy depends on object class.

| Object class | BLAS policy | TLAS policy | Notes |
| --- | --- | --- | --- |
| Static virtual geometry | Build offline/load-time with fast-trace flags, compact when settled. | Include only RT-relevant resident instances. Rebuild active TLAS each frame. | No per-frame BLAS work. |
| Rigid props | Shared mesh BLAS. Do not touch BLAS for transform-only movement. | Rebuild active TLAS with updated transforms. | This is the common dynamic-object path. |
| Near-rigid articulated objects | Separate BLAS per rigid part, or coarse proxy BLAS. | Rebuild TLAS for moved parts. | Avoid deforming BLAS if bones mostly move rigid chunks. |
| Skinned/deforming hero mesh | Skin to GPU buffer, refit/update BLAS when topology is unchanged. | Rebuild active TLAS. | Schedule full BLAS rebuild by deformation metric or frame interval. |
| Cloth | Use simplified RT proxy or refit low-detail BLAS. | Rebuild TLAS. | Full cloth BLAS every frame is not baseline. |
| Hair | Prefer specialized proxy/curves/cards depending on RT effect. | Include only when materially important. | Avoid heavy any-hit hair in default ray residuals. |
| Foliage | Trunks/large branches can be static BLAS. Leaves use simplified opaque proxy or are excluded by quality tier. | Rebuild active TLAS. | Avoid any-hit foliage unless the quality tier explicitly pays for it. |
| Destruction/topology change | Build new BLAS asynchronously. | Swap TLAS instance when BLAS is ready. | Until ready, use previous BLAS, proxy, or raster-only fallback. |

Refit/rebuild rules:

- Refit/update only when topology and primitive count are unchanged.
- Full rebuild when topology changes, primitive count changes, or repeated refits degrade traversal quality.
- Use deformation metrics: max vertex displacement vs. original BLAS bounds, AABB volume growth, triangle normal change, or skinned bone spread.
- Start with a conservative rebuild interval for hero deformers: every 30 to 120 frames, then tune with profiling.
- Acceleration structure build/update should run async when possible and be budgeted, not allowed to consume the frame.
- Static BLAS should prefer fast trace and compaction. Dynamic BLAS should prefer fast build/update.

## 4. Masked/Foliage Coverage Contract

A single `R32_UINT` visibility buffer assumes one winning primitive per pixel. Alpha-to-coverage assumes coverage across multiple subpixel samples and can involve overlapping masked surfaces. These are not the same model.

Decision:

```text
Main opaque visibility path: hard opaque, one ID per pixel.
Masked/foliage/hair coverage path: separate coverage-aware path.
```

Rules:

- Do not route dense foliage, hair cards, or alpha-to-coverage materials through the single-ID opaque visibility buffer.
- First implementation should render masked foliage/hair through a clustered Forward+ coverage path with MSAA/alpha-to-coverage or analytic coverage.
- Optional later path: multisampled visibility buffer for masked materials, with one visibility ID per sample and unique-sample material resolve.
- Alpha-tested hard-cutout props can use visibility rendering only if the alpha test is evaluated in the visibility pass and coverage is still represented correctly.
- Translucency stays outside the opaque visibility path.
- Distant foliage should prefer stable cards, aggregate voxels, volumes, impostors, or filtered cluster representations.

This is a deliberate carve-out, not an exception discovered mid-implementation.

## 5. Material Resolve Worklist Contract

The material resolve path must be sized for worst case, not average case.

Baseline flow:

```text
Visibility buffer
  -> count visible pixels/samples per material
  -> prefix sum material offsets
  -> scatter pixel/sample coordinates into MaterialPixelList
  -> indirect dispatch material resolve by material/bin
```

Rules:

- `MaterialPixelList` capacity is full visibility sample count:
  - single-sample opaque: `width * height`
  - multisample coverage path: `width * height * sampleCount`
- A material bin may legally contain 100% of the screen.
- No fixed average-size per-material bins.
- Counts and offsets are 32-bit for normal frame sizes; assert if a path can exceed that.
- If material count exceeds the material-bin table, use an explicit overflow path: split dispatch batches or resolve with a generic bindless material shader.
- If `MaterialPixelList` allocation is too small because resolution/sample count changed, reallocate before rendering. Do not clamp silently.
- Debug builds must validate that scatter writes stay inside `[materialStart, materialStart + materialCount)`.

This accepts that a wall, sky dome, terrain, ocean, or full-screen character close-up may be one material.

## 6. Streaming And Residency Contract

The render thread must never block on mesh or texture streaming I/O.

Rules:

- Every virtual mesh hierarchy must keep a coarse fallback cluster/page resident.
- If a requested fine cluster page is missing, select the nearest resident ancestor or a proxy.
- If no valid mesh fallback exists, render an explicit error proxy and emit a residency fault.
- Texture systems must keep fallback mip tails or default textures resident.
- Missing normal/ORM/control textures must resolve to neutral defaults.
- Streaming feedback is produced by visibility/material resolve, consumed asynchronously, and applied in future frames.
- The renderer may request pages; it may not wait synchronously for disk or decompression.
- Page eviction must not remove the last valid fallback for a visible asset.

Quality rule:

```text
Missing data may reduce detail this frame. It must not stall the frame.
```

Phase 7 therefore requires a versioned engine-native cluster/page artifact with hashes/dependencies plus an asynchronous residency manager that turns feedback into storage/decompression jobs, RHI uploads, GPU-safe descriptor/page-table updates, eviction, and nearest-resident fallback. A coarse page without that lifecycle does not meet the streaming exit criterion.

## 7. World-Space Precision Contract

Support large scenes from the start.

CPU/world rules:

- Store authoritative world transforms in double precision or the canonical sector/local form defined below. Do not keep both as independently mutable authorities.
- Physics may use local simulation islands or origin rebasing.
- Asset geometry remains object-local and quantized by meshlet/cluster bounds.

Accepted persistent-coordinate contract:

- `WorldGridPolicy` version 1 defines a default sector extent of 4096 engine units, signed 64-bit sector indices on each axis, and double-precision local coordinates. The eventual project/world setting may select a different valid extent, but one policy is immutable for a loaded world and its value/version must be serialized before sector/local transforms become persistent.
- Canonical local coordinates occupy the centered half-open interval `[-extent / 2, extent / 2)`. Centering preserves symmetric local precision around a sector origin. A nonnegative `[0, extent)` interval was rejected because representable negative values immediately below zero can round to `extent` during decomposition and lose a unique canonical form.
- Decomposition and normalization use remainder-based carries, including exact and negative boundaries. Positive half-extent is represented in the next sector at negative half-extent local; negative half-extent remains in the current sector. Carries and conversions reject non-finite values and signed-sector overflow.
- Composing sector/local values into one absolute `DVec3` is explicitly approximate and exists only for compatibility or diagnostics. Persistent state and relative calculations must retain sector identity; they must not round-trip through an absolute double once sector magnitude exceeds its exact precision.
- World bounds are min-inclusive and max-exclusive. A maximum lying exactly on a sector boundary does not add the adjacent sector. Range queries return inclusive minimum/maximum sector identities and must accept an explicit enumeration budget; oversized bounds are classified without attempting unbounded enumeration.
- A sector is persistent spatial identity, not a terrain tile, streaming page, physics island, or renderer translation origin. Those systems may map their own partitions onto sectors, but none owns or silently changes the world grid.

Accepted per-view origin policy:

- Exact-camera translation remains the default and preserves the current low-jitter camera-relative behavior. Sector-snapped origins are an optional project setting, not a mandatory cost imposed on every game.
- Policy version 1 reserves a 256-unit hysteresis band for the optional sector-snapped mode. A future tracker retains its current sector on an axis until the camera crosses `extent / 2 + hysteresis` from that origin; crossing selects the sector containing the camera directly, including multi-sector teleports rather than stepping one sector per frame.
- Origin state belongs to a stable view identifier, never a vector position or the Scene. Multiple editor, game, reflection, shadow, or capture views keep independent state and publish complete immutable view/origin epochs.
- Changing the origin mode, grid policy, or discontinuously relocating a view invalidates temporal history that depends on translated coordinates. It does not rewrite persistent world transforms.

GPU/render rules:

- Convert to camera-relative translated world space as early as possible.
- GPU scene transforms are per-view translated float matrices.
- Shaders should use translated world/camera-relative positions by default.
- Absolute world position is available only through explicit high-precision helper paths.
- Ray generation, TLAS instance transforms, ray queries, and hit reconstruction must use the same translated coordinate frame for the frame.
- Debug views must expose absolute-vs-translated coordinate errors.

This avoids float32 jitter at distance without paying double-precision shader costs everywhere.

Current implementation state:

- `TransformComponent` stores authoritative canonical signed-sector/local position data. Its local coordinates are double precision; conversion to an absolute `DVec3` is transient compatibility/diagnostic output, not another mutable transform authority. `EditorCamera` still uses its current double-precision position path.
- Scene format version 4 serializes `[WorldGrid]` before all version-4 scene sections. The Scene-owned immutable policy comprises `Version`, `SectorExtent`, `OriginHysteresis`, and `OriginMode`; each entity `Transform` serializes signed sector XYZ, local XYZ, rotation, and scale. Canonical v4 inputs are required on load.
- Version-4 `[MainCamera]` stores camera settings only. It must not write or accept `[MainCamera.Transform]`: the selected camera entity's `Transform` is the sole persistent camera-transform authority.
- Version 1-3 scenes retain their legacy absolute-double wire format. They load through deterministic conversion under the default `WorldGridPolicy`; if both the selected entity transform and the legacy duplicated main-camera transform are present, the selected entity transform wins. Parsing occurs into a temporary Scene, so rejected input leaves the destination Scene unchanged.
- Backend-neutral `WorldGrid` primitives implement policy validation, canonical signed decomposition, normalization, approximate absolute-double composition, exact sector/local relative conversion, and budgeted cross-sector range classification. Relative conversion rejects signed-sector subtraction overflow, non-finite products, and values outside translated float range without mutating its output. Scene version 4 uses that policy and canonical form as persistent authority.
- `CameraView` publishes compatibility/diagnostic double camera and origin positions, its stable view ID, the chosen canonical sector/local origin when the view publisher has it, and whether this epoch invalidates translated-coordinate temporal history. `BuildCameraView` retains the standalone double path. A tracked request may additionally provide its authoritative canonical camera position; the tracker then derives the camera-relative view translation and published origin from canonical data rather than decomposing the approximate double. The current Editor viewport uses the selected main-camera Scene transform for that canonical request.
- Each immutable render-snapshot epoch carries the complete editor-viewport `CameraView`. A consumer acquires one snapshot pointer and must not combine its view/origin with mesh records from another epoch. Publishing a new origin is an atomic epoch replacement; retained older epochs are not incrementally rebased or mutated.
- `CameraViewOriginTracker` resides in the Scene module but is owned by each view publisher, keys state by stable view ID, and is used by Editor viewport publication. It is not Scene data. It consumes a supplied canonical camera position when available and only decomposes the compatibility double for legacy callers. In `ExactCamera` mode it publishes the canonical camera origin without hysteresis. In `SectorSnapped` mode it retains each origin-sector axis through the `extent / 2 + hysteresis` band and compares equal/adjacent signed sectors with local-boundary tests rather than lossy floating sector subtraction or addition. Any non-adjacent sector change selects the camera's current sector directly on that axis. A requested discontinuous relocation selects the destination sector directly and invalidates temporal history. Policy/mode changes also invalidate history; no tracker state mutates an already-published view epoch or another stable view's state.
- Backend-neutral `PrepareSceneRasterFrame` derives camera-relative model and model-view-projection matrices from one snapshot. Its snapshot carries the immutable `WorldGridPolicy` and canonical sector/local transforms; it derives a mesh-relative double from the canonical mesh and selected canonical view origin, then converts that relative value to float. It never uses an approximate absolute mesh double as the raster transform source. The current D3D12 viewport consumes that result and issues one built-in prototype-geometry draw per visible snapshot mesh. Constants use distinct allocations per draw and per fenced presentation frame slot so later instance updates cannot overwrite commands still in flight.
- Deterministic verification exercises translation, arbitrary-origin invariance, retained epochs, mesh-only motion, camera-plus-mesh origin transitions, nonzero positive/negative extreme-sector local deltas whose approximate absolute doubles alias, relative overflow/range rejection, and serialization at trillion-unit coordinates. MSVC Debug `EngineTests` pass all 31 cases. The D3D12 smoke captures the same relative built-in prototype geometry on opposite sides of a real sector boundary and a distinct intermediate mesh-only boundary crossing; accepted diagnostics include both mesh and origin canonical sector/local values. Cases A/C are byte-identical, while B moves right by 196.24 pixels and the accepted capture reports a 13.20% non-background ratio.
- Persistent Scene integration, stable-ID view-origin tracking, and canonical snapshot/raster propagation are complete for the current built-in D3D12 prototype-geometry workflow. `RHI::CommandList` now binds renderer-owned color/depth targets, deterministically clears them, records viewport/scissor/pipeline/draw work, and transitions the color target back to shader-resource state. The D3D12 command adapter creates the transient native output views and tracks each RHI texture state; the scene renderer receives only RHI command-list/texture references. Presentation still owns the recording list, swapchain, viewport SRV exposure, capture/readback, and ImGui. The D3D12 smoke verifies the complete route. Vulkan now has an equivalent offscreen Scene route: `NVRHIVulkanViewportSceneRenderer` receives the renderer-published immutable snapshot, invokes `PrepareSceneRasterFrame`, uses only the existing Vulkan `Engine::RHI` device and the package's SPIR-V member, owns RGBA8/depth output replacement after synchronous GPU retirement, and verifies the final output by readback. It does not pass native Vulkan handles to Scene/Editor/gameplay or expose its output to presentation. The narrow Vulkan NVRHI-output-to-native-presentation/ImGui handoff remains the following item. Real mesh/material GPU resources, culling, coordinate error views, physics islands, and ray/TLAS/query consumers remain future work and receive no qualification from this evidence.

## 8. Spatial Antialiasing Contract

The antialiasing stack is layered:

```text
geometry/LOD prefiltering
  + MSAA or analytic/fractional coverage on silhouettes and masked edges
  + alpha-to-coverage where appropriate
  + specular/normal-map anti-aliasing
  + CMAA2 or SMAA final cleanup
```

Rules:

- CMAA2/SMAA is a final spatial cleanup pass, not a substitute for true coverage.
- MSAA or analytic coverage is required where binary point-sampled silhouettes would crawl.
- Use decoupled visibility multisampling where practical: more visibility/coverage samples than shading samples.
- Prefer CMAA2 over FXAA as the default post AA because it preserves sharpness better and can complement MSAA.
- No TAA dependency is introduced by this stack.

Deferred renderer MSAA requirements:

- Treat MSAA as a pass-level contract, not a swapchain checkbox.
- G-buffer encodings used on edges must preserve per-sample material inputs well enough that resolves do not blend corrupted albedo, normal, roughness, or material IDs.
- Stencil/edge masks that select sample-frequency shading must be validated with debug overlays; missed edges make the renderer look like MSAA is enabled while important edges remain binary.
- AO, SSDO, decals, lighting, and post-lighting resolves must declare whether they run at pixel frequency, sample frequency, or use a coverage-aware resolve. Pixel-frequency effects must not overwrite or destroy valid subsample lighting/coverage.
- Sample count should scale with output density and content: 4x MSAA plus CMAA2/SMAA is a plausible 1080p target, while 2x MSAA plus CMAA2/SMAA may be enough at 1440p+ for many scenes.

## 9. Topology, Quad Utilization, And LOD Contract

Modern GPUs shade in 2x2 pixel quads. Small or skinny triangles waste helper lanes and can cause the same screen area to be shaded repeatedly. Ray tracing also dislikes elongated triangles because their bounds overlap more and traversal becomes less efficient.

Import and LOD generation must score:

- Projected triangle area.
- Minimum angle and aspect ratio.
- Sliver/skinny triangle count.
- Estimated 2x2 quad occupancy.
- Cluster surface-area-to-boundary ratio.
- Vertex cache locality.
- Overdraw.
- RT BVH quality.
- Silhouette, UV seam, normal seam, material seam, skinning, and morph constraints.

Rules:

- Avoid triangle fans for large disks/caps.
- Avoid long skinny triangles in raster- or RT-important geometry.
- Prefer near-equilateral triangles for filled planar regions when compatible with asset constraints.
- Use max-area/quality triangulation for caps and polygon fills where it improves quad utilization.
- Do not maximize area blindly; a large sliver is still bad.
- Far LODs should reduce interior tessellation before sacrificing silhouette stability.
- Meshlet/cluster building should prefer compact coherent groups with good area/perimeter ratio.
- Debug views must show projected triangle size, sliver score, and quad-overdraw/helper-lane waste.

## 10. LOD Transition Contract

LOD transitions must hide popping without reintroducing temporal instability or long double-draw cost.

Rules:

- Default transition mode is stable ordered/complementary dither.
- Geometric morphing is allowed when source and target topology support it.
- Impostor/card/volume swaps may use explicit crossfade representations.
- Frame-varying stochastic dither that depends on TAA is not allowed as the baseline.
- Transition duration is asset-class controlled and budgeted.
- Transition manager limits simultaneous double-drawn objects/clusters.
- If budget is exceeded, high-priority transitions continue and low-priority transitions shorten or snap.
- Both old and new LODs count toward draw/overdraw/material cost during transition.
- Debug views must expose transition mode, duration, priority, mask, and cost.

## 11. Native Multithreading Contract

The engine is natively multithreaded from the first implementation slice.

Required systems:

- Central job system with work stealing.
- Frame task graph with explicit dependencies.
- Worker-thread simulation, animation, physics, visibility, render prep, asset processing, and editor automation.
- Immutable frame snapshots for renderer consumption.
- Multithreaded command list recording.
- Async shader compilation, asset import, mesh cluster building, texture compression, streaming I/O, and light baking.
- CPU profiler lanes for every worker thread and task queue.

Forbidden baseline behavior:

- Main-thread-only scene update.
- Blocking file I/O on render/game thread.
- Blocking shader compilation during gameplay.
- Synchronous mesh/texture page loads.
- Global locks around scene/render state in hot paths.
- Slow interpreted graph logic in hot runtime paths.

Visual graphs and guided workflows may exist, but hot runtime graphs must compile or lower to native code/IR with source maps and profiling.

Current implementation state:

- `JobSystem` uses an external FIFO injection queue plus worker-local deques. Workers consume their own newest work and steal the oldest work from peers; stable worker indices and submitted/completed/stolen counts are available to diagnostics.
- `FrameTaskGraph` validates explicit dependencies, duplicate/self/invalid edges, and cycles before execution. Independent tasks at one dependency level can use worker lanes while window, UI, renderer, and other thread-affine work remains on the calling thread.
- A failed execute or graph-owned publication commit is retained in the graph result, staged output is aborted, transitive dependents are skipped, and independent branches continue. Graph execution waits only for its own worker tasks rather than calling global `JobSystem::WaitIdle`.
- `FramePublication<T>` stages mutable producer output and exposes only `shared_ptr<const T>` after the graph commits the producer successfully. A publication has one validated producer.
- Deterministic single-thread mode executes a stable registration-ordered topological traversal on the caller even when workers are live. A graph invoked recursively from a worker uses the same nonblocking deterministic path rather than waiting on its own executor. Begin/end profiler events include frame/task identity, terminal status, thread identity, worker index, and duration.
- `Application::Run` now publishes immutable frame input and executes layer update/render as caller-affine dependency nodes. The old unconditional per-frame global idle barrier is removed; shutdown still drains unrelated asynchronous work before layer destruction. Headless smoke coverage exercises both normal and deterministic modes.
- `Scene::ExtractRenderSnapshot` copies records in stable Scene storage order. Visible mesh records retain source entity IDs, mesh/material asset handles (including invalid handles for later fallback diagnostics), authoritative transforms, and shadow state; hidden meshes are omitted. Light and camera records retain copied component values and source IDs, while the snapshot's main-camera identity comes from Scene authority rather than duplicate component flags. Current source entity IDs are scene-local, nongenerational provenance for one snapshot epoch; consumers must not treat them as persistent cross-scene renderer identities.
- The Editor publishes one complete snapshot after its mutable update work, stamped with the authoritative Application frame index shared by frame-task and later submit/present telemetry rather than an editor-local counter. The epoch includes the complete editor-viewport view/origin alongside Scene records. `Renderer` atomically replaces a `shared_ptr<const SceneRenderSnapshot>` and readers retain older epochs safely; it never traverses mutable Scene storage. Snapshot records contain no editor state, resolved GPU resources, RHI handles, NVRHI/native objects, or pointers into Scene.
- Scene camera records remain authored scene data and are distinct from the snapshot's current editor-viewport view. The D3D12 viewport acquires one snapshot, prepares every visible mesh against its view/origin, and submits a prototype-geometry draw for each record. UI transform edits naturally enter the following frame's snapshot and therefore affect the following D3D12 raster epoch.

Remaining consumers include worker-thread simulation/animation/physics/visibility preparation, real scene-resource resolution, Vulkan scene raster, persistent sector transitions, culling/debug/ray consumers, multithreaded command recording, queue/occupancy/stall visualization in the Profiler panel, priorities/cancellation, and longer-lived graph reuse. Their absence must not be presented as completion of those later roadmap items.

## 12. Unified Indirect Lighting Contract

Static and dynamic objects must not live in separate indirect-lighting worlds.

Required shader/API contract:

```cpp
struct IndirectLightingSample
{
    float3 diffuseIrradiance;
    float3 bentNormal;
    float  diffuseOcclusion;
    float3 specularRadiance;
    float  specularOcclusion;
    float  confidence;
    uint   sourceFlags;
};
```

Rules:

- Every lit material receives an `IndirectLightingSample`.
- Static surfaces may use lightmaps as the high-frequency source, but still go through the same indirect-lighting function.
- Dynamic/skinned objects sample probe/light-field volumes, with multiple anchors or proxy/per-pixel sampling for large objects.
- Screen-space GI/AO may add current-frame local correction, but may not replace the probe/light-field backbone.
- If probe coverage is missing, fall back to sky SH/SG and emit a debug warning.
- Zone/portal/time-of-day GI blending must work for both static and dynamic objects.
- Debug views must show indirect source flags: lightmap, probe, light-field, screen-space, ray, sky, fallback/error.

## 13. Selected Occluder Prepass Contract

The engine may use a Fox-style partial prepass idea, but it must be expressed through the modern visibility pipeline.

Allowed prepass candidates:

- Terrain tiles.
- Buildings, cliffs, walls, large props, HLOD cells, and other high-confidence occluders.
- Expensive opaque materials whose hidden pixels would otherwise waste material resolve or lighting work.

Rules:

- The prepass writes depth, coverage, and compatible visibility IDs only.
- It must not write shaded material G-buffer data.
- Alpha-tested/masked occluders require the coverage-aware path or a conservative proxy.
- Candidate selection must be automatic and backed by profiling: projected area, occlusion history, material cost, alpha mode, and command cost.
- A prepassed cluster must not disappear from material resolve; its visibility ID must decode through the same `DrawClusterBuffer` contract.
- Debug views must show prepass cost, occlusion saved, and cases where prepass work was wasted.

Rejected:

- Shaded terrain/base-pass duplication as a general rule.
- Drawing every object in a depth prepass by default.
- Treating prepass as an excuse to keep poor front-to-back ordering or bad occluder data.

## 14. Material ID And BRDF Table Contract

Material IDs are a core part of the "not plastic" material strategy.

Rules:

- `VisibilityID` identifies the visible draw cluster and local triangle.
- `MaterialID` or `brdfParamIndex` identifies the material class and BRDF parameter row.
- Material IDs live in `DrawClusterRecord`, compact G-buffer data, or material worklists depending on pass needs.
- Lighting fetches Callisto/Proxima/GGX parameters from structured buffers, not from a bloated per-pixel G-buffer.
- Material IDs must be stable enough for debugging, profiling, authoring, and material calibration captures.
- The renderer must expose a material-ID debug view and a "BRDF parameter heatmap" view.

Do not:

- Pack permanent asset GUIDs into the G-buffer.
- Require a new lighting shader permutation for every material instance.
- Use material IDs to hide bad texture packing or missing calibrated parameters.

## 15. Color Pipeline And Tone Mapping Contract

Tone mapping is not an arbitrary final-post effect. It is part of the physical-material and exposure system.

Rules:

- Internal lighting is scene-referred linear HDR.
- Exposure uses calibrated camera/eye-style controls plus artist zones where needed.
- Tone mapping happens before artistic color grading/LUTs.
- Color grading is allowed to stylize the image, but should not compensate for broken albedo, lighting units, or BRDF parameters.
- The engine must ship material/exposure validation scenes: neutral gray, saturated colors, metals, skin, wet surfaces, emissives, daylight, indoor mixed lighting, and night.
- Candidate tone mappers must be compared in those validation scenes before becoming defaults.

Baseline profiles to evaluate:

- Gran Turismo-style / simple neutral shoulder for crisp game output.
- AgX-style filmic profile for natural highlight handling.
- ACES/filmic profile for cinematic consistency.
- Khronos PBR Neutral for faithful PBR/base-color validation.

## Sources

- The Visibility Buffer, JCGT: https://jcgt.org/published/0002/02/04/
- Visibility Buffer Rendering with Material Graphs: https://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/
- Decoupled Visibility Multisampling: https://filmicworlds.com/blog/decoupled-visibility-multisampling/
- Nanite SIGGRAPH 2021 course slides: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- Nanite Virtualized Geometry documentation: https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- NVIDIA RTX Ray Tracing Best Practices: https://developer.nvidia.com/blog/rtx-best-practices/
- Vulkan Ray Tracing Best Practices for Hybrid Rendering: https://www.khronos.org/blog/vulkan-ray-tracing-best-practices-for-hybrid-rendering
- D3D12 raytracing acceleration structure flags: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_raytracing_acceleration_structure_build_flags
- Microsoft alpha-to-coverage documentation: https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-blend-state
- Intel CMAA2: https://www.intel.com/content/www/us/en/developer/articles/technical/conservative-morphological-anti-aliasing-20.html
- Vulkan MSAA sample: https://docs.vulkan.org/samples/latest/samples/performance/msaa/README.html
- Unreal Large World Coordinates Rendering: https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5
- Unity HDRP Camera-relative rendering: https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/Camera-Relative-Rendering.html
- Humus triangulation note: https://www.humus.name/index.php?ID=228&page=Comments
- Self Shadow, Counting Quads: https://blog.selfshadow.com/2012/11/12/counting-quads/
- NVIDIA, Creating Optimal Meshes for Ray Tracing: https://developer.nvidia.com/blog/creating-optimal-meshes-for-ray-tracing/
- AMD Mesh Shaders Optimization and Best Practices: https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
- Cesium dithered LOD transitions: https://cesium.com/blog/2022/10/20/smoother-lod-transitions-in-cesium-for-unreal/
- Decima official page: https://www.guerrilla-games.com/decima
- Nodes and Native Code: DECIMA's Visual Programming for Every Discipline: https://www.guerrilla-games.com/read/Nodes-and-Native
- Vulkan async compute sample: https://docs.vulkan.org/samples/latest/samples/performance/async_compute/README.html
- The Lighting Technology of Detroit: Become Human PDF: https://media.gdcvault.com/gdc2018/presentations/CAURANT_GUILLAUME_The_Lighting_Technology.pdf
- Real-Time Global Illumination using Precomputed Light Field Probes: https://research.nvidia.com/publication/2017-02_real-time-global-illumination-using-precomputed-light-field-probes
- Unreal Volumetric Lightmaps: https://dev.epicgames.com/documentation/unreal-engine/volumetric-lightmaps-in-unreal-engine
- Threat Interactive, Took For Granted: Why Fox Engine Is So Crazy Optimized: https://www.youtube.com/watch?v=aB5qxp6SPPQ
- Metal Gear Solid V Graphics Study: https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/
- Photorealism Through the Eyes of a FOX, GDC Vault: https://www.gdcvault.com/play/1031807/Photorealism-Through-the-Eyes-of
- Khronos PBR Neutral Tone Mapper: https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md
- Blender AgX color management notes: https://developer.blender.org/docs/release_notes/4.0/color_management/
- ACES project: https://www.oscars.org/science-technology/sci-tech-projects/aces
