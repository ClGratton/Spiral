# Low-Level Rendering Architecture

Status: Draft v0.1
Date: 2026-07-06

This document turns the high-level engine direction into concrete rendering, buffer, material, texture, vertex, and geometry decisions.

Implementation-level contracts for visibility IDs, HZB culling, BLAS/TLAS policy, masked coverage, material-bin sizing, streaming fallbacks, world precision, and spatial antialiasing live in [RENDERER_IMPLEMENTATION_CONTRACTS.md](RENDERER_IMPLEMENTATION_CONTRACTS.md).
RHI backend choice and daylight/baked-lighting architecture live in [RHI_AND_LIGHTING_DECISIONS.md](RHI_AND_LIGHTING_DECISIONS.md).
Probe lighting, Detroit: Become Human lessons, screen-space GI/AO, and static/dynamic GI consistency live in [PROBE_LIGHTING_AND_GI_DECISIONS.md](PROBE_LIGHTING_AND_GI_DECISIONS.md).
SSR, AO, reflection, shadow exclusion, profiling, instancing, mesh merging, and quad-utilization decisions live in [RENDERING_FEATURE_AND_PERFORMANCE_DECISIONS.md](RENDERING_FEATURE_AND_PERFORMANCE_DECISIONS.md).
The 2026 missing-research audit and optional accelerator policy live in [MISSING_RESEARCH_AUDIT_2026.md](MISSING_RESEARCH_AUDIT_2026.md).
LOD transition and native multithreading decisions live in [LOD_TRANSITIONS_AND_MULTITHREADING_DECISIONS.md](LOD_TRANSITIONS_AND_MULTITHREADING_DECISIONS.md).
Language, scripting, visual graph, shader, and DOTS-like concurrency decisions live in [LANGUAGE_AND_CONCURRENCY_DECISIONS.md](LANGUAGE_AND_CONCURRENCY_DECISIONS.md).

## Core Decision

The renderer should not be pure Forward+, classic deferred, or a straight clone of Nanite. Use a hybrid:

```text
Opaque geometry:
  GPU culling -> visibility buffer -> material resolve -> compact G-buffer -> clustered/deferred lighting

Special materials:
  clustered Forward+ / specialized passes for translucency, hair, particles, eyes, glass, and debug

Ray correction:
  classify -> sparse ray residuals -> current-frame spatial reconstruction -> composite
```

Why:

- Forward+ is excellent for MSAA, translucency, and simpler shading, but expensive opaque material+lighting work can repeat under overdraw and dense triangle workloads.
- Classic deferred gives useful screen-space data, but a fat G-buffer burns bandwidth and still runs material shaders during rasterization.
- A visibility buffer stores only "what primitive won" first. It can then resolve material data once per visible pixel, avoiding bad quad utilization on dense geometry.
- The engine still keeps a compact G-buffer because shadows, reflections, GI residuals, AO, filters, and debug views need stable screen-space guide data.

## Callisto BRDF Implementation Target

The Callisto paper confirms this structure:

```text
Lo = (c1(theta_d, theta_h) * f_diffuse(wi, wo) + f_ggx(wi, wo)) * c2(theta_i) * Li * cos(theta_i)
```

In Callisto's shipped evolution, `f_diffuse` can be `f_proxima` rather than Lambert:

```text
Lo = (c1(theta_d, theta_h) * f_proxima(wi, wo) + f_ggx(wi, wo)) * c2(theta_i) * Li * cos(theta_i)
```

The implementation target is **Callisto + Proxima + GGX**, not plain Lambert + GGX.

### Required BRDF Features

| Feature | Purpose | Storage |
| --- | --- | --- |
| Proxima diffuse | Replaces Lambert for rough/organic/material-rich diffuse response. | Material flag + roughness/alpha input. |
| Diffuse Fresnel | Brighten/darken diffuse at grazing/view-dependent angles. | Base parameter, optional texture. |
| Retroreflection | Control front-lit bounce-back response. | Base parameter, optional texture. |
| Smooth terminator | Soften harsh curved-surface light/shadow terminator. Applies to diffuse and specular via `c2`. | Base parameter + length/tint in advanced tier. |
| GGX specular | Default specular microfacet lobe. | Existing roughness/F0/metal. |
| Modified dual GGX | Second specular lobe for skin, eyes, wetness, cloth, layered materials. | Advanced tier. |
| Specular Fresnel falloff | Parameterized Schlick exponent/amplitude; default 0.5 matches normal Schlick. | Advanced tier. |

