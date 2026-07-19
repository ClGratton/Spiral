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

The project-owned policy is versioned in the `.spiralproject` manifest. `Responsive` is the default for competitive/esports work and has no intentional wait. `Smooth Frametime` is opt-in with a finite target in the inclusive 1-1000 FPS range; user/game resolution always selects the engine-owned `InterFrame` control point. The top-bar Settings menu owns that serialized project default and save action. A public `GameFramePacingSettings` can inherit the project, force `Responsive`, or force `Smooth Frametime`; it always resolves against the project target, forces `InterFrame` when Smooth is effective, and invalid Smooth target updates preserve the prior state. The explicit `--smooth-frametime-candidate` benchmark/smoke CLI, not the Profiler or game settings, owns separately named InterFrame and SubmissionGate experiments. No duplicate dockable Project Settings window exists.

`Responsive` resets pacing state and records `behavior=no-intentional-wait`. Opt-in product/game `Smooth Frametime` uses the engine-owned `InterFrame` deadline path:

- `InterFrame` waits after the prior frame's submission/`Present` but before the next loop samples its timestep, publishes `FrameStart`, or runs input/simulation. The released `FrameStart` timestamp therefore includes the pacing interval in the next start-to-start cadence instead of shifting the delay into a limiter-local graph or the following simulation delta.
- `SubmissionGate` waits after CPU recording/work and immediately before D3D12 `ExecuteCommandLists` or Vulkan `vkQueueSubmit`. It is retained only as a separately named developer benchmark experiment, rejected for production because the retained 60/120 FPS traces show materially worse 51.706/96.296 ms engine p99 tails. It is not a delay immediately before the current `Present` and is not labeled Front Edge Sync.

Both control points retain independent steady-clock deadline state for their separately named diagnostics. The first observation establishes `next = now + period`; an on-time observation waits until the deadline and advances from actual release; a late observation applies no wait and rebases from observed time. They never discard, replace, or skip input/simulation/render work. The resolved policy is copied at released FrameStart, so a policy or explicit benchmark-condition change during the frame affects only the next frame and cannot apply both control points to one frame.

The frame trace reports candidate, requested deadline, actual release, intentional wait, deadline miss, consecutive released-FrameStart cadence, CPU active work, native submission, `Present`, and later GPU-completion observation. Inter-frame wait is outside current-frame CPU active duration; submission-gate wait is subtracted from active work. DXGI latency-object and Vulkan acquire/fence waits remain mandatory and separately classified. Display cadence, replacement/drop, and input-to-photon latency remain explicitly unavailable rather than inferred from `Present`.

The current local D3D12/Vulkan smokes use a deliberately low 5 FPS target to make control-point placement and nonzero waits unambiguous. The retained production bundle additionally covers maintainable 60/120 target conditions and PresentMon correlation. Together they support the project production decision but do not prove visible smoothness, physical display cadence, active VRR, peripheral latency, or click-to-photon. Those are optional future validation, not a selection gate.

Live target application is now an explicit state transition rather than an assignment-only side effect. Each control point retains its target, next deadline, and last actual release independently. Publishing the same resolved policy does not disturb phase. A target or candidate change discards that control point's obsolete deadline and derives the next boundary from its last actual release; it waits if that boundary is still in the future, otherwise it records the miss and rebases from the current observation. The first-ever observation still establishes phase without an artificial startup wait. Editor publication during a released frame cannot alter that frame's frozen policy; InterFrame consumes the change at the next loop boundary, while SubmissionGate consumes it on the next frame's native pre-submit boundary.

Effective-limiter diagnostics are causally frame-aligned. `StartToStartMilliseconds` on record `N` is `FrameStart[N] - FrameStart[N-1]`; an applied InterFrame release uses the frozen policy on `N`, while SubmissionGate wait, CPU work, GPU work, and exact `Present` evidence use the retained record and frozen policy on `N-1`. Each successful classification publishes both its category and source frame. Delayed exact GPU evidence for `N-1` reclassifies only retained cadence `N`; first/gapped/evicted/missing evidence and multiple simultaneous qualifying sources remain `Unresolved`. D3D12 and Vulkan publish present success against the exact application-frame attempt, and a failed Vulkan attempt clears prior success before publication. The requested-cadence acceptance band is `max(1 ms, 15% of period)` to admit measured low-rate native scheduling jitter only when an exact applied pacing wait is still the sole qualifying source; CPU/GPU/present fill tests retain their separate tighter tolerance. None of these engine intervals are display cadence.

