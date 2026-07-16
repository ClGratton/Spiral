# Rendering Feature And Performance Decisions

Status: Draft v0.1
Date: 2026-07-06

Purpose: decide how SSR, AO, shadow exclusion, cubemaps, ray-traced reflections, profiling, instancing, mesh merging, meshlet LOD, and triangle/quad utilization fit this engine.

## Short Decisions

| Topic | Decision |
| --- | --- |
| Fox Engine lessons | Adopt photographic validation, material IDs, selected occluder prepass, probe/SSR pragmatism, measured shadow exclusions, and serious tone mapping. Do not copy Lambert/Blinn-Phong, old FXAA reliance, naive half-res artifacts, or front-face shadow culling as a global rule. |
| Frostbite/NFS 2015 SSR | Adopt the BRDF-aware stochastic SSR ideas, but not the original temporal filtering as a baseline. |
| Reflections | Hybrid: screen-space candidate first, ray tracing fills misses/high-value reflections, probes/cubemaps only for rough/background fallback. |
| Cubemaps | Not trusted for close glossy/mirror motion. Useful for rough IBL, distant fallback, and low-end modes. |
| HBAO+ | Good historical/optional reference, not the default. Prefer XeGTAO/GTAO-style spatial AO plus ray/probe assistance. |
| Shadow object exclusion | Yes, but conservative and receiver-aware. Never just "camera frustum visible = shadow caster." |
| Profiling | First-class engine feature: in-engine profiler plus Intel GPA, Nsight, RGP, PIX, RenderDoc support. |
| Optional accelerators | DLSS, XeSS, FSR, frame generation, neural denoisers, ray regeneration, SER, Cooperative Vector, and vendor SDKs are allowed only after native rendering is stable and measurable. |
| Instancing | Prefer GPU-driven instancing/indirect draws for repeated meshes. |
| Mesh/material merging | Use selectively and automatically, not blindly. Preserve culling, streaming, and LOD granularity. |
| Triangle topology | Auto import/LOD must score triangle shape, projected area, quad utilization, and RT BVH quality. |

## Fox Engine Lessons

The Threat Interactive Fox Engine video and independent MGSV frame studies are useful because they show a fast, sharp, practical renderer that did not need modern blurry temporal reconstruction to look coherent.

Adopt:

- Photographic validation and material calibration as a culture, not only a shader feature.
- Material IDs / BRDF table lookup to enrich lighting without bloating the G-buffer.
- Selected occluder prepass for large terrain/building/wall occluders.
- Packed material textures where the packed channels share color space, mip behavior, compression tolerance, and pass usage.
- Separate coverage/opacity data for shadow and depth work when base color RGB is not needed.
- Probe lighting plus screen-space/reflection tricks as practical current-frame ingredients.
- Planar or low-resolution re-rendered reflections for hero mirrors, glasses, water planes, and cinematics where the source is controlled.
- Stencil/tile/scissor/light-list restriction so local lights shade only pixels that need them.
- Shadow caster exclusion and proxy shadows, but only with receiver-aware rules and debug views.
- Tone mapping and exposure as foundations for realism before color grading.

Modernize:

- Fox's classic deferred renderer becomes our visibility-buffer + compact G-buffer + clustered/deferred lighting path.
- Fox's partial depth prepass becomes a depth/coverage/visibility-ID prepass, not a shaded G-buffer prepass.
- Fox's material ID becomes our `MaterialID` / `brdfParamIndex` table feeding Callisto/Proxima/GGX, not Lambert/Blinn-Phong.
- Fox's half-resolution AO/GI/SSR is allowed only with edge-aware reconstruction and confidence masks.
- Fox's baked probes evolve into adaptive probe/light-field volumes with unified static/dynamic sampling.

Reject as baseline:

- Lambert + normalized Blinn-Phong material response.
- FXAA as the main anti-aliasing strategy.
- Front-face shadow culling as a global rule.
- Naive low-resolution upscaling that makes indirect lighting pixelated.
- Static cubemaps as the answer for close glossy motion.
- Any temporal filter required to hide instability.

