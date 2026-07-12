# Physics Architecture And Real-Time Simulation Research

**Status:** Accepted architecture constraints; backend selection open
**Date:** 2026-07-13
**Scope:** Planning and evaluation only; no physics implementation or dependency admission

## Decision Summary

Spiral will not use one solver for every physical phenomenon. The production architecture is a tiered system:

1. A backend-neutral `Engine::Physics` contract owns fixed-step gameplay physics, stable handles, collision/query semantics, deterministic command publication, state capture, and capability reporting.
2. A qualified CPU rigid-body backend remains authoritative for gameplay bodies, characters, contacts, queries, replication inputs, saves, and AI/navigation observations.
3. Ordinary cloth, hair, and soft deformation starts with a portable bounded-cost PBD/XPBD-class path or a simplified proxy path.
4. PD with barrier contact, IPC-family methods, ABD, and FEM are evaluated for selected hero/contact-heavy or offline islands. They are not assumed to fit the frame budget, remain deterministic, or retain their paper guarantees after approximation.
5. GPU deformation is visual/secondary by default. CPU proxies remain authoritative, and the GPU result does not drive same-tick gameplay without a future explicit authority, latency, determinism, synchronization, and fallback decision.

This decision deliberately does not select Box3D, Jolt, PhysX, Havok, Bullet, or a custom solver. The initial open-source bake-off is Box3D versus Jolt behind the same engine-owned conformance harness. Box3D is the leading lightweight/architecture-fit candidate; Jolt is the leading maturity/feature-breadth control. Neither is presumed to win. A dependency enters `Docs/DEPENDENCIES.md` only after that evaluation is complete.

## Why The Seed Conversation Is Not Evidence

The attached Gemini conversation was useful for identifying subjects, but it mixed real papers with invented names, changed benchmark meanings, and promoted paper-specific results into universal engine claims. This document relies on papers, official project documentation, source repositories, and engine-owned future measurements instead.

### Claim Audit

| Seed claim | Verdict | Correct interpretation |
| --- | --- | --- |
| MiNNIE runs a 105k-vertex, 371k-element model in about 26 ms | Supported, with scope | The paper reports that average on an RTX 4090 for its demonstrated model. It also reports an 11.2 ms, 40k-vertex/112k-element octopus case. These are research-scene results, not a 3 ms engine budget or multi-vendor qualification. |
| MiNNIE proves general penetration-free gameplay FEM | False | MiNNIE addresses nonlinear near-incompressible elasticity and locking. Its reported collision treatment uses quadratic penalties and spatial hashing; it is not an IPC-style intersection-free gameplay contact system. |
| FEM, IPC, and Projective Dynamics are competing whole-engine choices | False category model | FEM discretizes continuum deformation; IPC is a variational barrier/contact framework; PD is a simulation/optimization framework. A system can combine selected parts, as the 2022 PD+IPC paper does. |
| PD+IPC universally costs 2-4 ms for a 100k-vertex asset | Unsupported | The 2022 RTX 3090 results vary sharply by scene and contact intensity. Its table includes tens of milliseconds in solver and CCD work and ranges from interactive to real-time, not a universal 2-4 ms result. |
| Capping iterations preserves an unconditional zero-intersection guarantee | Unsafe | The papers state guarantees for their algorithms under their feasibility, CCD, barrier, step, and solve assumptions. Engine approximations, finite precision, fallback paths, invalid initial meshes, topology changes, and early termination require separate validation and tolerance wording. |
| A 2024 solver resolved more than 180 million contacts in real time | Misrepresented | The ZOZO/PPF repository reports an extreme offline case beyond 180 million contacts, not real-time throughput. The repository explicitly targets offline use, excludes multi-day/week examples from CI, and requires NVIDIA CUDA. |
| DX12/Vulkan can reserve a fixed percentage of SMs or RT cores | False | They expose queues, synchronization, and some priority controls, but no portable fixed-silicon partition. Overlap and interference are measured outcomes. |
| DX12/Vulkan can pin arbitrary physics data in L2 | False as a portable graphics-API claim | CUDA exposes NVIDIA-specific persisting-L2 access policy controls on capable devices. That is not a cross-vendor DX12/Vulkan engine contract and does not eliminate cache contention. |
| DMA or Resizable BAR makes a GPU solver free | False | Copy engines can move data and overlap eligible transfers; they do not solve constraints. Residency can reduce transfers, but queue contention, synchronization, memory bandwidth, and publication latency remain. |
| RT cores directly accelerate general IPC | Unsupported | Research demonstrates hardware-ray-tracing acceleration for particular collision-query representations, including robot mesh/sphere trajectories. It does not establish a portable general IPC broadphase, CCD, or contact-response replacement. |
| Vox3D is a Source replacement physics engine | Fabricated name | The real project is Box3D by Erin Catto. Official sources describe Box3D, not Vox3D or “VPhysics Box3D.” |
| Box3D is proven production-ready and universally deterministic | Overstated | Box3D 0.1 is real, MIT-licensed, used by s&box and other projects, and advertises cross-platform determinism. Its author explicitly calls it alpha; deterministic application inputs/order and floating-point configuration remain application responsibilities. |
| The quoted Box3D-versus-PhysX timings are authoritative | Misattributed | Similar numbers appear in a third-party Unity binding benchmark with different settings. They are leads to reproduce, not selection evidence. |

