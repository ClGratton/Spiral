# Engine Roadmap

Status: Living roadmap
Date: 2026-07-15

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
- NVRHI renderer boundary plus RHI, scene, asset, and job-system modules.
- Architecture documents organized under `Docs/Architecture`.
- Dependency/license ledger and pinned NVRHI fetch script.
- Vendored NVRHI common, validation, and MSVC-gated D3D12 backend sources linked through Premake.
- Vendored Vulkan-Headers and DirectX-Headers pinned to the versions expected by the chosen NVRHI commit.
- NVRHI Vulkan backend sources are enabled in the vendor build when the pinned Vulkan headers are present.

Immediate gap:

- The editor has a GUI shell and a D3D12-backed viewport texture on Windows/MSVC. Each visible Scene mesh snapshot record now issues one draw using the current built-in indexed prototype geometry; real mesh/material GPU resources remain pending.
- The renderer now owns a native NVRHI D3D12 device, window swapchain, presentation command list, viewport texture, descriptor heaps, and ImGui DX12 path on Windows/MSVC.
- Vulkan bootstrap owns the native instance/device/queue/surface and wraps the resulting `nvrhi::DeviceHandle`; its `Engine::RHI` wrapper creates no second `VkDevice`. The exercised core supplies buffers, RGBA8/depth textures, output clear/transitions, one-shot graphics submission, deterministic updates, staging readback, markers, and synchronous retirement. The renderer consumes the published immutable `SceneRenderSnapshot`, prepares its existing camera-relative raster frame, compiles the stable `EditorViewport.hlsl` SPIR-V package, and records output bind/clear/pipeline/indexed draws only through `Engine::RHI`/NVRHI into renderer-owned RGBA8/depth outputs. Its final color is shader-resource state/layout and exposes only a borrowed NVRHI Vulkan image/view to the existing native ImGui/presentation bridge. The bridge owns descriptor/sampler registration/removal and waits for GPU retirement before resize replacement or shutdown; the Editor viewport queues exactly that descriptor. `VulkanSceneOutputCaptureV1` proves post-resize renderer-output content and `VulkanSceneOutputHandoffV1` proves descriptor registration, ImGui queueing, and successful post-resize swapchain consumption. Local Windows Vulkan evidence plus exact-head CI run `29369950355` passed Ubuntu lavapipe and macOS Intel Apple-Paravirtual/MoltenVK (three complete launches); the hosted Windows job preserves the D3D12 regression.
- The viewport prototype mesh uses a disk-backed Slang/HLSL-style shader asset loaded from `Engine/Shaders`, not an embedded shader string. It now provides a directly visible portable-pipeline demo using stable per-face UVs, procedural checker shading, and antialiased luminous face frames/inset lines/corner accents on the prototype cube without pretending that sampled-texture/material bindings exist. The framing remains within the existing constant-buffer-only contract and uses no texture or sampler. The original object-position checker was rejected because quantizing a face's constant coordinate exactly at a `floor` boundary produced triangle-dependent precision striping when the cube was rotated. Windows x86_64/MSVC produces one validated package per stage containing paired DXIL and SPIR-V through pinned Slang v2026.13.1 and its pinned DXC v1.9.2602 downstream compiler; the D3D12 viewport consumes DXIL while SPIR-V is also consumed by the isolated Vulkan indexed-draw smoke. Linux/macOS produce and validate SPIR-V-only packages through their staged Slang runtime; requests for DXIL on those hosts fail explicitly because a non-Windows DXC path is not admitted. Exact-head run `29363501290` passed the corrected UV input/reflection and checker on Windows D3D12, Ubuntu Vulkan, and macOS MoltenVK; the later luminous-frame refinement passed local D3D12 capture and Vulkan SPIR-V indexed-draw/readback evidence.
- Hosted Actions runs `29350139365` and `29352633796` exposed and isolated three portable-host defects: missing executable-relative Slang lookup on Linux, unsupported libc++ `std::atomic<std::shared_ptr<const T>>`, and an invalid assumption that non-Windows hosts had an admitted DXC downstream compiler. Generated Linux executables now carry `RUNPATH=$ORIGIN`, generated macOS executables carry `LC_RPATH @loader_path`, renderer snapshot publication uses the atomic specialization only where advertised and otherwise preserves release/acquire through the standard atomic `shared_ptr` free functions, and host shader targets are explicit. Final-head run `29354068102` passed all Windows, Ubuntu, and macOS jobs, including 35/35 `EngineTests`, real SPIR-V compilation/reflection/cache validation on both portable hosts, Linux Vulkan presentation, and macOS MoltenVK presentation. This qualifies the repaired portable build/test paths, not Vulkan scene rendering or physical-device breadth.
- The Windows/MSVC editor can capture the native viewport to `output/captures/editor-viewport.bmp` with `--capture-viewport`.
- `Scripts/TestRender.ps1` validates two successful structured portable-shader terminal packages, the active D3D12 Slang/DXIL pipeline and its exact compiler/targets/cache keys/reflection/conventions, a non-blank D3D12 viewport capture, and three comparative high-magnitude Scene-origin captures: equivalent pre/post-origin epochs must match, while mesh-only movement must change the image in the expected direction. Pending-only, failed, cancelled, unknown, or legacy source-compile evidence is rejected.
- GitHub Actions CI is live for Windows D3D12 render smoke, Linux X11 Vulkan presentation through Mesa lavapipe/Xvfb, and Linux/macOS portable headless smoke builds.
- D3D12 device creation falls back to WARP when no hardware adapter accepts the minimum feature level, mainly for CI and diagnostics.
- GMake/MinGW keeps OpenGL2 as its default editor fallback, while `--renderer-vulkan` selects the native Vulkan device/swapchain/ImGui path when a Vulkan 1.3 loader and device are available.
- Crash/error reports are written to `output/crashes` for caught top-level exceptions, fatal signals, and Windows unhandled exceptions.
- Code style is checked by `Scripts/CheckCodeStyle.ps1` / `.sh` and a GitHub Actions style job.
- Render graph construction validates named pass/resource declarations, explicit read/write/read-write state and stage intent, and optional explicit ordering constraints. It derives RAW/WAR/WAW dependency edges, rejects invalid uses and cycles, emits a stable topological pass order, computes logical lifetimes in that resolved order, and produces abstract state-barrier plus dependent cross-queue transition records. Focused `EngineTests` cover this compiler-only contract. The execution core now binds resources, executes callbacks, emits RHI barriers/submissions, and drives the current D3D12/Vulkan Scene viewport through named Clear, Raster, and Output Handoff passes; imported resource shape matching deliberately excludes current state because the executor separately validates the authoritative observed state. Stack-owned per-frame viewport graphs wait for their final graphics completion before context destruction. Smoke-only separate renderer-owned outputs retain the former direct recorder as an exact-byte reference oracle; it never publishes to the viewport. Unbound compatible transient resources now use the selected non-aliased pool with lifetime-based same-queue reuse and exact-token retirement.
- `RHI::CommandList` now records explicit whole-buffer transitions using the same backend-neutral `ResourceState` vocabulary as textures. The buffer description validates GPU-only compatible read-only (`ShaderResource`), write (`UnorderedAccess`), and copy states before recording; CPU-visible upload/readback buffers and texture-only states are rejected. D3D12 records and tracks native whole-resource barriers, treating vertex/index/constant reads as `GENERIC_READ`; Vulkan records NVRHI `setBufferState`/`commitBarriers` and suppresses tracked no-op repeats. The `RHIBufferTransitionSmokeV1` path rejects transitions outside recording, invalid state/usage, and transitions after close, then submits real CopyDest-to-CopySource work on both local Windows D3D12 and Vulkan/NVRHI. It is a graph-execution prerequisite only: no graph callback, binding, queue, or transient work is implemented here.
- `RHI::Device::QueryResourceState` observes only a live exact-owner texture or buffer wrapper's committed portable `ResourceState`; it rejects null, foreign-backend, same-backend-other-device, and `Unknown` values without exposing native state or accepting caller adoption. Transitions remain pending in a recording command list so the query reports the last successfully accepted state until `Submit` returns a valid token; failed recording, close, or submission cannot publish a new state. D3D12 commits its portable/native wrapper state together; Vulkan/NVRHI commits its wrapper state after list acceptance. `RHIResourceStateSmokeV1` proves CopyDest initial observation, invisible pending transitions and invalid-recording rollback, successful CopySource texture/buffer observation, and null/unknown rejection on real local D3D12 and Vulkan/NVRHI. The deterministic `EngineTests` contract covers foreign-backend and same-backend-other-device rejection; headed one-device smokes do not overclaim two-real-device evidence. No state adoption, graph execution, cross-queue scheduling, transient allocation, or viewport adoption is added.
- `RHI::Device` now has a completion-token boundary for graph-owned recording reuse: `Submit` accepts closed work and returns an opaque device/submission token without waiting; `QueryCompletion` is nonblocking; `WaitForCompletion` accepts an explicit finite deadline; and `SubmitAndWait` composes those primitives for existing bootstrap consumers. D3D12 stores each token's queue-local graphics/copy/compute fence value; Vulkan retains NVRHI's graphics submission instance and observes its existing timeline counter. A submitted owned command list records its last token and refuses `Begin` until that exact token completes. Zero, cross-device, and unissued token identities are rejected. `RHICompletionSmokeV1` uses a real graphics submission, accepts a legitimately fast initial completion while proving nonblocking query, then waits and reuses the same recording object. Local Windows/MSVC Debug `EngineTests` (40/40), `TestRender.ps1`, `TestVulkan.ps1`, and code style passed; exact-head CI run `29374369710` passed Windows D3D12, Ubuntu lavapipe Vulkan, macOS Intel MoltenVK, and code style. It does not implement render-graph execution, cross-queue graph scheduling, transient allocation, or viewport adoption.
- `RHI::Device` now resolves each requested Graphics/Compute/Copy class to a deterministic effective queue and independent/fallback identity, and dependency-aware `Submit` accepts only already-issued smaller completion tokens from the same device. Validation rejects zero, foreign, absent, duplicate, prospective-self, and forward identities before native submission or staged-state publication; the prior-only model makes cycles unrepresentable. D3D12 independently enabled queues use GPU queue fence waits. Vulkan now selects and creates concrete Compute/Copy handles when available, submits and retires work on their effective NVRHI queues, and preserves truthful Graphics fallback otherwise. Same-effective dependencies emit no wait; distinct effective queues use GPU waits. Different-family Vulkan resources remain queue-local, and the paired Vulkan ownership API rejects without pending publication until native release/acquire translation is implemented. RenderGraph cross-queue scheduling, Vulkan ownership translation, transient allocation, and viewport adoption remain separate.
- Buffer command lists expose portable paired ownership release/acquire recording backed by one production `BufferOwnershipTracker` shared by D3D12 and Vulkan adapters. Recording is private; an accepted release publishes only a pending pair keyed to its exact completion token, and only an accepted matching acquire submitted with that dependency publishes destination owner/state. Pending buffers reject ordinary transition, upload, copy, destruction, and second-release paths; explicit recovery requires completion of the exact release token and restores source owner/state without claiming native rollback. D3D12 release records no ownership barrier and acquire records the portable state transition that executes after the queue wait. `RHIBufferOwnershipSmokeV1` qualifies that exact lifecycle on the local RTX 3080 Ti with Graphics-to-independent-Copy GPU ordering/no intervening CPU wait, 4 KiB deterministic bytes, final Copy/CopySource publication, release/acquire retirement, and separately completed abandoned-release recovery. Forced-D3D12-Graphics and topology-adaptive Vulkan paths reject unavailable paired transfers with no pending state; Vulkan ordinary same-family resource use is separate from the deferred ownership API. Vulkan native ownership translation and RenderGraph cross-queue execution remain separate.
- `RHI::Device::OwnsResource` now validates a supplied texture or buffer against the exact creating RHI device instance before a future graph binder records commands. D3D12 and Vulkan/NVRHI wrapper resources retain an immutable monotonic owner identity; null, foreign-backend, and same-backend different-device resources return false without native-handle exposure. `RHIResourceOwnershipSmokeV1` creates real owned buffer/texture wrappers and rejects null on both headed local backends; `EngineTests` deterministically cover foreign and same-backend different-device rejection because the headed smoke environments create only one active device. This adds no graph execution, scheduling, transient allocation, or viewport work.
- The backend-neutral single-graphics-queue RenderGraph executor now binds only explicitly supplied exact-device-owned physical textures/buffers whose kind, description, usage, extent/size, and committed initial state match the graph declaration before recording. Compiled barriers and callbacks execute in stable compiled order; each callback sees only its declared resource handles plus its assigned RHI command list. Failed validation, callbacks, recording, submission, or completion stop later work and success. Successful submission publishes the imported final states and one opaque completion token. A bounded three-context pool reports exhaustion instead of overwriting in-flight work and re-records a context only after that context's exact token completes. Deterministic `EngineTests` cover binding/state rejection before recording, undeclared/wrong-kind access, callback and completion failure propagation, texture/buffer barrier/callback order, committed final states, pool exhaustion, and exact-token same-object reuse. Local Windows/MSVC Debug `EngineTests` (45/45), `TestRender.ps1`, `TestVulkan.ps1`, and `CheckCodeStyle.ps1` passed; each headed backend requires two real graph executions with graph-derived barriers, ordered callbacks, deterministic 3x2 RGBA8 readback, and GPU-retired same-context reuse. Cross-queue execution, transient allocation/reuse, and viewport adoption remain separate unchecked items.
- Editor camera and camera component scaffolding provide a shared `CameraView` for renderer code.
- Scene format version 4 makes canonical signed-sector/local position the private writable `Transform` authority and persists the immutable Scene `WorldGridPolicy`; it writes no duplicate `MainCamera.Transform` and migrates version 1-3 absolute-double scenes through the default policy with selected-entity camera-transform precedence. Exact policy/sector-local persistence, huge sectors, precision-safe single-axis mutation, legacy migration/default policy/entity-main-camera precedence, and invalid/noncanonical rejection with an unchanged destination are covered by Windows/MSVC and MinGW Debug `EngineTests`; real Editor headless save/reload, scene-authoring save/reopen, and undo/redo smokes also pass on the MSVC build. Each immutable snapshot epoch carries the complete stable-ID editor-viewport `CameraView`, including compatibility/diagnostic double world position and translated origin, canonical origin position, and temporal-history invalidation. The Editor viewport supplies its authoritative main-camera sector/local transform directly to its publisher-owned tracker instead of decomposing the approximate `EditorCamera` double again. The tracker keeps sector-snapped state independent per stable view ID, retains each axis through the configured hysteresis band, selects direct destination sectors on teleport, and leaves exact-camera translation as the default. Equal and adjacent extreme signed-sector comparisons preserve local hysteresis detail through canonical relative arithmetic; signed subtraction overflow, non-finite results, and values outside translated float range are rejected. Known Editor scene replacement/load/restore and discontinuous camera-state synchronization set a one-shot flag that is consumed only by a valid published view. Snapshot extraction retains the immutable world-grid policy and canonical sector/local transforms, while raster preparation derives each camera-relative float position from canonical mesh and view-origin data without first composing an absolute double. MSVC Debug builds with zero warnings/errors, all 31 focused `EngineTests` pass, and the D3D12 prototype capture crosses a real sector boundary with A/C byte-identical, B shifted right by 196.24 pixels, and a 13.20% non-background ratio. This evidence covers only the current built-in prototype geometry; real mesh/material resources, Vulkan scene raster, culling, coordinate debug views, physics, and ray/TLAS/query consumers remain pending.
- The native job executor now uses worker-local deques with peer stealing and observable worker/statistics identities. `FrameTaskGraph` provides validated dependencies, caller/worker lanes, graph-local completion, typed immutable publication, failure/skip propagation, deterministic caller-thread mode, and profiler events; the Application frame workflow consumes it without a per-frame global idle barrier. Scene snapshot publication runs inside the caller-affine layer-update node after mutable editor work. Workerized engine systems and Profiler lane visualization remain later consumers.
- Shader source loading and change detection are centralized through `ShaderLibrary`. Initial portable package compilation runs asynchronously through the job system during normal editor execution and deterministic-inline only for smoke tests; live pipeline rebuild after a source change remains the next separate item.
- GPU timestamp query contracts and renderer timing snapshots are stubbed/no-op; they are current-state inventory, not a completed roadmap behavior. D3D12 query heaps and Vulkan query pools/resolve remain pending as one backend-neutral profiling prerequisite that must be completed before Phase 3E lighting, PBR, shadows, and sky work begins.
- The first D3D12 viewport pass has resource debug names and capture markers for frame, viewport, ImGui, and capture readback scopes.
- The D3D12 viewport prototype mesh creates vertex, index, and constant buffers through the RHI buffer API, uploads immutable vertex/index payloads through a staged copy-queue submission, and records its indexed draw through the D3D12 RHI command-list bridge.
- The D3D12 viewport color and depth targets are allocated through the RHI texture API. Each output-capable texture owns its persistent CPU-only RTV or DSV descriptor, so output binding validates, transitions, and binds pre-created views without per-frame descriptor-heap allocation; native presentation retains swapchain, SRV/ImGui, and capture/readback descriptors.
- Viewport capture readback now uses the RHI buffer API; texture copy commands and BMP writing remain in the D3D12 presentation layer.
- The viewport prototype shader, graphics pipeline, root constant-buffer binding, vertex/index binding, indexed draw, and output-target descriptor binding now run through D3D12 RHI shader/pipeline/command-list APIs. The shader objects consume the DXIL member of the validated Slang package; the paired SPIR-V member is not executed by the current Vulkan presentation-only path. Texture copy commands and BMP writing remain in the D3D12 presentation/viewport bridge.
- The editor has an engine-owned Vulkan 1.3 device, window surface, FIFO swapchain, native ImGui presentation path, resize handling, and strict render smoke on Windows through both MSVC and MinGW plus Linux X11 through WSLg/Mesa llvmpipe. Vulkan Scene snapshot raster is exercised into renderer-owned offscreen outputs on local Windows/MSVC plus hosted Ubuntu lavapipe and macOS MoltenVK; completed-output-to-ImGui/swapchain handoff remains pending.
- The first macOS backend decision is MoltenVK through the existing NVRHI Vulkan boundary. Hosted macOS 15 Intel CI has verified portability enumeration, NVRHI wrapping on the Apple Paravirtual device, native ImGui presentation, swapchain recreation, and successful post-resize present. The intermittent four-frame/final-frame validation race is fixed by retaining the last successfully presented swapchain generation, exiting only after it matches the current post-resize generation, and using 60 frames solely as a failure deadline. Hosted CI repeated the complete strict launch three times successfully. The hosted device still requires MoltenVK Metal argument buffers and `MTLHeap` disabled; Apple Silicon and production scene-renderer qualification remain pending.
- The render-graph construction compiler resolves declared data hazards into a stable pass order and logical lifetime/state/queue-transition plan. It is not yet used by the real renderer and has no physical resource binding, callback execution, RHI command/barrier emission, queue submission, or GPU-retired transient allocation/reuse; those operations remain explicitly ordered after construction.
- D3D12 `RHI::Device::ReadbackTexture` now reads exact-device-owned single-mip/layer/sample RGBA8 offscreen CopySource textures through an RHI command list, readback buffer, completion token, and finite wait. It rejects invalid state/format/extent/ownership/lifecycle combinations before submission, leaves the source in its caller-finalized CopySource state, and returns compact initialized rows rather than D3D12 footprint padding. The Windows D3D12 `RHITextureReadbackSmokeV1` clears a 3x2 target, rejects wrong-state and R8 attempts, and verifies deterministic RGBA bytes and `RowPitchBytes=12`; native presentation capture is not involved. Graph execution, cross-queue scheduling, transient allocation, and viewport adoption remain separate unchecked work.
- The editor can serialize the active sample scene to a versioned `.spiral` scene file and reload-validate it through the same scene API.
- Scenes now expose a small entity/component authoring facade with scene-local entity IDs, names, transforms, optional cameras, and save/load coverage.
- The editor scene hierarchy lists actual scene entities rather than hard-coded placeholder rows.
- Scene entities now support transform, camera, light, and mesh renderer components, with the editor default scene authoring a directional light and prototype mesh renderer.
- The scene hierarchy selection now drives the Inspector, and the Inspector edits live transform, camera, light, and mesh renderer component data. Camera-bearing entities expose Position and Rotation but omit the ineffective Scale control; non-camera entities retain Scale.
- The asset registry creates stable path/type-based handles, saves and reload-validates a manifest, and assigns registered sample mesh/material handles into the editor scene.
- Asset source watching tracks registered files, warns on deletion, and queues reimport hooks on source changes.
- The editor can import `.gltf`/`.glb` mesh sources through cgltf, register a stable mesh handle, and cook a structural mesh manifest; GPU mesh buffers and material/texture conversion remain follow-on work.
- The KTX2/Basis texture import plan defines texture roles, color-space rules, target profiles, validation, streaming, and the future libktx boundary before any texture transcoder is vendored.
- Material assets are versioned `.spiralmat` files with PBR factors, alpha/shading modes, Callisto controls, and texture handles; editor changes save and reload-validate through the material library.
- GitHub dependency submission now reports vendored/tool dependencies from the dependency ledger so the repo dependency graph can show them.
- A portable `EngineTests` executable covers deterministic engine contracts that are too narrow for editor smoke tests, including job-system failure handling, strict scene deserialization, and backend-neutral render-snapshot extraction/lifetime.
- The editor can create an isolated project and starter scene from a name/location modal, and hierarchy controls create or delete entities with undo/redo coverage.
- The physics architecture is now a documented planning contract: CPU-authoritative fixed-step gameplay physics, an engine-owned backend boundary and bake-off, versioned collision cooking, explicit determinism levels, optional one-way GPU visual deformation, and measured FEM/PD/IPC/ABD hero research. No physics backend or runtime module is implemented or admitted yet.
- The terrain architecture is now a documented planning contract: project-selectable bounded, streamed, unbounded, and hybrid profiles; deterministic spatial sources; canonical versioned tile artifacts; separate generation/residency/render/collision authority; and a measured Terrain Diffusion evaluation. No terrain module, generator, model, or runtime dependency is implemented or admitted yet.
- The AI/automation architecture is now an accepted planning contract: human-owned durable intent, live-steerable planning, deterministic permission-scoped public tools, transactions/undo, validation, provenance, AI-optional parity, evidence-driven contract challenges, and actual-consumer minimality. No in-product AI runtime, provider adapter, generic workflow executor, action schema, or Automation module is implemented; the unused public `WorkflowDefinition`/`WorkflowStep` scaffold was removed rather than treated as a foundation. Hosted CI run `29251508300` and dependency-submission run `29251508226` for contract commit `ea45df6` created jobs but executed no steps; GitHub reported failed account payments or an Actions spending-limit increase requirement, so the runs provide no hosted evidence.
- Renderer capability state now has separate advertised, enabled, implemented, and exercised stages. D3D12 and Vulkan enumerate real devices before creation, query the versioned Phase 3 Bootstrap profile's API, queue/presentation, synchronization, texture-limit, RGBA8, and D32 requirements, and select through the shared deterministic evaluator. Reports retain every candidate evaluation, stable DXGI LUID/Vulkan UUID, rejection, queue/preference fallback, selected formats, and conservative feature lifecycle state. Exact adapter name or stable-ID preference and strict no-fallback launch are supported; unused D3D12 direct-indexing/enhanced-barrier requests and Vulkan buffer-device-address enablement remain off. The editor Profiler presents the active report through a renderer-owned read-only snapshot, including profile, adapter identity, Bootstrap qualification, queues, formats, feature lifecycle states, selected fallbacks, retained candidate rejection reasons, and versioned consumer groups. `Phase3FrameTimingV1` prefers GPU timestamps but currently selects and exercises portable CPU steady-clock timing because both native timestamp-query paths remain unimplemented; headed D3D12 and Vulkan smokes prove the group at Presentation without upgrading the device beyond Bootstrap. Hosted CI run `29245709930` for scene-snapshot implementation commit `4d16bb8` started no Code Style, Windows, Ubuntu, or macOS job steps; dependency-submission run `29245709977` also started no steps. GitHub reported failed account payments or an Actions spending-limit increase requirement, so these runs provide no hosted evidence. Future consumer groups, dedicated Vulkan queue enablement, physical-device breadth, and Scene/Production qualification remain pending beside their actual consumers.
- The headed presentation prototype does not deliberately discard completed engine frames, and its synchronized backend choices remain separate: D3D12 uses a waitable swapchain with maximum latency one and `Present(1, 0)`, Vulkan uses FIFO, and the dormant OpenGL path requests swap interval one. The top-bar Settings menu persists `Responsive` or opt-in `Smooth Frametime`; Profiler exposes a non-serialized runtime override and experimental `InterFrame`/`SubmissionGate` selector. `Responsive` applies no intentional wait. The experimental inter-frame path releases before the next timestep sample/FrameStart/input; the submission gate releases immediately before native queue submission, not before the current `Present`. Both preserve mandatory backend waits, frame results, and truthful unavailable display/replacement/input-latency states. Local 5 FPS smokes qualify control points and telemetry only; they do not select a production winner or prove visible smoothness/latency.

