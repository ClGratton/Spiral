# Missing Research Audit 2026

Status: Draft v0.1
Date: 2026-07-06

Purpose: record the deeper research pass after the first architecture draft and identify additions that should be designed in before implementation hardens.

## Core Finding

The existing architecture is still coherent:

```text
native stable raster base
  + visibility buffer / compact G-buffer
  + current-frame sparse ray residuals
  + spatial reconstruction
  + optional accelerators after the baseline is already good
```

The missing pieces are mostly extension points and asset-pipeline standards, not replacements for the renderer.

## Accepted Additions

| Area | Decision |
| --- | --- |
| Optional AI/upscaling | DLSS, XeSS, FSR upscaling/frame generation, AMD Ray Regeneration, AMD Radiance Caching, NRD, RTXDI, and similar SDKs are allowed only as optional quality/performance modes. They must not be required to make the baseline image stable, sharp, or fast enough. |
| GPU-driven future path | Design the render graph and RHI so DirectX Work Graphs, mesh nodes, Vulkan device-generated commands, and shader enqueue can become backend paths later. Do not require them for v0. |
| Geometry compression | Evaluate AMD DGF/DGFS as the preferred open cluster/meshlet-oriented geometry compression path. Runtime storage must still decode to conventional meshlets on unsupported hardware. |
| RT geometry acceleration | Evaluate RTX Mega Geometry / CLAS as an optional RTX backend for high-density ray-traced geometry and cluster LOD. Do not make it a content requirement. |
| Neural texture compression | Keep RTX NTC and future cross-vendor neural material compression as optional asset-package modes. Baseline remains BC/ASTC/BC7/KTX2/Basis/virtual textures. |
| Material interchange | Add OpenPBR + MaterialX as material interchange/authoring targets. Compile them into the engine material model instead of replacing the Callisto/Proxima runtime BRDF. |
| Scene/asset interchange | Add USD/OpenAssetIO for editor/pipeline integration, glTF for lightweight runtime/import interchange, and KTX2/Basis for portable texture delivery. Runtime shipping data remains engine-cooked and streamable. |
| GI research | Borrow idTech 8's world radiance cache / irradiance volume structure and Activision Neural Light Grid ideas as references. Reject hidden pixel-history temporal filtering as a baseline. |
| AO/reflection update | Use GTAO/CACAO-style AO as the default instead of HBAO+. Use Frostbite/AMD SSSR as a BRDF-aware screen-space candidate, not as a temporal reflection solution. |
| Profiling | Treat Intel GPA, Nsight Graphics, Radeon GPU Profiler/RMV/GPU Detective, PIX, RenderDoc, and the in-engine profiler as part of the product, not optional developer trivia. |

## Optional Accelerator Doctrine

Temporal and neural features are not banned because they are evil. They are banned as a **foundation**.

A user on a mid-range GPU, for example an RTX 3060, should be allowed to enable DLSS, XeSS, FSR, or vendor denoisers when they want extra performance or higher settings. The difference is that the engine must already have:

- a stable native-resolution image,
- acceptable current-frame RT/raster fallback quality,
- clear debug views for every history-dependent path,
- an off switch that does not collapse the renderer,
- a performance profile showing what the accelerator actually improves.

This keeps the engine honest. Upscalers and neural denoisers become scalability features, not a way to hide unstable lighting, bad LOD, poor roughness filtering, or subpixel geometry noise.

## Accepted With Caution

### DirectX Work Graphs And Mesh Nodes

Work Graphs match the engine's direction: GPU-generated cluster work, material queues, tile classifiers, particle/VFX expansion, and ray work bins. They should influence the API boundaries now:

- render graph nodes must support GPU-generated indirect work,
- transient buffers must allow producer/consumer GPU stages,
- RHI must expose backend-native escape hatches,
- debugging must show graph expansion and queue sizes.

Do not require Work Graphs in v0 because driver/API maturity and cross-platform parity are still moving.

### AMD DGF / DGFS

DGF is strongly aligned with our cluster-based geometry. DGFS is especially interesting because it can decode either to DGF blocks or ordinary meshlets, so one asset package can serve future DGF hardware and current non-DGF GPUs.

Adopt as an evaluated cook target, not a mandatory storage format until prototypes prove:

- compression ratio,
- decode cost,
- vertex/index compatibility,
- ray tracing AS build benefit,
- importer complexity,
- streaming behavior.