## Frostbite / Need For Speed 2015 SSR

The Frostbite stochastic screen-space reflection work is relevant, especially for wet roads, cars, and glossy rough surfaces like Need for Speed 2015.

What to adopt:

- BRDF importance sampling for glossy reflections.
- Roughness-aware ray distribution.
- Specular lobe elongation.
- Hierarchical depth traversal.
- Tile/material classification to spend rays where reflections matter.
- Spatial resolve that reuses neighboring reflection hits with BRDF-aware weights.
- Clear fallback classification for misses.

What not to adopt as baseline:

- Visible multi-frame temporal accumulation.
- Temporal filtering as the thing that makes the reflection acceptable.
- Screen-space-only reflection as the final answer for important glossy/mirror surfaces.

Decision:

```text
Use stochastic SSR as a current-frame screen-space candidate generator.
Use ray tracing or planar reflection for missing/high-value reflection information.
Use probes/cubemaps only for rough or low-importance fallback.
```

This keeps the good Frostbite insight: screen-space data is cheap, dense, and BRDF-aware. It rejects the part that conflicts with the engine's no-temporal-baseline rule.

## Reflection Stack

Runtime reflection priority:

| Surface class | Primary path | Fallback |
| --- | --- | --- |
| Mirror | Planar or full-rate RT reflection. | Explicit quality downgrade; do not fake with cubemap. |
| Eye/cornea | Specialized high-confidence reflection path. | Local probe only at low quality. |
| Car paint / wet roads | Stochastic SSR candidate + RT miss fill + local probes for rough tail. | Probe fallback if RT disabled. |
| Glossy rough material | Half/quarter-rate RT or SSR+RT hybrid depending on roughness and confidence. | Prefiltered local probe. |
| Very rough material | Probe/radiance cache/specular occlusion. | Sky/environment IBL. |
| Water planar surface | Planar or pixel-projected reflection, with SSR/RT detail where needed. | Probe fallback for low end. |
| Translucent front layer | T-buffer/front-layer path; first layer can trace. | Probe/SSR for deeper layers. |

Fox Engine reinforces that planar or reduced-resolution re-rendered reflections are still valuable for controlled hero cases. They are not old-fashioned if they are used deliberately: mirrors, glasses, water planes, cinematics, and inspection objects can justify a small extra render better than a fake cubemap.

Confidence rules:

- SSR is valid only when the ray hit is on-screen, depth-valid, normal-compatible, roughness-compatible, and not near a screen-edge failure.
- RT fills SSR misses for important pixels.
- Cubemap/probe fallback must be roughness-weighted and confidence-weighted to avoid hard source transitions.
- Debug view must show reflection source: SSR, RT, planar, probe, sky, fallback/error.

Cubemaps/probes are not "bad" universally. They are bad when used for close, glossy, parallax-sensitive, moving reflection detail. They remain excellent for cheap low-frequency environment lighting, rough reflections, sky, distant fallback, and lower quality modes.

## Ambient Occlusion

HBAO+ is a good technique, but not the default foundation for this engine.

HBAO+ strengths:

- Full-resolution depth path.
- No randomization texture.
- Interleaved rendering for cache behavior.
- Good contact shadows and reduced flicker compared to older SSAO variants.
- Open source GameWorks implementation exists.

Why not default:

- It is older and DX11-era in focus.
- Modern GTAO/XeGTAO-style work is more explicitly tied to ground-truth matching.
- HBAO+ is screen-space only, so it still suffers depth-heightfield limitations.
- The engine also needs specular occlusion and probe/RT-aware occlusion, not only diffuse AO.

Decision:

```text
Default AO: XeGTAO/GTAO-style spatial implementation, tuned against ray-traced ground truth.
Optional/reference AO: HBAO+ mode for comparison or compatibility.
High-end AO: screen-space GTAO + bent normals/specular occlusion + sparse ray residuals where needed.
```