### Parameter Tiers

Base tier, always available:

- `diffuseFresnelIntensity`, range 0..256, default 1.
- `retroreflectionIntensity`, range 0..256, default 1.
- `diffuseFresnelFalloff`, range 0..1, default 0.75.
- `retroreflectionFalloff`, range 0..1, default 0.75.
- `smoothTerminator`, range -1..1, default 0.

Advanced tier:

- `diffuseFresnelTint`.
- `retroreflectionTint`.
- `smoothTerminatorTint`.
- `specularFresnelFalloff`, range 0..1, default 0.5.
- `dualSpecularRoughnessScale`.
- `dualSpecularOpacity`.

Full/ML fitting tier:

- `diffuseFresnelTangentFalloff`, range 0..1, default 0.75.
- `retroreflectionTangentFalloff`, range 0..1, default 0.75.
- `smoothTerminatorLength`, range 0..1, default 0.5.

### Material Parameter Packing

Do not store all Callisto parameters in the G-buffer. Store only a compact material ID or parameter-set ID, then fetch per-material constants from a structured buffer.

Recommended material constant layout:

```cpp
struct MaterialCore
{
    uint baseColorTex;
    uint normalTex;
    uint ormTex;
    uint callistoControlTex;

    uint flags;          // material class, alpha mode, shading model, two-sided, etc.
    uint brdfParamIndex; // index into CallistoBrdfParams
    uint vtPageTable;    // virtual texture set/page table
    uint userData;
};

struct CallistoBrdfParams
{
    // Use fp16/unorm16 in the packed GPU form.
    float diffuseFresnelIntensity;     // 0..256
    float retroreflectionIntensity;    // 0..256
    float diffuseFresnelFalloff;       // 0..1
    float retroreflectionFalloff;      // 0..1

    float smoothTerminator;            // -1..1
    float smoothTerminatorLength;      // 0..1
    float specularFresnelFalloff;      // 0..1
    float dualSpecularOpacity;         // 0..1

    float dualSpecularRoughnessScale;
    float diffuseFresnelTangentFalloff;
    float retroreflectionTangentFalloff;
    uint tintSetIndex;
};
```

For most materials, these are constants. Use a texture only when a control must vary across the surface.

Optional Callisto control texture:

| Channel | Meaning |
| --- | --- |
| R | Diffuse Fresnel intensity scale |
| G | Retroreflection intensity scale |
| B | Smooth terminator scale |
| A | Specular Fresnel falloff or dual-spec mask |

This texture is linear, never sRGB.

## Render Path Choice

### Opaque: Visibility Buffer + Compact G-Buffer

Opaque pass writes:

- Depth.
- Packed visibility ID: `drawClusterId:25 | localTriangleId:7`.
- Optional sample coverage if MSAA/coverage is active.

Material resolve pass:

- Reads visibility ID.
- Decodes local triangle ID and fetches a per-frame draw-cluster record for instance, meshlet, vertex page, and material data.
- Reconstructs barycentrics and derivatives.
- Samples textures with explicit gradients.
- Emits compact G-buffer data once per visible pixel.

The visibility buffer does not use a flat global primitive table. See [RENDERER_IMPLEMENTATION_CONTRACTS.md](RENDERER_IMPLEMENTATION_CONTRACTS.md) for the exact bit layout and overflow rules.

Selected occluder prepass:

- Large terrain, walls, buildings, HLOD tiles, and other proven occluders may be drawn before the main visibility pass.
- This prepass writes depth and the same compatible visibility ID layout, not shaded material outputs.
- The prepass exists to establish early depth/HZB and avoid wasted material resolve or pixel work behind large occluders.
- Do not copy Fox Engine's old shaded G-buffer terrain prepass. The modern version is ID/depth/coverage only.
- Prepass candidate selection is automatic and measured: projected area, occlusion history, material cost, alpha mode, and draw/cluster cost decide eligibility.

Lighting pass:

- Uses clustered/tiled light lists.
- Evaluates current-frame dynamic sun/moon direct lighting for daylight cycles.
- Samples a unified indirect-lighting API backed by time-keyed probes/lightmaps/light-field data.
- Uses the same indirect sample path for static and dynamic objects.
- Evaluates Callisto + Proxima + GGX.
- Writes stable raster base lighting.

This path is the default for dense opaque geometry.

### Forward+: Specialized And Transparent

Use clustered Forward+ for:

- Transparent glass.
- Eyes/cornea/tearline.
- Hair/strand shading.
- Particles.
- Alpha-heavy VFX.
- Very simple materials where visibility-buffer overhead is not worth it.
- VR/mobile/simple profiles where MSAA dominates the design.

Forward+ should use the same clustered light grid as the deferred lighting pass and the ray-classifier pass.

### Why Not Pure Forward+

Forward+ is not wrong. It is just not the best only path for this engine. The no-temporal rule means we need strong current-frame guide buffers for spatial reconstruction. We also expect dense scanned geometry and expensive material graphs. A visibility/deferred-material path gives us one material evaluation per visible pixel and the guide data needed for ray residuals.

## Backend Future-Proofing

The first renderer should work through ordinary compute, indirect draws, meshlet draws, and explicit ray tracing. However, the low-level architecture must leave room for newer GPU-driven execution paths:

- DirectX Work Graphs and mesh nodes.
- Vulkan device-generated commands.
- Vendor shader-enqueue / GPU work-expansion experiments.
- Shader Execution Reordering for ray-heavy paths.
- Cooperative Vector or equivalent neural-shader acceleration.

These features are not required for v0. They are capability-gated backend paths. The render graph should still model them early by supporting GPU-generated work queues, producer/consumer transient buffers, indirect dispatch/draw argument buffers, and debug counters for queue expansion.

Rule:

```text
Build a stable portable path first.
Add backend-specific fast paths only when profiling proves they improve the same workload.
```

## Frame Buffers And Render Targets

### Persistent GPU Buffers

| Buffer | Purpose |
| --- | --- |
| `InstanceBuffer` | Transform, previous/debug transform, bounds, mesh reference, material override, flags. |
| `MeshPageTable` | Maps virtual mesh pages to resident GPU memory. |
| `MeshletBuffer` | Meshlet metadata: vertex/index ranges, bounds, cone, LOD error, material range. |
| `ClusterTree` | Hierarchical LOD/culling nodes. |
| `DrawClusterBuffer` | Per-frame records addressed by the high 25 bits of the visibility ID. |
| `MaterialTable` | Texture indices, shader flags, BRDF parameter index. |
| `CallistoBrdfParams` | Per-material BRDF constants. |
| `TextureDescriptorHeap` | Bindless texture descriptors. |
| `LightBuffer` | Light parameters, shadow info, cache references. |
| `LightGrid` | Clustered/Forward+ light bins. |
| `RayWorkQueues` | Shadow/GI/reflection tile queues and indirect dispatch args. |
| `BLAS/TLAS` | Ray tracing acceleration structures. |

### Per-Frame Textures

Recommended starting formats:

| Target | Format | Notes |
| --- | --- | --- |
| Depth | `D32_FLOAT` | Use `D24S8` only if stencil pressure requires it. |
| Visibility | `R32_UINT` | `drawClusterId:25 | localTriangleId:7`, with `0xFFFFFFFF` invalid/background. |
| Coverage | `R8_UINT`, MSAA mask, or per-sample visibility path | Optional, for opaque edge coverage. Masked foliage/hair use a coverage-aware carve-out path. |
| GBuffer0 | `R8G8B8A8_UNORM` | BaseColor RGB, material class/flags A. |
| GBuffer1 | `R16G16B16A16_SNORM` | Shading normal oct RG, geometric normal oct BA. |
| GBuffer2 | `R8G8B8A8_UNORM` | Roughness, metallic, AO/cavity, reconstruction flags. |
| GBuffer3 | `R32_UINT` | Material ID / BRDF parameter-set ID. This is distinct from the visibility ID. |
| LightBase | `R11G11B10_FLOAT` or `R16G16B16A16_FLOAT` | Stable raster direct+indirect base. |
| ShadowResidual | `R8_UNORM` or `R16_FLOAT` | Visibility ratio/delta. |
| GIResidual | `R11G11B10_FLOAT` | Additive diffuse residual. |
| ReflectionRadiance | `R11G11B10_FLOAT` or `R16G16B16A16_FLOAT` | Rough/glossy reflection result. |
| RayMeta | `R16G16B16A16_FLOAT` | Hit distance, confidence, sample rate, filter radius. |
| FinalColor | `R16G16B16A16_FLOAT` | HDR before tone mapping. |

