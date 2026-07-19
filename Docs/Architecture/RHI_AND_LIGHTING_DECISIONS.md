# RHI And Lighting Decisions

Status: Draft v0.1
Date: 2026-07-06

Purpose: choose the first real rendering abstraction layer and define support for daylight cycles, sun/sky, baked lighting, and time-of-day global illumination.

Probe lighting, Detroit: Become Human lessons, screen-space GI/AO, and static/dynamic GI consistency are specified in [PROBE_LIGHTING_AND_GI_DECISIONS.md](PROBE_LIGHTING_AND_GI_DECISIONS.md).
Cross-adapter capability negotiation, shader portability, descriptor fallback, and qualification levels are specified in [RENDERER_CAPABILITY_CONTRACT.md](RENDERER_CAPABILITY_CONTRACT.md).
Frame-graph state, barrier authority, execution, and GPU retirement are specified in [RENDER_GRAPH_ARCHITECTURE.md](RENDER_GRAPH_ARCHITECTURE.md).

## Short Decisions

1. **Use NVRHI for the first implementation backend**, wrapped by our own thin engine RHI facade.
2. **Keep NRI as the likely second backend / advanced backend**, especially for lower-level control, broader Vulkan-like platforms, and manual barrier experiments.
3. **Do not use bgfx for the main renderer.** It is excellent for broad portability and tools, but too high-level/general for this engine's bindless, visibility-buffer, RT, meshlet, and render-graph direction.
4. **Support daylight cycles as a first-class engine feature.**
5. **Use dynamic direct sun/moon lighting plus time-keyed probe/lightmap/static indirect lighting**, not fully static lightmaps for a moving sun.
6. **Choose baked lighting samples adaptively by lighting-error**, not by equal time spacing.
7. **Keep backend escape hatches for Work Graphs, device-generated commands, SER, Cooperative Vector, DGF, CLAS, and optional neural/RT SDKs**, but do not make any of them a v0 requirement.

## Rendering Abstraction Choice

### Why Not Write Raw Vulkan/D3D12 First

Raw API work is attractive for total control, but it is the wrong first move for this project.

This engine already has hard problems:

- Visibility buffer.
- Virtual geometry.
- Bindless resources.
- Ray residuals.
- Clustered lighting.
- Material resolve.
- Tooling/automation.
- AI-generated workflows.

Starting with raw Vulkan/D3D12 would make early work fragile. Barriers, queue ownership, resource lifetime, descriptor lifetime, and upload-buffer edge cases would absorb time before the renderer proves its image-quality ideas.

The first backend should give us guardrails while still allowing explicit modern rendering.

## NVRHI

Decision:

```text
NVRHI is the first production prototype backend.
```

Reasons:

- Supports D3D11, D3D12, and Vulkan 1.3.
- Works on Windows x64 and Linux x64/ARM64.
- Tracks resource states and inserts barriers automatically by default.
- Allows automatic barriers to be disabled per command list or temporarily for manual control.
- Tracks resource usage/lifetime and provides deferred safe destruction.
- Has a resource binding model with low runtime overhead.
- Supports graphics, compute, ray tracing, and meshlet pipelines.
- Supports parallel command-list recording and multi-queue rendering.
- Provides validation and resource reflection.
- Is used by multiple NVIDIA RTX SDKs, including RTXGI, RTXDI, RTX Mega Geometry, neural texture compression, and related samples.
- MIT-style permissive license.

Why this fits us:

- The engine needs modern explicit GPU features, but also needs to avoid incoherent barrier/resource bugs while the architecture is still moving.
- NVRHI's safety features are useful for a small team and AI-assisted code.
- We can opt out of automatic barriers in performance-critical sections once the render graph matures.

Risks:

- Platform scope is narrower than bgfx and somewhat narrower than NRI.
- It is NVIDIA-owned, though the API is intended to work across vendors.
- It is higher-level than NRI, so some very custom scheduling may eventually want a thinner backend.

Mitigation:

- Do not expose NVRHI directly through the whole engine.
- Build our own `Engine::RHI` facade over it.
- Keep backend-specific handles isolated.
- Add backend conformance tests early.
- Treat NVRHI automatic barriers as a debug/bootstrap default, not a permanent performance policy.

## NRI

Decision:

```text
NRI is not the first backend, but should be studied and kept as the advanced/portable backend candidate.
```

Reasons to like NRI:

- Lower-level, thinner abstraction over D3D12 and Vulkan.
- Explicit by design.
- Low overhead.
- Cross-platform/platform-independent goals, AMD/Intel friendly.
- Supports Vulkan, D3D12, D3D11, WebGPU, and Metal through MoltenVK.
- Official samples cover key needs:
  - `BindlessSceneViewer`: bindless GPU-driven rendering.
  - `AsyncCompute`: parallel graphics/compute work.
  - `MultiThreading`: multithreaded command recording.
  - `RayTracingBoxes` / `RayTracingTriangle`: ray tracing.
  - `DescriptorHeapIndexing`: dynamic descriptor heap indexing.

Why not first:

- It intentionally does not do hidden management.
- Its README explicitly says automatic barriers are better handled in a higher-level abstraction.
- This means our render graph/resource lifetime layer must be correct earlier.

Use NRI when:

- The render graph is mature.
- We want more direct scheduling/resource control.
- We want to target broader Vulkan-like platform experiments.
- We are ready to handle barrier/lifetime policy ourselves.

## bgfx

Decision:

```text
Do not use bgfx for the main renderer.
```

Reasons:

- bgfx has excellent platform reach: D3D11/12, Metal, OpenGL, OpenGL ES, Vulkan, WebGL, WebGPU, and many OS targets.
- It is permissively licensed.
- It is good for tools, simple viewers, debug utilities, or secondary lightweight renderers.