Before that bake-off, the engine needs a bounded benchmark-capture artifact rather than one mutable last-frame snapshot. It must retain raw frame-ID-keyed lifecycle records, preserve spikes and unavailable fields, derive deterministic distributions and low-percentile throughput statistics, and export stable CSV/JSON plus a condition manifest. Engine records and PresentMon rows remain separate source streams joined by explicit frame/process/time evidence; absent display, replacement, input, or VRR observations stay unavailable. Engine GPU headroom is a separate same-frame calculation and is not display evidence. RTSS configuration is not mutated automatically unless a safe reversible per-process interface is proven.

The installed RTSS 7.3.5.28001 interface was audited read-only on 2026-07-18 and does not prove that interface: its documented SDK profile operations (`Enum`/`Load`/`Get`/`Set`/`Save`/`Update`) expose `FramerateLimit` but no ASYNC/limiter-mode encoding; its ReadMe distinguishes default async, FES/back-edge, and Reflex without documenting that encoding; `Save`/`Update` lack documented atomic/crash recovery and `UpdateProfiles` reloads all running 3D applications. Shared memory remains telemetry rather than control. Exact-Editor RTSS ASYNC is therefore unavailable to automation; config guessing and UI automation are prohibited. A manual comparison may proceed only through the architecture manual gate: exact Editor path/profile and RTSS version/hash, before/after/restored captures of limiter target/mode plus FES/scanline/frame-generation and scope, one bounded benchmark, immediate exact restoration even on failure/timeout, and an operator-signed receipt. It may change no global/other profile, driver, FES, or frame-generation state implicitly. This protocol closure is not RTSS execution, display, latency, or production-selection evidence.

#### External Source Audit And Shipped-Behavior Boundary (2026-07-18)

Shipped `Smooth Frametime` is wholly engine-owned: it must not require RTSS, MSI Afterburner, their installation, runtime process, configuration/profile mutation, or shared-memory access. The selected opt-in `InterFrame` path and engine Platform deadline clock provide the internal-cap mechanism: release occurs after the prior `Present` and before the next frame's timestep, `FrameStart`, input, and simulation. It does not depend on an unproven story that RTSS holds completed CPU instructions. RTSS/Afterburner remain optional, separately named laboratory/reference conditions under the bounded manual gate, never a runtime dependency or selected default. The decision is `Responsive` by default and opt-in `InterFrame` Smooth at a maintainable target; `SubmissionGate` is rejected as production, and FES, Reflex, and frame generation remain distinct optional experiments.

