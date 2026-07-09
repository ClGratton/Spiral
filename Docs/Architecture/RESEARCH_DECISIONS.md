# Research Discoveries And Decisions

Date: 2026-07-06
Scope: rendering, materials, geometry, texture representation, antialiasing, ray interpolation, and physics direction for a new non-temporal engine.

## Executive Decisions

| Area | Decision |
| --- | --- |
| Core renderer | Use raster-stabilized ray residual correction. Keep the ray interpolation idea, but reconstruct residuals, not full lighting. |
| Temporal accumulation | Reject visible multi-frame accumulation as a baseline. Allow offline data and scheduling feedback, not color-history blending. |
| BRDF | Use a Callisto-inspired material model globally: Proxima-like diffuse, diffuse Fresnel, retroreflection, smooth terminator, improved Fresnel/specular controls. Lambert is debug/fallback only. |
| Shadows | Use optimized shadow maps as stable base, then sparse RT visibility residuals for important/uncertain regions. Do not use full-screen RT shadows blindly. |
| Reflections | Use classification. Rough reflections can be sparse and reconstructed. Mirrors and eyes need dedicated rays or reliable replacement data. |
| GI | Use raster/probe/static base plus current-frame sparse ray residuals. Avoid full-scene low-sample path tracing unless paired with a non-temporal reconstruction breakthrough. |
| Geometry | Build a Nanite-like virtualized geometry system with GPU culling, clusters, streaming, and projected-error LOD, but clamp runtime density before it becomes subpixel/quad-inefficient. Use ML for offline generation, not as the runtime LOD system. |
| Textures | Use virtual texturing and hardware compression first. Add neural texture compression and Fourier/basis encodings as optional high-end or asset-specific paths. |
| Anti-aliasing | Prefer MSAA/coverage, alpha-to-coverage, SMAA/CMAA, specular AA, normal filtering, mip correctness, and native resolution. |
| Physics | Spend the extra millisecond on ABD/IPC-style contact where it is visible. Use RT hardware for queries where it fits, not as a solver substitute. |
| Optional accelerators | Support DLSS, XeSS, FSR, frame generation, neural denoisers, SER, Cooperative Vector, and vendor SDKs only as optional scalability paths after the native baseline is stable. |
| Asset standards | Use OpenPBR/MaterialX/USD/OpenAssetIO/glTF/KTX2 as import, authoring, and pipeline bridges. Runtime shipping data remains engine-cooked. |
| Geometry compression | Evaluate AMD DGF/DGFS and RTX Mega Geometry/CLAS as optional backend/cook targets for cluster geometry. |

## Threat Interactive And Forum Takeaways

Threat Interactive videos are useful as a critique of blurry temporal pipelines, over-reliance on upscaling, and weak material/shadow defaults. Relevant starting points:

- [Fake Optimization in Modern Graphics](https://www.youtube.com/watch?v=lJu_DgCHfx4)
- [Near Photorealism Driven By MSAA](https://www.youtube.com/watch?v=jNR5EiqA05c)
- [How Incompetent Graphics Create Fake Realism](https://www.youtube.com/watch?v=qZtNU-4yqtI)
- [When Butchered Rasterization Leads To Raytracing](https://www.youtube.com/watch?v=PhEIa5RtKVA)
- [Devs Can Optimize Graphics By Never Letting THIS Happen](https://www.youtube.com/watch?v=T1MKlxM04L4)
- [Took For Granted: Why Fox Engine Is So Crazy Optimized](https://www.youtube.com/watch?v=aB5qxp6SPPQ)

Use these videos as prompts for tests, not as final authority. The [Level1Techs discussion](https://forum.level1techs.com/t/is-threat-interactives-yt-game-optimisation-channel-a-long-con/223785) shows that the channel is debated, so the engine rule is simple: if a claim matters, convert it into a benchmark or image-quality test.

The forum/user-sentiment side still matters. Communities such as [r/FuckTAA](https://www.reddit.com/r/FuckTAA/) and long-running Unreal forum threads about TAA show that many players notice blur, ghosting, and loss of texture detail. That supports making motion clarity a first-class quality target.

## Callisto Protocol Lessons

Primary sources:

- [The Character Rendering Art of The Callisto Protocol, SIGGRAPH 2023 PDF](https://advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf)
- [SIGGRAPH 2023 Advances course page](https://advances.realtimerendering.com/s2023/index.html)
- [Callisto BRDF UE4 port notes](https://unreal-engine-ace7bf.gitlab.io/docs/callisto-brdf/)
- [Wccftech summary of Mark James comments on hybrid RT shadows and eye reflections](https://wccftech.com/the-callisto-protocol-supports-ray-tracing-includes-some-ue5-elements/)

Discoveries:

- Callisto's material work is more important than simply saying "ray tracing." It used a custom BRDF direction with diffuse Fresnel, retroreflection, smooth terminator, specular Fresnel control, and later Proxima diffuse replacing Lambert in the combined form.
- The engine should adopt that philosophy globally, not only on faces: measured material behavior, orthogonal controls, and artist-fittable parameters.
- Faces still deserve special treatment: eyes, skin, transmission, and offline scan residuals are where players inspect realism most harshly.
- Callisto's hybrid RT philosophy also supports selective ray budgets. Their public comments describe ray-traced shadow detail applied where it matters, not uniform brute force.

Decision:

Use a Callisto/Proxima-like BRDF family as the default material foundation. Add skin/eye/hair specializations rather than hoping a generic Lambert+GGX stack will stop looking plastic.

## Ray Interpolation Decision

Decision: **yes, use it**, with a stricter definition.

The good version is:

```text
raster base = stable approximate lighting
ray samples = sparse ground-truth correction
reconstruction = same-frame spatial propagation of the residual
```

This works best when the raster result already contains the high-frequency stable signal and rays only correct the missing truth. Examples:

- Shadow-map visibility corrected by sparse RT visibility.
- Probe/SSGI diffuse GI corrected by sparse RT bounce data.
- SSR/probe rough reflections corrected by sparse RT where screen data fails.
- Skin/SSS thickness or transmission hints in flagged material classes.

This fails when the residual is the whole signal:

- Mirror reflections.
- Eye reflections.
- Thin highlight detail.
- Caustics.
- Hair strand coverage.
- Alpha-tested microdetail.

Best implementation path:

1. Implement a classifier pass before tracing.
2. Trace quarter/half/full rate only where useful.
3. Store residuals, not final color, where possible.
4. Reconstruct with joint bilateral or a-trous filtering guided by depth, normals, albedo, roughness, material ID, and instance ID.
5. Densify rays near discontinuities and important material classes.
6. Route mirror/eye pixels around interpolation.

Relevant research:

- [Sparse Sampling for Real-time Ray Tracing](https://www.scitepress.org/papers/2018/66558/66558.pdf)
- [Spatial Adaptive Sampling in Real Time Ray Tracing](https://lup.lub.lu.se/luur/download?fileOId=9067198&func=downloadFile&recordOId=9067197)
- [Ray Traced Shadows: Maintaining Real-Time Frame Rates](https://boksajak.github.io/files/RTG1_RayTracedShadows.pdf)
- [Gradient-Domain Path Tracing](https://www.cs.umd.edu/~zwicker/projectpages/GradientPathTracing-TOG15.html)
- [Gradient-Domain ReSTIR Path Tracing, 2026](https://research.nvidia.com/labs/rtr/publication/wang2026gradient/)

Gradient-domain reconstruction remains interesting, especially because it reconstructs from color differences, but current real-time gradient-domain work still leans on spatiotemporal reuse. Treat it as an experimental reconstruction path, not the first production path.

## Anti-Aliasing Decision

Primary sources:

- [A Survey of Temporal Antialiasing Techniques](https://research.nvidia.com/labs/rtr/publication/yang2020survey/)
- [SMAA: Enhanced Subpixel Morphological Antialiasing](https://www.iryoku.com/smaa/)
- [Alex Tardif, A Failed Adventure in Avoiding Temporal Anti-Aliasing](https://alextardif.com/Antialiasing.html)
- [Intel CMAA 2.0](https://www.intel.com/content/www/us/en/developer/articles/technical/conservative-morphological-anti-aliasing-20.html)
- [Ben Golus, Alpha To Coverage](https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f)
- [Threat Interactive, Why MSAA Should Be In EVERY Deferred Renderer](https://www.youtube.com/watch?v=SxCMaTEoBoI)

Decision:

The engine should not use TAA as a crutch. It should attack aliasing at the source:

- MSAA or coverage where geometry/alpha edges need it.
- Alpha-to-coverage for masked materials.
- Analytic line/strand coverage for hair and wires.
- SMAA/CMAA as final edge cleanup.
- Specular AA and roughness correction for normal maps.
- Correct mip generation and anisotropic filtering.
- Stable LOD and virtual geometry transitions.

Trade-off:

Without TAA, shader/specular/subpixel aliasing becomes a real problem. The engine must spend design effort on source-level filtering, not just final image AA.

Deferred MSAA note:

The useful lesson from the Threat Interactive Crysis 3 analysis is not "MSAA alone fixes everything." It is that MSAA in a deferred renderer has to be designed through the whole pass chain. Edge samples are easy to ruin with bad G-buffer packing, missing sample-frequency stencil coverage, pixel-frequency AO/SSDO resolves, or post-lighting resolves that collapse subsample data too early. CMAA2/SMAA remains valuable as final edge cleanup, especially after 2x/4x MSAA reaches diminishing returns, but it should complement real coverage instead of replacing it.

## Shadows Decision

Sources:

- [The Callisto Protocol SIGGRAPH 2023 PDF](https://advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf)
- [Ray Traced Shadows: Maintaining Real-Time Frame Rates](https://boksajak.github.io/files/RTG1_RayTracedShadows.pdf)
- [AMD FidelityFX Denoiser](https://gpuopen.com/fidelityfx-denoiser/)

Decision:

Use optimized shadow maps for the base and sparse RT for correction. This is faster and more stable than trying to ray trace all shadows everywhere, and more accurate than pure shadow maps where contact, penumbra, or many-light overlap matters.

The classifier should consider:

- Shadow-map texel density.
- Contact distance.
- Light size and penumbra risk.
- Face/eye/hero masks.
- Luminance/perceptual importance.
- Depth/normal discontinuities.
- Static shadow-cache validity.

Pros:

- Stable without temporal history.
- Ray budget goes where it is visible.
- Keeps many cheap lights viable.

Cons:

- Requires excellent debug tooling.
- Can produce inconsistent quality if the classifier is wrong.
- Hard shadow edges need densification, not filtering.

## Reflections Decision

Sources:

- [AMD FidelityFX SSSR documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/)
- [Frostbite Stochastic Screen-Space Reflections](https://www.ea.com/frostbite/news/stochastic-screen-space-reflections)
- [Lumen Technical Details](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine?lang=en-US)
- [Lumen GI and Reflections settings](https://dev.epicgames.com/documentation/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine?lang=en-US)
- [GI-1.1 glossy reflections with two-level radiance caching](https://gpuopen.com/learn/gi-1-1-glossy-reflection-rendering/)

Decision:

Do not use one reflection algorithm for all materials.

- Rough reflections: SSR/probes/radiance cache plus sparse rays and spatial reconstruction.
- Glossy reflections: half/quarter-rate RT is plausible if roughness is high enough.
- Mirror reflections: full-rate rays or a high-confidence replacement. No interpolation.
- Eyes: dedicated cornea/iris reflection path.
- Off-screen data: world-space cache/probes or real rays. The depth buffer cannot invent data that is not on screen.

Pros:

- Avoids wasting rays on rough surfaces.
- Keeps important reflections crisp.
- Avoids ghosting from temporal reflection denoisers.

Cons:

- Requires material-aware routing.
- World-space caches can become temporally stale if not controlled.
- Mirror-heavy scenes remain expensive.

## Global Illumination Decision

Sources:

- [GI-1.0 two-level radiance caching paper](https://arxiv.org/abs/2310.19855)
- [GI-1.0 project page](https://takahiroharada.github.io/gi-1.0/)
- [Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields](https://www.jcgt.org/published/0008/02/01/paper-lowres.pdf)
- [RTXGI DDGI algorithms](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Algorithms.md)
- [ReSTIR paper page](https://benedikt-bitterli.me/restir/)
- [ReGIR, Rendering Many Lights with Grid-Based Reservoirs](https://research.nvidia.com/labs/rtr/publication/boksansky2021rendering/)

Decision:

Use a layered GI strategy:

1. Static/offline light where it is valid.
2. Screen-space diffuse/contact estimates as a stable base only if they pass motion tests.
3. World-space probes/cache for low-frequency indirect light.
4. Sparse current-frame rays for residual correction and hero changes.
5. ReGIR-style world-space light sampling for many lights, because it avoids relying purely on screen-space temporal history.

Important caveat:

DDGI, GI-1.0, ReSTIR GI, and many neural/path-tracing approaches normally reuse samples over space and time. They are valuable references, but the baseline engine must not require multi-frame convergence for visible correctness. If a cache updates across frames, it must be treated as a bounded-latency world data structure with debug validity, not a hidden pixel-history denoiser.

## Geometry And LOD Decision

Sources:

- [Epic Nanite documentation](https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine?lang=en-US)
- [Nanite SIGGRAPH 2021 PDF](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)
- [Neural Mesh Simplification, CVPR 2022](https://openaccess.thecvf.com/content/CVPR2022/html/Potamias_Neural_Mesh_Simplification_CVPR_2022_paper.html)
- [Neural Geometric Level of Detail](https://research.nvidia.com/labs/toronto-ai/nglod/)
- [3D Gaussian Splatting](https://arxiv.org/abs/2308.04079)

Decision:

Build a Nanite-like virtualized geometry system, but do not inherit Unreal's whole renderer. Nanite's core idea is still right: cluster hierarchy, fine-grained streaming, GPU-driven culling, and projected-error LOD. Software/compute raster is only a measured fallback for clusters that are still cheaper that way after the importer has already avoided pathological subpixel density.

Threat-style criticism of Nanite should become benchmarks:

- Does virtual geometry beat classic LOD for this asset category?
- Are masked foliage, WPO, skinning, translucency, and tiny props slower?
- Does software raster actually win for the triangle-size distribution?
- What is the per-object overhead?

ML simplification is useful offline. It can help build better cluster hierarchies or error metrics. It should not replace the runtime system.

Gaussian splats and neural implicit LOD are valuable for captured static radiance fields and scenery experiments. They are not the default gameplay geometry representation because collision, editing, materials, animation, and deterministic interaction are still easier with meshes.

## Textures, Complex Bases, And Neural Compression

Sources:

- [NVIDIA Random-Access Neural Compression of Material Textures](https://research.nvidia.com/labs/rtr/neural_texture_compression/)
- [RTX Neural Texture Compression SDK](https://github.com/NVIDIA-RTX/RTXNTC)
- [Neural Geometric LOD](https://research.nvidia.com/labs/toronto-ai/nglod/)
- [Fourier PlenOctrees](https://openaccess.thecvf.com/content/CVPR2022/papers/Wang_Fourier_PlenOctrees_for_Dynamic_Radiance_Field_Rendering_in_Real-Time_CVPR_2022_paper.pdf)

Decision:

Do not replace all textures or polygons with complex numbers. Use basis representations where they are actually better:

- Fourier/complex coefficients for periodic signals, procedural textures, compression research, and radiance-field style representations.
- Spherical harmonics or spherical gaussians for lighting, diffuse irradiance, and fitted scan residuals.
- Neural texture compression for correlated PBR texture sets where decode cost and vendor support are acceptable.
- Traditional virtual textures and hardware compression for the baseline.

Pros of neural/basis texture data:

- Potentially large memory savings.
- Can preserve high-frequency material detail.
- Good fit for correlated PBR channels and scan data.

Cons:

- Runtime decode cost.
- Hardware/vendor fragmentation.
- Tooling complexity.
- Harder debugging and authoring.
- Some methods assume temporal upscalers or denoisers to hide decode noise.

## Physics Decision

Sources:

- [Incremental Potential Contact](https://ipc-sim.github.io/)
- [IPC GitHub](https://github.com/ipc-sim/IPC)
- [Affine Body Dynamics](https://arxiv.org/abs/2201.10022)
- [Autodesk ABD implementation](https://github.com/Autodesk/affine-body-dynamics)
- [Penetration-free Projective Dynamics on the GPU](https://www.math.ucla.edu/multiples/publication/lan2022pdipc.pdf)
- [Subspace-Preconditioned GPU Projective Dynamics with Contact](https://dl.acm.org/doi/fullHtml/10.1145/3610548.3618157)
- [StiffGIPC, 2024](https://arxiv.org/abs/2411.06224)
- [Hardware-Accelerated Ray Tracing for Collision Detection](https://arxiv.org/html/2409.09918v1)
- [Rethinking Collision Detection on GPU Ray Tracing Architecture, 2026](https://arxiv.org/html/2604.23520v1)

Decision:

Physics should be a visible quality feature, not an afterthought. Spend the extra millisecond on contact quality where the player can see it.

Recommended stack:

- ABD for stiff/rigid-like bodies and contact-heavy hero objects.
- GPU Projective Dynamics plus IPC-style barriers for cloth, shells, and selected soft bodies.
- Full IPC only for constrained hero cases, offline tools, or lower-frequency simulation islands until performance is proven.
- SDF/shallow neural SDF bodies for character-cloth-hair collisions.
- RT hardware for ray-style collision queries and high-count geometry queries when it benchmarks well.
- CPU gameplay physics remains authoritative where determinism and latency matter.

RT cores:

They can accelerate BVH traversal and ray-style tests, and newer papers show real collision-detection use cases. They do not solve constraint systems or contact response by themselves. The solver still needs compute/CPU work.

## Hair Decision

Sources:

- [Real-Time Hair Rendering with Hair Meshes](https://www.cemyuksel.com/research/hairmesh_rendering/)
- [VKHR real-time hybrid hair renderer](https://github.com/CaffeineViking/vkhr)
- [AMD TressFX](https://gpuopen.com/tressfx/)
- [Real-Time Neural Hair Denoising, 2026](https://arxiv.org/abs/2605.17557)

Decision:

Use strand or hair-mesh rendering with spatial antialiasing, not temporal smoothing as the baseline.

Best path:

- Hair meshes or guide-based generated strands for memory efficiency.
- Mesh shaders where available.
- MSAA or analytic strand coverage.
- Deep/approximate deep shadow maps for hair self-shadowing.
- Kajiya-Kay/Marschner-inspired shading depending on quality tier.
- PBD/XPBD guide simulation for ordinary cases, with stronger collision bodies and selective IPC/ABD contact for hero cases.

Neural hair denoising is interesting, but the 2026 paper still uses temporal accumulation for coverage recovery, so it is not baseline-compatible with the non-temporal rule.

## Open Questions And Prototype Order

1. **Ray residual shadows**
   - Build this first. It is the cleanest proof of the ray interpolation idea.
   - Compare pure shadow maps, pure RT shadows, and sparse RT residual correction.

2. **Callisto-inspired BRDF**
   - Implement Proxima diffuse, diffuse Fresnel, retroreflection, smooth terminator, and parameterized Fresnel.
   - Compare against Lambert+GGX under the same lighting and materials.

3. **Non-temporal AA stack**
   - Combine MSAA/coverage, alpha-to-coverage, SMAA/CMAA, specular AA, and mip correctness.
   - Test foliage, hair, normal maps, and shiny surfaces in motion.

4. **Rough reflection classifier**
   - Separate mirror, eye, glossy, rough, and off-screen cases.
   - Measure how much can be reconstructed spatially before artifacts appear.

5. **Virtual geometry prototype**
   - Implement meshlet/cluster visibility, projected-error LOD, and measured software/compute raster fallback only where it wins after anti-subpixel density rules.
   - Benchmark against classic LOD on dense meshes, foliage, skinned characters, and simple props.

6. **Physics hero-contact prototype**
   - Compare ordinary rigid/PBD behavior against ABD/PD+IPC on visible clipping cases.
   - Budget the extra millisecond deliberately.

## Pros And Cons Where The Decision Is Not Final

### Dynamic GI Cache

Pros:

- Best practical route to low-frequency diffuse GI.
- Good fit for off-screen indirect lighting.
- Can amortize expensive ray work.

Cons:

- Most known versions reuse data over time.
- Can lag or ghost if treated like hidden history.
- Needs strict validity debug views.

Current stance: allowed only as a world-space cache with bounded latency, not as pixel-history accumulation.

### Gradient-Domain Reconstruction

Pros:

- Theoretically strong fit for reconstructing sparse ray differences.
- Can preserve high-frequency changes better than blunt blur.

Cons:

- Poisson/solver cost.
- Risk of ringing and global error spread.
- Recent real-time work still leans on spatiotemporal reuse.

Current stance: research path after bilateral/a-trous residual reconstruction works.

### Neural Texture Compression

Pros:

- Strong memory savings on correlated material sets.
- Good fit for high-resolution scan assets.

Cons:

- Runtime decode cost.
- Hardware support differences.
- Potential dependence on neural hardware paths.

Current stance: optional high-end path, not baseline.

### Optional Upscalers And Neural Accelerators

Pros:

- Let users on mid-range hardware choose higher settings or frame rate.
- Can extend ray-traced and path-traced modes after the native path is working.
- DLSS/XeSS/FSR-style integrations are expected by many PC players.
- SER and Cooperative Vector may improve future ray/neural shader performance without changing content.

Cons:

- Temporal upscalers and denoisers can hide broken LOD, bad alpha coverage, unstable roughness, and noisy GI.
- Vendor support differs.
- Debugging becomes harder if the accelerator is silently part of the image.
- Frame generation improves perceived frame rate but not simulation latency.

Current stance: support them, but never depend on them. The renderer must pass quality gates native/no-accelerator first.

### OpenPBR, MaterialX, USD, glTF, KTX2

Pros:

- Better DCC and asset marketplace interoperability.
- Easier small-studio pipeline adoption.
- MaterialX/OpenPBR reduce custom material translation pain.
- USD/OpenAssetIO help bigger pipelines manage variants and asset resolution.
- glTF/KTX2 give lightweight, portable interchange and texture delivery.

Cons:

- Authoring formats are not optimized runtime formats.
- OpenPBR is not the same as the chosen runtime BRDF.
- USD can bring heavy pipeline complexity if treated as the engine's live runtime scene graph.

Current stance: use as import/authoring/pipeline standards, then cook to engine-native runtime data.

### Work Graphs, DGF, CLAS

Pros:

- Work Graphs and device-generated commands fit GPU-driven cluster/material/ray work.
- DGF/DGFS fits cluster-granular geometry compression and can target both future hardware and ordinary meshlets.
- RTX Mega Geometry/CLAS fits high-density ray-traced cluster geometry.

Cons:

- API and driver maturity vary.
- CLAS is vendor-specific.
- DGF hardware support is future-facing.
- Work Graphs cannot be the only execution path if portability matters.

Current stance: design extension points now; implement ordinary indirect/meshlet path first.

### Gaussian Splats / Neural Implicits

Pros:

- Excellent for captured static scenes and radiance assets.
- Can produce high realism from scans.

Cons:

- Harder gameplay collision, editing, destruction, material changes, and animation.
- Tooling still immature for general engine use.

Current stance: asset-specific renderer path, not core geometry.

### RT Hardware For Physics

Pros:

- Great for ray queries, visibility, scene probes, and some collision detection workloads.
- Can reuse acceleration structures conceptually with rendering.

Cons:

- Not a constraint solver.
- Data readback and synchronization can destroy gains.
- Vendor behavior and API access need profiling.

Current stance: use for query-heavy collision and scene tests where async GPU simulation can keep data on GPU.
