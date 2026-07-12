# Engine Roadmap

Status: Living roadmap
Date: 2026-07-12

This roadmap is the working plan for taking the engine from the current buildable shell to a shippable game engine. Deep rationale lives in [Docs/Architecture](Docs/Architecture/README.md); this file is the execution order.

## Checkmark Contract

`[x]` means the exact behavior written on that line is implemented, integrated into its real workflow, and verified. A build-only probe does not complete a runtime feature. Plans, stubs, placeholders, scaffolds, interfaces without behavior, and platform designs without a native implementation stay unchecked. Partial work must be split into an honestly narrow completed line and an unchecked follow-up. See [Docs/ROADMAP_GOVERNANCE.md](Docs/ROADMAP_GOVERNANCE.md).

## North Star

Build a modern, sharp-in-motion, automation-first engine for small teams and ambitious amateurs:

- no required TAA/temporal upscaling for baseline image quality,
- measured-material rendering instead of plastic default PBR,
- visibility-buffer / compact G-buffer renderer with sparse ray residual correction,
- automated asset, LOD, lighting, motion, profiling, and packaging workflows,
- C++ core, C# gameplay, visual graphs as generated code/IR,
- Windows, Linux, and macOS project generation from one repo.

## Current State

Phases 0, 1, and 2 meet their current exit criteria. Phase 3 is in progress.

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
- GitHub Actions CI is live for Windows D3D12 render smoke, Linux X11 Vulkan presentation through Mesa lavapipe/Xvfb, and Linux/macOS portable headless smoke builds.
- D3D12 device creation falls back to WARP when no hardware adapter accepts the minimum feature level, mainly for CI and diagnostics.
- GMake/MinGW keeps OpenGL2 as its default editor fallback, while `--renderer-vulkan` selects the native Vulkan device/swapchain/ImGui path when a Vulkan 1.3 loader and device are available.
- Crash/error reports are written to `output/crashes` for caught top-level exceptions, fatal signals, and Windows unhandled exceptions.
- Code style is checked by `Scripts/CheckCodeStyle.ps1` / `.sh` and a GitHub Actions style job.
- Render graph pass/resource declarations compile into registration-order lifetime and abstract barrier data, but the scaffold is unused by the real renderer and has no focused tests; it is current-state inventory, not a completed roadmap behavior.
- Editor camera and camera component scaffolding provide a shared `CameraView` for renderer code.
- Shader source loading and hot-reload detection are centralized through `ShaderLibrary`.
- GPU timestamp query contracts and renderer timing snapshots are stubbed/no-op; they are current-state inventory, not a completed roadmap behavior, and the D3D12 query heap recording/resolve path remains pending.
- The first D3D12 viewport pass has resource debug names and capture markers for frame, viewport, ImGui, and capture readback scopes.
- The D3D12 viewport prototype mesh creates vertex, index, and constant buffers through the RHI buffer API, uploads immutable vertex/index payloads through a staged copy-queue submission, and records its indexed draw through the D3D12 RHI command-list bridge.
- The D3D12 viewport color and depth targets are allocated through the RHI texture API, with D3D12 descriptors still created by the presentation layer.
- Viewport capture readback now uses the RHI buffer API; texture copy commands and BMP writing remain in the D3D12 presentation layer.
- The viewport prototype shader, graphics pipeline, root constant-buffer binding, vertex/index binding, and indexed draw now run through D3D12 RHI shader/pipeline/command-list APIs; render-target descriptor binding, texture copy commands, and BMP writing remain in the D3D12 presentation/viewport bridge.
- The editor has an engine-owned Vulkan 1.3 device, window surface, FIFO swapchain, native ImGui presentation path, resize handling, and strict render smoke on Windows through both MSVC and MinGW plus Linux X11 through WSLg/Mesa llvmpipe. Scene viewport rendering remains D3D12-only.
- The first macOS backend decision is MoltenVK through the existing NVRHI Vulkan boundary. Hosted macOS 15 Intel CI verifies portability enumeration, NVRHI wrapping on the Apple Paravirtual device, native ImGui presentation, swapchain recreation, and successful post-resize present. The hosted smoke disables MoltenVK Metal argument buffers and `MTLHeap`; Apple Silicon and production scene-renderer qualification remain pending.
- The current render-graph foundation compiles pass/resource declarations in registration order into first/last-use intervals and abstract state transitions. It does not yet resolve dependency order, execute passes, bind physical resources, record RHI barriers, or synchronize queues; the full frame/render graph is now an explicit Phase 3 prerequisite for transient reuse and later multi-pass rendering.
- The editor can serialize the active sample scene to a versioned `.spiral` scene file and reload-validate it through the same scene API.
- Scenes now expose a small entity/component authoring facade with scene-local entity IDs, names, transforms, optional cameras, and save/load coverage.
- The editor scene hierarchy lists actual scene entities rather than hard-coded placeholder rows.
- Scene entities now support transform, camera, light, and mesh renderer components, with the editor default scene authoring a directional light and prototype mesh renderer.
- The scene hierarchy selection now drives the Inspector, and the Inspector edits live transform, camera, light, and mesh renderer component data.
- The asset registry creates stable path/type-based handles, saves and reload-validates a manifest, and assigns registered sample mesh/material handles into the editor scene.
- Asset source watching tracks registered files, warns on deletion, and queues reimport hooks on source changes.
- The editor can import `.gltf`/`.glb` mesh sources through cgltf, register a stable mesh handle, and cook a structural mesh manifest; GPU mesh buffers and material/texture conversion remain follow-on work.
- The KTX2/Basis texture import plan defines texture roles, color-space rules, target profiles, validation, streaming, and the future libktx boundary before any texture transcoder is vendored.
- Material assets are versioned `.spiralmat` files with PBR factors, alpha/shading modes, Callisto controls, and texture handles; editor changes save and reload-validate through the material library.
- GitHub dependency submission now reports vendored/tool dependencies from the dependency ledger so the repo dependency graph can show them.
- A portable `EngineTests` executable covers deterministic engine contracts that are too narrow for editor smoke tests, including job-system failure handling and strict scene deserialization.
- The editor can create an isolated project and starter scene from a name/location modal, and hierarchy controls create or delete entities with undo/redo coverage.

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
- [x] CI workflow runs native Windows, Linux, and macOS build/smoke jobs.
- [x] First hosted CI run after GitHub remote exists.
- [x] Dependency/license ledger for current dependencies and vendor-admission requirements.
- [x] GitHub dependency graph submission for vendored/tool dependencies.
- [x] Basic crash/error reporting path.
- [x] Coding standards checked by script.
- [x] Portable engine contract-test target run on Windows, Linux, and macOS CI.

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
- [x] Renderer backend selector disables unavailable backend choices.
- [x] Renderer service owns D3D12 swapchain resize, presentation command list, viewport texture, descriptor heaps, and debug names.
- [x] Editor viewport displays a renderer-owned D3D12 render target.
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
- [x] Disk-backed shader loading and D3D12 shader compilation pipeline.
- [x] Screenshot capture for render tests.
- [x] Render smoke test scene and image validation script.

