# Current Handoff

Updated 2026-07-14. This file is a recovery aid, not roadmap authority; `PLAN.md` remains authoritative.

## Current Slice

Phase 3C's Windows D3D12 viewport-output prerequisite is complete. Vulkan is now honestly split into core resource/clear/readback, then SPIR-V/pipeline/indexed draw, then Scene-output/ImGui integration.

- The Vulkan context creates the one native device/queue and NVRHI device, then creates an `Engine::RHI::Device` wrapper around that NVRHI handle. It creates no second native device or raw Vulkan scene command path. Core supports NVRHI buffers, RGBA8/depth textures, output clear, explicit texture transitions, balanced debug markers, graphics command submission/wait/garbage collection, buffer update, and RGBA8 staging readback. Closed lists submit once only; open and previously submitted lists are rejected. Shader/pipeline/draw/query methods reject clearly.
- `--vulkan-rhi-core-smoke`, enabled by both `TestVulkan` scripts, creates an upload buffer plus 16x12 RGBA8/depth targets, rejects open and duplicate submission, proves that close rejects an unbalanced marker, issues a balanced marker with an owned null-terminated name, clears through `Engine::RHI`, reads RGBA8 through an NVRHI staging texture, validates extent/row pitch/every pixel (one byte tolerance), and emits `VulkanRHICoreV1` with adapter class and submission status. The marker status means the calls executed; it is not GPU-capture evidence.
- The D3D12 adapter creates texture-owned persistent CPU-only RTV/DSV descriptors at output-texture initialization. View binding validates usage, transitions texture state, and binds those existing non-shader-visible handles without per-frame descriptor-heap allocation; renderer-owned texture destruction releases the views. Scene marker lifetime is scope-bound, including output-bind/clear failure returns.
- The scene renderer now receives only `RHI::CommandList` and `RHI::Texture` references. It retains snapshot/raster behavior, per-draw constant-buffer lifetime, and shader validation. Presentation retains native command-list, swapchain, SRV/ImGui, and capture/readback ownership.

## Evidence

- MSVC Debug build: passed with zero warnings/errors.
- MSVC `EngineTests`: 35/35 passed.
- Local Windows only: `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022` passed on the selected Windows Vulkan device, including `lifecycle=pass`, `cpuMapNone=pass`, and `markers=executed-balanced` in `VulkanRHICoreV1`.
- Exact-head GitHub Actions run `29357979246` passed Code Style, Windows D3D12 regression, Ubuntu lavapipe Vulkan presentation/core smoke, macOS Apple-Paravirtual/MoltenVK presentation/core smoke, and all portable tests on commit `9c9570f`.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022`: passed. A/C captures were byte-identical; B shifted right by 196.24 pixels with a 13.20% non-background ratio.
- `Scripts/CheckCodeStyle.ps1`: passed.

This is real Windows x86_64/MSVC D3D12 viewport-output evidence. It is not Vulkan scene rendering or Vulkan handoff evidence.

## Limits And Next Work

The Vulkan RHI core and isolated SPIR-V indexed-draw items are complete. The hosted Windows job remains D3D12-only; its Vulkan evidence is the labeled local run, while hosted Ubuntu/macOS exercise their Vulkan paths. The next implementation item integrates the Scene viewport and the narrow completed-NVRHI-output-to-native-presentation/ImGui handoff.

The shared viewport shader renders a visible checker on stable per-face UVs plus antialiased luminous face frames, inset lines, and corner accents. This replaces the initial object-position quantization, whose `floor` boundary on constant face coordinates caused triangle-dependent precision striping when rotated. The refinement preserves the constant-buffer-only binding layout and adds no texture or sampler. Local D3D12 capture, Vulkan indexed-draw smoke (including unchanged deterministic interior/background pixels), and style pass; exact-head run `29363501290` previously passed the base UV correction on Windows D3D12, Ubuntu Vulkan, macOS MoltenVK, portable tests, and style. This is deliberately not a real texture/material path: sampled-resource and sampler bindings, texture upload/ownership, and material descriptors remain future infrastructure.

### Completed Phase 3C Indexed Draw

The scoped implementation adds SPIR-V-only RHI shader wrappers, reflected `ViewportConstants`/`POSITION0`/`COLOR0` pipeline validation, NVRHI framebuffer/binding/input-layout recording, and `--vulkan-rhi-indexed-draw-smoke`. It loads `Engine/Shaders/EditorViewport.hlsl` through `ShaderLibrary`, validates the existing package/reflection, and passes only SPIR-V to Vulkan. NVRHI graphics additionally gates and enables Vulkan 1.3 dynamic rendering/synchronization2, uses SPIR-V entry point `main`, and maps the current constant-buffer-only layout's HLSL `b0` to descriptor set 0/binding 0.

Local Windows Debug evidence passes: `VulkanRHIIndexedDrawV1` reports every stage `pass`, interior RGBA `210,59,40,255`, and 172 foreground pixels; `EngineTests` remains 35/35 and code style passes. Exact-head run `29361689869` passed Ubuntu lavapipe, macOS MoltenVK, the Windows D3D12 regression, all portable tests, and code style. The roadmap item is complete. No Scene snapshot or ImGui handoff was added.

## Working State

Baseline through Vulkan core is `ee42695`; indexed-draw implementation and hosted corrections are `256a31a`, `813fd9b`, and `f89f9d3`. Exact-head run `29361689869` is green. The first unchecked roadmap item is Vulkan Scene viewport integration and the narrow completed-NVRHI-output-to-native-presentation/ImGui handoff; it has not started.