## Phase Sizing And Design Gates

Phases are outcome gates, not equal-size sprints. Phase 3 is intentionally larger because every later renderer feature consumes its cross-backend RHI, task/snapshot, shader, render-graph, resource, color, lighting, and diagnostic foundations. It is divided into ordered sub-milestones so agents can implement bounded slices without declaring the whole renderer complete.

Phases 4 through 9 are feature layers and must name both their algorithms and the infrastructure they consume. Their technical traceability lives in [Docs/Architecture/TECHNICAL_ROADMAP_COVERAGE.md](Docs/Architecture/TECHNICAL_ROADMAP_COVERAGE.md).

Later non-renderer phases are product outcome summaries where some subsystem designs remain open. Phase 7 terrain, Phase 11 physics, and Phase 13 AI/automation have accepted planning contracts in [Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md](Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md), [Docs/Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md](Docs/Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md), and [Docs/Architecture/AI_AUTOMATION_ARCHITECTURE.md](Docs/Architecture/AI_AUTOMATION_ARCHITECTURE.md), with dependency-ordered checklist coverage below. Before implementing Phase 10 animation, Phase 14 game systems/networking, or Phase 16 packaging beyond terrain provenance/cooking requirements, add or update the task-relevant architecture contract and expand that phase's prerequisites, data ownership, fallback/error behavior, and focused verification. An accepted planning contract is not a checkable substitute for runtime behavior.

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