The supplied secondary source is Hardware Accent, *[120 FPS With 120 FPS 1% Lows… Too Good To Be True?](https://www.youtube.com/watch?v=5vXedjxa99U)*, supplied as uploaded 2026-03-06. Its video claim warns that RTSS frame-start metrics can hide on-screen/display irregularity; its description reportedly recommends Frame Presentation and calls FES best. A later supplied comment instead recommends Async plus a cap, VRR, and driver VSync, while saying an FES graph can be misleading. These are contradictory source hypotheses, not implementation facts or accepted engine behavior. The supplied comment's edit date, `05/29/27`, is future-dated relative to the repository date (2026-07-18), so its provenance and ordering are unresolved. Preserve the separate control points during any evaluation: engine `InterFrame` is the post-prior-`Present`/pre-next-input cadence mechanism; `SubmissionGate` is pre-native-submit; FES is a distinct delay immediately before the current `Present`; and Reflex-style JIT/queue reduction is a distinct optional vendor latency mechanism. None may be renamed, silently substituted, or inferred equivalent from a graph.

Primary contracts bound the next evidence work. Microsoft describes the DXGI frame-latency waitable object as allowing the prior presentation to finish before the application starts drawing the next frame, so it remains a mandatory pre-render latency control, not an intentional smoothness wait ([DXGI frame-latency waitable object](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)). Microsoft documents the D3D flip-model VRR-compatible/tearing path as an allow-tearing swapchain flag and `Present(0, DXGI_PRESENT_ALLOW_TEARING)` when supported; it is distinct from the current synchronized `Present(1, 0)` bootstrap and does not prove driver/display VRR is active ([Variable refresh rate displays](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays)). PresentMon derives OS/driver presentation scheduling from ETW and reports presentation/display timing fields, useful for correlation but not proof of panel photons or peripheral input ([GameTechDev PresentMon](https://github.com/GameTechDev/PresentMon)). NVIDIA describes LDAT as a cross-platform, all-GPU hardware luminance sensor for motion/click-to-photon measurement; an equivalent photodiode/high-speed-camera path is also admissible only with an explicit method/receipt ([NVIDIA LDAT](https://developer.nvidia.com/nvidia-latency-display-analysis-tool)).

Production selection therefore admits evidence by source class. PresentMon/native present timing may establish OS/presentation scheduling correlation only. Physical display cadence and input latency require a deterministic engine-owned high-contrast response marker plus exact input/frame identity and an optical hardware observation. A VRR condition additionally requires a receipt of exact monitor/connection, OS refresh configuration, driver/display state, engine requested and actual presentation capability/mode, and optical display cadence; an engine/API capability bit is not evidence that VRR was active. Until this repository has such hardware/control paths, physical-display, VRR-active, peripheral-input, and click-to-photon fields remain unavailable, and production selection is blocked. The roadmap carries explicit presentation-policy and optical/input-correlation prerequisites before the comparison.

The optical/input-correlation prerequisite now provides a schema-1 engine-owned readiness artifact and bounded receipt writer. In optical mode Core arms one first physical mouse-button press (or an explicitly labeled synthetic smoke trigger), binds it after `PollEvents` to the immutable current-frame input sample, then atomically publishes a new-only readiness artifact containing canonical Windows process/executable/QPC identity, trigger/marker/input/frame identity, backend, and engine-owned requested/actual presentation generation before the Editor presentation client draws one full-window white foreground marker in the same headed native ImGui path. `WriteOpticalInputCorrelationReceipt.ps1` admits only declared `LDAT`, `Photodiode`, or `HighSpeedCamera` raw observations that bind the exact process/QPC/trigger/marker/input/frame identities in a named timing domain; it rejects stale, duplicate, ambiguous, noncausal, missing, and surrogate observations, hashes raw evidence, refuses receipt collisions, and distinguishes its removable temporary publication from operator-owned device/raw evidence. PresentMon, `PresentEnd`, API capability, and overlays are rejected as optical sources. This is control and receipt instrumentation only: without actual hardware observations it does not establish photons, panel cadence, active VRR, peripheral input, or click-to-photon latency, and it changes no pacing, Reflex/FES/RTSS/Afterburner, or driver state.

The implemented capture publishes immutable renderer snapshots and schema-7 CSV/JSON. Percentiles use sorted nearest-rank samples; the 1% and 0.1% low FPS summaries are `1000 / p99` and `1000 / p99.9` start-to-start frametime. Requested and effective pacing targets are separate manifest fields. Schema 2 added run/process/QPC identity, schema 3 exact-frame GPU timing/headroom, schema 4 cadence predecessor and limiter-source identity, schema 5 exact input-sample source plus input-to-simulation/submit/`PresentEnd`, and schema 6 deadline-wait telemetry. Schema 7 replaces spoofable runner presentation labels with engine-owned requested presentation policy, capability, actual native mode, fallback, native generation, semantic synchronization, and tearing-present state; VRR-active remains `unavailable`. PresentMon join/supervision accepts schemas 2-7 and never converts engine-stage input or API/present capability into display evidence.

`PresentationPolicy` is project-owned and independent from Smooth Frametime: default `Synchronized`; opt-in `TearingAllowed` permits only a documented native tearing/immediate path and never claims VRR-active. A requested value, last-applied request, actual native mode, and generation are separate: an unsupported fallback commits the request once and is not recreated every frame. Renderer diagnostics keep requested policy, capability, actual native mode, fallback, sync interval, tearing-present flag, generation, and effective frame distinct, committing actual only after native success. D3D12 applies a pending replacement before that frame's waitable-object latency wait, then uses the replacement wait chain and present for the same generation; factory allow-tearing capability is separate from window/fullscreen eligibility. Vulkan transitions before acquire/fence/submit and chooses FIFO for synchronized and IMMEDIATE only for supported TearingAllowed, else FIFO; MAILBOX and FIFO_RELAXED are rejected. Failed recreation restores the prior native mode and emits one final truthful requested/actual/fallback marker, or publishes unavailable if restoration fails. Mandatory waits, exact frame identity, and no engine-side discard remain invariant. Schema-7 benchmark/attachment conditions are frozen at capture start and engine-own requested policy, capability, actual mode, fallback, sync, tearing, and generation; VRR remains `unavailable`. Monitor/VRR-active/display/input evidence stays unavailable.

### Windows PresentMon Correlation Prerequisite

Windows production-pacing measurement requires a separate PresentMon source stream rather than treating engine `Present` timestamps as display feedback. The engine now has an external-capture attachment boundary: after renderer/window initialization it publishes schema-1 readiness with a unique run ID, exact process ID/canonical executable path, QueryPerformanceCounter frequency/current tick, artifact location, and requested condition metadata, then waits on a bounded exact run/PID supervisor release before its 30-frame warm-up and retained window begin. Timeout or cancellation fails without publishing a benchmark artifact; running without the attachment option preserves the existing benchmark path.

Once readiness is published, the supervisor may start a uniquely named bounded PresentMon session attached to the exact editor PID, record PresentMon path/version/arguments, and release the engine gate only after the collector is ready. It owns cleanup of both process trees. Engine lifecycle export uses explicit QPC ticks plus frequency; it does not assume that `std::chrono::steady_clock` shares PresentMon's epoch.

Only PresentMon rows whose `ProcessId` matches the launched editor may enter the run. Raw engine and PresentMon CSV data remain separate. A deterministic join report pairs ordered engine `PresentBegin` observations with PresentMon presents in the common QPC domain and rejects missing, ambiguous, non-monotonic, or reused rows instead of guessing. The runner asserts the exact headers actually produced by the installed PresentMon binary before interpreting `MsBetweenPresents`, display-change/display-latency fields, presentation mode, or displayed/dropped classification.

PresentMon correlation may qualify present/display cadence and row classification for the exact captured process. It does not by itself identify the physical monitor, prove VRR state, prove RTSS/FES/frame-generation configuration, provide engine GPU timestamps/headroom, define replacement semantics, or measure click-to-photon latency. Those conditions remain declared `unknown` or `unavailable` until separately observed.

The NVIDIA FrameView PresentMon `1.7.12119.0` build exits without CSV in this account, but the official portable Intel/GameTechDev PresentMon `1.10.0` console asset works non-elevated when attached through the readiness gate. A local D3D12 diagnostic produced 528 exact-PID QPC rows: 16 warm-up rows followed by 512 rows causally aligned with the retained engine presents. `Scripts/JoinPresentMonCorrelation.ps1` is the separate parser prerequisite: it preserves and hashes both raw streams, requires the exact observed header order plus common QPC frequency/process identity, pairs exactly one unused PresentMon row in each half-open engine-present interval, bounds the final row by an explicit QPC tolerance, and atomically writes a distinct schema-1 report. It fails closed on changed inputs, missing, ambiguous, reused, non-monotonic, or noncausal rows. The helper reports original PresentMon present/display fields separately and does not turn them into engine observations; live collector supervision remains the following prerequisite.

### Required Frame-Lifecycle Telemetry Prerequisite

The prerequisite trace now carries the resolved project/runtime policy and one authoritative application frame ID across frame start, input/simulation, render submission, `Present` begin/end, and GPU-completion observation. It distinguishes intentional pacing wait from mandatory DXGI latency-object waiting and Vulkan acquire/fence correctness waiting. Backend capability/fallback diagnostics publish display cadence, replacement/drop, and input-to-photon latency as unavailable when those observations do not exist; a `Present` timestamp is not display feedback.

Deterministic trace tests prove frame-ID continuity, phase ordering, unavailable-state truthfulness, and separation of mandatory versus intentional waits. Native pacing tests consume this boundary without reclassifying the existing D3D12 maximum-latency wait, Vulkan acquire/fence waits, or a delay immediately before the current `Present` as smoothing.

Input sampling is an ordered prerequisite, not an alias for `Frame.PublishInput`. The audited implementation currently calls `glfwPollEvents()` only from `Window::OnUpdate()` after UI/presentation at the tail of logical frame `N`, then stages `{N+1, timestep}` without a sample timestamp on the next iteration. That state cannot support a truthful input-latency interval. The accepted boundary is one explicit platform event poll/sample after renderer timing frame `N` exists and mandatory pre-frame backend waiting has completed, but before immutable frame-input publication and `InputSimulation`. It retains exact frame ID and monotonic sample time plus QPC only where that clock is active. Repositioning the poll must preserve synchronous callback/event dispatch and window-close behavior. Task staging, a prior-frame tail poll, GPU completion, and `Present` are not input samples. The source-identity prerequisite owns this ordering and rejection evidence; only the following latency-source item may derive same-ID input-to-simulation/submit/present intervals.

Current implementation (2026-07-18): `Window::PollEvents` replaces the ambiguous tail update. Active frames poll once after `Renderer::BeginFrame(N)` and immediately publish one immutable `RendererInputSample` before frame-input staging; minimized windows continue polling without an active timing record. The renderer retains the exact frame, steady-clock offset, and optional QPC tick and inserts `InputSample` into the lifecycle only through an atomic validator. D3D12/Vulkan native markers and schema-4 lifecycle rows prove ordering; they still serialize `inputLatency=unavailable` because no interval or display source is admitted by this prerequisite.

The following latency-source implementation resolves that same record progressively as `InputSimulation`, `RenderSubmission`, and `PresentEnd` arrive. It publishes only finite nonnegative ordered differences from the exact sample on that frame. Profiler and schema 5 label these as engine-stage intervals and retain the source frame. GPU completion remains an identity observation rather than a latency endpoint; input-to-display, generic end-to-end input latency, and click-to-photon remain unavailable.

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
