# LOD Transitions And Native Multithreading Decisions

Status: Draft v0.1
Date: 2026-07-06

Purpose: decide how Decima-style LOD fading, automated scan/LOD processing, and native multithreading fit the engine.

## Short Decisions

| Topic | Decision |
| --- | --- |
| Decima-style LOD fades | Adopt stable ordered/complementary dithering for short LOD transitions. |
| Temporal dither | Reject frame-varying TAA-dependent dither as a baseline. |
| Transition length | Timed, triggerable, budgeted, and short enough to reduce pop without causing long double-draw cost. |
| LOD automation | Use an automated importer/LOD builder, not manual hundreds-of-samples labor. |
| Scan conversion | Convert raw scanned meshes into clean runtime LOD0, meshlet clusters, microdetail textures, and lower LODs. |
| Native multithreading | Core architecture: job system, task graph, data snapshots, multithreaded command recording, async asset/shader/build pipelines. |
| Visual scripting | Prefer generated native code or bytecode-to-native hot paths for game/editor graphs, not slow interpreted graphs in hot runtime. |

## Decima LOD Fade Assessment

The image claims Decima uses:

- Ordered dithering.
- Subtle transitions under motion.
- Triggerable and timed LOD transitions.
- Fast transitions to relieve overdraw and draw calls.

I could not verify that exact checklist as an official Guerrilla slide from the public primary sources I found. However, the technique is coherent and matches known production approaches:

- Dithered opacity masking avoids full translucency sorting and keeps depth/occlusion behavior closer to opaque rendering.
- Complementary dither masks can make two LODs stitch together during transition.
- Short transitions hide popping without keeping both LODs alive for too long.
- Too many simultaneous transitions can spike draw calls and uniform/instance updates.

Decision:

```text
Adopt Decima-like ordered/complementary dithered LOD transitions, but make them spatially stable, non-temporal, budgeted, and optional per asset/platform.
```

## LOD Fade Rules

Baseline transition:

```text
LOD A fading out uses ordered mask M < threshold
LOD B fading in uses complementary mask M >= threshold
```

Rules:

- Use ordered or blue-noise-like spatial masks that are stable in screen/object space for the transition.
- Do not use frame-varying stochastic dither that depends on TAA to look correct.
- Prefer complementary masks so the pair remains approximately watertight.
- Keep transitions short. Start with 0.15 to 0.35 seconds for ordinary props and tune by asset class.
- Allow instant/pop transitions for tiny or low-importance assets where fade cost is not worth it.
- Allow longer transitions only for hero assets, huge HLOD tiles, or cinematic camera moves.
- Limit the number of simultaneous fading objects/clusters per frame.
- If the fade budget is exceeded, finish high-priority transitions first and snap/shorten low-priority ones.
- During transition, both LODs must be counted in overdraw/draw-cost budgets.
- LOD fading must be visible in debug overlays.

Asset controls:

```text
Transition mode: instant | ordered-dither | geometric morph | impostor crossfade
Transition duration: auto/manual
Priority: hero | gameplay | normal | background
Budget group: character | foliage | prop | terrain | HLOD | scan
Mask space: object | screen | cluster
```

## Ordered Dither Caveats

Ordered dithering is not magic.

Good:

- Avoids harsh popping.
- Cheaper and simpler than real translucency.
- Can preserve depth testing and occlusion behavior.
- Works well for short transitions.
- Compatible with no-temporal baseline if the mask is stable and resolution-aware.

Bad:

- Draws both LODs during the transition.
- Can look stippled without TAA if transitions are too slow or masks are too coarse.
- Can shimmer if the mask changes frame-to-frame.
- Can spike draw calls if many objects transition together.
- Can be poor for very thin foliage if coverage/MSAA is not handled correctly.

Our rule:

```text
Ordered dither is a transition tool, not a permanent transparency mode.
```

## LOD AI / Automated Scan Conversion

The second image describes manual labor:

- Convert raw photogrammetry meshes.
- Produce LOD0 with max-area-like triangulation and polygonal density smoothing.
- Bake normals and depth bias/microdetail textures.
- Handmake LODs based on overdraw view.

The engine should automate this.

Decision:

```text
The importer builds runtime mesh data from raw scans automatically, with artist review and override.
```

Automated output:

1. Clean topology and remove scan noise.
2. Generate a runtime LOD0 that preserves silhouettes and visible shape but avoids pathological triangles.
3. Smooth polygonal density: more triangles where curvature/silhouette/material detail need them, fewer where texture/normal/depth can represent detail.
4. Build meshlets/clusters with good quad utilization and RT BVH quality.
5. Bake microdetail:
   - Normal.
   - Height/depth.
   - Bent normal/occlusion where useful.
   - Cavity/curvature.
   - Optional depth-bias/micro-shadow data.