### Phase 3A: Backend Bootstrap And Qualification

- [x] NVRHI D3D12 device integrated behind `Engine::RHI`, with the prototype viewport still using a scoped native D3D12 presentation bridge.
- [x] D3D12 first path on Windows.
- [x] Engine-owned Vulkan 1.3 device, window swapchain, FIFO presentation, and ImGui integration verified on Windows with MSVC and MinGW.
- [x] Native Linux X11 Vulkan editor presentation, resize, and post-resize present verified locally through WSLg with Mesa llvmpipe.
- [x] Hosted Ubuntu Vulkan presentation smoke through Mesa lavapipe and Xvfb.
- [x] Experimental x86_64 macOS editor presentation through MoltenVK and NVRHI Vulkan, including swapchain recreation and successful post-resize present on hosted macOS 15 Intel CI.
- [x] Hosted macOS MoltenVK resize/post-resize-present smoke uses generation-aware completion and passes three consecutive strict launches in CI, including the multi-recreation timing case.
- [x] Backend-neutral renderer capability lifecycle and deterministic profile evaluator distinguish advertised/enabled/implemented/exercised state, validate synthetic API/queue/presentation/format/limit requirements, rank candidates, retain rejection reasons, and select compatible-queue fallbacks with focused EngineTests.
- [x] Current D3D12 and Vulkan devices publish conservative Bootstrap capability reports with selected adapter identity, queue mapping, qualification, optional-feature lifecycle state, and fallback diagnostics; D3D12 and Vulkan smokes require the report markers without claiming Scene qualification.
- [x] Drive D3D12 and Vulkan adapter selection from versioned capability profiles before device creation: query required format usages/limits/queues per candidate, retain every rejection and selected fallback, honor adapter preference/strict failure, and gate optional device features rather than enabling them speculatively.
- [x] Expose the active D3D12/Vulkan Bootstrap capability report in editor diagnostics, including selected profile, adapter identity, qualification level, queue and format decisions, feature lifecycle states, selected fallbacks, and retained rejected-candidate reasons, with focused headed editor-workflow verification.
- [x] Versioned `Phase3FrameTimingV1` consumer group prefers usable GPU timestamps, otherwise selects portable CPU steady-clock timing with explicit reasons; deterministic tests cover selection/lifecycle and headed D3D12/Vulkan smokes exercise the CPU fallback at group-specific Presentation while the device report remains Bootstrap.

Capability groups are added as dependency-ordered checklist items immediately before their real consumers. Each group must expose explicit unavailable/fallback status and must exercise its selected path, functional fallback, and named backend/device coverage before the corresponding qualification claim.

### Phase 3B: Frame And RHI Infrastructure

- [x] D3D12 flip-model swapchain lifecycle and native graphics/compute/copy queues.
- [x] RHI command-list allocation, validated recording lifecycle, and synchronous queue submission.
- [x] GPU buffer resource-upload path with copy-queue synchronization and synchronous fence ownership.
- [x] Large-world coordinate foundation: authoritative double-precision Scene/editor-camera positions, precision-preserving versioned scene serialization, and camera-relative float translation integrated into the current D3D12 raster prototype, with high-magnitude deterministic tests.
- [x] CPU frame task graph on the native work-stealing job system with validated declared dependencies, caller/worker lanes, graph-local completion, immutable publication points, failure/skip propagation, deterministic single-thread fallback, profiler hooks, and Application-frame integration.
- [x] Backend-neutral scene-to-renderer extraction into an immutable per-frame render snapshot with mesh/material/light/camera handles and no editor or backend-native types.
- [x] Carry one editor-viewport `CameraView` and translated origin in each immutable snapshot epoch and drive the current D3D12 prototype-geometry raster from that epoch's Scene mesh transforms, with high-magnitude origin-transition invariance tests and comparative captures.
- [x] Specify the versioned sector/local coordinate and transition contract: signed 64-bit sector identity, a 4096-unit default extent, centered half-open double locals, deterministic negative/exact-boundary normalization, checked overflow, min-inclusive/max-exclusive cross-sector bounds, optional 256-unit origin hysteresis, exact-camera default, independent stable-ID view state, teleport behavior, and the Scene-version-4 migration rule.
- [x] Implement backend-neutral sector/local policy validation, conversion, normalization, explicitly approximate absolute-double composition, and bounded cross-sector-range primitives, with deterministic signed-boundary, custom-extent, trillion-unit, carry/overflow, exact-maximum, invalid, empty, and oversized tests.
- [x] Persist canonical sector/local Scene transforms and their immutable world-grid policy through scene format version 4 while retaining version 1-3 loads, without parallel transform authorities.
- [x] Implement stable-ID per-view sector-snapped origin tracking with hysteresis and teleport handling, publish independent immutable view epochs, and test no-flap and multi-view retention while preserving exact-camera translation as the default.
- [x] Propagate canonical sector/local transforms through snapshot/raster preparation and repeat comparative D3D12 captures across real sector transitions before real resources, culling, coordinate debug views, physics, and ray/TLAS/query consumers rely on them.