But it does not fit the main renderer because:

- This engine needs deep control over bindless resources, visibility buffers, meshlets, ray tracing, async compute, custom barriers, and render-graph scheduling.
- The core renderer is not a generic "draw stuff cross-platform" problem.
- bgfx's abstraction would likely fight the exact low-level architecture that differentiates the engine.

Possible use:

- Asset preview tools.
- Tiny launcher/diagnostic UI.
- Offline utilities that need broad platform display.

## Engine-Owned RHI Facade

The engine should not become "an NVRHI engine."

Add an engine-owned RHI layer:

```text
Engine::RHI
  Device
  Queue
  CommandList
  Buffer
  Texture
  Sampler
  DescriptorTable
  Pipeline
  RayTracingPipeline
  AccelerationStructure
  QueryPool
```

Backend plan:

```text
Backend 1: NVRHI
Backend 2: NRI or custom Vulkan/D3D12
Backend 3: platform-specific backend only if a shipping target requires it
```

Rules:

- Renderer code depends on `Engine::RHI`, not directly on NVRHI.
- Backend-native handles may be exposed only through explicit escape hatches.
- Render graph owns high-level resource state policy.
- NVRHI automatic barriers are allowed early; render graph validation must still model intended states.
- Once render graph matures, performance-critical passes can opt into manual barriers.

The graph-owned command-list mode is selected for the whole recording lifetime. Validated automatic barriers and explicit graph-derived barriers may be separate backend modes, but they must not be mixed ambiguously for the same graph-owned resources. Imported-resource states and cross-queue synchronization are always explicit graph contracts.

### Native Device And Presentation Ownership

NVRHI's Vulkan API consumes an application-created `VkInstance`, `VkPhysicalDevice`, `VkDevice`, and queues through `nvrhi::vulkan::DeviceDesc`; `nvrhi::vulkan::createDevice` then returns the NVRHI device used by renderer resources and command submission. Engine creation of those native bootstrap objects is therefore part of the NVRHI integration, not a competing raw-Vulkan renderer. The D3D12 integration follows the same pattern: the engine creates the native platform objects and wraps them with NVRHI.

Window-system presentation remains an explicit native escape hatch. The engine owns the DXGI/Vulkan swapchain, acquire/present synchronization, and timing because NVRHI does not own presentation. Dear ImGui may consume the native device, queue, render-pass, and descriptor handles through its official backend because it has no NVRHI renderer backend. These native bridges must stop at presentation and editor UI.

The current Vulkan path implements an NVRHI-backed `Engine::RHI::Device` for scene buffers, RGBA8/depth textures, clear/transitions, command submission, uploads, staging readback, markers, GPU-safe retirement, reflected SPIR-V pipeline consumption, constant updates, and indexed offscreen/viewport draws. `NVRHIVulkanViewportSceneRenderer` consumes the immutable Scene snapshot and records only `Engine::RHI` commands into renderer-owned outputs; the native Vulkan bridge remains limited to presentation and ImGui output handoff. It creates neither a second `VkDevice` nor raw Vulkan Scene commands. This is current named-device Scene/presentation evidence, not production-device breadth, a generalized mesh/material asset path, native placement/aliasing, or proof of macOS/Apple Silicon production conformance. The D3D12 implementation still has different native internals behind the same facade; shared NVRHI resource/command behavior remains a future convergence decision rather than a completed portability claim.

On macOS, the first implementation path is MoltenVK through this same Vulkan/NVRHI boundary. It is deliberately experimental because NVRHI does not list macOS as an upstream-supported platform. Hosted x86_64 editor presentation is verified; Apple Silicon coverage and production scene-renderer conformance are separate gates. Native Metal remains a future measured alternative rather than a parallel bootstrap renderer. See [MACOS_RENDERER_BACKEND_DECISION.md](MACOS_RENDERER_BACKEND_DECISION.md).

Backend selection happens before native device creation. Strict requests such as the Vulkan render smoke fail if the requested path cannot initialize, while ordinary launches may retain an explicitly supported fallback. Adapter selection is capability-based and must not require an NVIDIA device merely because NVRHI is the first backend.

### Buffer Upload Contract

Immutable GPU buffers use `BufferCpuAccess::None` and declare `BufferUsage::CopyDest` in addition to their consuming usage. They are populated through `Device::UploadBuffer`, which creates a CPU-write staging buffer and records `CommandList::CopyBuffer` on the copy queue.

The initial implementation signals and waits on an engine-owned D3D12 queue fence before the staging resource is released or the destination is used by another queue. The destination is returned to its prior state, currently `Common`, so the first graphics read can use D3D12 common-state promotion. A future asynchronous uploader must retain this explicit fence ownership in the render graph; it must not release staging allocations merely after recording a copy.

### Phase 3D Mesh Artifact Dependency

The current glTF importer validates supported float3-position triangle primitives and writes a schema-1 `.spiralmesh` artifact with source/handle provenance, `PositionColorUV32F` vertices, `UInt32` indices, and source primitive byte ranges. The current D3D12 and Vulkan viewport renderers still upload private prototype-cube buffers rather than resolve `SceneRenderSnapshot` mesh handles; the artifact is an Assets boundary, not a GPU-resource integration.

The first renderer-side prerequisite is an immutable `MeshArtifactResolver` catalog. The Editor publishes a copied `AssetRegistry` after initial registry construction, project-load replacement, history restoration, and successful glTF import; Renderer atomically publishes and reads that resolver independently of the immutable Scene snapshot. `Scene` remains stable-handle/transform-only. A resolution failure does not mutate the caller's prior artifact, so a later GPU-resource cache can retain its last valid generation until an explicit replacement policy is implemented. The resolver never reads glTF/cgltf and contains no native GPU types. It is not source-dependency freshness, generic hot reload, GPU residency, or a mesh-resource cache.