No required color history buffers in the baseline.

## Texture Storage

### Baseline Material Texture Set

| Texture | Format | Color Space | Notes |
| --- | --- | --- | --- |
| Base color | BC7 or BC1 | sRGB | BC7 for hero assets; BC1 for cheap opaque surfaces. |
| Normal | BC5 | Linear | Store XY, reconstruct Z. |
| ORM | BC7 or BC1 | Linear | R AO/cavity, G roughness, B metallic, A optional mask. |
| Height/displacement | BC4/BC5 | Linear | Separate if used for parallax, tessellation, or geometry generation. |
| Emissive | BC7 or BC6H | sRGB/linear per use | HDR emissive may need BC6H or scaled linear. |
| Callisto controls | BC7/BC4 | Linear | Optional; avoid if constants are enough. |

### Channel Packing Rules

Pack channels together only when they share:

- Color space.
- Mip behavior.
- Compression tolerance.
- Sample frequency.
- Streaming residency.

Good packs:

- `ORM`: AO/cavity, roughness, metallic.
- `CallistoControls`: diffuse Fresnel scale, retroreflection scale, smooth terminator scale, specular Fresnel/dual-spec mask.
- `Masks`: material blend masks, wetness, dirt, blood, wear, region IDs.
- Shadow/depth-only opacity: a compact alpha/cutout texture or mesh coverage representation used by shadow and depth passes without sampling base color.

Risky packs:

- Roughness in a low-quality BC1 channel if the material has smooth glossy gradients.
- Normal XY with unrelated scalar masks unless BC7 quality and shader cost are proven.
- Height in ORM alpha if height needs different resolution/mips.
- sRGB color with linear data.
- Opacity embedded only in base-color alpha when the same alpha is needed by shadow/depth passes. That forces RGB sampling in passes that need only coverage.

Rule: roughness quality matters more than people expect because it directly shapes specular lobe stability. Bad roughness compression becomes shimmer without TAA.

Fox Engine used compact material IDs to enrich deferred lighting without a huge G-buffer. Adopt that principle, but keep the implementation modern:

- `VisibilityID` identifies the winning draw cluster and triangle.
- `MaterialID` / `brdfParamIndex` identifies the material class, BRDF table row, and optional control textures.
- Lighting shaders fetch material constants from structured buffers, not from many extra G-buffer channels.
- Material IDs must be stable for debugging and authoring, but they are not the same thing as persistent asset GUIDs.

### Virtual Texturing

Use virtual texturing for large worlds and scan assets:

- Fixed tile size with borders for filtering.
- Per-material texture set pages, not individual random loose textures.
- Residency feedback from visibility/material resolve.
- Highest priority: base color, normal, roughness.
- Lower priority: decorative masks and non-visible material layers.
- Render thread never blocks on texture I/O. Missing pages fall back to resident mip tails or neutral default textures.

Neural texture compression remains optional. It can be a high-end path for correlated PBR texture sets, but the baseline must use ordinary hardware texture formats.

### Material And Asset Interchange

Authoring formats are not the same as runtime formats.

Use:

- OpenPBR + MaterialX for material interchange and DCC compatibility.
- USD/OpenAssetIO for editor/studio asset resolution, variants, and pipeline integration.
- glTF for lightweight import, marketplace assets, examples, and simple runtime interchange.
- KTX2/Basis for portable texture packages.

Cook to:

- engine-native material tables,
- engine-native virtual texture pages,
- engine-native meshlet/cluster pages,
- engine-native streaming metadata.

The runtime material shader remains Callisto/Proxima/GGX-based. OpenPBR/MaterialX are import and authoring bridges, not a reason to dilute the engine's chosen BRDF.

## Vertex And Mesh Data

### Asset Source Versus Runtime GPU Data

Keep source assets high precision. Build packed runtime data during import.

Recommended import processing:

1. Weld and validate topology.
2. Generate MikkTSpace tangents.
3. Split by material and hard normal/UV seams.
4. Build meshlets/clusters.
5. Optimize vertex cache.
6. Optimize overdraw where relevant.
7. Optimize projected triangle shape/area and quad utilization for generated triangulation and LODs.
8. Optimize vertex fetch.
9. Quantize.
10. Compress vertex and index data.
11. Build LOD/cluster hierarchy and streaming pages.

Use meshoptimizer-style processing for the baseline. It supports vertex/index optimization, quantization, simplification, and compression.

### Geometry Compression

Baseline geometry storage is engine-native cluster/meshlet pages with quantized vertices and compressed index data.

Add AMD DGF/DGFS as a serious cook-target candidate:

- DGF is block-based geometry compression for dense triangle data.
- DGFS is attractive because it can reconstruct DGF blocks or decode to conventional meshlets for non-DGF hardware.
- DGF/DGFS should be evaluated per asset class against meshoptimizer-style compression, vertex fetch cost, streaming behavior, BLAS/CLAS build cost, and visibility-buffer decode cost.

Add RTX Mega Geometry / CLAS as an optional RTX backend path:

- Useful for ray tracing dense cluster geometry and high-density animated/subdivision cases.
- Must sit behind `Engine::RHI` capability checks.
- Must not become a required content representation.

Rule:

```text
The content pipeline owns portable source data.
Backends may choose DGF, CLAS, ordinary meshlets, or decompressed vertex/index pages.
```

### GPU Vertex Streams

Use separate streams so visibility/culling reads only what it needs.

| Stream | Data | Recommended Encoding |
| --- | --- | --- |
| Position | Local position | Quantized 16-bit per component relative to meshlet/mesh bounds, or FP16 for deforming assets. |
| Normal/tangent | Tangent frame | Octahedral normal plus tangent angle/diamond encoding. Fallback `R10G10B10A2_SNORM`. |
| UV0/UV1 | Texture coordinates | 16-bit UNORM or FP16 depending on chart scale. |
| Color/masks | Vertex color, material masks | `R8G8B8A8_UNORM`. |
| Skinning | Joint indices/weights | 4x u8/u16 indices + normalized weights; 8 influences only for hero meshes. |
| Morphs | Deltas | Quantized per morph target/page; streamed only when active. |

Do not store bitangent. Reconstruct it from normal, tangent, and handedness.

### Meshlet/Cluster Shape

Starting target:

- 64 to 128 triangles per meshlet.
- 64 to 96 unique vertices per meshlet.
- Local 16-bit indices.
- Bounding sphere/AABB.
- Normal cone for culling.
- Material range.
- LOD error metric.
- Page residency info.

## Geometry And LOD

The engine should use precomputed LOD data, but not in the old "LOD0/LOD1/LOD2 pop" way.

Decision:

```text
Use offline-precomputed hierarchical cluster LOD + runtime GPU selection.
```

This answers both sides:

- Threat Interactive is right that stable, precomputed LODs can be better than spraying subpixel detail and hoping TAA hides it.
- Nanite is right that artists should not hand-author every LOD and draw calls should not dominate.

### Anti-Subpixel Policy

Because this engine rejects TAA as a baseline, geometry must be prefiltered.

Rules:

- Default LOD selection targets visible triangles in the 1 to 8 pixel range.
- Do not intentionally render huge fields of subpixel triangles.
- If a cluster would become subpixel, select a coarser cluster or representation.
- Preserve silhouettes more aggressively than interior surface detail using curvature-, boundary-, normal-, and attribute-aware simplification metrics rather than plain uniform QEM.
- Convert distant dense foliage/hair/detail into stable cards, volumes, impostors, aggregate voxels, analytic primitives, or filtered cluster LODs.
- Use software raster only for unavoidable near-pixel dense geometry, not as an excuse to render unfiltered noise.
- Every streamed mesh hierarchy must keep a coarse resident fallback. Missing fine pages select the nearest resident ancestor/proxy rather than stalling the frame.

This policy reduces shimmer from subpixel geometry and LOD swaps, but it does not by itself solve ordinary edge aliasing. A 4-pixel triangle can still crawl if its silhouette is sampled as binary in/out coverage. Cluster boundaries and silhouettes therefore need one same-frame spatial coverage solution:

- Prefer hardware MSAA where it fits the pass.
- For visibility-buffer/software-raster paths, support analytic or fractional coverage instead of a single point sample.
- Store enough coverage information for material resolve and post resolve to avoid hard binary crawling.
- Do not use frame-varying stochastic per-pixel dithering for LOD or impostor transitions. Use deterministic spatially stable masks, geometric morphs, or explicit crossfade representations.

### Asset-Class Geometry Policy

| Asset | Geometry Path |
| --- | --- |
| Static opaque scans | Virtual cluster hierarchy. |
| Modular hard-surface props | Meshlets + optional authored LOD anchors. |
| Tiny simple props | Classic instancing or meshlets; virtual geometry overhead may not be worth it. |
| Foliage | Coverage-aware Forward+/masked path with authored LOD/card/cluster/aggregate-voxel hybrid, alpha-to-coverage, and stable masks. |
| Skinned characters | Meshlets per LOD, GPU skinning, optional cluster hierarchy after skinning matures. |
| Hero faces | Traditional high-quality mesh LODs plus special material/SSS/eye passes. |
| Hair | Hair meshes/strands/cards depending on distance, with analytic coverage. |
| Gravel/debris fields | Filtered clusters, impostors, or material/height-field representation at distance. |
| Wires/cables/ropes | Analytic line/tube coverage where possible; avoid sliver-triangle point sampling. |
| Translucent/glass | Forward+ mesh path; no visibility-buffer-only assumption. |
| Captured background | Optional Gaussian splats/radiance fields if non-gameplay. |

### LOD Transition Policy

Use Decima-like ordered/complementary dithered LOD transitions when a visible pop would be worse than brief double drawing.

Rules:

- Stable ordered/complementary mask, not frame-varying TAA-dependent noise.
- Timed and triggerable per asset/cluster.
- Short default duration, tuned by asset class.
- Transition budget limits simultaneous double-draw cost.
- Instant transitions remain valid for tiny/background assets.
- Debug views show active transitions, cost, mask mode, and forced snaps.

## Ray Residual Passes

### Shadow Residuals

Baseline:

1. Raster shadow maps and static cache produce stable base visibility.
2. Classifier assigns none/quarter/half/full ray rate.
3. Trace RT visibility for important or uncertain tiles.
4. Reconstruct current-frame holes with bilateral rules.
5. Store `ShadowResidual`.
6. Composite into lighting.

Callisto-inspired low-level rules:

- Perceptual rate weighting should happen after lighting information is available.
- A one-frame-late rate decision is allowed only for scheduling, not visible color accumulation.
- Use depth bounds, static-cache rejection, and surface-to-light cone culling.
- Reuse the clustered light grid for shadow tile classification.
- Split queues by shading model: default, SSS, eye, branching.
- Denoise/reconstruct only tiles that were traced and have meaningful variance.

### Reflections

Reflection class rules:

| Class | Path |
| --- | --- |
| Mirror | Dedicated full-quality ray or high-confidence planar/probe solution. No interpolation. |
| Eye/cornea | Specialized reflection path, likely Forward+ or T-buffer style. |
| Car paint / wet roads | Stochastic SSR candidate + RT miss fill + local probe rough tail. |
| Glossy | SSR+RT hybrid or half/quarter-rate RT plus spatial reconstruction when roughness supports it. |
| Rough | Probe/radiance cache plus sparse correction and specular occlusion. |
| Translucent front layer | T-buffer-like front-layer geometry buffer; front layer traces, deeper layers use probes/SSR. |

Do not try to interpolate mirror/eye reflections from sparse neighbors.
Do not trust cubemaps for close glossy or mirror-like motion. Use them for rough IBL, low-frequency fallback, sky, and low-end modes.

### GI

Use:

- Static/baked data where valid.
- Probe/radiance cache for low-frequency bounce.
- Sparse current-frame ray residuals for visible changes.
- No hidden temporal color accumulation.

## Descriptor And Resource Model

Use bindless resources for read-only geometry and textures:

- One descriptor heap/table for textures.
- One descriptor heap/table for structured buffers.
- Error resources in unused slots.
- Frame-buffer descriptor updates for streaming.
- CPU and GPU validation of descriptor ranges.