Foundation inventory note: the unused render-graph declaration compiler and no-op timestamp/query interfaces are recorded in Current State, not checked here. Their executable, integrated, and verified behaviors are explicit unchecked Phase 3 items.

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
- [x] Material asset format.
- [x] Drag/drop asset browser.
- [x] Save/load project and scene.
- [x] Bounded undo/redo snapshot history for current editor-owned state.
- [x] New scene/project workflow in the editor UI.
- [x] Entity create/delete controls in the scene hierarchy.

Exit criteria:

- User can create a scene, add objects, assign a mesh/material, save, close, reopen.
- Editor UI is not just placeholders.

## Phase 3: Renderer V1

Goal: deliver a conventional-but-clean renderer before the advanced visibility-buffer path.

Required:

- [x] NVRHI D3D12 device integrated behind `Engine::RHI`, with the prototype viewport still using a scoped native D3D12 presentation bridge.
- [x] D3D12 first path on Windows.
- [x] Engine-owned Vulkan 1.3 device, window swapchain, FIFO presentation, and ImGui integration verified on Windows with MSVC and MinGW.
- [x] Native Linux X11 Vulkan editor presentation, resize, and post-resize present verified locally through WSLg with Mesa llvmpipe.
- [x] Hosted Ubuntu Vulkan presentation smoke through Mesa lavapipe and Xvfb.
- [x] Experimental x86_64 macOS editor presentation through MoltenVK and NVRHI Vulkan, including swapchain recreation and successful post-resize present on hosted macOS 15 Intel CI.
- [ ] Renderer capability negotiation and qualification: distinguish advertised/enabled/implemented/exercised features, validate required formats/limits/queues per adapter, expose fallbacks, and record backend/device coverage.
- [ ] Native Apple Silicon project generation, build, and MoltenVK editor-presentation verification.
- [x] D3D12 flip-model swapchain lifecycle and native graphics/compute/copy queues.
- [x] RHI command-list allocation, validated recording lifecycle, and synchronous queue submission.
- [x] GPU buffer resource-upload path with copy-queue synchronization and synchronous fence ownership.
- [ ] Backend-neutral scene-to-renderer extraction into an immutable per-frame render snapshot with mesh/material/light/camera handles and no editor or backend-native types.
- [ ] Shared scene-shader portability path with deterministic DXIL and SPIR-V output, reflected RHI layouts, backend convention validation, caching, and diagnostics; Slang/HLSL-style authoring remains the planned direction.
- [ ] Vulkan `Engine::RHI::Device` resources and command submission for the scene viewport, using the wrapped `nvrhi::DeviceHandle`; keep raw Vulkan confined to bootstrap, WSI/presentation, and ImGui.
- [ ] Frame/render graph construction: pass registration with declared resource reads/writes, automatic resource lifetime tracking, dependency resolution and pass ordering, and barrier/queue-transition insertion derived from the graph.
- [ ] Frame/render graph execution and real-workflow integration: bind imported/physical resources, invoke pass callbacks, record RHI barriers and commands, submit queue dependencies, retire frame contexts by GPU completion, and drive a representative multi-pass scene viewport.
- [ ] Transient resource allocation and reuse from render-graph lifetimes.
- [ ] Presentation pacing and measurement: DXGI waitable swapchain profiles, capability-gated Vulkan present timing, and separate app/present/display telemetry.
- [x] HLSL shader compilation pipeline through the D3D12 RHI; Slang remains a future portability option.
- [ ] Live D3D12 pipeline rebuild after shader source changes.
- [ ] D3D12 timestamp query heap recording and resolve.
- [ ] Scene mesh/index/constant/structured-buffer integration beyond the prototype draw, populated from the render snapshot through `Engine::RHI`.
- [ ] Texture upload, samplers, mip generation.
- [ ] Descriptor/sampler and read-only bindless table model with declared capacities, error resources, GPU-retired updates, writable-resource rules, and a capability-gated bounded fallback.
- [ ] Forward+/clustered light grid prototype.
- [ ] Basic PBR shading with material IDs.
- [ ] Directional, point, and spot lights.
- [ ] Shadow map prototype.
- [ ] Basic sky/atmosphere pass producing a visible sky and the lighting inputs required by the Phase 3 scene.
- [ ] Debug draw and overlays.
- [ ] Render-graph and scene-pass capture labels readable in RenderDoc/PIX/Nsight on every backend claimed by the item.
- [ ] Production macOS renderer qualification after the shared Vulkan scene and render-graph paths exist: validate representative resources, commands, shaders, captures, packaging, profiling, and fallbacks through MoltenVK/NVRHI, or implement native Metal where measured gaps justify it.

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
- [ ] Versioned engine-native mesh cluster/page format with dependency metadata, integrity validation, and deterministic cook outputs.
- [ ] Coarse resident fallback pages.
- [ ] Asynchronous mesh-page residency system with feedback, upload, eviction, GPU-safe retirement, and nearest-resident fallback; render threads never block on storage/decompression.
- [ ] Runtime GPU culling and LOD selection.
- [ ] Stable ordered/complementary LOD transitions.
- [ ] Asset-class policies: static scans, foliage, skinned meshes, hair, wires, debris.
- [ ] DGF/DGFS evaluation path.
- [ ] RTX Mega Geometry/CLAS capability-gated evaluation with a representative benchmark and a recorded adopt/defer/reject decision; ordinary meshlet data remains the fallback.

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
- [ ] Versioned baked-lighting/probe data format plus a single-time preview/final bake or validated import path before time-keyed variants.
- [ ] Adaptive time-of-day keyframe baker.
- [ ] Reflection probe integration.
- [ ] Measured idTech 8 / Neural Light Grid-inspired GI experiment with explicit baseline comparison, debug validity, and a recorded keep/defer/reject decision.