The viewport integration consumes that versioned, validated cooked mesh payload with supported primitive topology, explicit vertex/index layout and byte ranges, and asset-handle/source-path provenance. For each immutable snapshot mesh handle, Renderer resolves through the Engine-owned resolver and acquires a bundle only through `MeshGpuResourceCache` on its exact live `Engine::RHI::Device`. Both graph and smoke-only direct-reference recordings bind the same selected bundle/primitive ranges as `UInt32` indexed draws with `BaseVertex = 0`, and bind one per-instance constant buffer. Resolver/cache failure is explicit: it never substitutes a private renderer mesh, and the existing caller clear/last-valid behavior remains the only outcome. Each accepted graph submission retains its exact bundle references and constants alongside the replaceable selected pipeline until every accepted token retires. The cache is cleared only after device idle and retained-frame release, before device destruction/replacement. It must not expose cgltf source objects or native backend resource types to Scene, and it does not define source-dependency freshness, material/texture descriptors, GPU residency/streaming, or a generic asset hot-reload framework. No structured buffer is admitted until an actual current consumer needs it.

The shared `MeshGpuResourceCache` is the narrow resource construction boundary. It accepts one already validated `MeshArtifact` and an exact live `Engine::RHI::Device`, creates immutable `Vertex|CopyDest` and `Index|CopyDest` buffers with `BufferCpuAccess::None`, and uploads through `Device::UploadBuffer`. Its key is the exact live device instance plus a collision-free value identity over the asset handle, lexically normalized source provenance, primitive byte ranges, vertex values, and index values. It safely converts byte ranges to portable per-primitive element ranges; because cooked indices already address the combined vertex buffer, every portable draw range keeps `BaseVertex = 0`. The cache mutates only after both buffers upload. It has a deterministic LRU/generation tie-break; eviction drops cache ownership but does not invalidate an external `Ref` bundle. Because `Device` currently exposes no lifetime-stable cache token, this cache is explicitly cleared before its caller destroys/replaces a device; neither the cache nor an external bundle may outlive that device or cross an address-reused device lifetime. Source freshness, residency, hot reload, descriptors/materials/textures, and viewport binding policy remain outside the cache.

The normal default Editor scene cannot remain a private renderer cube once viewport integration begins. New scenes, loaded projects, and the bounded Vulkan Scene viewport fixture therefore reuse the original engine-owned `Engine/Generated/PrototypeCube.mesh` source identity and one shared registration/store/rollback helper before publishing the copied resolver. The helper invokes the existing schema-1 constructor, validation, and atomic publication path; its artifact retains the supported `PositionColorUV32F`/`UInt32` cube payload and exact primitive byte ranges. This is not a second mesh format, renderer fallback, or implicit source-freshness/hot-reload mechanism. On first publication failure it removes only the asset it just registered; replacement failure leaves the previously accepted artifact in place. D3D12 origin raster setup uses the normal Editor registry publication rather than a synthetic unresolvable handle.

### Read-Only Texture-Content Initialization Prerequisite

Before a logical sampled-table slot can represent real content, `RHI::Device::UploadTexture` owns one deliberately narrow initialization path. The destination is an exact-device-owned 2D texture with exactly one mip, one array layer, and one sample; it must declare `CopyDest|ShaderResource`, begin in `CopyDest`, and use RGBA8 UNORM or RGBA8 sRGB. The caller supplies engine-owned bytes with the exact format and extent, a row pitch at least `width * 4`, and an exact `rowPitch * height` byte count. No partial boxes, planes, cube faces, compression blocks, or implicit format conversion are admitted.

The D3D12 adapter creates a private upload buffer, copies each source row into the native footprint, records the full-subresource copy and `ShaderResource` transition on Graphics (a D3D12 Copy queue cannot record that shader-read transition), then publishes the wrapper state. Vulkan/NVRHI records its full `writeTexture` on Graphics, then the equivalent NVRHI state transition. Both use the existing `SubmitAndWait` completion-token authority: source bytes and native staging remain alive through submission acceptance and the final wrapper state is published only by the accepted completed submission. This is initialization, not a general asynchronous upload queue, mip generator, resource-residency manager, or descriptor/sampler binding mechanism.

### D3D12 Viewport Pipeline Reload Contract

The current D3D12 viewport renderer watches its selected Slang source and submits DXIL/SPIR-V package work through the shared asynchronous shader service. Each observed source revision receives a monotonic ticket in the current renderer/device epoch. A newer ticket supersedes older work; shutdown or device replacement invalidates every outstanding ticket. Only the current, previously unpublished ticket may publish.

Publication is failure-atomic at the renderer boundary: both portable packages, both D3D12 shader objects, and the complete graphics PSO must be valid before the active shader/pipeline references change. Compilation or PSO creation failure leaves the last valid generation active. The render graph captures the active PSO when recording a frame, and its submitted-frame owner retains that exact reference until the accepted GPU work retires, so live replacement cannot destroy an in-flight generation.

This is the smallest current consumer-specific mechanism. The source-path override and bounded smoke mode exist only to exercise a disposable copy without mutating the checked-in shader. They are not a public asset workflow, a generic file-watcher or hot-reload framework, or evidence for Vulkan and other backends; those boundaries require real consumers and backend-specific verification before extraction.

## Advanced Backend Features

The RHI facade must reserve extension points for features that are too new or too vendor/platform-specific to be baseline:

| Feature | Policy |
| --- | --- |
| DirectX Work Graphs / mesh nodes | Future GPU-driven backend for cluster culling, material queues, ray queues, and procedural work expansion. Not required for v0. |
| Vulkan device-generated commands / shader enqueue | Vulkan-side equivalent family for GPU-generated work. Keep as future backend path. |
| Shader Execution Reordering | Optional ray tracing acceleration path. Baseline ray shaders must still run without it. |
| Cooperative Vector / neural shader support | Optional neural rendering acceleration. Do not require it for core materials or lighting. |
| AMD DGF/DGFS | Optional geometry cook/runtime representation. RHI exposes buffers/metadata; content can decode to ordinary meshlets. |
| RTX Mega Geometry / CLAS | Optional high-end RT geometry backend. It may accelerate dense cluster AS builds but cannot be the only representation. |
| DLSS/XeSS/FSR/NRD/RTXDI/AMD Ray Regeneration/Radiance Caching | Optional integration modules after the native path passes quality/performance gates. |

All such integrations must report capability, cost, memory, debug source, and fallback path in the profiler.

## Daylight Cycle Support

Decision:

```text
The engine supports daylight cycles as a first-class feature.
```

A project can choose:

1. **Art-directed sun path**: default for games.
2. **Real geospatial sun path**: latitude, longitude, date, time zone, and time scale.
3. **Hybrid**: real sun path with art-directable offsets, compression of dawn/dusk, or cinematic season/time controls.

For real sun position, use a Solar Position Algorithm path. NREL SPA reports very high angular accuracy and is suitable for simulation/AEC/geospatial modes. Games should still expose an art-directed mode because real noon, sunrise, and seasonal paths are often not the most playable or dramatic.

## Sky And Atmosphere

Baseline:

- Physically based sun/sky model.
- HDR sun and sky radiance.
- Aerial perspective/fog.
- Dynamic exposure pipeline.
- Weather/cloudiness as an art-directed modifier.

Implementation path:

1. Start with Hosek-Wilkie or Preetham-style analytic sky for fast iteration.
2. Move to Bruneton/Hillaire-style precomputed atmosphere for production outdoor rendering.
3. Add volumetric clouds/weather after the sun/sky and lighting cache are stable.

The sun/sky system must output:

- Sun direction.
- Sun angular radius.
- Sun color/radiance.
- Sky irradiance SH/SG.
- Sky reflection cubemap or procedural sky sampling.
- Atmosphere/aerial perspective parameters.

## Baked Lighting For Daylight Cycles

The right model is not "fully baked day/night."

Decision:

```text
Current-frame dynamic direct lighting + time-keyed probe/lightmap indirect/static lighting.
```

Direct lighting:

- Sun/moon direction updates continuously.
- Direct sun/moon shadows are current-frame shadow maps plus optional ray residual correction.
- Hero contact shadows and high-frequency occlusion are current-frame.
- Do not bake moving sun direct shadows as the baseline; blending direct-shadow lightmaps creates double-shadow and ghosting problems.

Baked/static lighting:

- Static indirect bounce from sun/sky can be precomputed.
- Static skylight occlusion can be precomputed.
- Interior bounce, color bleeding, and ambient visibility can be precomputed.
- Dynamic objects sample probe volumes/light fields.

Runtime data:

| Data | Purpose |
| --- | --- |
| Directional lightmaps | Static surfaces with normal-map-aware indirect response. |
| Adaptive irradiance/light-field probe volume | Unified indirect GI for dynamic objects, static LODs, volumetrics, and zone transitions. |
| Reflection probes/cubemaps | Time-keyed rough/specular environment lighting. |
| Sky SH/SG | Cheap global diffuse/ambient term. |
| Occlusion/visibility terms | Stable contact/interior darkness and sky visibility. |

Static and dynamic objects must consume the same indirect-lighting API so dynamic characters do not pop out of baked environments.

## Adaptive Lighting Keyframes

The user idea of "10 lightmaps over the day" is good, but the spacing should not be equal in time.

Decision:

```text
Bake time-of-day keyframes adaptively by maximum interpolation error in lighting space.
```

Why:

- Noon changes slowly.
- Sunrise/sunset changes quickly.
- Shadows rotate faster at some times than others.
- Interior lighting may change suddenly when sun reaches a window.
- Weather/sky color may change perceptually even if the sun angle changes little.

Authoring controls:

```text
Min keyframes: 3
Max keyframes: user/project budget, often 6-16
Error threshold: low/medium/high or numeric
Priority masks: exterior, interior, hero route, gameplay space
Memory budget: MB
Bake quality: preview/final
```

Bake algorithm:

1. Generate a dense candidate timeline, for example every 5 to 15 minutes of game time.
2. For each candidate, compute sun/sky state:
   - Sun direction/color/intensity.
   - Sky irradiance.
   - Shadow direction.
   - Weather/cloud modifier.
3. Start with required anchors:
   - Night.
   - Dawn.
   - Noon.
   - Sunset.
   - Any user-pinned dramatic/gameplay time.
4. Bake or cheaply estimate lighting at anchors and candidates.
5. Interpolate between current keyframes.
6. Measure error against candidate reference data.
7. Insert the candidate with highest weighted error.
8. Repeat until error is below threshold or memory/keyframe budget is reached.

Error metric should include:

- Lightmap irradiance delta.
- Probe SH/SG coefficient delta.
- Directional irradiance lobe direction change.
- Shadow/occlusion change in marked gameplay areas.
- Perceptual luminance/color difference.
- Artist-painted importance masks.

This is better than using the sun-speed derivative alone. The derivative is useful as a heuristic for candidate placement, but final keyframe placement should minimize visible lighting error.

## Runtime Blending

Runtime chooses the two nearest keyframes or a small interpolation neighborhood:

```text
key A + key B + blend factor
```

Rules:

- Blend indirect lightmaps/probes/reflection captures in linear HDR.
- Keep direct sun/moon lighting live and current-frame.
- Use hysteresis only for resource residency, not for visible color accumulation.
- Preload upcoming keyframes before time reaches them.
- If a keyframe is missing, fall back to coarser resident lighting data, never block rendering.