No AO path may require temporal accumulation to be acceptable. Spatial denoising is allowed. Temporal history can be an optional quality mode only if the current-frame result remains acceptable.

Half-resolution AO/GI is allowed only with depth/normal/material-aware reconstruction. Naive bilinear upsampling is forbidden for shipped quality because it creates the exact low-resolution ambient-light mismatch the engine is trying to avoid.

## Optional Upscaling, Denoising, And Neural Accelerators

The engine should support optional user-facing accelerators because real users on mid-range hardware may want them. A player on an RTX 3060, for example, may reasonably enable DLSS to trade native pixels for higher settings or frame rate.

Decision:

```text
Optional accelerators are scalability features, not baseline image-quality requirements.
```

Allowed optional modules:

- DLSS, XeSS, FSR, and other upscalers.
- Frame generation, clearly labeled with latency implications.
- NRD, AMD Ray Regeneration, or similar ray denoisers.
- AMD Radiance Caching or future neural radiance caches for path-traced/high-end modes.
- RTXDI/ReSTIR-style many-light sampling when bounded and debug-visible.
- Shader Execution Reordering for ray coherence.
- Cooperative Vector or equivalent neural shader acceleration.
- RTX Neural Texture Compression or future cross-vendor neural texture compression.

Rules:

- Native-resolution output without the accelerator must pass motion clarity and quality gates first.
- Every accelerator must have a deterministic fallback.
- The profiler must show accelerator cost, saved cost, memory use, input resolution, output resolution, latency, and reactive/disocclusion risk where applicable.
- Debug overlays must identify pixels or passes affected by optional temporal/neural paths.
- Presets may recommend accelerators for performance, but engine correctness cannot depend on them.
- Marketing screenshots and baseline validation must include native/no-accelerator captures.

Forbidden:

- Designing a noisy renderer that only looks acceptable once a temporal upscaler or neural denoiser hides it.
- Shipping feature code paths that collapse visually when DLSS/FSR/XeSS/NRD is disabled.
- Hiding LOD shimmer, roughness shimmer, bad alpha coverage, or unstable GI behind upscaling.

## Shadow Map Object Exclusion

Yes, the engine should support shadow caster exclusion and culling. It must be conservative.

Allowed exclusion/culling:

- Per-light/per-cascade caster lists.
- Receiver-mask or receiver-frustum-aware caster culling.
- Surface-to-light cone culling.
- Static cache rejection.
- Projected shadow contribution threshold.
- Distance/importance culling for tiny objects.
- Proxy shadows for small or dense geometry.
- Artist flags: `CastShadow`, `CastHeroShadow`, `CastContactShadow`, `NeverCastShadow`, `ShadowProxy`.
- Material flags for vegetation, hair, translucent, emissive, and alpha-tested casters.
- Per-object/per-material shadow culling mode: default/back-face, two-sided, front-face optimization, proxy-only, or disabled.

Forbidden naive rule:

```text
If object is outside the camera frustum, exclude it from shadow maps.
```

Objects outside the camera frustum can cast visible shadows into the camera frustum. Directional sun shadows especially need receiver-aware caster bounds.

Front-face shadow culling may be exposed as an optimization mode for terrain/ground/proxy casters, but it must not be the global default. Characters, faces, hands, foliage, hair, and hero props need accurate two-sided/back-face-aware policies or specialized shadow proxies.

Debug views:

- Shadow caster inclusion/exclusion.
- Per-light/per-cascade caster count.
- Caster reason: visible receiver, hero, proxy, excluded too small, excluded material, outside influence, static cache.
- Shadow map texel density.
- Shadow culling mode.
- Wasted shadow draw cost.
- Popped or missing shadow warnings.

## Color Pipeline And Tone Mapping

Fox Engine is a useful reminder that tone mapping is part of realism, not a cosmetic afterthought.

Decision:

```text
Use calibrated scene-referred HDR lighting, explicit exposure, documented tone-mapper profiles, then artistic grading/LUTs.
```

Rules:

- Lighting and BRDF validation happen before color grading.
- Tone mapping must be stable in motion and must not rely on temporal adaptation to hide poor exposure.
- Grading LUTs are content style, not a repair layer for broken albedo/specular/light units.
- The editor needs side-by-side tone-mapper comparison against measured material test scenes.

Profiles to evaluate:

| Profile | Why |
| --- | --- |
| Gran Turismo-style / neutral shoulder | Strong game precedent for crisp, believable output. |
| AgX-style | Good highlight/color handling in modern DCC workflows. |
| ACES/filmic | Strong cinematic pipeline and HDR/display story. |
| Khronos PBR Neutral | Useful for faithful PBR/base-color validation and asset review. |

## Profiling And Performance Tooling

Profiling is not optional. The engine should ship with a built-in performance stack and be easy to capture with vendor tools.

Required external tool support:

| Tool | Use |
| --- | --- |
| Intel GPA | Frame analysis, API/resource inspection, bottleneck finding, multi-frame trace, integrated/Intel GPU checks. |
| NVIDIA Nsight Graphics | Frame capture, GPU Trace, shader/pixel inspection, RT acceleration structure debugging. |
| AMD Radeon GPU Profiler | AMD wave/occupancy/timing analysis and RDNA-specific bottlenecks. |
| Microsoft PIX | D3D12 captures, queue timing, event lists, counters, Windows GPU debugging. |
| RenderDoc | Cross-vendor frame debugging, texture/buffer/mesh inspection, shader debugging. |
| Tracy/Optick/Superluminal-style CPU profiler | CPU jobs, asset import, editor workflows, scripting, streaming. |

Required engine instrumentation:

- CPU frame time, GPU frame time, and present time.
- Configured and effective frame-pacing policy (`Responsive` or opt-in `Smooth Frametime`), target cadence/range, limiter or wait source, intentional pacing delay, deadline misses, and runtime/project override source.
- Per-frame simulation-start, render-submit, `Present` begin/end, GPU-complete, and display-timing markers where the platform exposes them. Keep application, present, and display cadence separate.
- Rendered, presented, displayed, replaced, and dropped frame counts/rates. A frame that never reaches the display must not disappear inside an aggregate FPS or frametime value.
- Swapchain mode, buffer count, configured queue depth, waitable-swapchain availability/use, and VRR/tearing policy.
- When frame generation is active, rendered-frame, generated-frame, present, and display rates, plus latency, reported separately.
- Per-pass GPU timestamps.
- Queue overlap: graphics, compute, copy.
- Draw/dispatch count.
- Material bin count and worst material bin size.
- Visible instance/cluster/triangle counts.
- Meshlet count, accepted/rejected, reason for rejection.
- Quad overdraw / helper-lane waste estimate.
- Pixel overdraw.
- Shadow caster counts and shadow-map draw cost.
- RT ray count, rays per class, hit/miss rate, TLAS/BLAS update cost.
- Reflection source breakdown.
- AO cost and sample count.
- Texture memory by format, mip residency, streaming faults.
- Mesh memory, cluster page residency, fallback usage.
- Shader permutation count and pipeline cache misses.
- Async upload/decompression time.

### Current Frame-Pacing Policy And Experimental Candidates

The project-owned policy is versioned in the `.spiralproject` manifest. `Responsive` is the default and `Smooth Frametime` is opt-in with a finite target in the inclusive 1-1000 FPS range. The top-bar Settings menu owns that serialized project default and save action. A public developer `GameFramePacingSettings` can inherit the project, force `Responsive`, or force `Smooth Frametime`; it always resolves against the project target and invalid Smooth target updates preserve the prior state. Profiler owns the non-serialized developer selector for the experimental `InterFrame` and `SubmissionGate` control points. No duplicate dockable Project Settings window exists.

`Responsive` resets pacing state and records `behavior=no-intentional-wait`. Opt-in `Smooth Frametime` currently has two experimental engine-owned deadline paths:

- `InterFrame` waits after the prior frame's submission/`Present` but before the next loop samples its timestep, publishes `FrameStart`, or runs input/simulation. The released `FrameStart` timestamp therefore includes the pacing interval in the next start-to-start cadence instead of shifting the delay into a limiter-local graph or the following simulation delta.
- `SubmissionGate` waits after CPU recording/work and immediately before D3D12 `ExecuteCommandLists` or Vulkan `vkQueueSubmit`. It is not a delay immediately before the current `Present` and is not labeled Front Edge Sync.

Both candidates use independent steady-clock deadline state. The first observation establishes `next = now + period`; an on-time observation waits until the deadline and advances from actual release; a late observation applies no wait and rebases from observed time. They never discard, replace, or skip input/simulation/render work. The resolved policy is copied at released FrameStart, so a Profiler change during the frame affects only the next frame and cannot apply both candidates to one frame.

The frame trace reports candidate, requested deadline, actual release, intentional wait, deadline miss, consecutive released-FrameStart cadence, CPU active work, native submission, `Present`, and later GPU-completion observation. Inter-frame wait is outside current-frame CPU active duration; submission-gate wait is subtracted from active work. DXGI latency-object and Vulkan acquire/fence waits remain mandatory and separately classified. Display cadence, replacement/drop, and input-to-photon latency remain explicitly unavailable rather than inferred from `Present`.

The current local D3D12/Vulkan smokes use a deliberately low 5 FPS target to make control-point placement and nonzero waits unambiguous. They qualify implementation seams and telemetry, not timer precision, visible smoothness, display cadence, latency, or the production default. The remaining roadmap bake-off must test maintainable target cadences, GPU headroom, realistic presentation modes, external RTSS `ASYNC`, and appropriate display/input measurement before selecting a production behavior.

Before that bake-off, the engine needs a bounded benchmark-capture artifact rather than one mutable last-frame snapshot. It must retain raw frame-ID-keyed lifecycle records, preserve spikes and unavailable fields, derive deterministic distributions and low-percentile throughput statistics, and export stable CSV/JSON plus a condition manifest. Engine records and PresentMon rows must remain separate source streams joined by explicit frame/process/time evidence; absent display, replacement, input, VRR, or GPU-headroom observations stay unavailable. RTSS configuration is not mutated automatically unless a safe reversible per-process interface is proven.

The implemented capture publishes immutable renderer snapshots and schema-2 CSV/JSON. Percentiles use sorted nearest-rank samples; the 1% and 0.1% low FPS summaries are `1000 / p99` and `1000 / p99.9` start-to-start frametime. Requested and effective policy targets are separate manifest fields. Presentation/sync/VRR/tearing strings are explicit runner inputs that default to `unknown`, and display/replacement/input/GPU-headroom stay literal unavailable fields until an external measurement source provides them. Schema 2 adds run ID, process ID, canonical executable path, QPC frequency, and lifecycle QPC ticks without treating any of them as display evidence.

### Windows PresentMon Correlation Prerequisite

Windows production-pacing measurement requires a separate PresentMon source stream rather than treating engine `Present` timestamps as display feedback. The engine now has an external-capture attachment boundary: after renderer/window initialization it publishes schema-1 readiness with a unique run ID, exact process ID/canonical executable path, QueryPerformanceCounter frequency/current tick, artifact location, and requested condition metadata, then waits on a bounded exact run/PID supervisor release before its 30-frame warm-up and retained window begin. Timeout or cancellation fails without publishing a benchmark artifact; running without the attachment option preserves the existing benchmark path.

Once readiness is published, the supervisor may start a uniquely named bounded PresentMon session attached to the exact editor PID, record PresentMon path/version/arguments, and release the engine gate only after the collector is ready. It owns cleanup of both process trees. Engine lifecycle export uses explicit QPC ticks plus frequency; it does not assume that `std::chrono::steady_clock` shares PresentMon's epoch.

