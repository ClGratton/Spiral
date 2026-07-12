# Engine Instructions

Status: Draft v0.1
Date: 2026-07-13

Low-level implementation details live in [LOW_LEVEL_ARCHITECTURE.md](LOW_LEVEL_ARCHITECTURE.md).
Renderer implementation contracts live in [RENDERER_IMPLEMENTATION_CONTRACTS.md](RENDERER_IMPLEMENTATION_CONTRACTS.md).
Frame/render graph construction, execution, synchronization, and transient-resource prerequisites live in [RENDER_GRAPH_ARCHITECTURE.md](RENDER_GRAPH_ARCHITECTURE.md).
Renderer adapter capability, feature enablement, fallback, shader portability, and qualification rules live in [RENDERER_CAPABILITY_CONTRACT.md](RENDERER_CAPABILITY_CONTRACT.md).
Technical feature-to-phase coverage and infrastructure ordering live in [TECHNICAL_ROADMAP_COVERAGE.md](TECHNICAL_ROADMAP_COVERAGE.md).
RHI and lighting decisions live in [RHI_AND_LIGHTING_DECISIONS.md](RHI_AND_LIGHTING_DECISIONS.md).
The first macOS renderer decision lives in [MACOS_RENDERER_BACKEND_DECISION.md](MACOS_RENDERER_BACKEND_DECISION.md).
Probe lighting, Detroit: Become Human lessons, screen-space GI/AO, and static/dynamic GI consistency live in [PROBE_LIGHTING_AND_GI_DECISIONS.md](PROBE_LIGHTING_AND_GI_DECISIONS.md).
Rendering feature and performance decisions live in [RENDERING_FEATURE_AND_PERFORMANCE_DECISIONS.md](RENDERING_FEATURE_AND_PERFORMANCE_DECISIONS.md).
The 2026 missing-research audit and optional accelerator policy live in [MISSING_RESEARCH_AUDIT_2026.md](MISSING_RESEARCH_AUDIT_2026.md).
Fox Engine lessons and architecture coherence audit live in [ARCHITECTURE_COHERENCE_AUDIT.md](ARCHITECTURE_COHERENCE_AUDIT.md).
LOD transition and native multithreading decisions live in [LOD_TRANSITIONS_AND_MULTITHREADING_DECISIONS.md](LOD_TRANSITIONS_AND_MULTITHREADING_DECISIONS.md).
Language, scripting, visual graph, shader, and DOTS-like concurrency decisions live in [LANGUAGE_AND_CONCURRENCY_DECISIONS.md](LANGUAGE_AND_CONCURRENCY_DECISIONS.md).
Physics authority, fixed stepping, backend evaluation, solver research, GPU synchronization, and fallback rules live in [PHYSICS_ARCHITECTURE_AND_RESEARCH.md](PHYSICS_ARCHITECTURE_AND_RESEARCH.md).
Engine-base evaluations live in [HAZEL_ENGINE_EVALUATION.md](HAZEL_ENGINE_EVALUATION.md).
Project skeleton/style guidance lives in [HAZEL_INSPIRED_SKELETON.md](HAZEL_INSPIRED_SKELETON.md).

## Research Tooling

When deeper research needs better extraction, install and use appropriate tools such as `yt-dlp`, PDF extractors, shader compilers, profilers, or format inspectors. The research process should favor primary sources, talks, papers, documentation, source code, and reproducible measurements over summaries.

## North Star

Build a modern real-time engine that is sharp in motion, physically credible, artist-controllable, and faster because it avoids inherited legacy assumptions. The engine must not depend on multi-frame temporal accumulation to make the image look finished. The target look is not "Unreal plastic PBR"; it is measured-material realism with stable raster lighting corrected by selectively traced rays.

The engine's core rendering idea is **ray-interpolated residual correction**:

```text
stable raster estimate + sparse ray-traced correction + same-frame spatial reconstruction
```

The ray pass corrects what raster approximations get wrong. It does not replace the whole renderer with noisy low-sample path tracing.

## Product Accessibility And Automation

The engine must win back small studios, solo developers, students, modders, and ambitious amateurs who currently default to Unreal or Unity because those tools feel like the only practical path. The technical core can be advanced, but the user experience must make that power easy, guided, and hard to misuse.

The default workflow should be automation-first:

1. Import an asset and the engine automatically generates mesh clusters, LODs, impostors/cards where useful, collision, material defaults, texture compression choices, streaming pages, and performance metadata.
2. Import or choose an animation pack and the engine prepares motion-matching data, retargeting, foot locking, trajectory features, inertialization settings, and character-controller defaults.
3. Pick a game type and the editor creates a guided project path: camera, controller, interaction model, save/load, input, performance targets, starter UI, and test map.
4. Pick a visual direction through understandable controls: realism/stylization, contrast, color response, material richness, shadow softness, texture sharpness, foliage density, ray-correction budget, and platform target.
5. Build and profile through guided workflows: first playable, visual pass, gameplay pass, optimization pass, packaging, storefront/demo build, and regression checks.

