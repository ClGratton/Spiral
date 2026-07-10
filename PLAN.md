# Engine Roadmap

Status: Living roadmap
Date: 2026-07-06

This roadmap is the working plan for taking the engine from the current buildable shell to a shippable game engine. Deep rationale lives in [Docs/Architecture](Docs/Architecture/README.md); this file is the execution order.

## North Star

Build a modern, sharp-in-motion, automation-first engine for small teams and ambitious amateurs:

- no required TAA/temporal upscaling for baseline image quality,
- measured-material rendering instead of plastic default PBR,
- visibility-buffer / compact G-buffer renderer with sparse ray residual correction,
- automated asset, LOD, lighting, motion, profiling, and packaging workflows,
- C++ core, C# gameplay, visual graphs as generated code/IR,
- Windows, Linux, and macOS project generation from one repo.

## Current State

Phase 0 is partially complete.

Already present:

- Premake workspace and build scripts.
- Vendored Premake bootstrap.
- Vendored GLFW window/input backend.
- Vendored Dear ImGui docking editor shell.
- `Engine` static library.
- `Editor` executable with dockable panels, renderer backend selector, and viewport prototype mesh.
- `Sandbox` executable.
- Engine-owned entry point and client `CreateApplication` hook.
- Layer stack and event dispatch.
- Basic logging, assertions, timestep, headless smoke-test path.
- NVRHI renderer boundary, RHI, scene, asset, automation, and job system modules.
- Architecture documents organized under `Docs/Architecture`.
- Dependency/license ledger and pinned NVRHI fetch script.
- Vendored NVRHI common, validation, and MSVC-gated D3D12 backend sources linked through Premake.
- Vendored Vulkan-Headers and DirectX-Headers pinned to the versions expected by the chosen NVRHI commit.
- NVRHI Vulkan backend sources are enabled in the vendor build when the pinned Vulkan headers are present.

Immediate gap:

- The editor has a GUI shell and a D3D12-backed viewport texture with a native indexed prototype mesh on Windows/MSVC, but it does not render actual scene entities yet.
- The renderer now owns a native NVRHI D3D12 device, window swapchain, presentation command list, viewport texture, descriptor heaps, and ImGui DX12 path on Windows/MSVC.
- The viewport prototype mesh uses a disk-backed HLSL shader asset loaded from `Engine/Shaders`, not an embedded shader string.
- The Windows/MSVC editor can capture the native viewport to `output/captures/editor-viewport.bmp` with `--capture-viewport`.
- `Scripts/TestRender.ps1` validates the D3D12 viewport capture as a non-blank BMP render smoke test.
- GitHub Actions CI workflow is live for Windows D3D12 render smoke plus Linux/macOS portable headless smoke builds.
- D3D12 device creation falls back to WARP when no hardware adapter accepts the minimum feature level, mainly for CI and diagnostics.
- GMake/MinGW keeps the OpenGL2 ImGui fallback and NVRHI common probe path until a portable Vulkan editor path is added, even though the NVRHI Vulkan vendor backend now compiles.
- Crash/error reports are written to `output/crashes` for caught top-level exceptions, fatal signals, and Windows unhandled exceptions.
- Code style is checked by `Scripts/CheckCodeStyle.ps1` / `.sh` and a GitHub Actions style job.
- Render graph pass/resource declarations now compile into resource lifetime and barrier data.
- Editor camera and camera component scaffolding provide a shared `CameraView` for renderer code.
- Shader source loading and hot-reload detection are centralized through `ShaderLibrary`.
- GPU timestamp query contracts and renderer timing snapshots are stubbed; the D3D12 query heap resolve path is still pending.
- The first D3D12 viewport pass has resource debug names and capture markers for frame, viewport, ImGui, and capture readback scopes.
- The D3D12 viewport prototype mesh creates vertex, index, and constant buffers through the RHI buffer API and records its indexed draw through the D3D12 RHI command-list bridge.
- The D3D12 viewport color and depth targets are allocated through the RHI texture API, with D3D12 descriptors still created by the presentation layer.
- Viewport capture readback now uses the RHI buffer API; texture copy commands and BMP writing remain in the D3D12 presentation layer.
- The viewport prototype shader, graphics pipeline, root constant-buffer binding, vertex/index binding, and indexed draw now run through D3D12 RHI shader/pipeline/command-list APIs; render-target descriptor binding, texture copy commands, and BMP writing remain in the D3D12 presentation/viewport bridge.
- The NVRHI Vulkan vendor backend compiles against the pinned Vulkan-Headers, but engine-owned Vulkan device, swapchain, presentation, and ImGui integration are not implemented yet.
- The editor can serialize the active sample scene to a versioned `.spiral` scene file and reload-validate it through the same scene API.
- Scenes now expose a small entity/component authoring facade with scene-local entity IDs, names, transforms, optional cameras, and save/load coverage.
- The editor scene hierarchy lists actual scene entities rather than hard-coded placeholder rows.
- Scene entities now support transform, camera, light, and mesh renderer components, with the editor default scene authoring a directional light and prototype mesh renderer.
- The scene hierarchy selection now drives the Inspector, and the Inspector edits live transform, camera, light, and mesh renderer component data.
- The asset registry creates stable path/type-based handles, saves and reload-validates a manifest, and assigns registered sample mesh/material handles into the editor scene.
- Asset source watching tracks registered files, warns on deletion, and queues reimport hooks on source changes.
- The editor can import `.gltf`/`.glb` mesh sources through cgltf, register a stable mesh handle, and cook a structural mesh manifest; GPU mesh buffers and material/texture conversion remain follow-on work.
- GitHub dependency submission now reports vendored/tool dependencies from the dependency ledger so the repo dependency graph can show them.

