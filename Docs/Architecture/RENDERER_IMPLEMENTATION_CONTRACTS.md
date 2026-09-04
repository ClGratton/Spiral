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
| Clustered lighting | Keep directional lights global; assign point/spot volumes to bounded logarithmic view clusters with explicit overflow. |
| Direct shadows | Begin with one stable primary-directional D32 map, explicit caster/material policy, point-sampled shader PCF, and exact graph/reference verification. |

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
- Backend-neutral `PrepareSceneRasterFrame` derives camera-relative model, model-view-projection, and inverse-transpose normal matrices from one snapshot. Its snapshot carries the immutable `WorldGridPolicy` and canonical sector/local transforms; it derives a mesh-relative double from the canonical mesh and selected canonical view origin, then converts that relative value to float. It never uses an approximate absolute mesh double as the raster transform source. Both viewport backends consume the prepared artifact geometry and material row. Constants use distinct allocations per draw and per fenced presentation frame slot so later instance updates cannot overwrite commands still in flight.
- Deterministic verification exercises translation, arbitrary-origin invariance, retained epochs, mesh-only motion, camera-plus-mesh origin transitions, nonzero positive/negative extreme-sector local deltas whose approximate absolute doubles alias, relative overflow/range rejection, and serialization at trillion-unit coordinates. MSVC Debug `EngineTests` pass all 31 cases. The D3D12 smoke captures the same relative built-in prototype geometry on opposite sides of a real sector boundary and a distinct intermediate mesh-only boundary crossing; accepted diagnostics include both mesh and origin canonical sector/local values. Cases A/C are byte-identical, while B moves right by 196.24 pixels and the accepted capture reports a 13.20% non-background ratio.
- Persistent Scene integration, stable-ID view-origin tracking, and canonical snapshot/raster propagation are complete for the current artifact-geometry workflow. `RHI::CommandList` binds renderer-owned color/depth targets, deterministically clears them, records viewport/scissor/pipeline/draw work, and transitions the color target to shader-resource state; the scene renderer receives only RHI command-list/texture references. The D3D12 command adapter owns native output views, recording, swapchain, SRV exposure, capture/readback, and ImGui. Vulkan's `NVRHIVulkanViewportSceneRenderer` likewise consumes the immutable snapshot, invokes `PrepareSceneRasterFrame`, uses only the existing Vulkan `Engine::RHI` device and the package's SPIR-V member, and owns RGBA8/depth output replacement. Its completed output is explicitly `ShaderResource`/`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. The only Vulkan escape hatch is a borrowed NVRHI `VkImage`/`VkImageView` export consumed by `NVRHIVulkanPresentation`; that bridge creates/removes the Dear ImGui descriptor and sampler, waits for GPU retirement before descriptor removal/output replacement, and removes it before shutdown. Editor queues the returned descriptor only after the Scene render succeeds. After the device observes a normally submitted Vulkan RHI token terminal and finalizes its timestamp result, it runs NVRHI garbage collection once for that token without an idle wait, so completed command-list references cannot indefinitely retain the renderer outputs or their binding/framebuffer objects. Before a later submission, the device compacts only the contiguous terminal prefix: successful results become scalar history, failed IDs remain explicit, and the live suffix retains native state across incomplete and out-of-order holes. Every issued token remains queryable and dependency-valid for the device lifetime; invalid, incomplete, compacted, and cached-terminal observations do not collect again. Invalid native handles, descriptor/sampler creation failure, missing output, or failed raster leave the viewport unavailable rather than substituting a raw-Vulkan Scene path. `VulkanSceneOutputCaptureV1` verifies the renderer-owned output dimensions/content and `VulkanSceneOutputHandoffV1` is emitted only after its registered descriptor is queued by Editor ImGui and the native swapchain present succeeds, including the post-resize output/swapchain generations. Raw Vulkan remains confined to bootstrap, WSI/presentation, and ImGui. PBR material evaluation, culling, coordinate error views, physics islands, and ray/TLAS/query consumers remain future work and receive no qualification from this evidence.

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
- `PrepareSceneRasterFrame` keeps Scene extraction handle-only, acquires one opaque immutable Renderer artifact-resolver snapshot, and retains it in the prepared frame. Material rows, runtime texture variants, and mesh artifacts for that frame all resolve against this same snapshot even if a live Inspector edit, import/reimport, or file-watch event publishes a newer global generation between preparation and draw. Row zero is the default/error row. Distinct valid material handles are sorted numerically into rows `1..N`; duplicates reuse a row, while invalid/missing handles, registered wrong-type handles, and registered Material handles missing from the retained material library select row zero. That row publishes `MaterialState = {0, 1, 0, 0}`. The resulting ID is frame-local and is not an asset, entity, or visibility ID. Debouncing live edits or applying them only on widget release is not an acceptable substitute for this frame-coherence rule.
- Mesh artifact schema 2 supplies a finite normalized object-space geometric normal with every vertex. The public completion helper rejects nonfinite, zero/mixed, and non-unit authored artifact normals without mutation. Schema-1 reads and supported triangle imports without `NORMAL` derive area-weighted normals deterministically from winding. glTF core `FLOAT` normals are accepted; `KHR_mesh_quantization` adds only normalized signed `BYTE`/`SHORT` normals and only when the extension is both used and required. Malformed authored accessors, illegal quantized declarations/types, mixed present/missing artifact normals, nonfinite values, degenerate triangles, and vertices without a valid accumulated direction reject the complete candidate before publication.
- Raster preparation publishes the row-vector `S^-1 * R` inverse-transpose of each model linear transform. Nonuniform scale is supported; zero, nonfinite, or singular scale on any mesh rejects the complete prepared frame transactionally. The shared 384-byte Scene payload preserves that accepted transform and material-state prefix, then appends row-major `Model * View` and `S^-1 * R * View` matrices; both backends allocate a 512-byte aligned constant buffer. The vertex stage interpolates unnormalized view-space position and transformed normal, and the pixel stage normalizes them to derive a per-pixel view vector. Accepted submissions retain those exact constants and resource bundles through their final graph token.
- The basic per-draw PBR path consumes the already accepted frame-local material row selected by `MaterialState.x`; row zero or an asserted error state produces deterministic bright magenta scene-linear error shading. There is no structured material table or persistent shader material ID in this slice. Every shader-visible material number and both material enums are validated before a complete constant payload replaces caller state; nonfinite input fails without partial publication.
- The baseline BRDF is Trowbridge-Reitz GGX distribution, height-correlated Smith GGX visibility, Schlick Fresnel with `F0=lerp(0.04, surfaceBaseColor, metallic)`, and Disney/Burley diffuse `surfaceBaseColor * (1-metallic) * Fd` with no extra `(1-F)` multiplier. Material base color times the decoded base-color texture defines `surfaceBaseColor`; vertex color does not decorate or retint this baseline. Constant publication requires every base-color component in `[0,1]`, preserving a bounded Schlick `F0`, while finite nonnegative emissive remains scene-linear HDR. Analytical-light roughness follows the accepted Filament convention: `p=max(saturate(perceptualRoughness),0.045)`, `alpha=p^2`, and Burley `Fd90=0.5+2*alpha*LoH^2`. Base-color/ORM metallic and roughness/emissive/alpha semantics feed an unclamped float32 scene-linear sum; the common storage finalizer then applies pre-exposure and the explicit finite RGBA16F boundary below. ORM occlusion is retained for a future indirect-light term and does not attenuate the direct preview light.
- Production Scene shading consumes the immutable V3 Scene-light payload through the direct-light contract below. The former renderer-owned neutral preview remains only in shader-tool permutations that deliberately compile without a Scene payload. Tangent-space normal mapping, Callisto/Proxima controls, two-sided material policy, shadows, and indirect-light consumption remain outside this closure.
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

## 14. Clustered Light Grid Contract

The Phase 3E CPU grid is the deterministic reference for the later GPU Forward+/lighting implementation. It is built from one immutable Scene snapshot, one view, and the exact current viewport dimensions.

Rules:

- Use 64-pixel screen tiles and 16 logarithmic depth slices for the first measured prototype; treat these as tunable policy, not shader ABI.
- Keep at most 16 directional lights in a compact global list instead of copying them into every cluster. Reject the complete candidate above that hard bound; never silently truncate it or admit an unbounded future pixel loop.
- Conservatively bound point and spot lights by their range; cone rejection can narrow spot assignments only after it remains conservative.
- Store local-light membership as one CSR offset/index table with a 64-reference per-cluster cap and explicit overflow count.
- Convert canonical sector/local light positions relative to the same snapshot view origin before float view-space projection.
- Reject malformed dimensions, projection matrices, lights, and capacities transactionally. Never publish a partial grid.
- Preserve snapshot light order so overflow, captures, and the CPU/GPU reference comparison remain deterministic.
- The native Scene paths must expose tile, slice, global/local-reference, and overflow counts in a bounded smoke marker.

The CPU prototype does not make the renderer CPU-driven permanently. The GPU build must match this reference on deterministic fixtures before replacing it, and the later light consumer must retain a bounded overflow/debug path rather than silently dropping unreported lights.

## 15. Photometric Light Authoring And Publication Contract

Phase 3E light authoring uses an explicit type-dependent photometric value/unit pair. Scene persistence, immutable snapshots, clustered records, Editor readout, and the V3 renderer payload preserve that identity through the production direct-light consumer.

Accepted semantics:

- Directional lights store illuminance in lux. Point and spot lights store luminous flux in lumens. `LightPhotometricUnit` is serialized and carried beside the numeric value; inferring a missing unit is allowed only while migrating scene schemas 1 through 4.
- The default directional light is `10,000 lux`. Accepted values are finite and nonnegative, with an explicit upper bound of `1,000,000,000 lux` for directional lights and `10,000,000 lm` for point/spot lights. A type/unit mismatch, invalid enum, negative/nonfinite value, or value above the type bound rejects the whole component or scene load without replacing the destination.
- Scene schema 5 writes `PhotometricValue` followed by `Lux` or `Lumens` in every light record. Schemas 1 through 4 migrate the old unitless scalar monotonically: directional values multiply by `10,000` and local-light values multiply by `100`. Thus the previous shipped directional value `3` becomes `30,000 lux` and the clustered-grid local fixture value `20` becomes `2,000 lm`. Each positive per-type scale preserves ordering, equality, zero, and ratios among legacy lights of the same type. Cross-type ratios are deliberately not preserved because the former directional and local scalars had no shared physical dimension.
- The migration bounds retain the former Editor scalar ceiling of `100,000`: multiplied by its type's migration scale, it lands exactly on the new accepted maximum. Larger legacy values reject instead of being clamped or silently reinterpreted.
- `SceneRenderSnapshot` and `ClusteredLightGrid` records carry the exact validated double-precision value and explicit unit. These two authoring/assignment layers do not convert the value; the renderer-owned V3 payload publication prepares the finite shader coefficient described in section 15D while retaining the exact source bits.
- The Inspector obtains its unit label from the public Scene helper, retains the prior valid value when an edit is invalid, and publishes one concise public diagnostic record containing selected light type/value/unit plus the effective project EV100 and `exp2(-EV100)` exposure scale. Editor code must not duplicate type-to-unit label policy or exposure math.

Calibration boundary:

- The current scene-linear target and project exposure convention remain relative: every HDR storage write multiplies rendered scene-linear RGB by `exp2(-effectiveEV100)` before finite RGBA16F storage, with EV100 `0` producing unit scale; tone mapping must not apply that scale again. Lux and lumens now drive the direct-light equations, but this first RGB-photometric pipeline is not a spectral-radiometric calibration and does not apply a blanket `683 lm/W` conversion to an RGB tint.
- Renderer normalization and attenuation occur only at the V3 payload/direct-light boundary described below. Scene persistence and clustered assignment retain their authored units and values unchanged.

The historical authoring-only Vulkan marker said `shaderConsumption=no`. Current acceptance additionally requires the V3 payload marker and the production-`PSMain` direct-light marker in section 15D. Neither Linux marker qualifies shadows, D3D12 execution, or MoltenVK execution.

## 15A. Fixed Graphics Structured-Buffer Binding Contract

The first graphics structured-buffer path exists only for the current light-payload consumer. A pipeline may declare one pixel-stage read-only `StructuredBuffer<uint4>` at `t0,space3`, count one and stride 16. Pipeline creation rejects any different register, space, stage, resource kind, type shape, count, or stride before native creation. Command binding accepts only an exact-device, GPU-only, nonempty, stride-16 buffer with exactly `BufferUsage::Structured` plus optional `CopySource`/`CopyDest`, whose byte size is a multiple of 16. Every bind attempt clears the active binding before validation, and a pipeline change clears it again; `Draw` and `DrawIndexed` require it only for a declaring pipeline and must fail without recording native work when the binding is absent or stale. Rebinding may select another valid buffer, but every native resource/binding object used by an earlier draw remains retained through the whole command-list recording and submission boundary.

Vulkan owns one set-3 `StructuredBuffer_SRV` layout/set and appends it to the graphics state without disturbing the existing set-1 sampled texture/sampler table. D3D12 owns one root-SRV parameter and uses `SetGraphicsRootShaderResourceView`; it must not replace or swap the sampled-table descriptor heaps. Native resources and binding objects remain backend-private. The Vulkan acceptance probe reads a nonzero, discriminating word through the real shader and compares the rendered byte against an independent expectation; shared D3D12 source is not a Windows-execution claim.

## 15B. Shader-Visible Scene Light Payload Publication Contract

Renderer packs the accepted immutable Scene snapshot and its matching `ClusteredLightGrid` into a bounded versioned sequence of explicit `uint32x4` words. The V3 header contains checked offsets and counts for complete light records, global directional indices, all CSR cluster offsets, and local-light indices, plus the exact float bits of the resolved frame pre-exposure scale in header word 5 `.w`. The immutable CPU payload also retains the complete color-pipeline settings and derived pre-exposure state that produced those bits.

Each V3 light record is seven words. Word 0 stores source entity, type, unit, and shadow flag. Word 1 stores the exact double value as low/high words plus its checked float consumption value. Word 2 stores view position and range. Words 3 and 4 store world/view emission directions plus inner/outer cone cosines. Word 5 stores authored linear RGB plus the prepared reciprocal cosine span. Word 6 stores the prepared RGB photometric coefficient plus reciprocal range. The builder validates snapshot/grid one-to-one agreement, color settings, finite representability of every prepared value, all copied/range/cone constraints, multiplication/addition overflow, maximum byte size, monotonic CSR, exact terminal offset, index bounds, the 16-directional hard limit, the 64-local-reference shader bound, global-directional membership, and local-only clustered membership before replacing caller output.

Publication owns at most `SubmittedRenderGraphFrameOwner::Capacity` reusable slots, each pairing one CPU-write `CopySource` staging buffer with one GPU-only `Structured|CopyDest` buffer. A slot with external ownership is in flight and neither member can be rewritten. The publisher maps, copies, and unmaps only the staging member, then the frame graph records the staging-to-GPU copy and transition to `ShaderResource` before the raster read; it must not call synchronous `Device::UploadBuffer` per frame or bind host-visible memory as the structured SRV. Packing, allocation, mapping, graph-recording, or copy failure and capacity exhaustion preserve the last valid generation/output. A successful payload object becomes immutable, is bound in both production Scene renderers before raster draws, and retains both buffers with mesh/constants/tone-map resources until all exact accepted graph completions retire. Shutdown waits for device idle, releases submitted frames, then clears payload slots before device teardown.

Production `PSMain` declares, binds, validates, and consumes this payload. The separate payload diagnostic still reads varied header, directional, point, spot, prepared, global-index, cluster-offset, and local-index words against an independent fixture expectation; it reports exact CPU/GPU agreement and graph-token retention while stating that lighting evaluation is exercised by a separate production-`PSMain` oracle. Shadows, a GPU-built cluster grid, general structured descriptors, D3D12 execution, and MoltenVK execution remain separate claims.

## 15C. Finite Pre-Exposed Scene-HDR Storage Contract

Lighting and emissive evaluation remain scene-referred linear float32 until one common output finalizer. Before any RGB value enters the RGBA16F Scene target, that finalizer validates finiteness, clamps negative channels to zero, bounds the unexposed input by `65504 / preExposure`, multiplies exactly once by `preExposure = exp2(-effectiveEV100)`, and applies a final `65504` ceiling. Alpha is not exposed and is clamped to `[0,1]`. Clear, lit, unlit, emissive, and deterministic error paths obey the same boundary. The 65504 saturation deliberately preserves the current Khronos PBR Neutral displayed result because that tone mapper has a lower 6.25 input ceiling; it does not claim preservation of unbounded HDR data.

Both native Scene renderers snapshot one accepted color-pipeline value before payload/graph construction. A valid Scene obtains the immutable derived state from its retained V3 light payload; a D3D12 no-Scene clear resolves the same state directly. Invalid settings, inconsistent derived state, nonfinite color, payload allocation/mapping failure, or graph failure cannot partially publish a new generation. The tone-map constant ABI remains 16 bytes and is still cached by the complete settings value for exact lifetime identity, but its former EV lane is reserved zero and `ToneMap.hlsl` never applies exposure.

Native acceptance must distinguish the new placement from both the old post-storage multiply and an accidental double application: read back exact half values from an exactly representable nonzero-EV clear, compare the final RGBA8 result to an independent one-application oracle and reject the two-application alternative, then render the accepted EV -16 extreme and prove exact `0x7bff` saturation with no half Inf/NaN anywhere. This prerequisite does not itself claim Scene-light evaluation, photometric normalization, shadows, D3D12 execution, or MoltenVK execution.

## 15D. Photometric Direct-Light Evaluation Contract

Production `PSMain` validates the complete V3 payload against the actual structured-buffer dimensions before following any payload-controlled offset. It evaluates at most 16 global directional records and the current cluster's monotonic CSR span of at most 64 local records. Invalid headers, section arithmetic, cluster spans, indices, record types/units, directions, or prepared values produce deterministic bright-magenta error shading instead of an unchecked read.

The first direct-light pipeline is RGB photometric rather than spectral. Authored nonblack linear RGB is divided by Rec.709 luminance `dot(rgb, {0.2126, 0.7152, 0.0722})`; black remains an intentional zero-output light. A directional record stores normalized RGB times authored lux. A point record stores normalized RGB times `lumens / (4*pi)`. A spot uses the squared cosine ramp

```text
q = saturate((cosTheta - cosOuter) / (cosInner - cosOuter))
A = q*q
Omega = 2*pi*((1-cosInner) + (cosInner-cosOuter)/3)
axialCandela = lumens/Omega
```

For equal nonzero inner/outer cones, the profile is a hard cone with `Omega=2*pi*(1-cosOuter)`. A zero-flux zero-solid-angle spot is valid and dark; positive flux with zero packed solid angle rejects transactionally. Positive finite local ranges must have a finite float reciprocal. One Scene unit is one metre.

Stored directions point in the light's emission direction; identity rotation emits along `+Z`. Directional surface-to-light is `-emission`. A spot compares its emission direction with local light-to-surface. Local lights use

```text
attenuation = max(1-(distance/range)^4, 0)^2 / max(distance^2, 0.01^2)
```

and range-zero or exact-center samples contribute zero. The accepted GGX/height-correlated-Smith/Schlick/Burley BRDF consumes the resulting incident illuminance; emissive remains additive and exposure is applied once at finite HDR storage. ORM occlusion is still reserved for indirect light. Shadowing is governed separately by section 15E; volumetric scattering, IES profiles, physically calibrated spectral conversion, GPU cluster construction, D3D12 execution, and MoltenVK execution are not established by this direct-light slice.

## 15E. Stable Primary-Directional Shadow Contract

The first shadow implementation selects the first shadow-enabled directional in the immutable global-light order. It fits all current visible raster-instance receiver bounds in camera-relative light space, quantizes a square orthographic extent, snaps the light-space center to the 1024-by-1024 map's texel grid, and publishes one finite D3D-style zero-to-one depth transform with explicit constant and slope bias. A missing eligible light or empty receiver set publishes a valid disabled shadow state rather than stale matrices.

Caster policy is explicit and deterministic. Opaque materials cast; masked materials reuse the declared base/opacity texture alpha and cutoff; blend materials and components with `CastsShadows=false` are excluded; and invalid/error material rows cast conservatively while retaining counted reason diagnostics. This initial receiver fit does not yet perform receiver-aware caster exclusion, cascade partitioning, atlas allocation, proxy selection, or cached-shadow residency.

The production graph inserts the named depth-only `Scene Primary Directional Shadow Map` pass after light-payload copy and before Scene clear/raster. It writes a D32 target, then production `PSMain` reads that map at fixed point-sampled `t0/s0,space2` and applies shader-defined 3-by-3 PCF only to the selected directional contribution. The material texture table remains available during alpha-tested shadow draws. Vulkan realizes the fixed sampled binding beside the existing material table; D3D12 uses an R32 typeless resource with D32 DSV/R32 SRV views and a static fixed sampler, avoiding a 2,049th shader-visible sampler when the 2,048-entry material table is full.

The graph/direct comparator must render an independent reference shadow map and replay the same prepared shadow draws; it may not sample depth produced by the graph under test. The later sky pass makes the production base graph seven passes, and an active selected-bounds overlay adds an eighth, so four retained maximum-shape frames require capacity for 32 timestamp-query states; the shadow slice's original six-pass/24-state acceptance remains historical evidence. Linux/Vulkan acceptance additionally requires an independent readback fixture in which an offset caster darkens the receiver, a control region stays unchanged, and disabling component casting removes the shadow. Shared D3D12 source is not Windows runtime qualification. Cascades, atlases, receiver-aware exclusions, proxy/cached shadows, transparent colored transmission, and sparse ray-traced residuals remain later work.

## 15F. Basic Analytic Sky And Indirect-Diffuse Contract

The first sky is a deterministic daytime baseline rather than the final production atmosphere. The first directional record in the immutable Scene-light order is the sun independently of whether that light casts shadows. Its stored emission direction is negated to obtain surface-to-sun. A missing directional, black tint, zero illuminance, or sun at/below the world horizon publishes a valid disabled state; malformed source data rejects transactionally instead of retaining a partly updated frame. Until Phase 8 introduces authoring, the atmosphere is fixed to Preetham/Perez turbidity `3`, Lambertian ground albedo `0.1`, solar angular radius `0.266` degrees, and a `100000 lux` daylight reference.

The CPU preparation evaluates the Preetham zenith `xyY` and Perez distributions, converts nonnegative sky luminance to linear RGB, and scales sky luminance by the selected sun's authored lux. Solar-disk radiance divides the authored illuminance by the disk solid angle and by the Rec.709 luminance of the authored tint, so integrating the tinted disk recovers the directional-light illuminance rather than inventing an unrelated artistic intensity. A fixed 512-sample equal-solid-angle Fibonacci hemisphere integration produces upper diffuse irradiance without requiring a cubemap/LUT resource. The lower irradiance lobe is upper irradiance multiplied by ground albedo, and the below-horizon dome radiance is that lower irradiance divided by pi.

The named `Scene Sky Atmosphere` full-screen pass writes the finite pre-exposed RGBA16F Scene target after `Clear` and before `Raster`. Its exact 160-byte `b0,space0` constants are held in four reusable slots and retained with the immutable prepared sky through the accepted graph tokens. The ordinary surface constants now exactly fill 512 bytes, with upper/lower sky irradiance at offsets 480/496. Production `PSMain` interpolates those lobes using the world geometric normal, applies base color and nonmetal energy selection, and applies the material ORM occlusion channel only to this indirect diffuse term. Direct directional/point/spot lighting and emissive remain unoccluded by ORM.

Smoke-only direct comparison must render its own sky into the reference HDR output and then rebind the reference color-plus-depth targets before raster; leaving the sky's color-only framebuffer bound is invalid even if a backend happens not to report it. Linux/Vulkan acceptance requires exact graph/reference bytes at multiple sizes plus a dedicated LDR/HDR oracle that distinguishes upper sky from ground, localizes at least one saturated HDR sample to the resolved solar disk, scans the complete HDR image for Inf/NaN, proves resize, and demonstrates nonzero surface indirect light becoming zero when the sky is disabled. D3D12 shares the source, reflection, pass order, and lifetime implementation but requires Windows execution before qualification. Authorable turbidity/albedo/time/weather, Hosek-Wilkie or scattering LUTs, aerial perspective, volumetrics, clouds, night/astronomical bodies, and specular environment lighting remain Phase 8 work.

## 15G. Scene Debug Visualization And Overlay Contract

Debug visualization is renderer state captured at raster preparation, not mutable UI state reread during GPU recording. One generation carries exactly `Lit`, `MaterialId`, `GeometricNormal`, or `ShadowCaster`, the stable selected entity, and whether selected bounds are enabled. A later Editor or typed-control publication affects the next prepared frame only. `MaterialId` hashes a deterministic 32-bit fold of the persistent 64-bit material asset handle; it must not hash the sorted frame-local material row, because unrelated visibility or material-order changes may renumber that row. `GeometricNormal` exposes the finite transformed geometric normal. `ShadowCaster` distinguishes opaque, alpha-tested, conservative-error, and excluded classifications already produced by the shadow preparation contract. Diagnostic surface colors cancel pre-exposure before the common finite storage boundary so project EV changes cannot change the selected diagnostic identity; row-zero material errors remain visibly magenta.

Selected bounds are computed from the resolved mesh artifact's finite object-space AABB and the exact prepared instance model-view-projection. Each of the twelve edges is clipped in homogeneous zero-to-one clip space before division and emitted in normalized top-left viewport coordinates; missing, hidden, invalid, disabled, or fully clipped selections produce no overlay rather than screen-spanning coordinates. The overlay is a renderer-owned post-tone-map full-screen pass. It reads a distinct RGBA8 intermediate and writes the final handoff texture, so it can remain a stable selection color independently of scene exposure while avoiding read/write aliasing. Its exact 224-byte constants and four reusable slots are retained through the accepted graph tokens. The seven-pass base graph remains unchanged when no segment is visible; an active selection adds only `Scene Debug Overlay` as pass eight. The smoke-only direct reference must independently record the same conditional pass and match final bytes.

A valid camera with zero visible mesh instances is not a failed Scene. Raster and shadow draw lists may be empty, but light-payload copy, shadow clear, Scene clear, sky, raster, tone map, and output handoff still execute; hiding the only mesh must never hide the sky. Linux/Vulkan acceptance requires production-shader readbacks for all four modes, a retained-bounds-on/live-bounds-off frame-coherence transition, post-tone-map overlay pixels, exact 8-pass and 7-pass graph/reference markers, and a nonuniform sky readback after all meshes are removed. D3D12 shares reviewed renderer/shader integration but remains unqualified until Windows execution. This first slice does not add physics shapes, light gizmos, skeletons, navmesh, wireframe, overdraw, visibility-buffer inspection, picking, or capture-tool qualification.

## 15H. Production Capture-Label Contract

The production base sequence is exactly `Scene Light Payload Copy`, `Scene Primary Directional Shadow Map`, `Scene Viewport Graph Clear`, `Scene Sky Atmosphere`, `Scene Viewport Graph Raster`, `Scene Viewport Graph Tone Map`, and `Scene Viewport Graph Output Handoff`. A frame with visible selected bounds inserts exactly `Scene Debug Overlay` between tone mapping and output handoff. These are RenderGraph pass identities, per-pass command-list debug names, GPU timestamp identities, and backend marker names; a capture verifier may not substitute the differently prefixed smoke-only direct-reference labels.

The RenderGraph executor owns marker placement. A pass marker encloses its ownership acquires, compiled barriers, callback commands, and ownership releases and must close before `CommandList::End`, including callback-false, exception, and worker-recording discard paths. RHI command lists reject close while marker nesting is nonempty and clear stale marker storage only when a new recording begins successfully. Vulkan forwards markers through NVRHI only when its optional instance-level `VK_EXT_debug_utils` advertisement was enabled before instance creation. D3D12 translates the same RHI calls to PIX command-list events; renderer code does not reach upward or bypass RHI to label production graph passes.

Deterministic fake-RHI tests prove exact command-list-name forwarding, scope ordering, balanced false/throw paths, and caller/worker recording behavior. External readability remains a separate runtime gate for each named backend/device/tool: Linux/Vulkan RenderDoc evidence does not qualify D3D12/PIX, Nsight, MoltenVK, or another device class.

## 16. Material ID And BRDF Table Contract

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

## 17. Color Pipeline And Tone Mapping Contract

Tone mapping is not an arbitrary final-post effect. It is part of the physical-material and exposure system.

Rules:

- Internal lighting is scene-referred linear HDR.
- Exposure uses calibrated camera/eye-style controls plus artist zones where needed.
- The executable Phase 3E exposure authority is project-owned until a later per-camera lighting workflow requires Scene camera ownership. `ManualExposureEV100` remains the default override mode, accepts only finite values in `[-16,+16]`, defaults schema-3-and-older project manifests to `0.0`, and rejects invalid loads or publications without replacing the prior accepted value.
- The calibrated project mode derives effective EV100 from aperture, shutter, and ISO using `EV100 = log2((N * N) / t * (100 / ISO))`, where `N` is f-number, `t` is shutter seconds, and ISO is the sensitivity. Defaults are `N=1.0`, `t=1.0`, and `ISO=100`, which resolve to `EV100=0`; schema-4/5 manifests preserve their existing manual EV value and default only the new mode/optical fields, while schema <=3 keeps the earlier manual `EV100=0` migration. The finite transactional bounds are aperture `[0.7,64]`, shutter `[1/8000,60]` seconds, and ISO `[1,102400]`; the resolved EV100 must also remain in `[-16,+16]`. Schema-6 parsing is order-independent and final validation occurs only after all color-pipeline fields are read. Project serialization uses round-trip-safe floating precision for these controls.
- Both manual and calibrated modes use `scale = exp2(-effectiveEV100)`, so increasing the effective value darkens the scene and `0.0` preserves unit scale. The scale is applied exactly once by the finite pre-exposed storage boundary in Section 15C, before RGBA16F conversion; the tone-map pass consumes that stored value without another exposure multiply. The current paper-white scale is fixed at `1.0`.
- Scene viewport renderers must snapshot the accepted color-pipeline settings before building the render graph, retain the matching V2 light-payload/pre-exposure state plus tone-map constants through the exact accepted graph submission token, and record graph and smoke-only direct-reference paths from the same immutable inputs. Repeated renders at unchanged complete settings may reuse immutable storage; any accepted settings change must publish bytes carrying the new scale while older accepted submissions retain their prior values. A mutable per-pass buffer must not be overwritten while in-flight work may still reference it; arbitrary rings are not authority unless their lifetime is tied to accepted completion.
- Tone mapping happens before artistic color grading/LUTs.
- The current executable post-tone-map grading prerequisite is analytic and project-owned: `PostToneMapSaturation` and `PostToneMapContrast` both default to identity `1.0`, accept only finite values in `[0,2]`, migrate older project manifests to identity, and reject invalid loads or publications without replacing the prior accepted settings. The shader bypasses grading arithmetic when both controls are identity so the pre-grading output path is preserved exactly; otherwise the formula operates on display-linear values after Khronos PBR Neutral and before output encoding: `lerp(luminance.xxx, displayLinear, saturation)`, then `(saturated - 0.5) * contrast + 0.5`, clamped before linear-to-sRGB conversion. The `0.5` contrast pivot is a simple display-linear midpoint chosen because the current RGBA8 handoff has no HDR-display/paper-white calibration beyond the fixed scale above.
- The analytic controls are current consumers because Project Settings can edit them, the Renderer publishes and snapshots them with manual EV100, and the existing Scene tone-map pass consumes the same immutable constants in graph and direct-reference recordings. They are deliberately not a texture-backed LUT asset, a 3D image capability claim, a grading-profile library, an alternate tone mapper, or exposure compensation.
- Color grading is allowed to stylize the image, but should not compensate for broken albedo, lighting units, or BRDF parameters.
- The engine must ship material/exposure validation scenes: neutral gray, saturated colors, metals, skin, wet surfaces, emissives, daylight, indoor mixed lighting, and night.
- Candidate tone mappers must be compared in those validation scenes before becoming defaults.

The manual/calibrated project exposure and analytic post-tone-map grading prerequisites are deliberately not automatic exposure, exposure zones, per-camera exposure, texture-backed LUTs, photometric light coupling, HDR display control, tone-map selection, or a profiler/debug readout. Those remain owned by the broader Phase 3E/Phase 5 lighting and validation work that has a current consumer for each mechanism. A real LUT path must name its dimensionality, asset/source authority, RHI binding/capability boundary, fallback, and validation scenes before it can replace or extend this analytic prerequisite.

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
- Preetham, Shirley, and Smits, A Practical Analytic Model for Daylight: https://doi.org/10.1145/311535.311545
