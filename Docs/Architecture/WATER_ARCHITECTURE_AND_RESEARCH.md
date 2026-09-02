# Water Architecture And Research

## Status And Scope

This document is the accepted planning contract for water bodies, water rendering, bounded interactive water, and optional physics coupling. It does not admit a third-party dependency or claim that a water runtime exists today. `PLAN.md` owns implementation order and completion state.

The target is a layered system rather than one universal simulation:

1. Cascaded spectral waves cover oceans and other large deep-water bodies.
2. Bounded 2.5D shallow-water zones provide real-time flow around shorelines, obstacles, sources, drains, and forces where interaction matters.
3. Authored or baked deformation fields cover breaking waves, waterfalls, and other shapes a single-valued heightfield cannot represent.
4. Spray, foam, bubbles, wakes, and wetness are sparse secondary representations, not reasons to make the whole water body volumetric.
5. A deterministic CPU-readable surface query is the gameplay and physics boundary. GPU simulation can enrich appearance but cannot silently become gameplay authority.

This division preserves portability, scales cost with visible value, and lets projects choose ocean, river, lake, pool, or hero-water features independently.

## Evidence Levels

This document keeps four kinds of statement separate:

- **Source claim** describes a cited product, talk, paper, or documentation page.
- **Inference** explains what that evidence suggests for this engine.
- **Accepted decision** is a project contract in this document.
- **Verified behavior** requires implementation plus repository-owned tests or captures. No water behavior is verified yet.

The user supplied **KWS2 Dynamic Water System 1.1** as a reference. The public documentation inspected on 2026-09-03 still labels its scripting API `1.0.x`; therefore the useful evidence below is tied to the documented mechanisms, not an unverified point-release distinction.

## Reference-System Findings

### KWS2: The Important Mechanism Is Layering, Not One Global Simulation

KWS2 demonstrates a practical separation between an infinite spectral ocean and finite interactive zones:

- The ocean uses several FFT wave cascades for small, medium, and large wavelengths. Its procedural camera-following quadtree selects LOD and frustum visibility on the GPU and renders an instanced surface. The ocean simulation is shared across cameras for a frame, while each camera renders its own view.
- The infinite ocean is explicitly noninteractive. Local and movable dynamic zones are composed with it for shoreline, river-mouth, vessel, and other localized interaction. Buoyancy samples the combined ocean and local displacement.
- A dynamic zone is a GPU 2.5D heightfield. Each cell stores one surface height and horizontal velocity, so it cannot represent overturning waves, pressure, a vertical water column, a cave interior hidden from the top-down capture, or free-falling water.
- Static terrain and geometry are captured into a top-down orthographic depth representation when a static zone initializes. Cached state does **not** automatically follow later static-terrain changes. `ForceUpdateZone(resetSimulation: true)` rebuilds baked data, clears dependent simulation textures and particles, and refreshes zone intersections.
- Runtime effectors are separate. Sources add mass and velocity, forces add directional or rotational motion without blocking flow, drains remove water, and obstacle meshes write a dynamic-obstacle depth representation. Dynamic obstacles can therefore reroute flow without rebaking the static capture.
- That real-time obstacle path has limits: the documentation recommends slow or static obstacles, can activate them only while intersecting water, and warns that rapid movement can drive the surface upward because the heightfield advances on discrete simulation timing. A dynamic obstacle also needs collision/layer filtering that prevents the same mesh from being captured twice.
- The published zone data separates simulation velocity/height/terrain, wetness/shoreline distance/foam/depth, effector accumulation, dynamic-obstacle depth, static-scene depth, signed-distance data, normals, and color. That separation is a useful diagnostic and scheduling precedent; its exact texture layout is not adopted as an engine contract.
- Advected foam follows simulated flow and curls around obstacles but costs more than a flowmap-based foam path. Particle buffers and overdraw are separately bounded.
- Zone extent is the dominant cost. KWS2's example compares a roughly 2 km square default-resolution zone approaching 1 GB with a 200 m square zone around 12 MB and roughly ninety times fewer simulated pixels. The transferable rule is small, non-overlapping, cached zones—not those hardware- and implementation-specific figures as engine budgets.
- Expensive optical features are independently scalable: volumetric lighting, dynamic-zone render targets, refraction, caustics, anisotropic reflections, underwater composition, planar reflection, and screen-space reflection are not one indivisible quality switch.