## Phase 0: Buildable Spine

Goal: make the repo cloneable, buildable, runnable, and navigable.

Required:

- [x] Root Premake workspace.
- [x] Engine/editor/sandbox split.
- [x] Setup/generate/build/run scripts.
- [x] GLFW native window backend.
- [x] ImGui docking editor shell.
- [x] Headless smoke-test mode.
- [x] Architecture docs moved out of root.
- [x] GitHub repo, remote, first clean commit.
- [x] CI workflow scaffold for Windows, Linux, and macOS.
- [x] First hosted CI run after GitHub remote exists.
- [x] Dependency/license ledger for GLFW, ImGui, Premake, and future vendors.
- [x] GitHub dependency graph submission for vendored/tool dependencies.
- [x] Basic crash/error reporting path.
- [x] Coding standards checked by script.

Exit criteria:

- Fresh clone can build editor and sandbox with one command.
- Editor opens to a dockable shell.
- Headless smoke tests pass in CI.
- No user needs to manually hunt for libraries.

## Phase 1: Basic Rendering Foundation

Goal: replace the renderer stub with the first real render path while preserving the future NVRHI/RHI boundary.

Required:

- [x] `Engine::RHI` interface: device, swapchain, command list, buffer, texture, shader, pipeline, query.
- [x] NVRHI common core and validation library vendored, built, linked, and probed from renderer startup.
- [x] Temporary backend decision:
  - short term: keep OpenGL only for ImGui/editor UI if needed,
  - production path: NVRHI-backed D3D12/Vulkan renderer.
- [x] Renderer backend selector with disabled unsupported/pending choices.
- [x] Renderer service owns D3D12 swapchain resize, presentation command list, viewport texture, descriptor heaps, and debug names.
- [x] Editor viewport displays a renderer-owned D3D12 render target, not only an ImGui placeholder rectangle.
- [x] Native D3D12 indexed prototype mesh pass with vertex, index, constant, and depth buffers.
- [x] Disk-backed HLSL shader asset for the D3D12 viewport prototype pass.
- [x] Renderer-owned OpenGL bootstrap backend removed.
- [x] DirectX-Headers and Vulkan-Headers pinned for NVRHI backend builds.
- [x] NVRHI D3D12 backend project enabled for Windows/MSVC.
- [x] NVRHI D3D12 native device, graphics/compute/copy queues, validation layer, and capability probe.
- [x] ImGui DX12 backend and `GLFW_NO_API` window path for the Windows/MSVC editor.
- [x] NVRHI Vulkan backend project enabled.
- [x] Real RHI triangle/mesh draw pass.
- [x] Camera component and editor camera.
- [x] Basic shader pipeline and shader hot reload stub.
- [x] Render graph skeleton: pass declaration, resource lifetimes, barriers as data even if backend is simple.
- [x] GPU timestamp query interface stub.
- [x] Screenshot capture for render tests.
- [x] Render smoke test scene and image validation script.

Exit criteria:

- Viewport panel shows a real rendered scene.
- Window resize does not break rendering.
- Headless smoke path still works.
- Renderer code depends on `Engine::RHI`, not editor internals.

## Phase 2: Scene, Assets, And Editor Usability

Goal: make the editor manipulate actual scene data and load simple assets.

Required:

- [x] Scene serialization format.
- [x] Entity/component authoring facade.
- [x] Transform, camera, light, mesh renderer components.
- [x] Scene hierarchy bound to actual entities.
- [x] Inspector edits live component data.
- [x] Asset registry with stable handles.
- [x] File watching and reimport hooks.
- [x] glTF import prototype.
- [ ] KTX2/Basis texture import plan.
- [ ] Material asset format.
- [ ] Drag/drop asset browser.
- [ ] Save/load project and scene.
- [ ] Undo/redo command stack.

Exit criteria:

- User can create a scene, add objects, assign a mesh/material, save, close, reopen.
- Editor UI is not just placeholders.

## Phase 3: Renderer V1

Goal: deliver a conventional-but-clean renderer before the advanced visibility-buffer path.

Required:

- [ ] NVRHI backend integrated behind `Engine::RHI`.
- [ ] D3D12 first path on Windows, Vulkan path kept in design.
- [ ] Swapchain, command queues, resource upload, transient resources.
- [ ] Slang or HLSL shader compilation pipeline.
- [ ] Mesh buffers, index buffers, constant/structured buffers.
- [ ] Texture upload, samplers, mip generation.
- [ ] Forward+/clustered light grid prototype.
- [ ] Basic PBR shading with material IDs.
- [ ] Directional, point, and spot lights.
- [ ] Shadow map prototype.
- [ ] Sky/atmosphere placeholder.
- [ ] Debug draw and overlays.
- [ ] RenderDoc/PIX/Nsight capture labels.

Exit criteria:

- Simple scenes render with mesh materials, camera, lights, and shadows.
- GPU captures are readable.
- Editor viewport is backed by the renderer, not special editor drawing.

## Phase 4: Non-Temporal Image Quality Baseline

Goal: establish the engine's motion-clarity promise before advanced ray features.

Required:

- [ ] MSAA or analytic/fractional coverage strategy for edges.
- [ ] Alpha-to-coverage path for masked materials.
- [ ] SMAA/CMAA-style spatial cleanup.
- [ ] Specular antialiasing and roughness remapping.
- [ ] Correct mip selection and anisotropic filtering.
- [ ] Normal-map filtering.
- [ ] Stable LOD transition manager.
- [ ] Motion clarity test scenes.
- [ ] Native-resolution validation captures.

Exit criteria:

- Camera pans preserve texture and geometry detail without TAA.
- Foliage/thin geometry tests have acceptable current-frame clarity.

## Phase 5: Callisto/Proxima Material System

Goal: make "not plastic" material behavior a default engine strength.

Required:

- [ ] Callisto/Proxima/GGX BRDF implementation.
- [ ] Diffuse Fresnel, retroreflection, smooth terminator controls.
- [ ] Specular Fresnel falloff controls.
- [ ] Optional dual specular lobe.
- [ ] Material class system: default, skin, eye, hair, cloth, foliage, glass, emissive.
- [ ] Compact BRDF parameter buffers.
- [ ] Material calibration scene.
- [ ] OpenPBR/MaterialX import mapping.
- [ ] Tone mapper comparison: GT-style, AgX, ACES/filmic, Khronos PBR Neutral.

Exit criteria:

- Materials are validated in controlled lighting and compared against references.
- Lambert exists only as debug/low-end fallback.

## Phase 6: Visibility Buffer And Compact G-Buffer

Goal: move opaque rendering to the intended architecture.

Required:

- [ ] Visibility buffer pass with `R32_UINT` ID.
- [ ] `drawClusterId:25 | localTriangleId:7` decode.
- [ ] `DrawClusterBuffer` and material table.
- [ ] Material resolve worklists sized for worst case.
- [ ] Compact G-buffer outputs.
- [ ] Selected occluder prepass: depth/coverage/visibility only.
- [ ] Two-pass HZB culling.
- [ ] Coverage-aware carve-outs for foliage/hair/masked materials.
- [ ] Debug views: visibility ID, material ID, overdraw, quad waste.

Exit criteria:

- Opaque material evaluation happens once per visible pixel/sample.
- Visibility and material IDs are distinct and debuggable.

## Phase 7: Virtual Geometry And Asset Cooking

Goal: automate high-detail geometry without inheriting subpixel instability.

Required:

- [ ] Meshlet/cluster builder.
- [ ] Vertex/index optimization and quantization.
- [ ] Projected-area/quad-utilization topology scoring.
- [ ] Curvature/silhouette-aware simplification.
- [ ] Coarse resident fallback pages.
- [ ] Runtime GPU culling and LOD selection.
- [ ] Stable ordered/complementary LOD transitions.
- [ ] Asset-class policies: static scans, foliage, skinned meshes, hair, wires, debris.
- [ ] DGF/DGFS evaluation path.
- [ ] RTX Mega Geometry/CLAS optional backend notes.