Optional quality modes:

- **Low**: 2-4 lighting keys, direct sun current-frame, probe-only indirect.
- **Medium**: 6-10 adaptive keys, directional lightmaps + probes.
- **High**: 10-16 adaptive keys, directional lightmaps, probe volumes, reflection keys, ray residuals.
- **Cinematic/static**: direct lighting may be baked into keyframes for fixed scenes.

## Compression

Naively storing 10 full HDR lightmap sets can be expensive.

Baseline:

- Use virtualized lightmaps.
- Stream only needed regions/mips.
- Store lightmaps in HDR-capable compressed formats where supported.
- Store probes as quantized SH/SG coefficients.

Next step:

- Delta-compress lighting keys against a neutral/base bake.
- PCA or basis-compress temporal lightmap variation.
- Separate time-invariant occlusion from time-varying irradiance.
- Use lower resolution for slowly varying indirect terms.

Important rule:

```text
Do not solve memory by reducing lighting key count until transitions visibly pop.
Use adaptive keys plus compression.
```

## Editor Workflow

Guided workflow:

```text
choose world scale -> choose sun mode -> choose time range -> choose bake budget -> preview key placement -> bake -> inspect error heatmap -> accept or refine
```

The editor should show:

- Sun path curve.
- Keyframe positions on the day timeline.
- Memory cost estimate.
- Bake time estimate.
- Error heatmap.
- Areas where interpolation fails.
- Probe/lightmap residency.
- Runtime cost per platform target.

Automation:

- For beginner projects, choose sane default keys automatically.
- For realistic projects, ask for location/date/time.
- For stylized projects, expose dawn/noon/sunset color and contrast sliders.
- For performance targets, reduce lightmap/probe resolution first, then reduce key count.

## First Implementation Order

1. Add NVRHI backend behind `Engine::RHI`.
2. Add render graph/resource validation above NVRHI.
3. Add presentation pacing and instrumentation at the native swapchain boundary.
4. Add dynamic directional sun/moon light with art-directed path.
5. Add analytic sky model and sky SH output.
6. Add static lightmap/probe data format.
7. Add single-time bake import/runtime sampling.
8. Add time-keyed probe blending.
9. Add time-keyed directional lightmap blending.
10. Add adaptive keyframe selection/error heatmap.
11. Add geospatial SPA sun mode.
12. Add compression/PCA/delta storage for lighting keys.
13. Add production atmosphere and weather.

## Presentation Timing And Latency Policy

Presentation belongs to the platform swapchain layer, outside NVRHI. The engine must treat simulation cadence, CPU `Present` cadence, GPU completion, and display cadence as separate signals. No single overlay metric is sufficient evidence of smoothness or input latency.

### User And Project Policy

`Smooth Frametime` is an opt-in frame-pacing setting, not the engine default and not a synonym for VSync, VRR, low-latency queue control, or fixed-step simulation.

- The engine default is `Responsive`. It adds no engine-side cadence-smoothing cap or delay and does not deliberately discard an already-produced render result to manufacture a smoother metric. This is the competitive/esports-safe baseline; a project or player may still choose an explicit high cap, synchronization mode, or tearing policy.
- `Smooth Frametime` may target the display cadence or a project-defined rate/range and may deliberately reduce peak render throughput to improve cadence. It paces at frame start before input sampling and simulation, never by sleeping after rendering or immediately before `Present`. Missing a target is reported; the engine should not render work merely to throw it away.
- Project Settings store the serializable default. A developer can expose the same contract in shipped game settings, where the runtime/player value overrides the platform/project default, which overrides engine-default `Responsive`.
- Presentation sync/VRR/tearing, frame-delivery mode (preserve FIFO versus an explicitly selected latest-ready/replacement policy), maximum frames in flight, portable or vendor latency mode, and fixed simulation cadence remain independent settings. Any mode allowed to replace or drop a queued presentation must say so explicitly.
- Disabling `Smooth Frametime` does not disable swapchain acquire, per-image fence, resource-lifetime, or queue-completion waits required for correctness. Those waits are not smoothing.

### Smooth Frametime Mechanism And Research Provenance

The design input was a user-supplied Gemini discussion containing a community explanation of RTSS `ASYNC`, Front Edge Sync (FES), a maintainable frame cap, VRR, VSync, driver low-latency modes, and NVIDIA Reflex. The named modes and their timing points are preserved here because collapsing them into generic “frame pacing” loses the proposed mechanism. The explanation is research input, not proof of RTSS internals; implementation must use engine-owned clocks, markers, source inspection where available, and display/latency measurements.

The accepted product shape is:

- `Responsive` remains the default competitive profile. It adds no artificial cadence wait. Portable low-queue-depth control and optional Reflex/Anti-Lag-class integrations are measured independently.
- `Smooth Frametime` is an optional engine-owned analogue of the reported RTSS `ASYNC` goal: trade some peak throughput and latency for evenly spaced CPU-frame starts and GPU work delivery under a maintainable cap.
- A smoothness-oriented test profile combines the ASYNC-style pacer with GPU headroom, VRR, and an explicitly selected VSync/boundary policy. A cap several FPS below the VRR ceiling is a starting test condition, not a universal constant or hidden default.

The primary engine candidate is an **inter-frame boundary wait**:

1. Complete and submit the current frame, call `Present`, and record the actual timing markers.
2. Compute the next target CPU-frame start from a monotonic phase accumulator; do not derive it by repeatedly adding sleep duration to “now.”
3. After the prior frame's submission/`Present` path and before polling/sampling input or simulating the next frame, use a coarse sleep plus an optional short measured high-resolution wait to reach the target boundary.
4. Start the next frame from fresh input, simulate, render, submit, and present normally. If late, begin immediately and advance/reset the phase without catch-up bursts or sacrificial rendered frames.