The editor should expose expert control without forcing every user to become a rendering engineer:

- No-code and low-code paths must exist for common game types.
- Sliders and presets should map to real engine settings, not vague post-process filters.
- Every automatic decision should be inspectable, overridable, and explainable.
- Generated LODs, packed textures, motion databases, collision, and lighting caches must show diagnostics when they are likely to fail.
- Defaults should be good enough that an amateur can get a clean first result, while advanced users can drill down to buffers, shaders, cluster trees, BRDF parameters, and profiler captures.

AI assistance is a first-class workflow layer, not a replacement for deterministic tools. The project should develop a custom game-making agent with engine-specific instructions, tools, validators, and workflows. It should help users plan games, set up scenes, wire gameplay, tune materials, build animation graphs, optimize performance, and fix errors. The same guided workflows must still work without AI, so the engine remains teachable, debuggable, and trustworthy.

The onboarding funnel should feel like:

```text
choose game type -> create playable base -> choose visual style -> import/author content -> tune feel -> profile -> package
```

The goal is not only to make a better renderer. The goal is to make a modern engine where excellent defaults, automation, guided creation, and AI assistance let small teams make games that look and feel expensive without inheriting the complexity cost of legacy engines.

## Hard Rules

1. No TAA, TSR, DLSS, FSR2/3/4-style temporal upscaling, temporal denoisers, or multi-frame color-history accumulation as required baseline image quality.
2. Native-resolution rendering is the quality target. Spatial upscalers such as CAS/FSR1-class sharpening can be optional performance modes, not the visual foundation.
3. Every effect must look acceptable on the current frame by itself. If it needs several frames to converge, it is not a baseline effect.
4. Lambert is not the default diffuse BRDF. It may exist only as a debug fallback or ultra-low-end fallback.
5. Ray tracing is budgeted by perceptual and material importance, not spread evenly across the screen.
6. Mirror reflections, eyes, thin highlights, hair coverage, alpha-tested foliage, and hard visibility edges are treated as special cases. They are not blindly interpolated from sparse rays.
7. Static and offline data are allowed. Baked lightmaps, measured BRDF fits, texture-space residuals, probe captures, and asset preprocessing are not the same as screen-space temporal accumulation.
8. One-frame or low-rate feedback is allowed only for scheduling decisions, such as "trace this tile more next frame." It must not be used to blend visible pixel color.
9. Any world-space lighting cache must expose validity, latency, and ghosting debug views. It can support diffuse/non-hero lighting, but hero objects and visible high-frequency changes must have current-frame correction.
10. Performance decisions must be measured. Treat video/forum claims as hypotheses until a profiler proves them in this engine.
11. DLSS, XeSS, FSR, frame generation, neural denoisers, ray regeneration, neural radiance caching, SER, Cooperative Vector, and similar accelerators are optional scalability features. They may improve performance or quality after the native baseline is already stable, but they must not be required to make the engine reach acceptable motion clarity, image stability, or frame time.

## Visual Identity

The image should preserve texture and geometry detail in motion. It should not smear, shimmer excessively, or become a soft reconstruction of the previous frame.

Material response should come from better BRDFs and better calibration, not from post-process gloss. The default opaque model should be Callisto-inspired:

- Proxima-like microfacet diffuse instead of Lambert.
- Diffuse Fresnel control.
- Retroreflection control.
- Smooth terminator control for curved forms.
- GGX or visible-normal GGX specular, with parameterized Fresnel falloff.
- Optional dual specular for skin, eyes, wet materials, layered paint, and certain fabrics.
- Material categories for skin, eye, hair, cloth, metal, dielectric, translucent, and emissive.

Skin and hero faces get a stronger path:

- Physically plausible subsurface/transmission.
- Eye-specific cornea, iris, tearline, and reflection treatment.
- Optional Realis-style offline residuals from scan/OLAT data, stored in texture space or compact spherical-gaussian bases.

## Per-Frame Pass Layout

This is the default frame. Specific projects can remove passes, but should not invert the design.

1. **Simulation and scene update**
   - Run gameplay simulation.
   - Run physics on a fixed or semi-fixed step.
   - Update animation, skinning, morphs, cloth, hair guides, and transforms.
   - Update changed BLAS/TLAS instances and virtual-geometry page residency.

2. **GPU visibility and virtual geometry**
   - GPU-driven frustum, occlusion, and cluster LOD selection.
   - Nanite-like microcluster hierarchy for dense static geometry.
   - Meshlet/cluster path for skinned and deforming meshes.
   - Compute/software raster path only as a measured fallback after the importer has already avoided pathological subpixel density.
   - Hardware raster path for larger triangles.