Only PresentMon rows whose `ProcessId` matches the launched editor may enter the run. Raw engine and PresentMon CSV data remain separate. A deterministic join report pairs ordered engine `PresentBegin` observations with PresentMon presents in the common QPC domain and rejects missing, ambiguous, non-monotonic, or reused rows instead of guessing. The runner asserts the exact headers actually produced by the installed PresentMon binary before interpreting `MsBetweenPresents`, display-change/display-latency fields, presentation mode, or displayed/dropped classification.

PresentMon correlation may qualify present/display cadence and row classification for the exact captured process. It does not by itself identify the physical monitor, prove VRR state, prove RTSS/FES/frame-generation configuration, provide engine GPU timestamps/headroom, define replacement semantics, or measure click-to-photon latency. Those conditions remain declared `unknown` or `unavailable` until separately observed.

The NVIDIA FrameView PresentMon `1.7.12119.0` build exits without CSV in this account, but the official portable Intel/GameTechDev PresentMon `1.10.0` console asset works non-elevated when attached through the readiness gate. A local D3D12 diagnostic produced 528 exact-PID QPC rows: 16 warm-up rows followed by 512 rows causally aligned with the retained engine presents. The parser/join contract is a separate prerequisite before live collector supervision: preserve both raw streams, require the exact observed header set and common QPC frequency/process identity, pair exactly one PresentMon row in each engine-present interval, bound the final row, and fail closed on missing, ambiguous, reused, non-monotonic, or noncausal input.

### Required Frame-Lifecycle Telemetry Prerequisite

The prerequisite trace now carries the resolved project/runtime policy and one authoritative application frame ID across frame start, input/simulation, render submission, `Present` begin/end, and GPU-completion observation. It distinguishes intentional pacing wait from mandatory DXGI latency-object waiting and Vulkan acquire/fence correctness waiting. Backend capability/fallback diagnostics publish display cadence, replacement/drop, and input-to-photon latency as unavailable when those observations do not exist; a `Present` timestamp is not display feedback.

Deterministic trace tests prove frame-ID continuity, phase ordering, unavailable-state truthfulness, and separation of mandatory versus intentional waits. Native pacing tests consume this boundary without reclassifying the existing D3D12 maximum-latency wait, Vulkan acquire/fence waits, or a delay immediately before the current `Present` as smoothing.

### Presentation Measurement Rules

Use PresentMon on Windows performance runs to compare `MsBetweenPresents`, `MsBetweenDisplayChange`, `MsUntilDisplayed`, `DisplayLatency`, and presentation mode with the engine's own markers. These metrics describe different pipeline stages; a stable `Present` cadence alone does not prove a stable display cadence.

Click-to-photon claims require an appropriate input/display measurement path. Engine timestamps and presentation telemetry are necessary for diagnosis, but do not by themselves measure peripheral or panel response latency.

Evaluate external pacing tools, driver settings, VRR modes, and frame-generation modes as named test conditions. Do not promote one configuration to a default based on an overlay alone or assume a fixed latency cost across APIs and hardware.

Compare `Responsive`, the engine's RTSS-ASYNC-inspired inter-frame wait, the separately instrumented submission-gate interpretation from the supplied research, external RTSS `ASYNC`, and FES as distinct named conditions. Test uncapped/high-cap competitive workloads, maintainable caps with GPU headroom, fixed-refresh synchronized presentation, VRR, and frame generation where supported. Report p50/p95/p99 frame and latency measurements plus 1%/0.1% lows. `Smooth Frametime` succeeds only when its displayed-cadence improvement and throughput/latency cost are both visible; intentionally reduced throughput or a flat application-side graph is never sufficient evidence. Keep VSync/VRR/tearing, present replacement/drop policy, maximum frames in flight, fixed simulation cadence, and portable/vendor latency modes as independent axes in the test matrix.

Instrumentation must identify the pacing control point: inter-frame wait after prior submission/`Present`, gate before GPU submission, or FES-style delay immediately before the current `Present`. Record requested and actual wake/release times, sleep and high-resolution-wait duration, overshoot, CPU/GPU queue depth, GPU utilization/headroom, and the frame ID observed at every simulation/render/submit/present/display marker. This prevents a limiter hook from making its own frametime graph look stable while display cadence or input latency remains poor.