The supplied explanation also describes RTSS `ASYNC` as holding CPU-produced work and releasing it to the GPU at the target boundary. Because that is a different control point from an after-`Present` inter-frame wait, preserve it as a separately instrumented **submission-gate candidate** rather than silently claiming the two are identical. Compare both placements for CPU/GPU overlap, queue depth, cadence, displayed smoothness, and input latency before accepting one.

FES is a third and distinct candidate: it delays immediately before the current `Present` to target the display's front edge. The supplied research warns that this placement can make telemetry sampled near `Present` look flat without eliminating visible engine stutter. It is not the normal `Smooth Frametime` path; retain it only as a measured experiment, especially for frame-generation timing, and require display-stage evidence rather than a flat application frametime graph.

“Frames lost to a cap” means intentionally not starting extra work above the selected cadence, not rendering completed frames and throwing them away. Rendered, submitted, presented, replaced, and displayed frames remain separately counted.

### In-Game Frametime Capture Contract

The in-game profiler owns its timestamps. It must not derive “frametime” solely from calls observed at the limiter or `Present` hook, as an Afterburner/RTSS-style overlay can sample beside the injected delay and display an artificially flat graph. One monotonic frame ID must follow the same work through all available stages.

The default profiler view reports separate series rather than one ambiguous number:

- **Game-frame interval:** `FrameStart[N] - FrameStart[N-1]`, captured at the engine's authoritative start boundary. This includes the inter-frame pacing wait and exposes the cadence actually delivered to input/simulation.
- **CPU active work:** from input/simulation start through render-submission completion, excluding the intentional pacer wait.
- **Intentional pacing wait:** requested target, actual wake/release time, sleep/spin duration, and overshoot. It must remain visible rather than being absorbed into a misleadingly short CPU-work value.
- **Present call cost and present cadence:** `Present` begin-to-end duration and the interval between matching present markers, reported separately.
- **GPU work and completion:** per-frame calibrated GPU begin/end/completion where supported, correlated to the same frame ID.
- **Display cadence:** actual displayed-time intervals from platform feedback or PresentMon-equivalent evidence. If unavailable, show `unavailable`; never rename present cadence as display cadence.
- **Input latency markers:** input sample to simulation, submit, present, and display where measurable. Do not infer click-to-photon latency from CPU timestamps alone.

The input sample is the completion of the engine-owned platform event poll for exact application frame `N`, after the timing frame and mandatory pre-frame backend wait are established but before immutable frame-input publication and `InputSimulation`. It is not the task that copies timestep data, a callback from the preceding frame's tail, or a GPU/presentation marker. The sample record must carry exact frame identity and a monotonic timestamp; QPC is additional attachment/correlation evidence when active, not a requirement for normal traces. Duplicate, late, wrong-frame, or missing samples are unavailable rather than borrowed. This boundary must be implemented and verified before input-to-stage intervals are published.

The implemented boundary uses `PollEvents` once per active frame and records `InputSample` before simulation; minimized polling has no renderer sample. Its pure validator is failure-atomic, and native D3D12/Vulkan traces retain mandatory waits separately. This enables the following same-frame interval work but does not itself make `InputLatencyAvailable` true.

Same-frame engine-stage latency publication is now implemented: input-to-simulation, input-to-native-submit, and input-to-`PresentEnd` share the sample's exact application frame and must be finite, nonnegative, and ordered. Missing or contradictory endpoints publish nothing. These fields are diagnostic CPU/presentation intervals, not input-to-GPU-completion, input-to-display, or click-to-photon measurements; `InputLatencyAvailable` remains false until an admitted end-to-end source exists.

Source-alignment invariant: cadence interval `FrameStart[N] - FrameStart[N-1]` terminates on timing record `N`. The InterFrame release for `N` is inside that interval, while SubmissionGate wait, CPU/GPU work, and `Present` that can delay the next start originate on `N-1`. Diagnostics must retain those source frame IDs and their frozen policies rather than comparing cadence `N` with current work `N`. Delayed exact GPU publication for `N-1` may amend only its associated retained cadence `N`. Backend presentation evidence must be an immutable frame-keyed publication; copying the latest mutable backend state is not sufficient. Missing, gapped, evicted, contradictory, or non-exact evidence remains `unavailable`/`Unresolved`, and no engine or present interval becomes display cadence.

Current implementation (2026-07-18): renderer timing records retain the consecutive cadence predecessor and the exact effective-limiter source frame. The pure classifier evaluates InterFrame on `N`, all prior-work candidates on `N-1`, requires the candidate's own valid Smooth policy, and rejects multiple qualifying candidates rather than imposing an unsupported priority. D3D12/Vulkan present attempts publish an exact application frame ID; delayed GPU `N-1` can amend only retained `N`, including benchmark schema 4. Deterministic tests cover first/gapped state, current-work non-borrowing, both pacing candidates, conflicting evidence, mismatched present identity, low-rate scheduling tolerance, and delayed GPU reclassification. Local native D3D12 and Vulkan target-change smokes enforce the same source-frame relationship. This qualifies engine/present-source diagnosis only, not monitor cadence or input latency.

The graph must retain spikes and missed deadlines. Aggregation may add p50/p95/p99 and 1%/0.1% lows, but it must not average away individual long frames or rescale each mode so that different pacing policies appear equally stable. External overlay data remains a comparison condition, not the in-game source of truth.

### Backend Mechanisms

Windows D3D12:

- Use a flip-model swapchain with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` when the window/presentation mode supports it.
- Wait on the frame-latency object before input sampling, simulation, and rendering, then present the freshly rendered result immediately. Do not make a generic sleep immediately before `Present` the engine pacing policy.
- Expose queue depth as a profiled policy input. Maximum frame latency one is the initial low-latency candidate; allow two when measurements show that added CPU/GPU overlap is needed. Queue depth is not `Smooth Frametime` and must not be changed implicitly by that toggle.

Vulkan:

- Keep FIFO presentation and engine-side timing as the portable preserve-order fallback. Latest-ready/mailbox replacement and immediate/tearing behavior are separately named frame-delivery policies, never hidden smoothing behavior.
- When the selected device and surface expose `VK_EXT_present_timing`, opt in behind a capability check, collect presentation feedback, and use its timing controls only through the swapchain policy. Never make this extension mandatory for portability.
- Do not design around the older `VK_GOOGLE_display_timing` path; it remains a possible compatibility experiment, not the preferred cross-vendor contract.

Current implementation audit (2026-07-13): no current backend contains an explicit engine frame-skip/discard policy. D3D12 nevertheless hard-codes the waitable swapchain, maximum latency one, and synchronized `Present(1, 0)`; Vulkan hard-codes FIFO; the dormant OpenGL path requests swap interval one. The project/runtime settings contract above is absent, so these are bootstrap implementation choices rather than a completed default-policy implementation. The corresponding `PLAN.md` items remain unchecked.

VRR and vendor latency technologies are profile inputs, not correctness mechanisms. Respect the user's VRR and driver settings, but do not claim that VRR repairs missed frame budgets. NVIDIA Reflex may become an optional Windows/NVIDIA latency integration after the portable pacing and instrumentation path exists; it is not a substitute for that path.

RTSS `ASYNC`, RTSS FES, MSI Afterburner, and Special K may be useful external reference conditions. The engine must not require them or claim undocumented implementation equivalence. An automated RTSS condition requires a supported, versioned, exact-process/profile control interface plus captured before state, verified applied state, bounded failure cleanup, exact restoration, and a receipt; installation alone is not such an interface. If no supported non-UI transactional path exists, keep the candidate unavailable to automation and use an explicit bounded manual evidence gate rather than guessing persistent config formats or driving the UI. Claims such as a fixed one-frame cost, display-level timing, or frame-generation benefit require measurements on the target hardware and presentation path before they affect an engine default.

### RTSS `ASYNC` Protocol Audit And Manual Gate (2026-07-18)

Read-only audit evidence identifies installed `RTSS.exe` as 7.3.5.28001, with valid Authenticode signer Alexey Nicolaychuk and SHA-256 `786D9C786C085D4BF14C6E2F08AB1779E01741CEEEB367F8C11DB4AB99F9F68D`; no RTSS process was running during the audit. The bundled official SDK profile interface exports `Enum`, `Load`, `Get`, `Set`, `Save`, and `Update`. Its documented properties include `FramerateLimit`, but no ASYNC/limiter-mode selector. Bundled ReadMe page 6 distinguishes the default async limiter, FES/back-edge, and Reflex, and says Reflex falls back to the default async limiter, but supplies no profile-property encoding for that choice. `Save`/`Update` are not documented as an atomic transaction or crash-recovery protocol, and `UpdateProfiles` reloads all running 3D applications. RTSS shared memory is telemetry, not a control surface.

Accordingly, exact-Editor `ASYNC` is unavailable to automation for this installed/versioned interface. No configuration-file format may be inferred, and UI automation is prohibited. The accepted manual evidence gate is deliberately bounded: an operator records the exact `Editor.exe` path/profile and RTSS version/hash; captures before state for limiter mode, target, FES/scanline/frame-generation, and global/profile scope; changes only that exact Editor profile in the RTSS UI to the requested async condition; captures after state; runs one bounded benchmark; restores every exact prior value immediately, including after failure or timeout; captures restored state; and signs a receipt. The gate must not mutate a global profile, another profile/process, driver state, FES, or frame-generation state implicitly. It establishes a safe reference-condition protocol only; it neither proves that RTSS ASYNC executed nor supplies a production Smooth winner.

### Production Smooth Evidence Dependency Order (2026-07-18)

The production comparison is intentionally split into independently reviewable evidence prerequisites before the default-selection decision. First, the automated native bundle runs the existing schema-5 60/120 FPS engine matrices for D3D12 and Vulkan, attaches PresentMon to the exact D3D12 editor PID, and records a fail-closed Vulkan attachment attempt. It preserves immutable raw streams, joined reports, and condition manifests. It is provisional engine/present-source evidence: it does not establish physical display cadence, monitor/VRR state, input or photon latency, RTSS execution/configuration, or a winner.

Second, each admitted external reference condition uses the bounded exact-Editor RTSS manual gate and produces signed before/apply/bounded-run/restore evidence. FES and frame generation remain distinct mechanisms: they enter only when explicitly named as separate conditions with separately captured state and evidence, never through an implicit profile, driver, or application mutation.

Third, selection criteria that rely on input latency, physical display cadence, or VRR require an admitted source and control path for those signals before they are used. Same-frame engine input-to-`PresentEnd` remains a diagnostic CPU/presentation interval, not photon latency. An unavailable admitted source remains unavailable for that condition; engine or present cadence cannot be substituted. Only after these prerequisites may the parent comparison reconcile candidate conditions and select or reject a production default.

The first production bundle exposed a deadline-primitive prerequisite rather than a candidate winner. On the local Windows D3D12 path, waited releases overshot their requested deadlines by roughly 14.3 ms p50 and 15.5 ms p99, so the existing `steady_clock` plus `std::this_thread::sleep_until` implementation cannot qualify 60/120 FPS behavior. The magnitude and source code make Windows timer/scheduler oversleep the strongest inference, but the project does not call it the sole cause until the replacement A/B evidence isolates it. Platform therefore owns the system deadline primitive while Renderer retains target policy, InterFrame/SubmissionGate control points, phase anchoring, source alignment, and no-drop behavior. Windows first requests one unnamed, non-inheritable `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer per pacing clock, waits only until an at-most-0.5 ms final-tail boundary, and reports timer wait, active tail, actual release, overshoot, and fallback reason. The final active tail is bounded so the engine never busy-spins an interval. Creation, arm, or wait failure degrades to the existing portable steady-clock wait and remains visible; it cannot silently move the wait before `Present`, alter mandatory waits, or discard/replace a frame. Global `timeBeginPeriod` is rejected because it changes process/system timer policy and has documented scheduler/power and Windows 11 occlusion qualifications. Microsoft documents the high-resolution waitable-timer flag for Windows 10 version 1803 and later and the required handle lifetime: [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw), [SetWaitableTimerEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex), and [timeBeginPeriod](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod).