The accepted inference is that KWS2's visually convincing obstacle response is real and useful, but it is a **bounded dynamic heightfield interacting with separately captured geometry**, not arbitrary volumetric fluid simulation. This engine adopts that decomposition while making recapture state, authority, validity, memory, and failure behavior explicit.

KWS2 also exposes constraints we must avoid hiding:

- A single global ocean height is convenient for planar reflection but is insufficient for local water at different elevations.
- One shared mesh/material and experimental infinitely-deep custom meshes are product constraints, not desirable engine contracts.
- Very large storm waves can exceed a heightfield or quadtree's stable operating range.
- Top-down capture cannot discover closed or occluded volumes.
- Mobile support and temporal quality assumptions must be capability decisions, not implied by a desktop reference.

### Sea Of Thieves: Readability, Foam, And Local Deck Water

Rare's 2018 technical-art presentation describes a Tessendorf FFT ocean whose shape, scattering approximation, foam, specular treatment, and underwater view are tuned as one readable art system:

- Deep and subsurface colors blend from view direction, sun direction, and a wave-peak mask derived from choppy displacement.
- Foam appears at wave peaks and near intersecting objects inside a camera-centered window. Depth comparisons find intersections; a blurred feedback field disperses foam; authored texture provides final structure.
- Calm, normal, and storm profiles vary the same coherent system rather than enabling unrelated effects.
- A closest-point-on-sphere approximation broadens low-sun glitter, and a Snell-window treatment makes the underwater transition legible.
- Water on ship decks uses a separate GPU shallow-water simulation. A camera depth projection into that surface drives streams, waterfalls, and object interaction.

The accepted lesson is that photorealism is not just geometric wave density. Stable art direction, lighting coherence, foam history, object contact, and explicit transitions between deep water and bounded local water matter at least as much.

### Horizon Forbidden West: Hero Waves Are Authored Representations

Guerrilla's 2022 presentation treats breaking waves as localized, streamable deformations because a general real-time ocean solver could not produce the required controllable shape within budget:

- A Houdini simulation or hand-authored curve is reduced to one animated cross-section. Position offsets are stored over cross-section and time, with separately baked partial derivatives and attributes for UV correction, foam, relative height, and deformation strength.
- At runtime a compute pass evaluates quads around an authored wavefront, samples the deformation, transforms it into the local frame, and blends it with the base water surface.
- Shape, guide, and animation curves give artists control. Expensive curve evaluation is baked into a grid indexed by time and transverse distance, stored with world tiles, and streamed.
- Variation textures create gaps in the breaking front.
- More general waterfall impacts use authored/baked three-dimensional deformation volumes, with localized tessellation where the base mesh lacks enough vertices.

The accepted lesson is to provide an explicit non-heightfield route. Trying to force breakers, curls, and waterfalls through the ocean FFT or shallow-water grid would produce worse results at higher cost.

### Research Baselines And Alternatives

- Tessendorf remains the baseline for efficient statistical deep-water spectra and inverse-FFT displacement.
- Bruneton, Neyret, and Holzschuch show how wave geometry can transition into a filtered ocean reflectance model so distant detail does not shimmer or disappear abruptly.
- Chentanez and Müller demonstrate terrain-aware shallow water with wet/dry boundaries, arbitrary slopes, nonreflecting boundaries, and mass/momentum exchange into spray and foam particles. Advected small FFT ripples restore high-frequency detail.
- Water Surface Wavelets and Wave Particles are candidates for sparse, directional, spatially local wave propagation and interaction. They require a measured bake-off against the simpler bounded heightfield rather than automatic admission.
- Unreal Engine's shared quadtree water mesh and spline-authored water bodies support the mesh/body split; its multiple-sphere pontoon model is a useful low-cost buoyancy baseline.
- A hybrid multi-band FFT plus local shallow solver is a research candidate, but the exact coupling, boundary energy, determinism, and readback cost must be measured in this engine.

## Ownership And Dependency Boundaries

The planned `Engine::Water` module owns:

- stable water-body, interaction-zone, and cooked-water-artifact identities;
- body profiles, spectral parameters, local-zone policies, boundaries, connections, and provenance;
- immutable render/query descriptions published to consumers;
- the low-frequency CPU surface evaluator used by gameplay and physics;
- explicit simulation epochs, validity, cache state, recapture requests, and diagnostics;
- deterministic authoring/cook inputs for hero breakers, waterfalls, and deformation volumes.

It does not own renderer passes, the physics world, Scene entities, terrain generation, editor panels, audio, or native graphics types.

| Module | Water relationship |
| --- | --- |
| `Terrain` | Publishes versioned shoreline, bathymetry, river-flow, obstruction, and wettable-surface artifacts. It does not own runtime water. |
| `Scene` | Stores authoring components that refer to stable water identities and profiles. It does not expose mutable component storage to simulation. |
| `Renderer` | Owns GPU simulation execution, surface generation, shading, reflection/refraction, caustics, underwater composition, and visual-only secondary effects. |
| `RHI` / `RenderGraph` | Provide backend-neutral resources, queues, passes, synchronization, lifetime, aliasing, and readback boundaries. |
| `Physics` | Consumes fixed-tick immutable water queries and optionally publishes bounded wake/impulse inputs. It never waits for render-time GPU water. |
| `Assets` | Imports and cooks body profiles, spectra, flow fields, shoreline data, breaker cross-sections, and deformation volumes with provenance. |
| `Jobs` | Schedules CPU evaluation, streaming, cook, and snapshot production without owning water policy. |
| `Editor` | Authors bodies, zones, effectors, optical profiles, budgets, and debug views through public contracts. |

No new dependency is admitted by this document. The initial implementation must use the existing math, RHI, render graph, task, asset, and snapshot foundations.

## Public Data And Authority Contracts

The exact C++ names remain an implementation decision, but the public boundary must express at least:

```text
WaterSurfaceQuerySnapshot {
    bodyId
    simulationTick
    epoch
    authority              // analytical, CPU shallow, visual approximation, unavailable
    validity               // valid, stale-with-age, outside-domain, not-ready
    surfacePointOrHeight
    surfaceNormal
    surfaceVelocity
    flowVelocity
    depthOrSubmergence
}
```

Requirements:

- Physics and gameplay sample an immutable snapshot associated with a fixed tick. They never read mutable renderer textures or wait for asynchronous GPU completion.
- The CPU evaluator reproduces the admitted low-frequency spectral components and authoritative local flow closely enough for buoyancy, queries, replay, and save/load. High-frequency GPU detail is visual.
- Body/profile/artifact version, spectrum seed, time origin, phase, zone epoch, and origin-rebase state are explicit. Save/load never depends on wall-clock reconstruction.
- An unavailable, stale, evicted, or recapturing zone has a defined fallback and reports its state. It must not return plausible-looking unlabelled zeros.
- Cross-body overlaps have deterministic priority and blend rules. Ocean-to-river and zone-to-zone connections conserve a documented quantity within a tolerance or visibly fall back.
- World-origin rebasing changes representation, not wave phase or query identity.

## Simulation Layers

### Deep-Water Spectral Layer

The baseline uses multiple directional spectrum bands with deterministic seeds and phases. A portable compute FFT is preferred where capabilities and measured budgets permit it; a finite directional-wave evaluator is the required fallback and CPU query representation.

Each band defines its physical wavelength range, update frequency, resolution, choppiness limit, and contribution to displacement, slopes, foam precursors, and velocity. Bands may update at different rates, but interpolation and phase continuity are explicit. Extreme profiles must clamp or route to authored hero deformations rather than destabilize the shared surface.

### Surface Mesh And Filtering

Large surfaces use a camera-relative quadtree or projected grid with continuous LOD transitions, frustum/occlusion-aware submission, bounded patch count, and origin-rebase safety. The mesh system is independent of the water-body and shading contracts.

As projected triangles become subpixel, unresolved geometric wave energy transitions into filtered normals and the water BRDF. This transition is measured for sparkle, horizon stability, and energy consistency; simply dropping spectral bands at distance is not acceptable.

### Bounded Shallow-Water Zones

The baseline interactive solver is a finite 2.5D heightfield over terrain/bathymetry inputs. It supports wet/dry cells, sources, drains, forces, boundary inflow/outflow, slow dynamic obstacles, cached static capture, zone connections, and an explicit recapture operation.