3. **Coverage and depth prepass**
   - Produce high-quality depth and coverage.
   - Use selected large occluder depth/visibility prepass for terrain, walls, buildings, HLODs, and expensive occluders.
   - The prepass writes compatible depth/coverage/visibility IDs, not shaded G-buffer material outputs.
   - Prefer MSAA or programmable coverage where practical.
   - Use alpha-to-coverage or analytic coverage for foliage, hair cards, wires, and thin geometry.

4. **G-buffer or visibility-buffer material resolve**
   - Store or reconstruct depth, geometric normal, shading normal, albedo, roughness, metallic, material ID, instance ID, coverage, and importance mask.
   - Include material class masks: default, skin, eye, hair, cloth, foliage, glass/translucent, emissive.
   - Store enough data for spatial reconstruction: depth, normal, albedo, roughness, motion/debug velocity if useful, instance/object ID.

5. **Stable raster base lighting**
   - Clustered/tiled direct lighting.
   - Optimized shadow maps for cheap, stable shadows.
   - Static lightmaps or cached static occlusion where available.
   - Screen-space AO/GTAO as a base only if it passes motion clarity tests.
   - SSR for visible on-screen reflection data.
   - Probe/cubemap fallback for rough or off-screen reflection data.

6. **Ray-budget classifier**
   - Assign per-tile and per-pixel budgets before tracing.
   - Inputs: material class, roughness, screen size, luminance, face/eye/hero mask, shadow-map confidence, raster/RT disagreement from recent scheduling feedback, depth/normal/albedo discontinuities, SSR miss flags, and camera focus.
   - Outputs: no trace, quarter rate, half rate, full rate, or specialized full-quality path.

7. **Sparse ray correction**
   - Trace shadow visibility only where shadow maps are uncertain or perceptually important.
   - Trace GI residual where raster/probe lighting is likely wrong.
   - Trace reflection rays only where SSR/probes cannot provide the signal.
   - Use full-quality rays for mirrors, eyes, wet highlights, important skin, and hero closeups.
   - Use roughness-driven sparse rays for glossy/rough reflections.

8. **Residual computation**
   - Shadows: store visibility ratio or visibility delta.
   - Diffuse GI: store additive indirect-light residual.
   - Ambient occlusion: store multiplicative/contact residual.
   - Rough reflections: store filtered radiance residual or replacement, depending on confidence.
   - Mirror/eye reflections: store replacement signal, not interpolated residual.

9. **Same-frame spatial reconstruction**
   - Reconstruct sparse residuals using edge-aware spatial filters guided by depth, normals, albedo, roughness, material ID, instance ID, and importance masks.
   - Start with joint bilateral or a-trous wavelet filters.
   - Use gradient-domain or Poisson reconstruction only as an experimental path if the solver cost and ringing are controlled.
   - Densify rays at discontinuities instead of blurring across them.

10. **Material shading and composition**
    - Composite raster base and ray residuals.
    - Evaluate the Callisto-inspired BRDF family.
    - Apply skin/eye/hair specialized shading.
    - Keep material debug modes for diffuse, specular, Fresnel, retroreflection, terminator, residual, and ray density.

11. **Spatial anti-aliasing and post**
    - Resolve MSAA/coverage.
    - Apply SMAA/CMAA-class edge cleanup if needed.
    - Apply specular antialiasing, normal-map filtering, roughness remapping, and texture mip correctness before post AA.
    - Use calibrated exposure and a documented tone mapper. Treat tone mapping as color science before artistic grading, not as arbitrary post gloss.
    - Evaluate Gran Turismo-style, AgX, ACES/filmic, and Khronos PBR Neutral profiles against the material calibration suite.
    - Do not add blur to hide unstable rendering.

## Ray Interpolation Contract

Ray interpolation is accepted as a core engine feature, but only under this contract:

- It reconstructs **residuals**, not full lighting.
- It is **spatial within the current frame**.
- It uses the raster result as a stabilizer.
- It has explicit failure classes.
- It can increase ray density when interpolation is unsafe.

Good targets:

- Soft shadow correction.
- Contact shadow correction.
- Diffuse GI residuals.
- Rough/glossy reflection correction.
- AO and indirect occlusion residuals.
- Low-frequency transmission and skin thickness cues.

Bad targets:

- Perfect mirrors.
- Eye cornea/iris reflections.
- Sharp caustics.
- Thin specular highlights.
- Single-pixel alpha detail.
- Hair strand coverage.
- Any signal where adjacent pixels can legally see unrelated radiance.

## Geometry Strategy

Build a virtualized geometry system rather than a classic hand-authored LOD stack.

