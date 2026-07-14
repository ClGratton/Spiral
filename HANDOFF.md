# Current Handoff

Updated 2026-07-14. This file is a recovery aid, not roadmap authority; `PLAN.md` remains authoritative.

## Current Slice

### Latest: Phase 3C Render-Graph Construction

The first graph-construction roadmap item is implemented and checked. `RenderGraph` validates logical pass/resource declarations (read/write/read-write, queue/state/stage intent), derives RAW/WAR/WAW and explicit-order edges, rejects invalid handles/declarations, transient read-before-write, and cycles, and stable-topologically orders passes by registration index among ready peers. It calculates resource lifetimes in that compiled order and produces abstract state barriers plus cross-queue transition records only where an ordered resource dependency crosses queues.

This is intentionally a compiler-only plan: there are no pass callbacks, physical/imported resource bindings, RHI barrier emission, command recording, queue signal/wait submission, transient allocation/reuse, GPU retirement, or viewport integration. The next ordered roadmap item owns those mechanisms.

Focused local Windows MSVC Debug evidence: `EngineTests` builds and passes 38/38, including graph-order/lifetime, RAW/WAR/WAW/barrier/queue-transition, and invalid-declaration/cycle contract cases. Exact-head CI run `29371379723` passed Code Style, Windows D3D12 regression/`EngineTests`, Ubuntu portable build/tests plus lavapipe Vulkan smoke, and macOS portable build/tests plus MoltenVK smoke for commit `5921701`; dependency-submission run `29371379763` also passed.

Phase 3C's Vulkan completed-output-to-native-ImGui/swapchain handoff is complete and checked with local Windows and exact-head hosted evidence.

- The Vulkan context creates the one native device/queue and NVRHI device, then creates an `Engine::RHI::Device` wrapper around that NVRHI handle. It creates no second native device or raw Vulkan scene command path. Core supports NVRHI buffers, RGBA8/depth textures, output clear, explicit texture transitions, balanced debug markers, graphics command submission/wait/garbage collection, buffer update, and RGBA8 staging readback. Closed lists submit once only; open and previously submitted lists are rejected. Shader/pipeline/draw/query methods reject clearly.
- `--vulkan-rhi-core-smoke`, enabled by both `TestVulkan` scripts, creates an upload buffer plus 16x12 RGBA8/depth targets, rejects open and duplicate submission, proves that close rejects an unbalanced marker, issues a balanced marker with an owned null-terminated name, clears through `Engine::RHI`, reads RGBA8 through an NVRHI staging texture, validates extent/row pitch/every pixel (one byte tolerance), and emits `VulkanRHICoreV1` with adapter class and submission status. The marker status means the calls executed; it is not GPU-capture evidence.
- The D3D12 adapter creates texture-owned persistent CPU-only RTV/DSV descriptors at output-texture initialization. View binding validates usage, transitions texture state, and binds those existing non-shader-visible handles without per-frame descriptor-heap allocation; renderer-owned texture destruction releases the views. Scene marker lifetime is scope-bound, including output-bind/clear failure returns.
- The scene renderer now receives only `RHI::CommandList` and `RHI::Texture` references. It retains snapshot/raster behavior, per-draw constant-buffer lifetime, and shader validation. Presentation retains native command-list, swapchain, SRV/ImGui, and capture/readback ownership.
- `NVRHIVulkanViewportSceneRenderer` reuses the renderer-published immutable snapshot, `PrepareSceneRasterFrame`, and `EditorViewport.hlsl` SPIR-V package. It creates no native Vulkan object or command path. It owns RGBA8/depth output replacement after synchronous GPU retirement, records NVRHI/RHI output bind/clear/draw/transition commands, and its smoke readbacks the final 64x48 color after a 48x36-to-64x48 resize. `VulkanSceneViewportRasterV1` requires raster/readback/geometry/background/resize pass states, final row layout, the clear background, and a bounded cube foreground count. It does not expose a texture to ImGui or a swapchain.
- The real Vulkan editor viewport now renders that same completed Scene output, leaves it in shader-resource/read-only layout, and exposes only a borrowed NVRHI image/view to `NVRHIVulkanPresentation`. Presentation owns its ImGui sampler/descriptor registration and removal. Before an output resize/replacement it waits for the one shared queue to retire, removes the descriptor, then the renderer replaces and rasterizes the target; shutdown follows the same retired removal order. `VulkanSceneOutputCaptureV1` validates actual post-resize renderer-output content, while `VulkanSceneOutputHandoffV1` is emitted only after Editor queued the matching descriptor and the native swapchain present succeeded. Local Windows output generations 3/4 and swapchain generation 2 passed; no raw Vulkan Scene command path or second device was added.

