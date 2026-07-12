# RHI And Lighting Decisions

Status: Draft v0.1
Date: 2026-07-06

Purpose: choose the first real rendering abstraction layer and define support for daylight cycles, sun/sky, baked lighting, and time-of-day global illumination.

Probe lighting, Detroit: Become Human lessons, screen-space GI/AO, and static/dynamic GI consistency are specified in [PROBE_LIGHTING_AND_GI_DECISIONS.md](PROBE_LIGHTING_AND_GI_DECISIONS.md).

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

### Native Device And Presentation Ownership

NVRHI's Vulkan API consumes an application-created `VkInstance`, `VkPhysicalDevice`, `VkDevice`, and queues through `nvrhi::vulkan::DeviceDesc`; `nvrhi::vulkan::createDevice` then returns the NVRHI device used by renderer resources and command submission. Engine creation of those native bootstrap objects is therefore part of the NVRHI integration, not a competing raw-Vulkan renderer. The D3D12 integration follows the same pattern: the engine creates the native platform objects and wraps them with NVRHI.

Window-system presentation remains an explicit native escape hatch. The engine owns the DXGI/Vulkan swapchain, acquire/present synchronization, and timing because NVRHI does not own presentation. Dear ImGui may consume the native device, queue, render-pass, and descriptor handles through its official backend because it has no NVRHI renderer backend. These native bridges must stop at presentation and editor UI.

The current Vulkan slice ends at that boundary: it wraps the native device with NVRHI and presents the editor shell, but it does not yet provide an `Engine::RHI::Device` implementation for Vulkan scene resources or general command submission. The future Vulkan scene path must implement those operations behind `Engine::RHI` using the returned `nvrhi::DeviceHandle`; it must not grow a parallel raw-Vulkan scene renderer. The current D3D12 RHI implementation also uses native D3D12 operations behind the engine facade, so migration toward shared NVRHI resource/command behavior must be stated as future convergence rather than completed portability.

On macOS, the first implementation path is MoltenVK through this same Vulkan/NVRHI boundary. It is deliberately experimental because NVRHI does not list macOS as an upstream-supported platform. Hosted x86_64 editor presentation is verified; Apple Silicon coverage and production scene-renderer conformance are separate gates. Native Metal remains a future measured alternative rather than a parallel bootstrap renderer. See [MACOS_RENDERER_BACKEND_DECISION.md](MACOS_RENDERER_BACKEND_DECISION.md).

Backend selection happens before native device creation. Strict requests such as the Vulkan render smoke fail if the requested path cannot initialize, while ordinary launches may retain an explicitly supported fallback. Adapter selection is capability-based and must not require an NVIDIA device merely because NVRHI is the first backend.

### Buffer Upload Contract

Immutable GPU buffers use `BufferCpuAccess::None` and declare `BufferUsage::CopyDest` in addition to their consuming usage. They are populated through `Device::UploadBuffer`, which creates a CPU-write staging buffer and records `CommandList::CopyBuffer` on the copy queue.

The initial implementation signals and waits on an engine-owned D3D12 queue fence before the staging resource is released or the destination is used by another queue. The destination is returned to its prior state, currently `Common`, so the first graphics read can use D3D12 common-state promotion. A future asynchronous uploader must retain this explicit fence ownership in the render graph; it must not release staging allocations merely after recording a copy.

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

Windows D3D12 baseline:

- Use a flip-model swapchain with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` when the window/presentation mode supports it.
- Wait on the frame-latency object before input sampling, simulation, and rendering, then present the freshly rendered result immediately. Do not make a generic sleep immediately before `Present` the engine pacing policy.
- Expose a profiled queue-depth profile. Start at a maximum frame latency of one; allow two only when measurements show that the extra CPU/GPU overlap is needed to meet the target cadence.

Vulkan baseline:

- Keep normal FIFO presentation and engine-side timing as the portable fallback.
- When the selected device and surface expose `VK_EXT_present_timing`, opt in behind a capability check, collect presentation feedback, and use its timing controls only through the swapchain policy. Never make this extension mandatory for portability.
- Do not design around the older `VK_GOOGLE_display_timing` path; it remains a possible compatibility experiment, not the preferred cross-vendor contract.

VRR and vendor latency technologies are profile inputs, not correctness mechanisms. Respect the user's VRR and driver settings, but do not claim that VRR repairs missed frame budgets. NVIDIA Reflex may become an optional Windows/NVIDIA latency integration after the portable pacing and instrumentation path exists; it is not a substitute for that path.

RTSS, MSI Afterburner, and Special K may be useful external test conditions. The engine must not require them or reproduce their implementation details. Claims such as a fixed one-frame cost, display-level timing, or frame-generation benefit require measurements on the target hardware and presentation path before they affect an engine default.

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
- NVIDIA Reflex: https://developer.nvidia.com/performance-rendering-tools/reflex
- Sandia SPA summary: https://pvpmc.sandia.gov/modeling-guide/1-weather-design-inputs/sun-position/solar-position-algorithm-spa/
- Precomputed Radiance Transfer: https://cseweb.ucsd.edu/~ravir/6998/papers/p527-sloan.pdf
- Deferred Radiance Transfer Volumes, Far Cry 3: https://www.gdcvault.com/play/1015326/Deferred-Radiance-Transfer-Volumes-Global
- Far Cry 3 DRTV slides: https://fileadmin.cs.lth.se/cs/Education/EDAN35/lectures/L10b-Nikolay_DRTV.pdf
- Real-Time Global Illumination using Precomputed Light Field Probes: https://research.nvidia.com/publication/2017-02_real-time-global-illumination-using-precomputed-light-field-probes
- Godot Lightmap GI docs: https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/using_lightmap_gi.html
- Directional Light Map from the Ground Up: https://agraphicsguynotes.com/posts/directional_light_map_from_the_groud_up/
- Bruneton precomputed atmospheric scattering implementation: https://ebruneton.github.io/precomputed_atmospheric_scattering/
- Unreal Sky Atmosphere docs: https://dev.epicgames.com/documentation/unreal-engine/sky-atmosphere-component-in-unreal-engine