## Research Findings

### MiNNIE And Real-Time FEM

[MiNNIE](https://www.tiantianliu.cn/papers/ruan24minnie/ruan24minnie.pdf) is credible evidence that mixed FEM with a specialized CUDA multigrid solver can run near-incompressible nonlinear elastic examples interactively or in real time on high-end hardware. It addresses volumetric locking as Poisson's ratio approaches 0.5, supports large deformation and inversion recovery, and reports its results on an RTX 4090.

It does not establish FEM as the general engine rigid-body/contact backend:

- the highest cited example averages about 26 ms before considering the rest of a game frame;
- the implementation is CUDA-specific and not a D3D12/Vulkan/Metal portability result;
- collision/contact is not the paper's primary contribution or an IPC guarantee;
- paper meshes, time steps, material settings, iteration counts, and convergence tolerances do not predict arbitrary game assets;
- handling inversions and recovering volume is not exact conservation: the paper reports measurable loss in examples, including 0.8% for its front-page bunny;
- gameplay needs bodies, joints, triggers, queries, sleeping, characters, streaming, events, state capture, and networking semantics in addition to deformation.

FEM is therefore a valid hero soft-body, flesh, rubber, destructible, tooling, or offline-bake candidate after the baseline architecture exists. It is not the baseline physics engine.

### IPC And Barrier Contact

[Incremental Potential Contact](https://ipc-sim.github.io/) provides an important intersection- and inversion-free variational contact formulation for deforming solids. [C-IPC](https://ipc-sim.github.io/C-IPC/) extends the family to codimensional objects and strain limiting. [GIPC](https://arxiv.org/abs/2308.09400) and [StiffGIPC](https://arxiv.org/abs/2411.06224) improve GPU optimization and stiff-contact convergence.

These results justify an engine prototype, not a blanket marketing promise. Spiral must report collision thickness, tolerances, initial-feasibility requirements, supported primitive/topology classes, solver residual, termination reason, and any fallback. The engine may say a qualified path maintained its stated separation tolerance in a named test suite; it must not claim universal “0% penetration” across all backends and quality tiers.

### Projective Dynamics With Barrier Contact

[Penetration-free Projective Dynamics on the GPU](https://www.math.ucla.edu/multiples/publication/lan2022pdipc.pdf) is direct evidence that PD can be integrated with IPC-style barriers and CCD. On an RTX 3090 with a 1/100-second simulation step, the paper reports 69k-249k DOF examples whose frame rates vary significantly with contact intensity. The largest demonstration ranges from 7.7 to 26.8 simulation FPS; other scenes have much faster best frames but still show expensive worst frames. The authors also state that PD is not fully physically accurate and is sensitive to time-step size.

The correct conclusion is that PD+barrier is a strong hero-deformation candidate and may be real time for suitable assets. It is not proven to fit a fixed 3 ms production budget alongside Spiral's renderer. The prototype must measure p50/p95/p99 cost, not only average or best FPS.

Dense contact can be much worse than the headline range: the paper's close-contact armadillo case falls to 1.3 FPS in its slowest frames. Its GPU global solve is iterative A-Jacobi, not the transcript's claimed constant prefactorized Cholesky/back-substitution path.

[Efficient GPU Cloth Simulation with Non-Distance Barriers and Subspace Reuse](https://wanghmin.github.io/publication/lan-2024-egc/) is a newer candidate for high-resolution garment contact. Its non-distance barrier and subspace-reuse design should be compared with XPBD and PD+IPC for cloth, but it does not replace rigid-body gameplay physics.

### Cubic Barrier And PPF Solver

[A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness](https://doi.org/10.1145/3687908) targets penetration-free contact resolution and strain limiting. The associated [PPF contact solver](https://github.com/st-tech/ppf-contact-solver) is valuable reproducible research and documents shells, solids, rods, strict strain-bound examples, and an extreme case beyond 180 million contacts. That is a scalability/offline result, not a real-time result: the repository explicitly describes the solver as built for offline use, notes that large examples take days to weeks, and requires NVIDIA CUDA/x86. It is a prototype/reference candidate, not a portable runtime dependency.

### ABD

[Affine Body Dynamics](https://arxiv.org/abs/2201.10022) reduces stiff or nearly rigid deformables to affine bodies and integrates IPC-style contact. This can be valuable for hero contact where ordinary rigid bodies look visibly wrong, but it is not automatically better for thousands of normal gameplay props. Spiral will compare ABD with ordinary rigid bodies and simplified articulated/proxy models on the same contact scenes.

## Authoritative Physics Boundary

The planned `Engine/src/Engine/Physics` module owns:

- `PhysicsWorld` lifetime and backend selection;
- typed, generation-safe body, shape, constraint, material, query, and snapshot handles;
- the fixed-step accumulator and tick numbering;
- staged mutation/force/kinematic command buffers;
- collision layers, masks, materials, trigger/contact event semantics, and stable result ordering;
- body/constraint/query capability reporting;
- immutable result and event snapshots;
- state hashing, record/replay hooks, and snapshot/restore capability reporting;
- backend-neutral debug primitives and metrics.

It does not own Scene entities, editor panels, asset importing, renderer passes, or backend-native graphics objects.

### Data Flow And Task Order

```text
gameplay input and queued world mutations
  -> animation/root-motion and kinematic targets
  -> fixed physics tick(s) on the CPU frame task graph
  -> immutable transforms, velocities, contacts, triggers, and query results
  -> Scene finalization
  -> render extraction
  -> optional GPU visual deformation
  -> renderer consumes published transforms/deformation resources
```

The CPU frame task graph schedules physics. The render graph does not schedule CPU gameplay physics. Optional GPU solvers use `Engine::RHI` resources, queues, and fences; the renderer/render graph may consume a published resource only after its epoch and synchronization contract are satisfied.

Late commands, entity destruction, origin rebasing, pause/single-step, catch-up overflow, and failed ticks must have deterministic documented behavior. Physics never reads mutable editor or Scene component storage while solving.

## Fixed-Step And Authority Contract

Before backend selection, Spiral must specify:

- fixed tick rate and exact accumulator arithmetic;
- maximum substeps/catch-up time and overload behavior;
- interpolation/extrapolation rules for render frames;
- stable command ordering and creation/destruction epochs;
- whether contacts/events are ordered, deduplicated, and delivered once or per substep;
- snapshot lifetime and stale query-result behavior;
- world-origin shift rules;
- save/restore, replay, and rollback capability levels.

Authority is explicit:

| State | Default authority |
| --- | --- |
| Gameplay body transforms/velocities | CPU physics world |
| Character grounding and collision-resolved motion | CPU physics world |
| Hit tests, triggers, contact events, AI/nav queries | CPU physics/query snapshot |
| Cloth/hair/flesh surface detail | Optional GPU visual solver or portable fallback |
| Gameplay interaction with deformables | Coarse CPU proxy until a stronger authority contract is accepted |
| Renderer TLAS/visibility data | Rendering only; never gameplay collision authority |

## Determinism Is A Capability Matrix

“Deterministic” is not one Boolean. Qualification records these separately:

1. repeatability in one binary with identical input order;
2. repeatability across worker counts;
3. repeatability across debug/release or compiler options;
4. repeatability across CPU vendors and instruction sets;
5. repeatability across Windows, Linux, and macOS;
6. stable state serialization and restore;
7. rollback/resimulation suitability;
8. deterministic application-side command and event ordering.

Box3D and Jolt both document deterministic modes and constraints, but Spiral must reproduce required levels in its own harness. PhysX explicitly documents limited same-platform determinism and caveats. GPU visual solvers are not assumed deterministic across devices, drivers, or vendors.

Phase 11 establishes the backend/data requirements, hashes, and state APIs. Phase 14 decides which game types actually use authoritative-server, prediction, lockstep, rollback, or hybrid networking and implements the relevant policy.

## CPU Rigid-Body Backend Evaluation

### Initial Candidates

| Candidate | Why evaluate | Current risk |
| --- | --- | --- |
| [Box3D 0.1](https://github.com/erincatto/box3d) | Leading architecture-fit candidate: small MIT/C17 dependency, SIMD/multithreading, CCD, large-world doubles, recording/replay, explicit determinism goals, and real s&box/game use | Alpha maturity, evolving API/docs, limited public history and incomplete character/joint work |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Mature open-source C++ engine, broad platform use, multithreading, characters/vehicles/soft bodies, documented deterministic modes | Integration size, C++ ABI/configuration, cross-platform mode cost and application-order requirements |
| [PhysX](https://github.com/NVIDIA-Omniverse/PhysX) | Mature feature breadth, articulations, tooling, CPU/GPU options, permissive source license | Limited cross-platform determinism, larger integration, CUDA-only GPU features and portability boundaries |
| Havok | Mature commercial option with support and production history | Private SDK evaluation, cost/license/platform terms, non-public evidence |
| [Bullet](https://github.com/bulletphysics/bullet3) | Permissive and broad multi-physics history | No equally clear current cross-platform determinism contract found; integration/maintenance must be measured |

Box3D is the leading lightweight and architectural-fit candidate despite its alpha status. Jolt is the leading mature open-source comparison and fallback candidate. The bake-off is intentionally designed to let Box3D win on simplicity, determinism, integration, and measured behavior without hiding its maturity risk. PhysX/Havok enter the measured shortlist only if their feature/tooling/support advantages justify their integration and portability costs.

### Bake-Off Rules

All candidates receive identical:

- fixed steps, substeps, solver iterations, sleeping and CCD policy;
- collision geometry and cooked scale/material data;
- deterministic command creation order;
- worker-count and floating-point compiler settings;
- test hardware and capture methodology.

Measure correctness and p50/p95/p99 step time, memory, scaling, query latency, event stability, capture/restore cost, and determinism hashes across:

- piles and resting-contact drift;
- large islands, joint chains, ragdolls, vehicles, and constraints;
- dynamic bodies against triangle meshes/heightfields;
- high-speed translation and rotation CCD;
- character slopes, steps, moving platforms, and pushes;
- concurrent batched ray/shape/overlap queries;
- streaming compound insertion/removal and origin shifts;
- Windows/Linux/macOS and x64/ARM64 where supported.

Vendor or third-party benchmark numbers never replace this harness.

## Collision And Simulation Assets

Physics consumes versioned cooked assets rather than render meshes directly:

- primitives and compound shapes;
- convex hulls and deterministic convex decomposition;
- static triangle meshes and heightfields;
- character capsules/proxies and optional SDFs;
- deformable surface/shell meshes and tetrahedral volumes;
- material, density, scale, coordinate-system, collision-layer, and source-hash metadata.

Cooking validates degenerates, non-manifold/open surfaces where disallowed, inverted tetrahedra, extreme scale, unsupported topology, excessive aspect ratio, and budget limits. Cook outputs have a version, backend-neutral semantic hash, and explicit backend-derived cache version. Invalid assets fail diagnostically; the engine does not silently substitute a render mesh.

## GPU Solver And Hardware Policy

- Portable compute through `Engine::RHI` is the baseline GPU interface. CUDA may be used only for an optional NVIDIA research/plugin path with a functional portable fallback.
- GPU deformation stays resident when profitable, but residency does not remove synchronization or bandwidth costs.
- Async compute is scheduled from measured queue overlap. No code assumes dedicated SMs, guaranteed concurrency, or free cache capacity.
- Copy/DMA queues handle eligible transfers only. They do not execute solver math.
- RT collision acceleration remains an optional query prototype. It must beat compute/CPU broadphase and CCD on representative deforming assets after acceleration-structure build/update cost.
- Every GPU path reports queue time, wall-clock critical-path impact, memory, transfer volume, fence waits, publication epoch, and fallback use.
- No same-tick GPU readback may influence gameplay authority until measured latency and deterministic fallback behavior are explicitly accepted.

## Deformation Quality Tiers

| Tier | Intended use | Required fallback |
| --- | --- | --- |
| Proxy/skinning only | Low-end, distant, unsupported, or over-budget assets | Always available |
| Portable PBD/XPBD-class deformation | Ordinary cloth, hair guides, simple soft bodies | Proxy/skinning |
| GPU PD plus qualified barrier/contact | Selected cloth/shell/soft hero assets | Portable solver, then proxy |
| ABD or reduced stiff deformation | Contact-heavy near-rigid hero assets | CPU rigid/articulated proxy |
| Mixed FEM/MiNNIE-like path | Selected near-incompressible hero assets or tools | Reduced/portable deformation or bake |
| Full IPC/GIPC/StiffGIPC-class path | Research, offline tools, lower-frequency or tightly bounded hero islands | PD/portable/proxy path |

Budget degradation is explicit and observable: reduce iterations/resolution/frequency, select a portable solver, then select proxy/skinning. It never silently changes the authoritative gameplay representation.

## Diagnostics And Verification Contract

Required debug views and metrics:

- shapes, AABBs, broadphase pairs, islands, sleeping state;
- contact points/normals/separation, trigger/event ordering, friction/material IDs;
- constraints, impulses, errors, solver iterations/residuals and termination reason;
- CCD sweeps, time of impact, speculative contacts and tunneling failures;
- query batches, epochs, latency, cancellation and stale results;
- fixed-step accumulator, substeps, catch-up drops and interpolation alpha;
- CPU jobs, GPU queues, fences, transfers, memory and renderer contention;
- deformation collision thickness, fallback tier and authority proxy.

Required future fixtures:

- deterministic replay/thread-count/platform hash tests at each claimed level;
- stacks, joints, sleeping/waking, triggers, event order and friction;
- high-speed thin-object CCD and rotating-body CCD;
- pause, single-step, overload/catch-up, save/restore and origin rebasing;
- scene create/update/destroy and late-command behavior;
- collision-cook golden files and invalid-input tests;
- character slopes/steps/platforms and cloth/hair proxy interaction;
- hero-contact comparisons against the portable baseline and offline reference;
- GPU queue/fallback tests under light and saturated renderer workloads.

## Open Decisions

- CPU rigid-body backend after the Box3D/Jolt bake-off.
- Exact fixed tick and overload defaults by project profile.
- Minimum required determinism level for the base engine versus optional game profiles.
- Portable ordinary deformable solver implementation or dependency.
- Whether any GPU solver qualifies beyond visual/secondary authority.
- Which hero path, if any, provides enough visible gain to retain: PD+barrier, ABD, mixed FEM, IPC family, or asset-specific combinations.
- Whether RT-assisted collision beats ordinary broadphase/CCD after build/update and renderer contention costs.

## Primary Sources

- [MiNNIE paper](https://www.tiantianliu.cn/papers/ruan24minnie/ruan24minnie.pdf) and [official implementation](https://github.com/LwRuan/MiNNIE)
- [Incremental Potential Contact](https://ipc-sim.github.io/) and [IPC source](https://github.com/ipc-sim/IPC)
- [Codimensional IPC](https://ipc-sim.github.io/C-IPC/)
- [Penetration-free Projective Dynamics on the GPU](https://www.math.ucla.edu/multiples/publication/lan2022pdipc.pdf)
- [Efficient GPU Cloth Simulation with Non-Distance Barriers and Subspace Reuse](https://wanghmin.github.io/publication/lan-2024-egc/)
- [GIPC](https://arxiv.org/abs/2308.09400) and [StiffGIPC](https://arxiv.org/abs/2411.06224)
- [A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness](https://doi.org/10.1145/3687908) and [PPF solver](https://github.com/st-tech/ppf-contact-solver)
- [Affine Body Dynamics](https://arxiv.org/abs/2201.10022) and [Autodesk implementation](https://github.com/Autodesk/affine-body-dynamics)
- [Hardware-Accelerated Ray Tracing for Discrete and Continuous Collision Detection](https://arxiv.org/abs/2409.09918)
- [Box3D announcement](https://box2d.org/posts/2026/06/announcing-box3d/), [source](https://github.com/erincatto/box3d), [determinism documentation](https://box2d.org/documentation3d/md_simulation.html), and [s&box confirmation of nearly one year of use](https://sbox.game/news/update-26-07-01/)
- [Jolt Physics source and architecture documentation](https://github.com/jrouwe/JoltPhysics)
- [PhysX determinism guidance](https://nvidia-omniverse.github.io/PhysX/physx/5.3.0/docs/BestPractices.html)
- [Vulkan queue priorities](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_global_priority.html)
- [D3D12 command queue priorities](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_command_queue_priority)
- [CUDA L2 access-policy documentation](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)
