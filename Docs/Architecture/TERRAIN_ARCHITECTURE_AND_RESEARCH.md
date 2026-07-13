# Terrain Architecture And Research

Status: Accepted planning contract
Date: 2026-07-13

This document defines the engine-owned terrain contract before a terrain module, generator, or runtime dependency is admitted. It covers bounded authored terrain, large finite worlds, deterministic unbounded worlds, and hybrid terrain. It also records the evaluation boundary for learned generators such as Terrain Diffusion.

## Decision Summary

1. The engine will not impose one terrain model on every game. Each project selects a terrain profile, topology, sources, channels, quality targets, and streaming policy.
2. Terrain generation and terrain streaming are separate contracts. A source answers deterministic spatial queries; the runtime schedules, caches, publishes, and retires the resulting artifacts within bounded budgets.
3. The portable heightfield baseline is a tiled quadtree with regular-grid patches, geometric-error selection, and continuous distance-based LOD transitions. Geometry clipmaps remain a measured fast-path candidate for extremely large regular heightfields; they are not the universal representation.
4. Heightfields do not represent caves, overhangs, or arbitrary vertical surfaces. Projects that need those features compose heightfields with ordinary meshes, virtual-geometry pages, voxels, or sparse signed-distance data.
5. CPU collision and gameplay data are authoritative by default. Render-only displacement or learned detail does not silently become collision, navigation, save, replication, or AI authority.
6. Terrain Diffusion is an evaluation candidate for offline or asynchronous macro terrain and climate generation. It is not an admitted dependency and is not the close-range terrain, streaming, collision, or editing system.

No dependency is admitted by this decision. A future dependency or model admission must update [DEPENDENCIES.md](../DEPENDENCIES.md) with its version, license, integration boundary, transitive risk, and measured keep/defer/reject result.

## Project-Selectable Terrain Profiles

The project manifest selects one or more profiles. Profiles configure the same engine contracts; they are not separate renderer forks.

| Profile | Typical use | Required representation and behavior |
| --- | --- | --- |
| Bounded authored heightfield | Arenas, levels, city surroundings | Imported or sculpted finite elevation tiles, deterministic cooking, optional streaming, authored borders. |
| Large finite streamed terrain | Open worlds with known bounds | Partitioned elevation/material/collision artifacts, asynchronous residency, origin-safe coordinates, coarse fallback. |
| Deterministic unbounded heightfield | Exploration, simulation, flight | Random-access spatial source, order-independent generation, bounded caches, stable negative coordinates, sparse persistent edits. |
| Hybrid terrain | Caves, cliffs, arches, destructible or built spaces | Heightfield macro surface plus mesh, virtual geometry, voxel, or SDF regions with explicit authority and transition rules. |
| Spherical or planetary terrain | Planet-scale games | Face/topology-aware tile keys, curvature-aware bounds and error metrics, floating-origin compatibility. This is a deferred specialization, not a Phase 7 baseline promise. |

“Infinite terrain” means that the address space can be queried without a fixed authored edge while runtime memory, generation work, physics, navigation, saves, and simulation remain bounded. It does not mean infinite resident data or that every system simulates the entire address space.

## Ownership And Boundaries

The planned `Engine::Terrain` module owns:

- terrain topology and project profile contracts;
- source queries, canonical tile identity, generation versions, and provenance;
- terrain-specific generation scheduling, caches, edit layers, and publication state;
- canonical cooked/runtime terrain artifacts and terrain diagnostics data.

It may consume `Core`, `Jobs`, diagnostics contracts, cooked `Assets` artifacts, and backend-neutral `RHI` upload services. The surrounding modules retain their existing authority:

| Module | Terrain relationship |
| --- | --- |
| `Assets` | Imports source rasters, graphs, masks, models, and authored data; cooks and stores versioned terrain artifacts. |
| `Jobs` | Executes cancellable generation, decode, cook, and preparation tasks; it does not choose terrain policy. |
| `Scene` | References terrain assets/profiles and stable terrain instances; it does not own generator caches. |
| `Renderer` | Selects visible LODs and consumes immutable render payloads; it does not call Python, mutate sources, or own gameplay elevation. |
| `Physics` | Consumes finalized collision tiles and publishes readiness/results through its own contract. |
| `Automation` | Drives reproducible terrain workflows through public terrain and asset APIs. |
| `Editor` | Owns profile selection, import/generation UI, previews, sculpting, diagnostics views, and user-facing errors. |

`Terrain` must not own renderer passes, native graphics types, a physics world, Scene entities, editor panels, vegetation simulation, or project-specific gameplay rules.

## Source Contract

A terrain source is a deterministic, versioned spatial query provider. Conceptually it accepts:

```text
TerrainQuery {
    profile_id
    topology_or_face
    level
    signed_tile_coordinates
    requested_channels
    seed
    generator_version
    configuration_hash
}
```

and produces a canonical tile or a structured failure. Sources may be composed:

- imported elevation/DEM or authored raster data;
- deterministic procedural graphs and noise;
- regional erosion or hydrology bakes;
- editor sculpt and stamp layers;
- roads, rivers, coastlines, biomes, and gameplay constraints;
- external DCC or geospatial imports;
- learned generators;
- a composite that resolves these layers in declared order.

The engine contract is the query and canonical result, not the implementation language or generator technology. A game can replace a learned source with an imported map or deterministic graph without replacing renderer, collision, streaming, or save contracts.

### Source Families And Trade-Offs

| Source family | Best fit | Strengths | Limits and required controls |
| --- | --- | --- | --- |
| Imported or hand-authored height data | Fixed maps, real locations, art-directed levels | Predictable, directly editable, easy to validate and cook | Finite coverage; source resolution, licensing, holes, projection, resampling, and border cleanup must be explicit. |
| Deterministic procedural graph/noise | Fast iteration, roguelikes, unbounded worlds, stylized terrain | Cheap random access, compact seed/config, portable CPU baseline | Raw noise does not create plausible drainage or gameplay; needs macro masks, domain warping/ridges, constraints, erosion/detail layers, and versioned parameters. |
| Simulation-based erosion/hydrology/tectonics | Natural-looking finite regions and high-quality bakes | Coherent ridges, valleys, catchments, and sediment structure | More expensive and region-dependent; random tile queries need shared coarse context, halos, cached regional results, or offline cooking. |
| Learned generation | Rapid macro ideation, varied world layouts, climate-conditioned synthesis | Can model broad learned structure and correlated channels | Model size/runtime, control, provenance, reproducibility, close-range detail, portability, and failure behavior require a bake-off. |
| Composite authored/procedural/learned stack | Most production games | Art direction and gameplay constraints can override generated foundations while preserving automation | Layer order, ownership, hashes, migrations, and edit conflicts must remain inspectable and deterministic. |