The in-game frametime plot uses consecutive engine `FrameStart` markers as its primary cadence series, not consecutive samples taken at `Present` or at the active limiter hook. Plot CPU active work and intentional pacing wait separately, then correlate present and actual display intervals by frame ID. Preserve raw spikes and deadline misses in the history; summaries supplement the trace but never replace it. An external Afterburner/RTSS graph is a reference capture only and cannot validate the engine graph or displayed smoothness by itself.

Required debug views:

- Overdraw.
- Quad overdraw/helper-lane waste.
- Texture size/mip/residency.
- Material complexity.
- Shader cost.
- Draw/meshlet/cluster IDs.
- LOD choice and projected triangle area.
- Reflection source/confidence.
- AO radius/confidence.
- Shadow caster inclusion/exclusion.
- Ray density and residual confidence.

## Instancing, Batching, And Mesh Merging

Draw calls matter, but blindly merging meshes is not the answer.

Decision order:

1. GPU-driven culling.
2. Instancing for repeated mesh/material sets.
3. Multi-draw indirect / execute indirect / meshlet dispatch.
4. Material sorting and bindless tables.
5. Selective static assembly merging.
6. Texture/material atlas or channel packing when it actually reduces state/material count.

Instancing rules:

- Use for repeated identical meshes with shared material/shader state and per-instance transforms/data.
- Support per-instance material overrides through compact indices, not unique shader state.
- Cull instances on GPU before draw emission.
- Keep instance data stream compact and cache-friendly.

Mesh merging rules:

- Merge only when objects are known to move together, stream together, share lifetime, and share LOD behavior.
- Merge static kits/assemblies when it reduces draw/setup cost without hurting culling.
- Do not merge across rooms, occluders, streaming cells, lighting zones, or different LOD needs.
- Do not merge just to reduce draw calls if it creates huge bounds and ruins occlusion/LOD.
- Prefer cluster/meshlet grouping over old "one giant mesh" batching.

Material merging rules:

- Reduce material slots where possible.
- Use texture atlases/arrays/virtual texture sets for related material variants.
- Pack compatible masks into channels.
- Preserve physically meaningful roughness/normal quality.
- Avoid merging materials if it creates shader permutation explosion or texture streaming waste.

## Meshlet LOD And Distance Swaps

The engine should combine user-friendly automatic LOD with GPU-friendly cluster selection:

```text
source mesh -> import cleanup -> topology optimization -> meshlets -> cluster hierarchy -> distance/error-based runtime selection
```

Runtime selection uses:

- Projected screen-space error.
- Projected triangle area.
- Silhouette importance.
- Material/detail frequency.
- Normal/curvature change.
- Quad utilization estimate.
- Page residency.
- Motion speed and camera speed.
- Hardware tier.

LOD swaps should be:

- Deterministic.
- Spatially stable.
- Coverage-aware.
- Free of frame-varying stochastic dither.
- Biased toward coarser clusters when triangles would become subpixel or quad-inefficient.

## Triangle Topology And Quad Utilization

The screenshot points to the right concern. Modern GPUs shade in 2x2 quads, so small or skinny triangles waste helper lanes. A single covered pixel in a quad can still cause work for the whole quad. If many tiny triangles cross the same quad, the shader may run repeatedly for the same screen area.

Decision:

```text
Auto import and auto LOD must optimize for projected area and triangle quality, not just triangle count.
```

Import/LOD scoring should include:

- Minimum triangle angle.
- Aspect ratio / sliver score.
- Area distribution.
- Projected screen-space triangle area.
- Estimated 2x2 quad occupancy.
- Cluster surface-area-to-boundary ratio.
- Vertex cache efficiency.
- Overdraw.
- Material/UV/normal seam preservation.
- RT BVH quality, including AABB overlap from elongated triangles.