Accepted constraints:

- Zones are small, sparse, non-overlapping where practical, distance/budget culled, and independently resolvable. A world-sized interactive grid is rejected.
- Static obstruction artifacts and runtime dynamic obstacles are distinct inputs. Static changes mark the artifact stale and schedule an explicit rebuild; dynamic obstacles update a bounded depth/occupancy field.
- Fast obstacle movement is detected. The zone substeps within budget, clamps/invalidate affected cells, or falls back to visual splash/foam; it may not silently explode the heightfield.
- The implementation declares height, velocity, wetness, foam precursor, boundary, obstacle, and diagnostic representations separately so memory and pass costs are visible.
- Zone cache/eviction preserves or intentionally resets state according to body policy and reports which occurred.
- Cave interiors, overhang-hidden water, curls, breaking crests, and falling sheets are outside this solver and use another representation.

Water Surface Wavelets, wave particles, lattice Boltzmann, or higher-order shallow solvers are bake-off candidates only if the baseline cannot meet measured interaction, stability, or coupling requirements.

### Breakers, Waterfalls, Spray, And Foam

Hero breaking waves use cooked animated cross-sections along authorable wavefronts, including derivative, UV, foam, and blend metadata. Waterfalls and impacts use ribbon/sheet meshes or bounded deformation volumes. These artifacts stream with the relevant world tile and degrade to a cheaper static/particle representation when absent.

Spray, mist, bubbles, and foam particles are sparse secondary systems with explicit emission, lifetime, buffer, overdraw, and simulation budgets. They do not carry gameplay mass unless a future measured contract explicitly promotes them.

Foam has distinct sources: spectral crest/curvature, shallow-water compression/vorticity, shoreline/intersection, wakes, and authored masks. A cheap flowmap/static path and a history/advected path are separate quality choices. History must reject disocclusion, zone reset, body changes, and invalid motion.

## Rendering And Lighting Integration

Water participates in the accepted linear-HDR lighting and exposure pipeline. It consumes the same sun, sky, atmosphere, probes, reflection routing, volumetric lighting, shadowing, and view data as opaque rendering rather than building a parallel lighting system.

The surface model requires:

- dielectric Fresnel with complementary reflected/transmitted energy;
- absorption by traveled water distance and bounded in-scattering driven by optical profile and available lighting;
- refraction of a defined pre-water opaque scene with depth/thickness validity and a fallback for missing/incorrect samples;
- multi-scale slopes/normals derived from the admitted wave layers;
- exposure-aware foam and spray that remain reflective/scattering materials rather than emissive white overlays;
- optional anisotropy/glitter with a bounded sample count and stable filtering;
- shoreline depth/color/foam transitions based on versioned terrain and water data.

Reflection sources are additive only through an explicit priority/miss policy:

1. sky and reflection probes are always-correct portable fallbacks;
2. current-frame screen-space reflection supplies visible on-screen detail;
3. bounded planar reflection is allowed for selected bodies/views;
4. sparse ray tracing may fill qualified misses when Phase 9 capabilities and budgets allow it.

Ray tracing is never required for correct water. Local water elevations carry their own body plane/bounds; a single global ocean height cannot drive every planar view.

The baseline has no mandatory temporal accumulator. Optional temporal reflection, volumetric, caustic, foam, or simulation histories declare motion inputs, rejection rules, reset events, and non-temporal fallbacks.

Underwater rendering reuses the Phase 8 froxel/volumetric lighting contract for depth-dependent absorption and scattering, with a defined air/water crossing, Snell window and total internal reflection where appropriate, waterline stabilization, and bounded caustics. It must coexist with atmosphere, exposure, transparent objects, and local water volumes without double-applying fog.

Caustics are optional and capability-scaled: a cheap projected approximation is the baseline, spectral or local-zone-derived caustics are enhanced paths, and ray-traced caustics are research-only until measured.

## Optional Physics And Gameplay Coupling

Physics remains CPU fixed-step authority. The initial buoyancy path samples the immutable water query with a bounded set of pontoons/spheres or volume probes. It applies displaced-volume buoyancy and drag from body-relative velocity. Lift, planing, propulsion, slam, and added-mass approximations are optional capabilities with independent tests and budgets.