### RTX Mega Geometry / CLAS

CLAS is a good optional backend for dense dynamic/ray-traced geometry and cluster LOD. It should inform the BLAS/TLAS abstraction and cluster metadata. It should not define the baseline because it is RTX-specific and still belongs behind backend capability checks.

### Neural Rendering

Track:

- DirectX Shader Model 6.9 / Cooperative Vector,
- Shader Execution Reordering,
- RTX Neural Texture Compression,
- AMD FSR Ray Regeneration,
- AMD FSR Radiance Caching,
- future Intel/cross-vendor neural compression and denoisers.

The engine should expose an `OptionalAccelerator` layer:

```text
Baseline pass output
  -> optional vendor/cross-vendor enhancer
  -> validation/debug source view
  -> final composite
```

No accelerator may become the only path for a feature.

## Rejected As Baseline

- TAA/TSR/DLSS/FSR/XeSS as required image stability.
- Temporal SSR denoising as the only way reflections look acceptable.
- HBAO+ as the default AO.
- Vendor-only RT geometry or neural compression as required content formats.
- bgfx as the main renderer abstraction.
- USD as runtime shipping format for hot rendering data.
- Full low-sample path tracing plus temporal denoising as the main renderer.

## Open Follow-Ups

1. Prototype DGF/DGFS against meshoptimizer clusters and the engine's visibility-buffer path.
2. Keep a Work Graphs design branch after the ordinary indirect renderer works.
3. Build a material import test: OpenPBR/MaterialX -> engine BRDF parameter set.
4. Add a reflection benchmark: Frostbite/AMD SSSR candidate + RT miss fill + probe fallback, with no temporal filter.
5. Add a GI benchmark based on idTech 8-style world radiance cache and irradiance volume storage, with visible temporal components disabled.
6. Add optional DLSS/FSR/XeSS integration requirements to the render graph and profiler once native rendering quality gates pass.

## Sources

- D3D12 Work Graphs: https://devblogs.microsoft.com/directx/d3d12-work-graphs/
- DirectX Work Graphs spec: https://microsoft.github.io/DirectX-Specs/d3d/WorkGraphs.html
- AMD Work Graphs mesh nodes: https://gpuopen.com/learn/work_graphs_mesh_nodes/work_graphs_mesh_nodes-intro/
- Vulkan device-generated commands: https://vulkan.lunarg.com/doc/view/1.4.304.1/mac/antora/features/latest/features/proposals/VK_EXT_device_generated_commands.html
- AMD DGF SDK: https://gpuopen.com/dgf/
- AMD DGF SuperCompression: https://gpuopen.com/learn/introducing-amd-dgf-supercompression/
- NVIDIA RTX Mega Geometry: https://github.com/NVIDIA-RTX/RTXMG
- RTX Mega Geometry Vulkan samples: https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples/
- OpenPBR Surface: https://academysoftwarefoundation.github.io/OpenPBR/
- MaterialX: https://materialx.org/
- OpenUSD asset resolver: https://openusd.org/dev/api/ar_page_front.html
- OpenAssetIO: https://docs.openassetio.org/
- glTF: https://www.khronos.org/gltf/
- KTX2/Basis Universal: https://www.khronos.org/ktx/
- idTech 8 Global Illumination slides: https://advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf
- Activision Neural Light Grid: https://research.activision.com/publications/2024/08/Neural_Light_Grid
- Frostbite stochastic SSR: https://www.ea.com/news/stochastic-screen-space-reflections
- AMD Hybrid Reflections: https://gpuopen.com/fidelityfx-hybrid-reflections/
- NVIDIA HBAO+: https://developer.nvidia.com/rendering-technologies/horizon-based-ambient-occlusion-plus
- XeGTAO: https://github.com/GameTechDev/XeGTAO
- RTX Neural Texture Compression: https://github.com/NVIDIA-RTX/RTXNTC
- AMD FSR Redstone: https://gpuopen.com/learn/amd-fsr-redstone-developers-neural-rendering/
- AMD FSR Radiance Caching: https://gpuopen.com/amd-fsr-radiancecaching/
- AMD FSR Ray Regeneration: https://gpuopen.com/amd-fsr-rayregeneration/
- D3D12 Shader Execution Reordering: https://devblogs.microsoft.com/directx/ser/
- D3D12 Cooperative Vector: https://devblogs.microsoft.com/directx/cooperative-vector/