Exit criteria:

- Dense scanned meshes stream and render with stable LODs.
- Importer prevents pathological subpixel/skinny-triangle workloads by default.

## Phase 8: Probe Lighting, Lightmaps, And Daylight

Goal: create a stable indirect-lighting backbone for static and dynamic objects.

Required:

- [ ] Photometric light units and exposure debug.
- [ ] Basic sky/sun model.
- [ ] Adaptive probe volume.
- [ ] Unified `IndirectLightingSample`.
- [ ] Static/dynamic same-pass indirect lighting.
- [ ] Probe leak/confidence debug views.
- [ ] Portal/zone GI blending.
- [ ] Directional lightmap support.
- [ ] Adaptive time-of-day keyframe baker.
- [ ] Reflection probe integration.
- [ ] idTech 8 / Neural Light Grid-inspired research prototype.

Exit criteria:

- Dynamic characters do not pop out of baked environments.
- Day/night indirect lighting blends by measured lighting error, not equal time slices.

## Phase 9: Sparse Ray Residual Rendering

Goal: implement the signature renderer idea: stable raster base plus sparse current-frame ray correction.

Required:

- [ ] BLAS/TLAS object-class update policy.
- [ ] Ray-budget classifier.
- [ ] Sparse RT shadow residuals.
- [ ] Sparse RT reflection miss fill.
- [ ] Sparse RT AO/GI residuals.
- [ ] Same-frame spatial reconstruction guided by depth, normal, albedo, roughness, material ID, instance ID.
- [ ] Densification near discontinuities and hero materials.
- [ ] Mirror/eye/hair/thin highlight special-case routing.
- [ ] Debug views: ray density, residual magnitude, confidence, fallback source.

Exit criteria:

- Ray tracing improves the raster base without becoming a noisy temporal path tracer.
- Current-frame result is acceptable without temporal denoising.

## Phase 10: Animation, Motion Matching, And Characters

Goal: provide modern character motion without requiring every small team to build AAA animation tech from scratch.

Required:

- [ ] Skeleton, clips, blend trees, animation graph.
- [ ] Retargeting pipeline.
- [ ] Motion matching database builder.
- [ ] Starter motion-matched packs.
- [ ] Trajectory prediction and feature extraction.
- [ ] Foot locking, inertialization, warping.
- [ ] Character controller templates.
- [ ] Facial/eye/skin rendering hooks.
- [ ] Cloth/hair simulation hooks.

Exit criteria:

- A user can import a humanoid, choose a starter locomotion pack, and get usable movement.

## Phase 11: Physics And Interaction

Goal: make contact/clipping quality a visible engine feature.

Required:

- [ ] General rigid-body physics integration.
- [ ] Continuous collision detection for fast movers.
- [ ] Character collision bodies.
- [ ] SDF/proxy collision workflow for characters.
- [ ] Cloth/hair collision hooks.
- [ ] ABD/IPC hero-contact prototypes.
- [ ] Physics debug draw and profiler.
- [ ] Async scene-query interface.

Exit criteria:

- Gameplay physics is stable.
- Hero contact/clipping cases have a higher-quality path.

## Phase 12: Scripting, Visual Graphs, And Runtime Data

Goal: make the engine programmable by beginners and scalable for experts.

Required:

- [ ] C#/.NET host through `hostfxr`.
- [ ] Script assembly compile/load/reload.
- [ ] Generated native bindings.
- [ ] Component lifecycle: `OnStart`, `OnUpdate`, events.
- [ ] Script diagnostics and error surfaces.
- [ ] DOTS-like archetype/chunk ECS runtime.
- [ ] C# restricted jobs.
- [ ] Command buffers for structural changes.
- [ ] Visual gameplay graph compiling to C#/IR.
- [ ] Animation graph and material graph foundations.

Exit criteria:

- User can write C# gameplay scripts and hot reload them.
- Beginner graph workflows compile to inspectable code/IR.

## Phase 13: Automation And Guided Workflows

Goal: turn advanced engine systems into approachable workflows.

Required:

- [ ] New project wizard.
- [ ] First playable workflow.
- [ ] Visual style workflow.
- [ ] Asset import workflow.
- [ ] Lighting bake/probe workflow.
- [ ] Motion pack workflow.
- [ ] Performance workflow.
- [ ] Packaging workflow.
- [ ] AI-agent tool bridge with explainable actions.
- [ ] Undoable generated changes.
- [ ] Validation checklist per workflow.