Exit criteria:

- Dynamic characters do not pop out of baked environments.
- Day/night indirect lighting blends by measured lighting error, not equal time slices.

## Phase 9: Sparse Ray Residual Rendering

Goal: implement the signature renderer idea: stable raster base plus sparse current-frame ray correction.

Required:

- [ ] `Engine::RHI` ray-tracing capability and resource contracts: acceleration structures, build/update/compaction, ray pipeline/shader-table binding, synchronization, diagnostics, and stable raster/probe fallback when unavailable.
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
- [ ] Runtime visual-graph compiler foundations: animation integration builds on Phase 10's animation runtime, while material graph output builds on the Phase 3 shader path and Phase 5 material model.

Exit criteria:

- User can write C# gameplay scripts and hot reload them.
- Beginner graph workflows compile to inspectable code/IR.

## Phase 13: Automation And Guided Workflows

Goal: turn advanced engine systems into approachable workflows.

Required:

- [ ] Guided project-template wizard extending Phase 2's basic project creation with game-type choices, explainable generated systems, validation, and reversible changes.
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

- [ ] Packaged Player build and clean-machine launch verification on Windows, Linux, and macOS; Phase 0 editor/sandbox CI is foundation evidence only.
- [ ] Platform abstraction audit.
- [ ] Asset cooker and package format.
- [ ] Runtime player executable.
- [ ] Project templates.
- [ ] Installer/export pipeline.
- [ ] Production crash reporting and logs: packaged Player coverage, symbols/build identity, actionable reports, privacy/retention policy, and platform-appropriate collection; Phase 0 local crash files are the foundation only.
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