### Phase 3C: Portable GPU Execution

- [x] Shared asynchronous scene-shader portability path with deterministic admitted-host target output, reflected RHI layouts, backend convention validation, versioned caching, and diagnostics. Windows x86_64/MSVC produces paired DXIL+SPIR-V through Slang v2026.13.1 and pinned DXC v1.9.2602; D3D12 consumes DXIL while SPIR-V is compiled/reflected/convention-validated package evidence only until the next Vulkan scene-RHI item consumes it. Linux/macOS produce SPIR-V-only packages and reject DXIL requests explicitly because a non-Windows DXC path is not admitted. Normal editor startup uses job-system fire-and-poll, smoke tests use deterministic-inline execution, and redistribution remains blocked until the admitted binary-component/notice audit is resolved. Linux, macOS, MinGW, Windows ARM64, Vulkan scene rendering, and physical-device breadth are not qualified by this completion.
- [x] Define and exercise backend-neutral viewport-output recording for renderer-owned color/depth target binding, deterministic clear, viewport/scissor, pipeline/draw recording, and resource-state transitions so the current Windows x86_64/MSVC D3D12 scene renderer requires no native command-list access; native presentation retains swapchain, viewport exposure, capture/readback, and ImGui ownership.
- [x] NVRHI Vulkan `Engine::RHI::Device` core after real device creation: buffers, RGBA8 color/depth textures, output clear, explicit transitions, command begin/end/submission, deterministic buffer upload/update, staging texture readback, debug names/markers, and GPU-safe retirement. Deterministic offscreen clear/readback through NVRHI verifies dimensions, row layout, and sampled pixels. Local Windows `TestVulkan.ps1` evidence is local only; exact-head hosted run `29357979246` passed the Linux lavapipe and macOS Apple-Paravirtual/MoltenVK Vulkan smoke jobs (the hosted Windows job is D3D12-only). No shader/pipeline/draw, Scene, or ImGui claim.
- [x] SPIR-V shader consumption, reflected bindings/input layout, graphics pipeline/framebuffer state, constant-buffer update, and deterministic indexed offscreen draw/readback using the exercised Vulkan RHI core primitives. The smoke selects the admitted paired DXIL+SPIR-V package on Windows and SPIR-V-only package on Linux/macOS, passes only SPIR-V to NVRHI, gates Vulkan 1.3 dynamic rendering/synchronization2, and validates the indexed triangle's interior/background pixels after readback. Exact-head hosted run `29361689869` passed Ubuntu lavapipe and macOS MoltenVK plus the Windows D3D12 regression. No Scene snapshot or ImGui output handoff claim.
- [x] Vulkan Scene viewport raster integration using the exercised Vulkan RHI primitives: consume the immutable Scene snapshot, render through `Engine::RHI`/NVRHI into renderer-owned color/depth outputs, and prove the result through deterministic offscreen readback without adding a raw-Vulkan scene path. `VulkanSceneViewportRasterV1` publishes one immutable camera/mesh snapshot, rasterizes it at 48x36 then replaces its GPU-retired outputs at 64x48, and requires readback dimensions/row pitch, clear-background, bounded foreground-footprint, and output-generation assertions. Local Windows/MSVC `TestVulkan.ps1` passed, and exact-head run `29368558656` passed Code Style, Windows D3D12 regression, Ubuntu lavapipe Vulkan, and macOS MoltenVK jobs. The following native presentation/ImGui item remains separate.
- [x] Vulkan completed-NVRHI-output-to-native-presentation/ImGui handoff: expose the renderer-owned Scene output to the existing native Vulkan ImGui/swapchain bridge with resize-safe lifetime and strict presentation/capture evidence; keep raw Vulkan confined to bootstrap, WSI/presentation, and ImGui. Local Windows Vulkan `TestVulkan.ps1` and exact-head run `29369950355` passed the post-resize renderer-output capture and ImGui/swapchain-consumption markers on Ubuntu lavapipe and macOS Intel Apple-Paravirtual/MoltenVK; the hosted Windows job passed the D3D12 regression.
- [x] Frame/render graph construction: pass registration with declared resource reads/writes, automatic resource lifetime tracking, dependency resolution and pass ordering, and barrier/queue-transition insertion derived from the graph. The compiler validates queue/state/stage intent and invalid handles/uses, derives RAW/WAR/WAW plus explicit ordering dependencies, rejects cycles/read-before-write, preserves stable independent ordering, and emits logical barrier/cross-queue dependency records; MSVC Debug `EngineTests` cover the contract. Physical binding, callback execution, RHI command emission, submission, and viewport integration remain next.
- [x] Backend-neutral RHI buffer transition recording required by graph execution: expose explicit buffer-state transitions on `RHI::CommandList`, implement and validate them on D3D12 and Vulkan/NVRHI under the same barrier-authority rules as textures, and exercise invalid and real submission paths without leaking native state policy above `Engine::RHI`. Local Windows/MSVC Debug `EngineTests` (39/39), `TestRender.ps1`, and `TestVulkan.ps1` passed; each headed backend smoke requires `RHIBufferTransitionSmokeV1 ... invalid=rejected, lifecycle=pass, submission=pass, result=pass`. Exact-head hosted CI run `29372850292` passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, and code style.
- [x] Backend-neutral asynchronous GPU completion and reusable recording contexts required by graph execution: submit graphics work with an opaque monotonic completion token, query or wait for that token through `Engine::RHI`, and reset/reuse command-list or frame-context recording state only after the GPU reports completion on D3D12 and Vulkan/NVRHI. Preserve the existing synchronous helper as a compatibility wrapper, reject invalid/cross-device/stale tokens explicitly, and prove nonblocking incomplete-to-complete observation plus GPU-retired context reuse with focused real-submission evidence.
- [x] Backend-neutral RHI resource/device ownership validation required by graph binding: allow a device to validate whether a supplied texture or buffer was created by that exact device without exposing native handles or backend types, reject null and cross-device resources before command recording, and cover D3D12 and Vulkan/NVRHI with focused contract and real-resource evidence. Local Windows/MSVC Debug `EngineTests` (41/41), `TestRender.ps1`, and `TestVulkan.ps1` passed; same-backend different-device rejection is deterministic contract/adapter evidence because these headed smokes create one active device. Exact-head CI run `29375133065` and dependency submission `29375133081` passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, code style, and dependency submission.
- [x] Backend-neutral D3D12 texture readback parity required by graph-execution evidence: `RHI::Device::ReadbackTexture` accepts only exact-device-owned single-mip/layer/sample RGBA8 offscreen CopySource textures, validates state/format/extent/footprint/lifecycle before submission, records an RHI-owned D3D12 copy/readback-buffer command, waits through the bounded completion-token API, preserves CopySource state, and returns compact deterministic bytes with no native-footprint padding. Windows x86_64/MSVC `RHITextureReadbackSmokeV1` rejects wrong-state and R8 attempts then verifies a real 3x2 clear/copy/readback at `RowPitchBytes=12`; `TestVulkan.ps1` remains the Vulkan/NVRHI readback regression. Exact-head CI run `29376236937` passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, and code style; dependency submission `29376236959` passed. Native presentation capture is not used; graph execution, cross-queue scheduling, transient allocation, and viewport adoption remain unchecked.
- [x] Backend-neutral authoritative resource-state query required by graph imports: allow an exact owning `RHI::Device` to report the wrapper-tracked current state of a live texture or buffer without exposing native state values, reject null/foreign/unknown state explicitly, and prove that successful transitions update the value on D3D12 and Vulkan/NVRHI. This is observation only; external state adoption remains forbidden unless a later real bridge requires and specifies it. Local Windows/MSVC Debug `EngineTests` (41/41), `TestRender.ps1`, `TestVulkan.ps1`, and `CheckCodeStyle.ps1` passed; each headed backend requires the complete `RHIResourceStateSmokeV1` acceptance marker. The deterministic test, rather than a second physical device, proves same-backend-other-device rejection. Exact-head CI run `29377364744` and dependency submission `29377364769` passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, code style, and dependency submission.
- [x] Frame/render graph execution core: bind imported and explicitly supplied physical resources, invoke pass callbacks through a restricted execution context, record graph-derived same-queue RHI barriers and commands, submit deterministic single-queue work, and retire frame contexts by GPU completion with focused real-resource evidence. Deterministic `EngineTests` (45/45) and local Windows/MSVC `TestRender.ps1`, `TestVulkan.ps1`, and `CheckCodeStyle.ps1` passed; `RenderGraphExecutionSmokeV1` proves real D3D12 and Vulkan/NVRHI transitions, ordered callbacks, deterministic offscreen bytes, successful state publication, token retirement, and same-context reuse. Exact-head implementation CI run `29378695587` and dependency submission `29378695596` passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, code style, and dependency submission for commit `c8468e9`.
- [x] RHI queue-topology and GPU-dependency prerequisite: `QueueResolution` preserves requested Graphics/Compute/Copy class while reporting the effective class and independent/fallback identity; dependency-aware `Submit` accepts ordered prior same-device completion tokens without exposing native synchronization. The monotonic validator rejects zero, foreign, unissued, duplicate, forward/cycle-forming, and impossible-self identities before native acceptance or staged-state publication. D3D12 uses GPU queue fence waits for distinct effective queues and ordinary ordered submission for same-effective queues; local RTX 3080 Ti `RHIQueueDependencySmokeV1` proved Copy-to-Graphics and Graphics-to-Compute independent waits with no intervening CPU wait, exact 4 KiB output, committed `CopySource`, and retirement, while the forced mode proved both graphics fallbacks/elisions. The historical Vulkan evidence for this prerequisite was Graphics fallback; Vulkan multi-queue admission is now separately complete at line 258. Deterministic `EngineTests` (47/47), focused D3D12 normal/forced launches, focused Vulkan launch, and `CheckCodeStyle.ps1` passed locally. Exact-head CI run `29409715948` and dependency submission `29409715908` passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, code style, and dependency submission for harness commit `373300e`, which contains implementation commit `556281b`. RenderGraph translation remains the following separate item.
- [x] Backend-neutral RHI buffer queue-ownership lifecycle implementation and deterministic contract: portable buffer-only release/acquire descriptors and command-list methods feed one production tracker used by D3D12 and Vulkan adapters. Exact-live-device, usage/state, resolved queue, pair, token, dependency, duplicate, accepted-release transfer-state, publication, ordinary-operation, destruction, and recovery rules are covered by focused deterministic tests. D3D12's production seam requires no release barrier and a portable acquire transition; current Vulkan same-effective Graphics fallback is rejected before transfer-state publication. Local MSVC Debug build and all 49/49 `EngineTests` passed. Exact-head CI run `29415941902` and dependency submission `29415941895` passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, code style, and dependency submission for implementation commit `0a4fb2e`. No headed ownership smoke or native queue-transfer qualification is claimed; that remains the next item.
- [x] RHI buffer queue-ownership real-device qualification: `RHIBufferOwnershipSmokeV1` exercises the implemented lifecycle on the local RTX 3080 Ti D3D12 Graphics-to-independent-Copy queues with no CPU wait between release/acquire submissions, exact 4 KiB bytes, final Copy owner/CopySource state, completed abandoned-transfer recovery, and token retirement; bounded forced D3D12 Graphics and current Vulkan Graphics fallback launches reject transfer creation without publishing a transfer record. Local MSVC Debug build, 49/49 `EngineTests`, `TestRender.ps1`, `TestVulkan.ps1`, and code style passed. This is buffer-only local device evidence; texture parity, Vulkan multi-queue/different-family translation, and RenderGraph cross-queue execution remain unchecked.
- [x] RHI texture queue-ownership parity: exact-device-owned whole-resource textures use the same paired release/acquire, in-transfer use/destruction/state guards, exact-token dependency/publication, and completed abandoned-release recovery authority as buffers. Unsupported multi-mip/layer/sample shapes and incompatible usage/state are rejected. Local RTX 3080 Ti D3D12 `RHITextureOwnershipSmokeV1` proved Graphics-to-independent-Copy GPU-fence ordering with no CPU wait between release/acquire, deterministic RGBA8 bytes through `ReadbackTexture`, final Copy owner/CopySource state, retirement, and recovery; bounded forced D3D12 Graphics and current Vulkan Graphics fallback runs reject release without transfer publication. MSVC Debug build, 50/50 `EngineTests`, `TestRender.ps1`, `TestVulkan.ps1`, and code style passed. Vulkan multi-queue/different-family translation and RenderGraph cross-queue execution remain unchecked.
- [x] Vulkan/NVRHI multi-queue admission prerequisite: deterministically enumerate family flags/counts, select concrete Graphics/Compute/Copy handles only when an unused queue index exists, pass them through the engine-created `VkDevice` and NVRHI boundary, and map queue-class submission/dependency/completion tokens to the effective NVRHI queue. Graphics remains the truthful fallback. Ordinary same-effective/same-family resource use is admitted; different-family ordinary use rejects, while the later completed ownership-translation gate admits only paired transfers without exposing native synchronization. Local Windows RTX 3080 Ti evidence selected Graphics family 0/index 0, Copy family 1/index 0, Compute family 2/index 0 and proved Copy-to-Graphics plus Graphics-to-Compute NVRHI timeline waits and per-queue retirement without a CPU wait. Deterministic tests cover fallback, independent resolution, same/different-family policy, and the existing dependency-token invalid cases; both Vulkan harnesses adapt to actual topology. Hosted Vulkan runs remain fallback/regression evidence only until they execute the available topology.
- [x] Vulkan cross-family shared-resource ownership translation: matched native buffer/image release/acquire barriers use the selected source/destination family indices inside the NVRHI adapter, reseed NVRHI state tracking without publishing the hidden wrapper state, and preserve same-family GPU-order-only operation. Completed abandoned different-family releases are compensated forward (destination acquire, destination-to-source release, source acquire) before exact-token tracker recovery; no native handle or mutable tracker authority is public. Local RTX 3080 Ti Vulkan selected Graphics family 0/index 0 and Copy family 1/index 0. `RHIBufferOwnershipSmokeV1` proves GPU-only source and validation-destination transfers, a real Copy-queue copy, Graphics copy to CPU readback, exact 4 KiB bytes, final Copy/CopySource source state, no CPU wait between forward release/acquire submissions, recovery, and retirement. `RHITextureOwnershipSmokeV1` proves equivalent RGBA8 ownership/readback/recovery evidence. `EngineTests` (51/51), MSVC Debug zero-warning build, bounded `TestVulkan.ps1`, bounded D3D12 `TestRender.ps1`, bash syntax, and code style passed locally. Vulkan hosted jobs remain fallback/regression evidence unless their topology supplies an independent different-family pair.
- [x] Frame/render graph cross-queue execution: compiled passes submit on their effective RHI queue with stable fan-in dependencies and paired portable ownership release/acquire only across distinct effective queues; accepted submissions publish prefix state/owner and aggregate completion tokens retire the bounded contexts keyed by effective queue plus compiled pass identity. Imported initial/final state and owner are verified. The deterministic contract covers two buffers plus a texture on one edge, exact ordered release/acquire publication, a deduplicated producer token, complete-batch rejection with no partial publication, per-pass three-context exhaustion/reuse, and missing producer-token accepted-prefix failure. `RenderGraphExecutionSmokeV1` proves a real Graphics-to-Copy GPU wait, readback, aggregate retirement/reuse on local RTX 3080 Ti D3D12 and split-family Vulkan; forced/unavailable queues use ordered Graphics fallback without fabricated ownership. MSVC Debug zero-warning build, 53/53 `EngineTests`, bounded `TestRender.ps1`, and bounded `TestVulkan.ps1` passed locally. Hosted queue topology remains hardware-dependent.
- [x] Frame/render graph real-workflow integration: drive a representative multi-pass Scene viewport through the execution path on every backend claimed, with graph/pass capture labels and output equivalence against the pre-graph path before removing that bootstrap path. The live D3D12 and Vulkan viewports use imported renderer-owned outputs through named Clear, Raster, and Output Handoff graph passes; `SceneViewportRenderGraphV1` requires a separate direct-recorder reference output to match the graph readback byte-for-byte, reports size/byte count, and fails the smoke on any mismatch. Local MSVC Debug `EngineTests` (53/53), `TestRender.ps1`, `TestVulkan.ps1`, code style, diff check, and bash syntax passed. Exact-head CI run `29430304670` passed Windows D3D12, Ubuntu Vulkan/lavapipe, and macOS Intel/MoltenVK with the required exact-byte comparator markers; dependency submission `29430304809` passed.
- [x] Phase 3 transient-resource capability group for placement/aliasing/barriers with a correct committed/pooled non-aliased fallback and GPU-retired reuse, defined and exercised before transient allocation is integrated. `Phase3TransientResourcesV1` keeps RHI-owned placed-resource and alias-barrier lifecycle reporting separate from RenderGraph policy: only both usable states select `PlacedAliasedTransient`; otherwise it selects `NonAliasedGpuRetiredPool`, whose future reuse is completion-token-gated rather than CPU-frame-number-gated. Current D3D12 and Vulkan/NVRHI adapters truthfully select the fallback because their backend-neutral translations are unimplemented. Deterministic `EngineTests` (55/55), local RTX 3080 Ti D3D12 `TestRender.ps1`, local RTX 3080 Ti Vulkan `TestVulkan.ps1`, and `CheckCodeStyle.ps1` passed. The headed smokes exercise group publication/diagnostics only; no native placement/alias barrier, physical allocation/reuse, memory-cost, or hosted fallback-regression claim is made.
- [x] Transient resource allocation and reuse from render-graph lifetimes: the RenderGraph binds unbound transient textures/buffers to compatible physical RHI objects, reuses one object only for sequential same-effective-queue lifetimes with an unchanged declared hand-off state, and otherwise allocates separately. Current D3D12/Vulkan select `NonAliasedGpuRetiredPool`; `PlacedAliasedTransient` fails explicitly until RHI can create placed resources and emit alias barriers. Each accepted pass immediately attaches its exact token to every touched pool entry, so a later failure preserves the accepted prefix and cannot expose in-flight storage; no CPU frame number is consulted. `ExecuteResult` reports selected mode plus estimated logical allocated/pooled bytes, not native heap footprints. `EngineTests` (58/58), local `TestRender.ps1`, local `TestVulkan.ps1`, and `CheckCodeStyle.ps1` passed; `RenderGraphTransientAllocationSmokeV1` reports compatible lifetime reuse, 64-byte estimated logical cost, exact-token retirement, and retired reuse on both current backends. No native placed-resource/alias-barrier or hosted/macOS qualification is claimed.
- [x] **Phase 3C prerequisite P1 — worker-safe Scene viewport preparation:** `Frame.PrepareSceneRaster` now consumes the immutable Scene snapshot on the CPU task graph worker lane and atomically publishes the immutable current-frame raster payload before caller-affine rendering; deterministic-single-thread executes that exact task inline. D3D12/Vulkan graph consumers reject missing/stale prepared payloads rather than silently recomputing them. Local RTX 3080 Ti D3D12 and Vulkan runs each emitted `SceneRasterPreparationV1` with a worker in parallel and `caller` inline, plus `SceneViewportRenderGraphV1 ... comparator=exact-byte-pass` in both modes; MSVC Debug Editor build and `CheckCodeStyle.ps1` passed. This is CPU preparation only, not concurrent command recording or a changed GPU submission policy.
- [x] **Phase 3C expected-before RHI prerequisite:** compiler-supplied texture/buffer `Before` states now remain command-list-local while recording and are compared with the exact live D3D12 wrapper/Vulkan tracker state at submission; a mismatch rejects before native acceptance and publishes no staged state. Vulkan locally seeds NVRHI state tracking without global mutation during recording. Deterministic `EngineTests` (59/59) prove stale expected-before rejection/no publication; local RTX 3080 Ti D3D12/Vulkan `TestRender.ps1`/`TestVulkan.ps1` pass the real three-pass viewport graph in worker and inline modes. This is state validation for safe pre-recording, not cross-queue acquire recording or the following end-to-end multithreaded-render item.
- [x] **Phase 3C prerequisite P2 — worker-safe graph recording contract:** pass-level eligibility is explicit and safe-by-default: undeclared callbacks remain caller-affine, while eligible same-effective passes without ownership operations receive separate RHI contexts and pre-record together. Compiled graph order remains the sole submission/publication authority and supplies dependency tokens; cross-queue acquire/release stays caller-recorded. Deterministic `EngineTests` (59/59) prove bounded independent overlap without a sleep gate, inline equivalence, same-effective token ordering, cross-queue deferral, false/throw accepted-prefix and unsubmitted-suffix disposal, fail-closed unsupported expected-before adapters, and exact retirement/reuse. On the local RTX 3080 Ti, D3D12 and Vulkan worker runs emitted `RenderGraphRecordingV1 ... workerPasses=2 overlap=yes submitted=3 result=pass`; their inline companions emitted `overlap=no`, and both retained `SceneViewportRenderGraphV1 ... comparator=exact-byte-pass`. MSVC Debug build, `TestRender.ps1`, `TestVulkan.ps1`, `CheckCodeStyle.ps1`, bash syntax, and diff check passed.
- [x] Multithreaded render preparation and command recording driven by the CPU task graph and compiled render graph, with deterministic single-thread validation mode, after Phase 3C prerequisites P1 and P2: accepted worker-recorded work now submits and publishes resource/context state only in compiled graph order. The same local RTX 3080 Ti D3D12/Vulkan parallel viewport runs jointly emit worker `SceneRasterPreparationV1`, `RenderGraphRecordingV1 ... overlap=yes submitted=3`, and `SceneViewportRenderGraphV1 ... comparator=exact-byte-pass`; their inline runs exercise the same routines with caller preparation and `overlap=no`. The bounded harnesses also retain `RHIQueueDependencySmokeV1 ... cpuWaitBetween=no`, and the executor has no CPU completion wait between accepted submissions. Deterministic `EngineTests` (59/59), MSVC Debug zero-warning build, `TestRender.ps1`, `TestVulkan.ps1`, code style, bash syntax, and diff check passed locally. This is local D3D12/Vulkan evidence only; cross-queue ownership acquire/release remains caller-recorded.
- [x] Project-owned frame-pacing policy with a developer-exposable runtime game-setting override: `Responsive` is the engine/project default and imposes no engine-side smoothness cap, delay, or deliberate render-result discard; opt-in `Smooth Frametime` serializes a validated target cadence. The version-2 `.spiralproject` manifest migrates version 1 projects to `Responsive`, rejects malformed/invalid policies transactionally, and the top-bar Settings menu plus public `GameFramePacingSettings` resolve project/override state. Profiler owns the non-serialized experimental control-point selector. This item establishes policy/serialization/UI authority; pacing implementation and evidence are the later checked candidate item. Keep presentation sync/VRR/tearing, frame-delivery replacement, maximum frames in flight, fixed simulation cadence, and portable/vendor latency modes as independent controls.
- [x] **Phase 3 presentation-pacing prerequisite — shared frame lifecycle telemetry and policy handoff:** resolved project/runtime policy now reaches renderer timing without an intentional wait; the authoritative application frame ID carries FrameStart, input/simulation, native submission, Present begin/end, and later fence-completion observation. Intentional pacing is recorded `not-applied:0`; DXGI latency-object and Vulkan acquire/fence waits remain mandatory. Display cadence and replacement/drop are explicitly unavailable. `EngineTests` (61/61) plus local RTX 3080 Ti D3D12/Vulkan harnesses emit `FrameLifecycleTelemetryV1 ... result=pass`.
- [x] **Phase 3 experimental presentation-pacing candidates and control-point qualification:** `Responsive` resets pacing state and applies no intentional wait. Opt-in `Smooth Frametime` uses a backend-neutral steady-clock deadline state machine with independent `InterFrame` and `SubmissionGate` candidate state; late frames skip the wait and rebase without dropping, replacing, or skipping work. `InterFrame` releases after the prior `Present` and before the next timestep sample, FrameStart, and input/simulation. `SubmissionGate` releases immediately before D3D12 `ExecuteCommandLists` or Vulkan `vkQueueSubmit`, not immediately before the current `Present`. The resolved policy is frozen per released frame so an in-frame UI change cannot apply both candidates. `EngineTests` (62/62) prove deadlines, overruns, resets, independent candidate state, and value-snapshot behavior. Local RTX 3080 Ti D3D12/Vulkan scripts exercise Responsive plus both candidates at a 5 FPS diagnostic target with nonzero intentional waits, correct native seam markers, consecutive released-FrameStart cadence, separate CPU active work, mandatory backend waits, GPU completion, and explicit unavailable input/display/replacement feedback. This is control-point qualification, not a smoothness, latency, timer-precision, or production-default selection claim.
- [ ] **Phase 3 presentation-pacing benchmark-capture prerequisite:** retain a bounded raw, frame-ID-keyed engine lifecycle history across released `FrameStart`, input/simulation, intentional and mandatory waits, submission, `Present`, and GPU-completion observation; derive/export deterministic p50/p95/p99, 1%/0.1% lows, deadline misses/overshoot, start-to-start cadence, and CPU-active versus intentional-wait distributions without replacing raw spikes. Export stable CSV/JSON records with explicit unavailable display/replacement/input/GPU-headroom fields and a condition manifest containing backend, adapter, target, policy/candidate, swapchain mode, and runner-supplied sync/VRR/tearing state. Add bounded D3D12/Vulkan maintainable-target benchmark launches plus deterministic retention/statistics/schema tests. This is engine capture fidelity only; it neither claims displayed smoothness/input latency nor selects a production candidate.
- [ ] Measure, compare, and select the production `Smooth Frametime` behavior at maintainable target cadences through DXGI waitable-swapchain and Vulkan presentation paths. Compare the engine inter-frame candidate, the separately instrumented submission gate, external RTSS `ASYNC`, and any explicitly named FES/frame-generation experiment without silently substituting a delay immediately before the current `Present`. Preserve mandatory acquire/fence correctness waits and verify Responsive/each candidate with shared frame IDs, p50/p95/p99 start-to-start/display cadence, 1%/0.1% lows, CPU active versus intentional wait, deadline misses/overshoot, render submit, `Present`, GPU-complete, display/replacement/drop where available, GPU headroom, and an appropriate input-latency measurement path. Do not select a default from the current 5 FPS control-point smoke or an engine/overlay graph alone.
- [x] D3D12 RHI shader/pipeline consumption, now fed by the shared Slang path's validated DXIL package member rather than the legacy source-compile route.
- [ ] Live D3D12 pipeline rebuild after shader source changes.
- [ ] **Phase 3 GPU timing prerequisite before lighting/shading:** implement backend-neutral timestamp allocation, command recording, resolve/readback, validity/disjoint handling, and frame-ID publication through `Engine::RHI`; translate it to D3D12 timestamp query heaps and Vulkan timestamp query pools without a per-frame CPU stall; instrument whole-frame and named RenderGraph/pass scopes; publish actual `GpuMilliseconds` plus per-pass GPU duration in Profiler while retaining truthful CPU fallback on unsupported devices; and qualify D3D12/Vulkan with deterministic ordering/availability tests plus native smoke evidence. This item must be checked before starting Phase 3E Forward+/clustered lighting, PBR shading, lights, shadows, or sky/atmosphere so their GPU cost is measurable as they land.