Do not use bindless casually for read-write resources in the first renderer. Keep UAV writes explicit until the barrier/debug story is solid.

## Language And Runtime Model

Renderer, RHI, streaming, asset cooking, and job-system core are C++.

User gameplay scripts are C#/.NET hosted by the C++ engine. Visual scripting graphs compile to C#, native IR, Slang, or job graph nodes depending on domain. Engine shaders use Slang/HLSL-style authoring.

Runtime entity-scale systems use a DOTS-like archetype/chunk data model and native task graph. The editor can expose object-style scene authoring, but hot runtime systems must bake into data-oriented storage and explicit jobs.

## Low-Level Pass Graph

```text
Frame begin
  Build CPU frame task graph
  Run native worker jobs for gameplay/animation/physics/tools
  Upload/stream requests
  GPU scene update
  Skin/morph/cloth/hair update
  BLAS/TLAS updates by object-class policy

Visibility
  Multithreaded render packet and command-list preparation
  Select large occluder prepass candidates
  Depth + VisibilityBuffer occluder prepass
  Build early/current HZB from prepass where useful
  Main cull pass against previous HZB + current occluder HZB
  Emit main indirect mesh/meshlet draws
  Depth + VisibilityBuffer main pass
  Build current frame HZB
  Post cull previously-occluded candidates
  Depth + VisibilityBuffer post pass

Material resolve
  Count visible materials
  Prefix sum exact material bins
  Scatter full-screen/sample-count pixel worklist
  Resolve G-buffer by material shader
  Emit texture residency feedback

Lighting base
  Build clustered light grid
  Evaluate current sun/moon/sky state
  Sample unified probe/light-field/lightmap indirect lighting
  Compute spatial GTAO/XeGTAO-style AO where enabled
  Shadow map pass for stable base
  Static cache/probe sampling
  Callisto BRDF direct lighting

Ray correction
  Classify shadow/GI/reflection tiles
  Run screen-space GI/contact-bounce candidate pass where valid
  Run SSR candidate pass for valid on-screen reflection hits
  Trace sparse rays
  Spatial reconstruction
  Composite residuals

Special passes
  Forward+ translucency
  Hair
  Eyes
  Particles/VFX

Post
  MSAA/coverage resolve
  CMAA2/SMAA spatial cleanup optional
  Calibrated exposure and tone mapping
  Color grading/LUTs after tone mapping
  Debug overlays
```

## First Prototype Slice

Build this in order:

1. Bindless resource tables and GPU scene buffers.
2. Meshlet import path with quantized vertices and optimized indices.
3. Visibility buffer with `R32_UINT` packed draw-cluster IDs.
4. Material resolve into compact G-buffer.
5. Clustered light grid.
6. Callisto + Proxima + GGX BRDF shader.
7. Shadow-map base lighting.
8. Sparse RT shadow residual classifier and reconstruction.
9. MSAA/coverage + SMAA/CMAA image clarity test.
10. Geometry LOD stability test without TAA.

## Sources

- The Callisto Protocol SIGGRAPH 2023 PDF: https://advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf
- Threat Interactive, video 20: https://youtu.be/qZtNU-4yqtI
- Threat Interactive, video 21: https://youtu.be/PhEIa5RtKVA
- Threat Interactive, Took For Granted: Why Fox Engine Is So Crazy Optimized: https://www.youtube.com/watch?v=aB5qxp6SPPQ
- Metal Gear Solid V Graphics Study: https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/
- Photorealism Through the Eyes of a FOX, GDC Vault: https://www.gdcvault.com/play/1031807/Photorealism-Through-the-Eyes-of
- The Reyes Image Rendering Architecture: https://dl.acm.org/doi/pdf/10.1145/37402.37414
- Nanite SIGGRAPH 2021 course slides: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- Nanite Foliage documentation: https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage
- Visibility Buffer Rendering with Material Graphs: https://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/
- The Visibility Buffer, JCGT: https://jcgt.org/published/0002/02/04/
- Nanite Virtualized Geometry documentation: https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- meshoptimizer: https://meshoptimizer.org/
- Bindless Oriented Graphics Programming: https://alextardif.com/BindlessProgramming.html
- Octahedron Normal Vector Encoding: https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
- Quantizing Tangent Frames: https://zeux.io/2026/04/30/quantizing-tangent-frames/