- Use mesh clusters or meshlets as the fundamental unit.
- Stream visible cluster data at fine granularity.
- Cull on GPU.
- Select LOD by projected error, not by artist-authored distance bands.
- Use software/compute raster only as a measured fallback after LOD has already reduced subpixel/quad-inefficient workloads.
- Use hardware raster for ordinary triangles.
- Support skinned and deforming meshes with a separate path rather than forcing every asset into the same static system.

ML simplification is useful as an offline cluster-building assistant, not as a replacement for runtime virtualization.

Neural implicit geometry and 3D Gaussian splats are research/asset-type paths:

- Good for captured static scenes, distant scenery, matte-painting-like backgrounds, and non-gameplay radiance assets.
- Not the default for gameplay collision, skinned characters, editable worlds, or precise material interaction.

## Texture And Material Data

Default path:

- Virtual texturing.
- BC/ASTC/BC7-class hardware texture compression.
- Correct mips and anisotropic filtering.
- Material sets authored as correlated channels, not isolated random maps.

Advanced path:

- Neural texture compression for high-end GPUs where runtime decode cost is proven acceptable.
- Fourier, spherical harmonic, spherical gaussian, or wavelet bases for lighting, BRDF residuals, scan corrections, and procedural detail.
- Complex-valued/Fourier representations can be used internally when they compress or reconstruct a signal better, but the runtime shader interface should expose real-valued material parameters.

Interchange and authoring:

- Support OpenPBR and MaterialX as material interchange/authoring inputs, then compile into the engine's Callisto/Proxima runtime material model.
- Support USD/OpenAssetIO in the editor and studio pipeline where they help asset resolution, variants, and DCC workflows.
- Support glTF for lightweight import/runtime interchange, but cook shipping assets into engine-native streamable packages.
- Use KTX2/Basis where portable texture delivery matters, especially tools, marketplace assets, web/mobile paths, and cross-GPU starter projects.

Do not replace the entire texture and mesh system with a "complex number" representation unless a prototype beats the baseline in memory, quality, editability, streaming, and shader cost.

## Physics Strategy

The engine may spend a measured extra budget where better contact is visibly valuable, but no fixed “extra millisecond” or paper timing is assumed. Bad collision and clipping are visual bugs; gameplay authority, determinism, fallbacks, and frame pacing remain correctness requirements.

The accepted hierarchy is:

- a qualified CPU rigid-body backend owns fixed-step gameplay authority, characters, contacts/events, and queries;
- ordinary cloth, hair guides, and soft bodies use a portable bounded-cost solver or proxy fallback;
- GPU PD+barrier, IPC-family, ABD, and mixed-FEM paths are candidates for selected visual hero/offline islands only after engine-owned measurements;
- gameplay interaction with a GPU deformable uses an authoritative CPU proxy unless a later two-way authority contract is accepted;
- SDF/proxy collision is evaluated for character-cloth-hair interaction with a coarse portable fallback.

RT cores may help narrowly defined collision queries if measured. They do not solve constraint/contact response. Consumer graphics APIs expose queues and priority/synchronization controls, not fixed SM/RT-core partitions; async compute and copy/DMA are measured scheduling tools, not free compute. See [PHYSICS_ARCHITECTURE_AND_RESEARCH.md](PHYSICS_ARCHITECTURE_AND_RESEARCH.md) for the fact-checked evidence and mandatory planning contract.

## Quality Gates

Every renderer milestone needs these tests:

- Native-resolution camera pan with no temporal accumulation.
- Thin geometry and foliage in motion.
- High-contrast specular surfaces.
- Faces and eyes under hard, soft, colored, and moving lights.
- Rough and mirror reflections, including off-screen reflectors.
- Shadowed corridors with many local lights.
- Dark horror lighting and bright outdoor lighting.
- 30, 60, 90, and 120 Hz motion clarity checks.
- Side-by-side against a path-traced/offline reference for material response.
- Debug overlays for ray density, residual magnitude, classifier decision, reconstruction confidence, and cache validity.

The renderer should fail loudly in debug views instead of silently hiding instability with blur.

## Performance And Profiling

Performance is a product feature. The engine must provide clear in-editor profiling views for frame time, pass cost, overdraw, quad overdraw, texture size/residency, meshlet/cluster counts, material complexity, draw/dispatch count, shadow caster cost, ray density, and reflection/AO source confidence.

External capture workflows must be supported and documented for Intel GPA, NVIDIA Nsight Graphics, AMD Radeon GPU Profiler, Microsoft PIX, and RenderDoc. Performance claims are accepted only when reproduced in the in-engine profiler and at least one vendor/frame-debugging tool.

The asset pipeline must optimize for GPU behavior automatically: instancing repeated meshes, selectively merging static assemblies, reducing material slots, building meshlet/cluster LODs, preserving culling granularity, and avoiding small/thin triangle topology that wastes 2x2 pixel quads.