### Phase 3D: Scene Resources And Asset Inputs

- [ ] Full Phase 3 Scene capability group for actual scene formats/usages, queue/synchronization, shared shader targets/reflection, descriptors/samplers, and timing paths, with functional fallbacks and Scene evidence on every backend/device class claimed.
- [ ] Scene mesh/index/constant/structured-buffer integration beyond the prototype draw, populated from the render snapshot through `Engine::RHI`.
- [ ] Texture upload, samplers, mip generation.
- [ ] KTX2/Basis texture import and deterministic target cooking: versioned `TextureAsset` metadata, validated libktx boundary, `DesktopBC`/`Astc`/`RGBAFallback` artifacts, color-space/role rules, and headless fixtures.
- [ ] Descriptor/sampler and read-only bindless table model with declared capacities, error resources, GPU-retired updates, writable-resource rules, and a capability-gated bounded fallback.

### Phase 3E: Conventional Renderer Baseline

- [ ] Forward+/clustered light grid prototype.
- [ ] Scene-referred HDR color pipeline with calibrated exposure, a neutral baseline tone mapper, and grading/LUTs applied only after tone mapping.
- [ ] Photometric light units and exposure/debug readouts used by the actual Phase 3 lights and material validation path.
- [ ] Basic PBR shading with material IDs.
- [ ] Directional, point, and spot lights.
- [ ] Stable shadow-map base with explicit caster/material modes and capture/debug visibility; advanced receiver-aware exclusions remain Phase 9.
- [ ] Basic sky/atmosphere pass producing a visible sky and the lighting inputs required by the Phase 3 scene.
- [ ] Debug draw and overlays.
- [ ] Render-graph and scene-pass capture labels readable in RenderDoc/PIX/Nsight on every backend claimed by the item.