A conventional procedural baseline should expose engine-owned graph semantics rather than hard-code one noise library. Portable libraries such as [FastNoise Lite](https://github.com/Auburn/FastNoiseLite) are useful implementation references for OpenSimplex, Perlin, cellular noise, fractals, and domain warping, but no noise dependency is selected by this contract. Noise supplies signals; it does not by itself solve terrain topology, hydrology, streaming, collision, or authoring.

### Determinism And Seams

Every source admitted for an unbounded or regenerable profile must prove:

- the same query inputs produce the same semantic tile independent of traversal order, thread schedule, cache state, or neighboring requests;
- signed coordinates, including negative tile coordinates, have a canonical mapping and seed derivation;
- shared border samples are bit-identical where the chosen representation requires equality;
- filters, erosion, normal reconstruction, and material classification use declared halo samples or shared parent data rather than hidden neighbor mutation;
- generator, model, configuration, source-data, and cook revisions participate in provenance and cache keys;
- failure and cancellation do not publish partial mutable tiles.

A strict deterministic CPU reference path is required for conformance. GPU generation may be offered when its reproducibility scope and fallback are measured and reported rather than assumed.

## Canonical Terrain Artifact

A canonical tile is versioned independently from its source implementation. Depending on the selected profile it contains or references:

- topology/face, level, signed tile coordinates, world/sector origin, and sample convention;
- elevation samples and scale/offset, or mesh/voxel/SDF payload identity;
- bounds, min/max height, geometric error, and parent/coarse fallback;
- border/halo policy and seam metadata;
- normals or reconstruction inputs;
- material, biome, moisture, climate, flow, water, river, road, exclusion, and gameplay masks requested by the project profile;
- renderer payloads and collision/navigation cooking inputs as distinct sub-artifacts;
- source, generator, model, configuration, edit-layer, and cook provenance;
- semantic and cooked hashes, dependencies, compatibility version, and failure diagnostics.

Render payloads, collision payloads, and editable source data may have different layouts and lifetimes. They share identity and provenance; they do not alias mutable memory.

## Generation And Authoring Layers

The default conceptual order is:

1. macro topology and low-frequency elevation;
2. regional hydrology, drainage, uplift, and erosion where the project requests them;
3. local deterministic surface detail;
4. roads, rivers, water bodies, settlements, traversal, and other gameplay constraints;
5. authored non-destructive edits and stamps;
6. material, biome, vegetation, and render/collision classification;
7. sparse runtime deformation where explicitly supported.

Projects may omit or replace layers, but the ordering and provenance are explicit. Hydrology and erosion cannot be implemented as unrelated per-tile filters that disagree at borders or invent incompatible catchments. They require coarse shared drainage context, sufficient halos, or a bounded regional bake with deterministic stitching.

Authored constraints are first-class inputs, not best-effort decorations applied after generation. Roads that must remain traversable, protected build zones, coastline limits, spawn areas, and navigation requirements must be able to constrain or override generated results before collision and material artifacts are cooked.

## Streaming And Residency

World partition and terrain generation cooperate but remain separate:

- streaming sources publish position, velocity, priority, and requested render/collision/navigation/gameplay radii;
- the terrain scheduler resolves source queries and cache hits into cancellable job graphs;
- jobs generate or decode immutable CPU artifacts, prepare GPU uploads, and publish only complete epochs;
- render, collision, and navigation consumers acknowledge distinct readiness states;
- cache eviction is budgeted across CPU memory, GPU memory, disk, and generator/model memory;
- a resident parent/coarse tile or project-defined safe surface remains available while finer data streams;
- teleports invalidate obsolete work and reprioritize without blocking the render thread;
- device loss, generator failure, cache corruption, and incompatible provenance produce explicit fallbacks and diagnostics.

Near-player collision and gameplay tiles normally require a larger look-ahead margin than their visible boundary. A project must not allow an authoritative character or vehicle to enter terrain whose required collision state is unavailable; it may delay traversal, retain a coarse authoritative tile, or use an explicit loading boundary.

## Rendering And Spatial LOD

The portable heightfield baseline uses tiled regular grids organized by a quadtree. Selection combines projected geometric error, distance, view, occlusion, and residency. Neighbor constraints plus morphing, stitching, or validated skirts prevent cracks. Temporal history may stabilize an already correct spatial selection; it must not conceal unstable LOD or seam errors.

This baseline follows the adaptive regular-grid direction of [Continuous Distance-Dependent Level of Detail for Rendering Heightmaps](https://doi.org/10.1080/2151237X.2009.10129287). [Geometry clipmaps](https://hhoppe.com/proj/geomclipmap/) are retained as a benchmark candidate for continuous traversal over very large regular heightfields because their nested viewer-centered grids offer bounded incremental updates. They are not selected for projects whose density, topology, editing, or hybrid geometry makes a quadtree or virtual-geometry path more appropriate.

Hybrid terrain routes non-heightfield regions through the general mesh/virtual-geometry streamer. The transition between heightfield and non-heightfield authority must have explicit bounds, material continuity, collision ownership, and LOD behavior. A heightfield API must never claim to represent caves or overhangs; this limitation is also reflected by established heightfield collision APIs such as [Godot's HeightMapShape3D](https://docs.godotengine.org/en/4.4/classes/class_heightmapshape3d.html).

Terrain participates in the existing selected-occluder, depth, shadow, material, and residency contracts. Terrain-specific renderer code consumes `Engine::RHI`; it does not add backend-native scene paths.

## Gameplay, Collision, Navigation, And Persistence

- CPU fixed-step gameplay consumes cooked terrain collision, not transient render displacement.
- Navigation, vegetation, water, audio, AI, and gameplay systems consume canonical masks or public spatial queries rather than reading renderer buffers.
- Base terrain identity is the project profile, seed, source configuration, source revisions, and generator/model versions.
- Persistent edits are sparse, ordered, versioned layers when possible. A cooked shipping build may materialize them into tiles while retaining provenance.
- Regeneration after a source or model change is a migration with previewable affected regions and explicit edit compatibility; it is not a silent cache refresh.
- Multiplayer and replay support must define whether terrain is pre-cooked, regenerated from an identical deterministic contract, or distributed as authoritative tile content.

## Terrain Diffusion Evaluation

[Terrain Diffusion](https://github.com/xandergos/terrain-diffusion) is a serious research candidate for macro-world synthesis, but its current repository does not replace the engine contracts above.

The project describes a hierarchical diffusion pipeline with deterministic seed consistency, random-access infinite generation, elevation plus climate channels, and constant-memory tiled inference. Its published models provide approximately 30 m/pixel or 90 m/pixel native data; the repository's REST API returns signed 16-bit elevation in meters and floating-point climate fields. API output scaling above native resolution is bilinear upsampling, so it does not create learned centimeter- or meter-scale gameplay detail.

The current implementation is Python/PyTorch-based and recommends CUDA; CPU operation is substantially slower. The repository includes ONNX export for individual coarse, base, and decoder models, but that does not export the complete infinite scheduler, cache, post-processing, or engine publication lifecycle. The 30 m model artifact is approximately 1.14 GB. These facts make it suitable for an editor tool, build farm, offline cooker, or optional asynchronous service evaluation—not a mandatory shipping runtime.

The research claim of infinite, deterministic, constant-time random access and consumer-GPU interactive generation is recorded from the authors' [InfiniteDiffusion paper](https://arxiv.org/abs/2512.08309); it remains an author claim until reproduced in this engine's workload. Repository and model licensing is MIT, but model/data provenance and every transitive dependency still require a normal admission audit.

### Bake-Off Contract

Before admitting Terrain Diffusion, compare a pinned revision against at least an imported/authorable baseline and a deterministic procedural baseline using identical canonical queries. Record:

- shared-edge equality, traversal-order independence, negative coordinates, seed stability, and results after cache eviction;
- cold and warm p50/p95/p99 latency by tile level and requested channel;
- CPU time, GPU time, RAM, VRAM, disk cache, model load, upload, and concurrent renderer cost;
- Windows and Linux behavior, plus the truthful macOS/CPU-only limitation or fallback;
- cancellation, timeouts, malformed/corrupt cache entries, device loss, and service unavailability;
- close-range detail quality after the engine's local-detail stage;
- hydrology continuity, biome/climate usefulness, roads/gameplay-constraint compliance, and author editability;
- reproducibility of cooked artifacts and compatibility across generator/model upgrades;
- repository, model, training-data/provenance, dependency, distribution, and commercial-license risk.

The result is a written keep/defer/reject decision. “Keep” still means an optional source behind the canonical contract; it does not make PyTorch, CUDA, Flask, or the model a baseline runtime requirement.

## Verification Contract

Terrain implementation cannot be checked complete from compilation or a plausible screenshot. Focused verification must include, as applicable:

- golden semantic hashes for fixed seeds, configurations, coordinates, and source revisions;
- exact shared-edge and LOD-transition tests across parent/child and same-level neighbors;
- randomized query order, concurrency, cancellation, retry, cache eviction, and cold restart;
- negative coordinates, origin shifts, high-magnitude positions, world/profile boundaries, and teleports;
- bounded CPU/GPU/disk residency with deterministic coarse fallback under pressure;
- renderer stall and frame-pacing captures while generation, upload, and eviction are active;
- render/collision/nav alignment and readiness under fast traversal;
- save/reload and regeneration migration with sparse edits;
- representative authored, procedural, unbounded, and hybrid scenes;
- hardware-, backend-, model-, revision-, and quality-scoped performance claims.

## Implementation Order

1. Define project terrain profiles, topology, canonical queries, artifacts, provenance, and source conformance tests.
2. Implement imported/authorable finite heightfield cooking as the simplest truth source.
3. Add deterministic procedural queries and seam/order tests without renderer coupling.
4. Implement portable quadtree heightfield LOD and immutable render publication.
5. Add world-partition streaming, job cancellation, cache budgets, collision readiness, and diagnostics.
6. Route hybrid mesh/voxel/SDF regions through existing geometry and physics contracts.
7. Add editor authoring, generation previews, provenance, migration, and sparse edit workflows.
8. Run the Terrain Diffusion and geometry-clipmap bake-offs; admit only measured optional paths.

## Open Decisions

- Exact tile sample dimensions, border convention, compression, and cooked binary schema.
- Whether planetary topology enters Phase 7 or remains a later project-driven specialization.
- Which deterministic procedural/noise implementation, if any, is admitted as a dependency.
- The first erosion/hydrology representation and its regional bake granularity.
- The edit-layer algebra, conflict rules, and multiplayer distribution policy.
- The measured Terrain Diffusion keep/defer/reject result and supported deployment modes.

## Primary References

- Xander Gos, [Terrain Diffusion repository](https://github.com/xandergos/terrain-diffusion), including the [REST API](https://github.com/xandergos/terrain-diffusion/blob/82a0431281f21a6ec3d691a12ee61525de5b0790/API_README.md), [world pipeline](https://github.com/xandergos/terrain-diffusion/blob/82a0431281f21a6ec3d691a12ee61525de5b0790/terrain_diffusion/inference/world_pipeline.py), and [ONNX exporter](https://github.com/xandergos/terrain-diffusion/blob/82a0431281f21a6ec3d691a12ee61525de5b0790/terrain_diffusion/onnx/export.py), inspected at commit `82a0431281f21a6ec3d691a12ee61525de5b0790`.
- Xander Gos, [InfiniteDiffusion: Procedural Terrain Generation on an Infinite Canvas](https://arxiv.org/abs/2512.08309).
- Xander Gos, [Terrain Diffusion 30 m model card and files](https://huggingface.co/xandergos/terrain-diffusion-30m/tree/main).
- Frank Losasso and Hugues Hoppe, [Geometry Clipmaps](https://hhoppe.com/proj/geomclipmap/).
- Filip Strugar, [Continuous Distance-Dependent Level of Detail for Rendering Heightmaps](https://doi.org/10.1080/2151237X.2009.10129287).
- Eric Galin et al., [Large Scale Terrain Generation from Tectonic Uplift and Fluvial Erosion](https://diglib.eg.org/items/13e52c36-0200-4652-aacf-17aa3098c5fd).
- Jean-David Génevaux et al., [Terrain Generation Using Procedural Models Based on Hydrology](https://www.cs.purdue.edu/cgvlab/www/resources/papers/Genevaux-ACM_Trans_Graph-2013-Terrain_Generation_Using_Procedural_Models_Based_on_Hydrology.pdf).
- Epic Games, [World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine) and [Landscape Outdoor Terrain](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-outdoor-terrain-in-unreal-engine).
- OGC, [3D Tiles standard](https://www.ogc.org/standards/3dtiles/), retained as an interchange/streaming reference rather than the engine runtime format.
- Pixar, [Introduction to USD](https://openusd.org/dev/intro.html), retained as an authoring/interchange reference rather than the terrain runtime format.
- Auburn/FastNoiseLite contributors, [FastNoise Lite](https://github.com/Auburn/FastNoiseLite), retained as a conventional procedural-noise implementation reference rather than an admitted dependency.