Rules:

- Avoid triangle fans for disks/caps unless they are tiny or unshaded.
- Avoid long skinny triangles where raster or RT cost matters.
- Prefer near-equilateral triangles when triangulating large filled regions.
- Use max-area/quality triangulation for caps and planar polygon fills where compatible with UVs/materials.
- Preserve silhouettes and deformation constraints over pure max-area triangulation.
- For far LODs, reduce interior tessellation aggressively and keep silhouettes stable.
- For meshlets, prefer compact coherent clusters with good area/perimeter ratio.

Important nuance:

Maximizing triangle area is not the only goal. A giant long sliver can have large area and still be bad. The goal is high useful covered area per shaded quad, good triangle shape, and stable silhouettes.

## Sources

- Frostbite Stochastic Screen-Space Reflections: https://www.ea.com/news/stochastic-screen-space-reflections
- PresentMon console metrics: https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md
- NVIDIA PC latency measurement: https://developer.nvidia.com/blog/understanding-and-measuring-pc-latency/
- Tomasz Stachowiak, Stochastic SSR notes: https://h3.gd/stochastic-ssr/
- SIGGRAPH 2015 Advances course: https://advances.realtimerendering.com/s2015/
- Hybrid screen-space reflections: https://interplayoflight.wordpress.com/2019/09/07/hybrid-screen-space-reflections/
- Efficient GPU Screen-Space Ray Tracing: https://jcgt.org/published/0003/04/04/
- NVIDIA HBAO+ source: https://github.com/NVIDIAGameWorks/HBAOPlus
- XeGTAO: https://github.com/GameTechDev/XeGTAO
- Practical Realtime Strategies for Accurate Indirect Occlusion: https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf
- Shadow Caster Culling for Efficient Shadow Mapping: https://dcgi.fel.cvut.cz/en/publications/2011/bittner-i3d-scc/
- Threat Interactive, Took For Granted: Why Fox Engine Is So Crazy Optimized: https://www.youtube.com/watch?v=aB5qxp6SPPQ
- Metal Gear Solid V Graphics Study: https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/
- Photorealism Through the Eyes of a FOX, GDC Vault: https://www.gdcvault.com/play/1031807/Photorealism-Through-the-Eyes-of
- Khronos PBR Neutral Tone Mapper: https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md
- Blender AgX color management notes: https://developer.blender.org/docs/release_notes/4.0/color_management/
- ACES project: https://www.oscars.org/science-technology/sci-tech-projects/aces
- Intel GPA: https://www.intel.com/content/www/us/en/developer/tools/graphics-performance-analyzers/overview.html
- NVIDIA Nsight Graphics: https://docs.nvidia.com/nsight-graphics/index.html
- AMD Radeon GPU Profiler: https://gpuopen.com/rgp/
- Microsoft PIX GPU captures: https://devblogs.microsoft.com/pix/gpu-captures/
- AMD FSR Redstone: https://gpuopen.com/learn/amd-fsr-redstone-developers-neural-rendering/
- AMD FSR Ray Regeneration: https://gpuopen.com/amd-fsr-rayregeneration/
- AMD FSR Radiance Caching: https://gpuopen.com/amd-fsr-radiancecaching/
- RTX Neural Texture Compression: https://github.com/NVIDIA-RTX/RTXNTC
- D3D12 Shader Execution Reordering: https://devblogs.microsoft.com/directx/ser/
- D3D12 Cooperative Vector: https://devblogs.microsoft.com/directx/cooperative-vector/
- Humus triangulation note: https://www.humus.name/index.php?ID=228&page=Comments
- Self Shadow, Counting Quads: https://blog.selfshadow.com/2012/11/12/counting-quads/
- NVIDIA, Creating Optimal Meshes for Ray Tracing: https://developer.nvidia.com/blog/creating-optimal-meshes-for-ray-tracing/
- AMD Mesh Shaders Optimization and Best Practices: https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
- meshoptimizer: https://meshoptimizer.org/