Exit criteria:

- A beginner can create a playable prototype without manually discovering every system.
- Experts can inspect and override all automation.

## Phase 14: Audio, UI, Save, Networking, And Game Systems

Goal: cover the non-rendering systems needed to ship real games.

Required:

- [ ] Audio playback, spatialization, buses, snapshots.
- [ ] Runtime UI system.
- [ ] Input action mapping.
- [ ] Save/load framework.
- [ ] Localization.
- [ ] Gameplay tags/events.
- [ ] Navigation/pathfinding.
- [ ] AI behavior tools.
- [ ] Networking/replication plan.
- [ ] Determinism/rollback evaluation for game types that need it.

Exit criteria:

- Engine can support complete small games, not only rendering demos.

## Phase 15: Tooling, Profiling, And Validation

Goal: make performance and correctness visible.

Required:

- [ ] CPU profiler lanes and job timing.
- [ ] GPU pass timing.
- [ ] Memory profiler.
- [ ] Asset size and texture residency views.
- [ ] Overdraw and quad-waste views.
- [ ] Draw/dispatch/material-bin counters.
- [ ] Shadow caster cost view.
- [ ] Ray density/confidence views.
- [ ] External capture docs for Intel GPA, Nsight, RGP/RMV/GPU Detective, PIX, RenderDoc.
- [ ] Golden image tests.
- [ ] Automated render scene suite.

Exit criteria:

- Performance claims require in-engine profiler data plus at least one external capture.
- Regression tests catch rendering and asset-pipeline breakage.

## Phase 16: Platform, Packaging, And Distribution

Goal: ship games reliably.

Required:

- [ ] Windows, Linux, macOS build verification.
- [ ] Platform abstraction audit.
- [ ] Asset cooker and package format.
- [ ] Runtime player executable.
- [ ] Project templates.
- [ ] Installer/export pipeline.
- [ ] Crash reporting and logs.
- [ ] Settings/scalability system.
- [ ] Optional DLSS/XeSS/FSR integrations as scalability features.
- [ ] Steam/storefront demo packaging.

Exit criteria:

- A project can be exported as a runnable game build.
- Optional accelerators improve performance but are not required for correctness.

## Phase 17: Shipping Readiness

Goal: turn the engine from a tech project into a reliable product.

Required:

- [ ] Full documentation pass.
- [ ] Beginner tutorials.
- [ ] Sample projects.
- [ ] API stability review.
- [ ] License/dependency audit.
- [ ] Security review for scripts/plugins/assets.
- [ ] Bug triage and performance budget closure.
- [ ] Upgrade/migration path.
- [ ] Marketplace/asset package policy.
- [ ] Public roadmap and contribution guide.

Exit criteria:

- Small teams can start and finish games in the engine.
- The engine can be maintained without relying on undocumented tribal knowledge.

## Rendering Start Checklist

Before adding renderer code beyond stubs:

- [x] Keep ImGui/OpenGL as editor UI only.
- [x] Do not let OpenGL types leak into public `Renderer` or `Engine::RHI` headers.
- [x] Define the first RHI device/swapchain interface.
- [x] Keep headless smoke tests green.
- [ ] Keep editor panels useful even when renderer initialization fails.
- [x] Add debug names and capture markers from the first GPU pass.

## Near-Term Next Tasks

1. [x] Add a real renderer-owned clear path and editable clear color.
2. [x] Add `Engine::RHI` device/swapchain interface files.
3. [x] Add dependency/license ledger and pinned NVRHI fetch script.
4. [x] Integrate NVRHI common core as vendored source with license notes.
5. [x] Render a visible indexed prototype mesh into the editor viewport.
6. [x] Pin DirectX-Headers and Vulkan-Headers for NVRHI backend builds.
7. [x] Add renderer backend selector and remove the renderer-owned OpenGL bootstrap backend.
8. [x] Enable first NVRHI backend and create a renderer-owned viewport texture.
9. [x] Replace ImGui OpenGL backend with the active native graphics backend on Windows/MSVC.
10. [x] Move the viewport prototype shader out of embedded C++ and into `Engine/Shaders`.
11. [x] Add screenshot capture for the viewport.
12. [x] Add a render smoke test scene.
13. [x] Add CI workflow scaffold.
14. [x] Create/push GitHub remote and verify first hosted CI run.