## Evidence

- MSVC Debug rebuild: passed with zero warnings/errors.
- MSVC `EngineTests`: 35/35 passed.
- Local Windows only: `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022` passed on the selected Windows Vulkan device, including post-resize `VulkanSceneOutputCaptureV1 ... capture=pass` and `VulkanSceneOutputHandoffV1 ... imgui=queued present=pass swapchainGeneration=2`.
- Exact-head GitHub Actions run `29369950355` passed Code Style, Windows D3D12 regression, Ubuntu lavapipe Vulkan Scene-output handoff smoke, macOS Intel Apple-Paravirtual/MoltenVK Scene-output handoff smoke across three launches, and all portable tests on commit `45878d0`.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022`: passed. A/C captures were byte-identical; B shifted right by 196.24 pixels with a 13.20% non-background ratio.
- `Scripts/CheckCodeStyle.ps1`: passed.

This is real local Windows x86_64/MSVC D3D12 regression and Vulkan Scene-offscreen evidence. It is not Vulkan ImGui/swapchain handoff, Linux/macOS Scene evidence, or physical-device breadth qualification.

## Limits And Next Work

The Vulkan RHI core, isolated SPIR-V indexed draw, Scene-output raster, output-to-ImGui/swapchain handoff, and deterministic frame/render graph construction are complete. The hosted Windows job remains D3D12-only; Ubuntu lavapipe and macOS Intel Apple-Paravirtual/MoltenVK handoff evidence is exact-head CI `29369950355`, not physical-device breadth. Render-graph construction evidence is exact-head CI `29371818096` plus dependency submission `29371818107`. The former combined execution/integration item is split into execution core, cross-queue submission, and representative Scene-viewport adoption. Execution-core evaluation found that construction emits buffer barriers while `RHI::CommandList` exposes only texture transitions; the new first unchecked prerequisite adds backend-neutral D3D12/Vulkan buffer transition recording before execution resumes.

The shared viewport shader renders a visible checker on stable per-face UVs plus antialiased luminous face frames, inset lines, and corner accents. This replaces the initial object-position quantization, whose `floor` boundary on constant face coordinates caused triangle-dependent precision striping when rotated. The refinement preserves the constant-buffer-only binding layout and adds no texture or sampler. Local D3D12 capture, Vulkan indexed-draw smoke (including unchanged deterministic interior/background pixels), and style pass; exact-head run `29363501290` previously passed the base UV correction on Windows D3D12, Ubuntu Vulkan, macOS MoltenVK, portable tests, and style. This is deliberately not a real texture/material path: sampled-resource and sampler bindings, texture upload/ownership, and material descriptors remain future infrastructure.

### Completed Phase 3C Indexed Draw

The scoped implementation adds SPIR-V-only RHI shader wrappers, reflected `ViewportConstants`/`POSITION0`/`COLOR0` pipeline validation, NVRHI framebuffer/binding/input-layout recording, and `--vulkan-rhi-indexed-draw-smoke`. It loads `Engine/Shaders/EditorViewport.hlsl` through `ShaderLibrary`, validates the existing package/reflection, and passes only SPIR-V to Vulkan. NVRHI graphics additionally gates and enables Vulkan 1.3 dynamic rendering/synchronization2, uses SPIR-V entry point `main`, and maps the current constant-buffer-only layout's HLSL `b0` to descriptor set 0/binding 0.

Local Windows Debug evidence passes: `VulkanRHIIndexedDrawV1` reports every stage `pass`, interior RGBA `210,59,40,255`, and 172 foreground pixels; `EngineTests` remains 35/35 and code style passes. Exact-head run `29361689869` passed Ubuntu lavapipe, macOS MoltenVK, the Windows D3D12 regression, all portable tests, and code style. The roadmap item is complete. No Scene snapshot or ImGui handoff was added.

## Working State

Baseline through Vulkan core is `ee42695`; indexed-draw implementation and hosted corrections are `256a31a`, `813fd9b`, and `f89f9d3`. Vulkan Scene raster/readback is committed as `f2ff2f6`; exact-head run `29368558656` passed Code Style, Windows D3D12 regression, Ubuntu lavapipe Vulkan, macOS MoltenVK, and all portable smoke/test steps. The raster checklist item is complete. The native completed-output-to-presentation/ImGui handoff is the first unchecked item.