Implementation acceptance on 2026-07-18 isolated the deadline primitive as a real dominant contributor at 60 FPS. The Platform waiter owns a least-privilege timer handle and a narrow deterministic platform seam; schema 6 records the selected primitive, fallback/reason, timer and portable wait durations, final-tail budget/actual, process-CPU/wall proxy, requested deadline, release, and overshoot per applied intentional wait. The complete D3D12/Vulkan 60/120 matrices reported the Windows high-resolution timer with no fallback and a tail budget no greater than 0.5 ms. D3D12 release overshoot became roughly 0.0010-0.0014 ms p50 and 0.0195-0.3891 ms p99 across the four waited conditions instead of the pre-fix roughly 14-15 ms. This A/B result supports the timer/scheduler diagnosis for the prior 60 FPS doubling. It does not claim that the timer is the only cause of cadence misses: at 120 FPS measured CPU/render/present work frequently already exceeds the 8.33 ms target, and Vulkan presentation stalls remain separately visible.

## Open Questions

- Whether first bake implementation should be internal GPU baker or imported from external tools.
- Whether probe volume should be sparse brick/octree, cascaded grid, or artist-placed probes for v0.
- Whether high-end static GI should use light field probes, DDGI-like probes, or directional lightmaps first.
- How much direct baked lighting to support for mobile/static scenes.

## Sources

- NVRHI GitHub: https://github.com/NVIDIA-RTX/NVRHI
- NVRHI Programming Guide: https://github.com/NVIDIA-RTX/NVRHI/blob/main/doc/ProgrammingGuide.md
- NRI GitHub: https://github.com/NVIDIA-RTX/NRI
- NRI Samples: https://github.com/NVIDIA-RTX/NRISamples
- bgfx GitHub: https://github.com/bkaradzic/bgfx
- NVRHI license: https://raw.githubusercontent.com/NVIDIA-RTX/NVRHI/main/LICENSE.txt
- NRI license: https://raw.githubusercontent.com/NVIDIA-RTX/NRI/main/LICENSE.txt
- bgfx license: https://raw.githubusercontent.com/bkaradzic/bgfx/master/LICENSE
- D3D12 Work Graphs: https://devblogs.microsoft.com/directx/d3d12-work-graphs/
- DirectX Work Graphs spec: https://microsoft.github.io/DirectX-Specs/d3d/WorkGraphs.html
- Vulkan device-generated commands: https://vulkan.lunarg.com/doc/view/1.4.304.1/mac/antora/features/latest/features/proposals/VK_EXT_device_generated_commands.html
- AMD DGF SDK: https://gpuopen.com/dgf/
- RTX Mega Geometry: https://github.com/NVIDIA-RTX/RTXMG
- D3D12 Shader Execution Reordering: https://devblogs.microsoft.com/directx/ser/
- D3D12 Cooperative Vector: https://devblogs.microsoft.com/directx/cooperative-vector/
- NREL Solar Position Algorithm: https://midcdmz.nlr.gov/spa/
- Microsoft DXGI waitable swap chains: https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
- Khronos `VK_EXT_present_timing`: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_present_timing.html
- Khronos present-timing proposal: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_present_timing.html
- Khronos Vulkan present modes: https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html
- Unreal Engine Smooth Frame Rate project setting: https://dev.epicgames.com/documentation/unreal-engine/smooth-frame-rate?application_version=4.27
- NVIDIA Reflex: https://developer.nvidia.com/performance-rendering-tools/reflex
- NVIDIA low-latency esports aiming study: https://research.nvidia.com/index.php/publication/2021-05_case-study-first-person-aiming-low-latency-esports
- Sandia SPA summary: https://pvpmc.sandia.gov/modeling-guide/1-weather-design-inputs/sun-position/solar-position-algorithm-spa/
- Precomputed Radiance Transfer: https://cseweb.ucsd.edu/~ravir/6998/papers/p527-sloan.pdf
- Deferred Radiance Transfer Volumes, Far Cry 3: https://www.gdcvault.com/play/1015326/Deferred-Radiance-Transfer-Volumes-Global
- Far Cry 3 DRTV slides: https://fileadmin.cs.lth.se/cs/Education/EDAN35/lectures/L10b-Nikolay_DRTV.pdf
- Real-Time Global Illumination using Precomputed Light Field Probes: https://research.nvidia.com/publication/2017-02_real-time-global-illumination-using-precomputed-light-field-probes
- Godot Lightmap GI docs: https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/using_lightmap_gi.html
- Directional Light Map from the Ground Up: https://agraphicsguynotes.com/posts/directional_light_map_from_the_groud_up/
- Bruneton precomputed atmospheric scattering implementation: https://ebruneton.github.io/precomputed_atmospheric_scattering/
- Unreal Sky Atmosphere docs: https://dev.epicgames.com/documentation/unreal-engine/sky-atmosphere-component-in-unreal-engine