6. Generate lower LODs and HLOD/cluster hierarchy.
7. Generate impostor/card/volume/aggregate representations for asset classes where triangles fail.
8. Produce diagnostics:
   - Overdraw view.
   - Quad waste.
   - Sliver triangles.
   - Projected triangle area histogram.
   - LOD transition cost.
   - Texture/microdetail error.
   - Silhouette error.

AI/ML role:

- Suggest cleanup, retopology, density smoothing, LOD thresholds, and microdetail bake settings.
- Classify asset type and choose a default LOD strategy.
- Predict bad transitions and show error heatmaps.
- Never silently replace source data. All generated outputs must be inspectable and reproducible.

## Microdetail And Depth Bias

For scanned assets, lower triangle density must be compensated by better material/detail representation.

Use:

- Normal maps for high-frequency shading.
- Height/depth maps for parallax, contact, and micro-shadow cues.
- Curvature/cavity maps for material response and dirt/wear.
- Optional screen-space or material-space micro-shadow term.
- Per-material displacement/parallax only where it survives motion clarity tests.

Depth-bias/microdetail data is useful, but it must not create crawling. It needs:

- Correct mips.
- Stable derivatives.
- Specular anti-aliasing.
- Distance fade to normal/roughness representation.
- Debug view for bias strength and self-shadow artifacts.

## Native Multithreading Architecture

Native multithreading is not optional. It is a foundation.

The engine should use:

- A central job system with work stealing.
- A frame task graph for simulation, animation, physics, visibility, rendering prep, streaming, and tools.
- Data-oriented snapshots between major phases.
- Multithreaded scene traversal and culling.
- Multithreaded command recording.
- Async asset import, mesh cluster building, texture compression, shader compilation, and light baking.
- Async file I/O and decompression.
- Dedicated queues for render prep, streaming, compile/build work, and editor automation.
- GPU async compute/copy where profiling shows overlap without latency or barrier harm.

Hot runtime paths should avoid:

- Main-thread-only scene mutation.
- Global locks in simulation/render prep.
- Blocking file I/O.
- Blocking shader compilation.
- Synchronous mesh/texture page loads.
- Interpreted visual graph logic in per-frame hot paths.

## Frame Task Model

Frame-level model:

```text
Input snapshot
  -> gameplay jobs
  -> animation jobs
  -> physics jobs
  -> scene transform finalization
  -> visibility/culling jobs
  -> render packet build
  -> multithreaded command recording
  -> GPU submission
```

Rules:

- Systems write into staged buffers or command streams, then publish.
- Renderer consumes immutable frame snapshots.
- Editor/AI automation schedules jobs and produces undoable changes; it does not mutate live engine state from arbitrary threads.
- Streaming requests are produced by render/asset systems and fulfilled asynchronously.
- Profiler must show CPU worker occupancy, job queues, stalls, locks, and task dependencies.

## Native Graphs And Automation

Decima publicly describes NodeGraph-style visual programming that can scale across systems and compile to native code. This is highly aligned with our product goal.

Decision:

```text
Guided workflows and visual scripting should compile or lower to native/hot runtime code where performance matters.
```

Use:

- Visual graphs for game logic, animation, audio, effects, materials, and automation workflows.
- Generated C++/C#/native IR for hot runtime.
- Interpreted/debug mode for editor iteration.
- Deterministic graph validation.
- Profiling per graph node and generated function.
- Source maps from generated code back to graph nodes.

This gives beginners a friendly workflow without making the runtime slow or opaque.

## Sources

- Decima official page: https://www.guerrilla-games.com/decima
- Decima Visibility in Horizon Zero Dawn: https://www.guerrilla-games.com/read/decima-engine-visibility-in-horizon-zero-dawn
- Decima Advances in Lighting and AA: https://www.guerrilla-games.com/read/decima-engine-advances-in-lighting-and-aa
- Nodes and Native Code: DECIMA's Visual Programming for Every Discipline: https://www.guerrilla-games.com/read/Nodes-and-Native
- Cesium dithered LOD transitions: https://cesium.com/blog/2022/10/20/smoother-lod-transitions-in-cesium-for-unreal/
- Unreal dithered LOD transition forum performance discussion: https://forums.unrealengine.com/t/option-to-disable-dithering-between-lod-transitions-on-hismcs-and-foliage-in-4-13/70522
- GameDev StackExchange ordered dithering discussion: https://gamedev.stackexchange.com/questions/47844/why-are-some-games-using-some-dithering-pattern-instead-of-traditional-alpha-for
- Vulkan async compute sample: https://docs.vulkan.org/samples/latest/samples/performance/async_compute/README.html
- NVIDIA async compute and overlap: https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/
- meshoptimizer: https://meshoptimizer.org/