### Phase 3F: Platform Qualification Gate

Native Apple Silicon work is deferred to this platform-qualification gate because the current workspace has no native arm64 macOS execution environment and hosted GitHub Actions currently creates jobs but executes zero steps due to the account billing/spending-limit restriction. Cross-generation and source inspection are not Apple Silicon runtime evidence.

- [ ] Native Apple Silicon project generation, build, and MoltenVK editor-presentation verification on a native arm64 macOS environment.
- [ ] Production macOS renderer qualification after the shared Vulkan scene and render-graph paths exist: validate representative resources, commands, shaders, captures, packaging, profiling, and fallbacks through MoltenVK/NVRHI, or implement native Metal where measured gaps justify it.

Exit criteria:

- Simple scenes render with mesh materials, camera, lights, and shadows.
- The representative scene uses the shared render snapshot, portable shader contract, `Engine::RHI`, and executable render graph on every backend/device class claimed by the phase wording.
- CPU task scheduling, render preparation, command recording, GPU retirement, and transient reuse are observable and have deterministic validation modes.
- GPU captures are readable.
- Editor viewport is backed by the renderer, not special editor drawing.

## Phase 4: Non-Temporal Image Quality Baseline

Goal: establish the engine's motion-clarity promise before advanced ray features.

Required:

- [ ] Phase 4 image-quality capability group for sample counts, multisampled attachment/storage behavior, alpha-to-coverage, sample-rate interpolation, anisotropy, sampler LOD behavior, and resolve formats, with every selected fallback exercised before its consumer claim.
- [ ] Pass-level MSAA or analytic/fractional coverage strategy with declared pixel/sample-frequency behavior and resolves that preserve material IDs, normals, roughness, lighting, AO, and edge coverage.
- [ ] Alpha-to-coverage path for masked materials.
- [ ] SMAA/CMAA-style spatial cleanup.
- [ ] Specular antialiasing and roughness remapping.
- [ ] Correct mip selection and anisotropic filtering.
- [ ] Normal-map filtering.
- [ ] Stable LOD transition manager for conventional meshes and impostor/card swaps; Phase 7 extends it to virtual-geometry clusters.
- [ ] Motion-clarity suite covering camera pans/cuts, thin geometry, foliage, alpha masks, high-contrast specular, normal/roughness maps, and LOD changes at 30/60/90/120 Hz without temporal accumulation.
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
- [ ] Material texture-set compilation and channel packing by color space, mip behavior, compression tolerance, residency, and pass usage, including separate coverage/opacity data when depth/shadow passes do not need base-color RGB.
- [ ] CPU/GPU BRDF reference tests for finite outputs, grazing angles, energy behavior, parameter defaults/ranges, and Lambert-only debug fallback.
- [ ] Material calibration scene.
- [ ] OpenPBR/MaterialX import mapping.
- [ ] Tone mapper comparison: GT-style, AgX, ACES/filmic, Khronos PBR Neutral.

Exit criteria:

- Materials are validated in controlled lighting and compared against references.
- Lambert exists only as debug/low-end fallback.

## Phase 6: Visibility Buffer And Compact G-Buffer

Goal: move opaque rendering to the intended architecture.

Required:

- [ ] Phase 6 visibility/material capability group for `R32_UINT`, attribute reconstruction alternatives, descriptor/material-table limits, subgroup and indirect execution, and compact G-buffer formats, with explicit unsupported/fallback paths.
- [ ] Visibility buffer pass with `R32_UINT` ID.
- [ ] `drawClusterId:25 | localTriangleId:7` decode.
- [ ] `DrawClusterBuffer` and material table.
- [ ] Visible-attribute reconstruction: barycentric or functional fallback reconstruction, vertex/UV fetch, derivative recovery, and explicit texture gradients validated across D3D12 and Vulkan.
- [ ] Material resolve worklists sized for worst case.
- [ ] Compact G-buffer outputs.
- [ ] Selected occluder prepass: depth/coverage/visibility only.
- [ ] Two-pass HZB culling in the same per-view translated coordinate frame as scene rasterization.
- [ ] Coverage-aware carve-outs for foliage/hair/masked materials.
- [ ] Clustered Forward+ special/transparent pass reusing the Phase 3 light grid and Phase 5 material model for glass, eyes, hair, particles/VFX, and other visibility-buffer exclusions.
- [ ] Texture and geometry residency feedback emitted by visibility/material resolve for the Phase 7 streamers.
- [ ] Debug views: visibility ID, material ID, overdraw, quad waste, and absolute-vs-translated coordinate error.

Exit criteria:

- Opaque material evaluation happens once per visible pixel/sample.
- Visibility and material IDs are distinct and debuggable.

## Phase 7: Virtual Geometry, Terrain, And Asset Cooking

Goal: automate high-detail geometry and project-shaped terrain without inheriting subpixel instability or unbounded runtime work.

Required:

- [ ] Phase 7 virtual-geometry capability group for indirect draw count or bounded fallback, optional buffer device address, vertex limits, subgroup/culling support, and optional mesh shaders with indexed-indirect fallback.
- [ ] Meshlet/cluster builder.
- [ ] Vertex/index optimization and quantization.
- [ ] Projected-area/quad-utilization topology scoring.
- [ ] Curvature/silhouette-aware simplification.
- [ ] Versioned engine-native mesh cluster/page format with dependency metadata, integrity validation, and deterministic cook outputs.
- [ ] Coarse resident fallback pages.
- [ ] Asynchronous mesh-page residency system with feedback, upload, eviction, GPU-safe retirement, and nearest-resident fallback; render threads never block on storage/decompression.
- [ ] Virtual-texture page generation and runtime residency: mip tails, visibility/material feedback, async transcode/decompression/upload, GPU-safe page-table updates, eviction, and neutral fallbacks.
- [ ] Project-selectable terrain topology/profile and deterministic source-query contract covering bounded authored, large finite streamed, deterministic unbounded, and hybrid terrain without imposing one generator on every game.
- [ ] Versioned canonical terrain tile artifacts with signed coordinates, bounds/geometric error, shared-border/halo rules, requested world/material/gameplay channels, collision inputs, source/model/edit/cook provenance, dependencies, and semantic/cooked hashes.
- [ ] Imported/authorable finite heightfield baseline plus deterministic procedural-source conformance for seed stability, negative coordinates, traversal-order independence, shared edges, cancellation, cache eviction, and regeneration.
- [ ] Portable tiled-quadtree heightfield rendering with geometric-error selection, crack-free continuous LOD, immutable publication, and resident parent/coarse fallback; geometry clipmaps remain a measured optional candidate for very large regular heightfields.
- [ ] Terrain world-partition and residency scheduler with prioritized render/collision/navigation/gameplay radii, cancellable jobs, bounded CPU/GPU/disk caches, teleports, upload/retirement, and explicit failure fallbacks that never block the render thread.
- [ ] Hybrid terrain routing for caves, overhangs, cliffs, and project-selected voxel/SDF regions through the general mesh/virtual-geometry streamer, with explicit render/collision/material authority at representation boundaries.
- [ ] Terrain layer/cook pipeline for macro shape, regional hydrology/erosion, local detail, authored constraints and sparse edits, material/biome masks, roads/rivers/water, and downstream vegetation/navigation inputs with deterministic provenance.
- [ ] Terrain Diffusion evaluation at a pinned revision as an optional offline or asynchronous macro terrain/climate source, compared with imported and deterministic procedural baselines for seams, determinism, quality, latency, memory, renderer contention, portability, failure behavior, reproducibility, and license/data risk; record keep/defer/reject before admitting any model or Python/PyTorch/CUDA runtime boundary.
- [ ] Portable runtime GPU culling and LOD selection through compute plus indirect indexed draws, with mesh shaders only as a capability-gated fast path.
- [ ] GPU-driven instancing and selective static assembly/material consolidation that preserves occlusion bounds, streaming cells, lighting zones, LOD independence, and material quality.
- [ ] Stable ordered/complementary LOD transitions.
- [ ] Asset-class policies: static scans, foliage, skinned meshes, hair, wires, debris.
- [ ] DGF/DGFS evaluation path.
- [ ] RTX Mega Geometry/CLAS capability-gated evaluation with a representative benchmark and a recorded adopt/defer/reject decision; ordinary meshlet data remains the fallback.

Exit criteria:

- Dense scanned meshes stream and render with stable LODs.
- Importer prevents pathological subpixel/skinny-triangle workloads by default.
- Representative bounded and unbounded terrain profiles stream with stable crack-free LOD, bounded residency, authoritative collision readiness, deterministic regeneration, and measured failure fallbacks; hybrid terrain proves a cave or overhang without misrepresenting it as a heightfield.

## Phase 8: Probe Lighting, Lightmaps, And Daylight

Goal: create a stable indirect-lighting backbone for static and dynamic objects.

Required:

- [ ] Phase 8 probe/volume capability group for 3D images, required view compatibility or alternate layout, HDR/compressed formats, and sampling/storage limits.
- [ ] Production daylight controller and sky/sun/atmosphere model extending the calibrated Phase 3 lighting/color foundation.
- [ ] Adaptive probe volume.
- [ ] Unified `IndirectLightingSample`.
- [ ] Static/dynamic same-pass indirect lighting.
- [ ] Spatial GTAO/XeGTAO-style diffuse AO with bent normals, specular occlusion, confidence, and comparison against ray-traced ground truth; no temporal dependency.
- [ ] Current-frame screen-space GI/contact-bounce candidate with validity/confidence and probe/sky/ray fallback for missing data.
- [ ] Probe leak/confidence debug views.
- [ ] Portal/zone GI blending.
- [ ] Spatially resolved volumetric fog/froxel lighting using the same sky/probe/zone data, with no temporal baseline and a layout fallback when 2D views of 3D images are unavailable.
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

- [ ] Phase 9 ray-residual capability group for acceleration structures, ray pipelines/queries, shader-table requirements, update/compaction, queue/build limits, and exercised raster/probe fallback.
- [ ] `Engine::RHI` ray-tracing capability and resource contracts: acceleration structures, build/update/compaction, ray pipeline/shader-table binding, synchronization, diagnostics, and stable raster/probe fallback when unavailable.
- [ ] BLAS/TLAS object-class update policy.
- [ ] One per-view translated coordinate frame shared by ray generation, TLAS instance transforms, ray queries, and hit reconstruction.
- [ ] Ray-budget classifier.
- [ ] Receiver-aware shadow caster culling, per-material/object culling modes, proxy shadows, conservative exclusions, and reason/cost diagnostics.
- [ ] BRDF-aware current-frame stochastic SSR candidate pass with hierarchical traversal, spatial resolve, confidence, and explicit miss classification.
- [ ] Planar/reflected-scene path for bounded hero mirrors, water, glasses, and cinematics where it is cheaper or more reliable than full-rate rays.
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
- [ ] Character locomotion/controller-intent templates that publish root motion and desired motion through the future physics contract; collision resolution is Phase 11.
- [ ] Facial/eye/skin rendering hooks.
- [ ] Animation/render-facing cloth and hair attachment/deformation hooks; simulation ownership and contact are Phase 11.