A higher-cost submerged-volume fallback may be used for selected bodies, but it cannot make normal rigid-body stepping depend on render resolution or GPU readback. Query density, force clamps, out-of-domain behavior, sleeping, teleport, origin rebase, save/load, and replay are explicit.

Physics-to-water coupling is deliberately asymmetric at first:

- Physics immediately samples the authoritative CPU water state.
- Physics may publish bounded obstacle transforms, wake emitters, impulses, or displacement estimates for a later visual-water update.
- Visual wakes and shallow-water response may be one or more frames late; that latency is measured and labelled.
- GPU foam, spray, and high-frequency displacement never feed back into collision, networking, or saves.

Promoting local water to two-way gameplay authority requires a separate deterministic solver/state-capability decision, not an incidental GPU readback.

## Authoring, Streaming, And Diagnostics

Projects author stable water bodies and profiles: ocean/lake/river/pool topology, level and flow policy, spectrum/wind, optical coefficients, shore/bathymetry inputs, local interaction zones, connections, effectors, hero deformations, quality tiers, and budgets.

Cooked artifacts include version/provenance, coordinate frame, body bounds, terrain dependency versions, resolution/validity range, spectrum seeds, zone boundary data, flow fields, deformation data, and fallback representation. Stale terrain or body inputs invalidate dependent artifacts deterministically.

Diagnostics must show, per view/body/zone:

- selected simulation and surface-mesh path;
- spectrum bands, update cadence, patch count, and LOD transitions;
- zone resolution, active/wet cells, substeps, cache/recapture state, obstacles, and boundary flux;
- artifact and terrain versions plus query authority/age;
- reflection/refraction/underwater/caustic routes and fallbacks;
- foam/particle counts, history validity, and overdraw;
- GPU/CPU time, memory, queue placement, waits, and critical-path contribution.

## Performance And Capability Qualification

Quality tiers independently budget spectral simulation, surface mesh, local simulation, reflection, refraction, underwater volume, caustics, foam/particles, CPU queries, and physics sampling. A preset is a collection of limits, not a hidden algorithm switch.

Measure p50/p95/p99 CPU and GPU cost, transient and persistent memory, bandwidth, queue overlap, synchronization waits, visible error, and fallback frequency in at least calm ocean, storm ocean, shore, river/obstacles, multiple local elevations, underwater, and mixed-body scenes. Async compute is admitted only when timing shows reduced critical-path cost on the tested backend/device.

Capability selection occurs before resource creation and reports the actual path. Required qualification includes Vulkan and D3D12 on available hardware; portable fallback claims require running the fallback. Optional RT, mesh/task shader, wave-operation, subgroup-size, or async paths cannot become unconditional requirements.

## Rejected Shortcuts

- One world-sized shallow-water or volumetric simulation.
- Calling a top-down 2.5D heightfield fully volumetric or universally obstacle-aware.
- Rebaking static terrain every frame to simulate dynamic obstacles.
- Making render-time GPU displacement or readback physics authority.
- One global ocean plane for rivers, lakes, pools, and stacked water.
- A water-only sun, fog, exposure, or reflection system inconsistent with the renderer contract.
- Mandatory screen-space, planar, ray-traced, or temporal effects without a correct fallback.
- Encoding breakers and waterfalls as increasingly unstable ocean waves.
- Checking roadmap work complete from a still screenshot, shader compilation, or average frame time.

## Verification Requirements

Implementation evidence must include deterministic spectrum/query replay, CPU/GPU low-band agreement, origin-rebase continuity, body overlap priority, shoreline/depth validity, zone wet/dry and mass/flux bounds, obstacle appearance/disappearance and recapture behavior, fast-obstacle failure handling, cache reset/restore, boundary handoff, fallback selection, and save/load state policy.

Rendering captures must exercise day/night, low sun, roughness/wind ranges, shore contact, object intersections, multiple water elevations, above/below-water crossing, disocclusion/history resets, and each reflection/refraction fallback. Performance captures report the independent costs named above and the critical path, not only total frame rate.

Physics evidence must cover buoyancy equilibrium, relative-current drag, partial submergence, sleep/wake, teleport, missing/stale queries, fixed-step variation limits, deterministic replay for the claimed tier, and visual-coupling latency. The required backend/platform is run before claiming it.

## Roadmap Placement

The dependency order is:

1. Phase 7 publishes canonical shoreline, bathymetry, river-flow, obstruction, and streaming inputs.
2. Phase 8 establishes shared lighting, probes, atmosphere, volumetrics, and exposure.
3. Phase 8W implements water bodies, queries, rendering, bounded interaction, authoring, and qualification.
4. Phase 9 may add sparse RT reflection/refraction residuals without becoming a baseline dependency.
5. Phase 11 physics optionally consumes authoritative water queries and publishes visual coupling inputs.
6. Later editor, diagnostics, cooking, and shipping phases expose and qualify the corresponding workflows.

## Open Bake-Offs

- Stockham versus another portable FFT schedule, including subgroup and non-subgroup paths.
- Quadtree rings versus projected grid for the admitted view and multi-camera cases.
- Shallow-water discretization and boundary coupling against wavelets/particles for localized interactions.
- Analytical CPU low-band agreement tolerance and update cadence.
- Breaker cross-section versus deformation-volume artifact thresholds.
- Foam feedback/advection quality versus flowmap and particle costs.
- SSR/planar/probe/RT priority and underwater optical approximations by quality tier.
- Pontoon, probe-volume, and submerged-volume buoyancy cost/behavior.

## Primary References

- Kripto289, [KWS2 documentation](https://kripto289.gitbook.io/kripto289-docs), especially [Ocean](https://kripto289.gitbook.io/kripto289-docs/manual/getting-started/ocean), [Dynamic Simulation](https://kripto289.gitbook.io/kripto289-docs/manual/getting-started/dynamic-simulation), [Dynamic Simulation Zone Settings](https://kripto289.gitbook.io/kripto289-docs/manual/water-modules-settings/dynamic-simulation-zone-settings), [Effector Settings](https://kripto289.gitbook.io/kripto289-docs/manual/water-modules-settings/dynamic-simulation-effector-settings), [Zone API](https://kripto289.gitbook.io/kripto289-docs/manual/api/dynamic-simulation-zone-api), [Simulation Performance Tips](https://kripto289.gitbook.io/kripto289-docs/manual/advanced-usage-and-optimization/simulation-performance-tips), [Rendering Performance Tips](https://kripto289.gitbook.io/kripto289-docs/manual/advanced-usage-and-optimization/rendering-performance-tips), [Water Settings](https://kripto289.gitbook.io/kripto289-docs/manual/water-modules-settings/water-settings), and [Quality Settings](https://kripto289.gitbook.io/kripto289-docs/manual/water-modules-settings/quality-settings).
- Keith Judge and Roberto Padovani, Rare, [The Technical Art of Sea of Thieves](https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf), SIGGRAPH 2018.
- Julian Malan, Guerrilla, [Water Rendering in Horizon Forbidden West](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Water-Malan.pdf), SIGGRAPH 2022.
- Jerry Tessendorf, [Simulating Ocean Water](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf), SIGGRAPH course notes.
- Eric Bruneton, Fabrice Neyret, and Nicolas Holzschuch, [Real-time Realistic Ocean Lighting using Seamless Transitions from Geometry to BRDF](https://diglib.eg.org/items/fb727c69-2be2-4edd-833e-f7a9baea9b63), Eurographics 2010.
- Nuttapong Chentanez and Matthias Müller, [Real-time Simulation of Large Bodies of Water with Small Scale Details](https://diglib.eg.org/items/d0320015-4b07-416b-8f41-047485c9f7f3), SCA 2010.
- Stefan Jeschke and Chris Wojtan, [Water Surface Wavelets](https://visualcomputing.ist.ac.at/publications/2018/WSW/), SIGGRAPH 2018.
- Cem Yuksel, Donald House, and John Keyser, [Wave Particles](https://cemyuksel.com/research/waveparticles/), SIGGRAPH 2007.
- Epic Games, [Water Meshing System and Surface Rendering](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine) and [Water Buoyancy Component](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-buoyancy-component-in-unreal-engine).
- Zhenyu Mao and Kui Wu, [Open World Water Rendering and Real-Time Simulation](https://media.gdcvault.com/gdc2023/Slides/Open-World%2BWater%2BRendering%2Band%2BReal-Time%2BSimulation_Mao_Zhenyu%26Wu_Kui.pdf), GDC 2023.