Exit criteria:

- A user can import a humanoid, choose a starter locomotion pack, and get usable movement.

## Phase 11: Physics And Interaction

Goal: make contact/clipping quality a visible engine feature.

Required:

### Phase 11A: Physics Foundation And Authority

- [ ] Backend-neutral `Engine::Physics` world/API with generation-safe handles, backend isolation, capability reporting, and no Scene, Editor, or renderer-native type leakage.
- [ ] Engine-owned fixed-step clock and accumulator with configurable tick rate, bounded catch-up/overload policy, render interpolation, pause/single-step, and replayable tick numbering.
- [ ] Physics authority and determinism contract in the runtime: stable command/event ordering, deterministic single-thread validation, replay hashes, state snapshot/restore capability, and an explicit supported/unsupported matrix for same-build, thread-count, cross-platform, rollback, and network guarantees.
- [ ] Staged Scene-to-Physics commands and immutable Physics-to-Scene result/event snapshots on the CPU frame task graph, including origin shifts, late commands, destruction, and failed-tick behavior.
- [ ] Collision asset cooking and versioned runtime artifacts for primitives, compounds, convex hulls/decomposition, static triangle meshes, heightfields, character proxies/SDFs, and deformable shell/tet meshes, with deterministic semantic hashes and invalid-input diagnostics.
- [ ] Engine-owned backend conformance and bake-off harness; qualify architecture-fit candidate Box3D and maturity-control candidate Jolt first under identical assets/settings, and admit a dependency only after correctness, maturity, determinism, platform, tooling, memory, and p50/p95/p99 cost evidence.

### Phase 11B: Gameplay Collision And Queries

- [ ] Qualified CPU-authoritative rigid-body backend with body, shape, constraint, material, filter, event, threading, serialization, and fallback contracts on every claimed platform.
- [ ] General rigid bodies: broadphase, narrowphase, islands, constraints, sleeping/waking, triggers, and deterministic contact-event publication.
- [ ] Continuous collision detection and speculative-contact policy for fast translation/rotation, with per-body controls and tunneling fixtures.
- [ ] Snapshot-scoped immediate and asynchronous batched ray/shape-cast/overlap queries with filters, result epochs, cancellation/lifetime, stable ordering, and stale-result rules.
- [ ] Collision-resolved character controller/body pipeline consuming Phase 10 locomotion/root-motion intent, including slopes, steps, moving platforms, grounding, and push interaction.
- [ ] Character SDF/proxy workflow for body, cloth, and hair interaction, with a coarse portable fallback.

### Phase 11C: Deformables And Hero Contact

- [ ] Cloth, hair, and soft-body simulation contract plus an ordinary portable PBD/XPBD-class baseline with attachments, collision thickness, bounded substeps, and proxy/skinning fallback.
- [ ] CPU/GPU ownership and synchronization for optional deformation solvers: RHI capability gates, explicit queues/fences/publication epochs, no gameplay-critical same-step readback by default, memory budgets, and renderer-safe scheduling.
- [ ] Measured PD+barrier, IPC-family, ABD, and mixed-FEM hero/offline prototypes on representative assets and low/mid/high hardware while rendering; retain only paths that beat the portable baseline within stated quality and p50/p95/p99 budgets.
- [ ] Tiered over-budget/unsupported fallback: reduced iterations/resolution/frequency, then portable deformation, then proxy/skinning, without changing gameplay authority.

### Phase 11D: Diagnostics And Qualification

- [ ] Physics debug draw and profiler for shapes, contacts, broadphase/islands, constraints, sleeping, CCD/TOI, queries, substeps, solver residuals/termination, CPU/GPU time, queues/fences/transfers, memory, collision thickness, authority proxy, and fallback tier.
- [ ] Deterministic fixtures and stress suite covering replay/thread-count/platform hashes at every claimed level, stacks, joints, events, CCD, queries, origin shifts, cook golden files, pause/step/catch-up, scene mutation, characters, and GPU fallback/synchronization.
- [ ] Platform/backend performance and quality qualification with fixed settings, representative scenes, renderer contention, failure behavior from infeasible inputs, and documented determinism/capability scope.

Exit criteria:

- Fixed-step gameplay physics is stable and render-rate independent within documented catch-up limits.
- CPU-authoritative rigid bodies, character motion, contacts/events, and queries work on every claimed platform with reproducible focused tests.
- Cooked collision artifacts are versioned/reproducible and invalid inputs fail diagnostically.
- Optional GPU/hero paths never silently become gameplay authority, expose synchronization and cost, and have validated portable fallbacks.
- Representative hero contact materially reduces clipping within a measured budget; no absolute zero-penetration promise exceeds the qualified algorithm, tolerance, input, backend, and fallback scope.

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

- [ ] First deterministic non-AI vertical slice: extend Phase 2's project creator into a guided project-template workflow with game-type choices, an editable intent/plan preview, public editor commands, atomic undoable application, validation, and a provenance receipt.
- [ ] Extract only the stable model-neutral action, transaction, receipt, and deterministic headless-runner contracts proven by that workflow or a second concrete consumer; do not recreate a generic workflow scaffold in advance.
- [ ] Permission and approval policy that rejects unauthorized, stale, malformed, or unknown-version actions and blocks destructive, external, or sensitive actions until explicitly approved at the correct boundary.
- [ ] Declared action-effect semantics and recovery: atomic commit/complete rollback for transactional actions, explicit recorded compensation for compensatable actions, and just-in-time approval plus external-result recording for irreversible actions; include cancellation, retry/idempotency, and history-linked provenance with secret redaction.
- [ ] Live workflow progress, targeted mechanism explanations, interrupt/correct/resume, and complete durable handoff state without exposing private model reasoning.
- [ ] Provider-neutral AI planning/selection adapter over the same registered deterministic tools, with versioned scenario evaluation and no loss of the non-AI workflow.
- [ ] First playable workflow.
- [ ] Visual style workflow.
- [ ] Asset import workflow.
- [ ] Terrain/world workflow for profile and source selection, bounded/unbounded preview, generation provenance, affected-region regeneration, non-destructive local edits, gameplay constraints, validation, and expert overrides.
- [ ] USD/OpenAssetIO editor/studio pipeline evaluation for asset resolution, variants, and DCC workflows; shipping runtime data remains engine-cooked.
- [ ] Lighting bake/probe workflow.
- [ ] Motion pack workflow.
- [ ] Performance workflow.
- [ ] Packaging workflow.
- [ ] Every workflow's typed tool bridge has explainable previews/actions/results, undoable generated changes, validation, and provenance; no provider bypasses the shared command path.
- [ ] Validation and conformance matrix per workflow, including AI/non-AI semantic parity, permission rejection, injected transactional rollback, compensation success/failure, irreversible-action approval and external outcomes, retry/idempotency, provenance redaction, and exact user-visible outcomes.

Exit criteria:

- A beginner can create a playable prototype without manually discovering every system.
- Experts can inspect and override all automation.
- Model/provider unavailability never blocks a supported guided workflow, and committed changes remain attributable, validated, and undoable.

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
- [ ] Networking authority, prediction, replication, lockstep, and rollback evaluation for game types that need them, consuming Phase 11's measured physics determinism/state capabilities rather than choosing them after physics integration.

Exit criteria:

- Engine can support complete small games, not only rendering demos.

## Phase 15: Tooling, Profiling, And Validation

Goal: make performance and correctness visible.

Required:

- [ ] CPU profiler lanes and job timing.
- [ ] GPU pass timing.
- [ ] Memory profiler.
- [ ] Asset size and texture residency views.
- [ ] Terrain diagnostics for source/provenance, generation queues and latency, CPU/GPU/disk tile residency, cache hit/eviction, LOD/geometric error, seams, fallback state, and render/collision/navigation readiness.
- [ ] Overdraw and quad-waste views.
- [ ] Draw/dispatch/material-bin counters.
- [ ] Presentation telemetry validation with engine markers plus PresentMon or platform-equivalent evidence that correlates one frame ID across engine start, input/simulation, render submit, `Present`, GPU completion, and display; distinguishes start-to-start cadence, CPU active work, intentional pacing delay, present/display intervals, deadline misses, queue depth, sync/delivery mode, and rendered, presented, displayed, replaced, and dropped frames; and rejects hook-local Afterburner/RTSS-style flat graphs as sufficient proof.
- [ ] Shadow caster cost view.
- [ ] Ray density/confidence views.
- [ ] External capture docs for Intel GPA, Nsight, RGP/RMV/GPU Detective, PIX, RenderDoc.
- [ ] Golden image tests.
- [ ] Automated render scene suite.
- [ ] Offline/path-traced reference comparisons for material response, lighting, shadows, reflections, AO/GI, and motion-clarity failure scenes.

Exit criteria:

- Performance claims require in-engine profiler data plus at least one external capture.
- Regression tests catch rendering and asset-pipeline breakage.

## Phase 16: Platform, Packaging, And Distribution

Goal: ship games reliably.

Required:

- [ ] Packaged Player build and clean-machine launch verification on Windows, Linux, and macOS; Phase 0 editor/sandbox CI is foundation evidence only.
- [ ] Platform abstraction audit.
- [ ] Asset cooker and package format, including project-selected terrain source/model provenance, canonical terrain tiles, sparse edits or materialized shipping tiles, compatibility versions, and deterministic regeneration policy.
- [ ] Runtime player executable.
- [ ] Project templates.
- [ ] Installer/export pipeline.
- [ ] Production crash reporting and logs: packaged Player coverage, symbols/build identity, actionable reports, privacy/retention policy, and platform-appropriate collection; Phase 0 local crash files are the foundation only.
- [ ] Settings/scalability system.
- [ ] Optional DLSS/XeSS/FSR integrations as scalability features.
- [ ] Optional Work Graphs/device-generated commands, SER, Cooperative Vector, and neural/vendor fast-path evaluations with ordinary portable execution and content fallbacks.
- [ ] Backend-2 evaluation after render-graph conformance exists: NRI versus a custom Vulkan/D3D12 path, with no migration based on feature lists alone.
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
