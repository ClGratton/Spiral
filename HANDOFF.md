# Current Handoff

Updated 2026-07-14. This file is a recovery aid, not roadmap authority; `PLAN.md` remains authoritative.

## Current Slice

Phase 3C's Windows D3D12 viewport-output prerequisite is complete. Vulkan is now honestly split into core resource/clear/readback, then SPIR-V/pipeline/indexed draw, then Scene-output/ImGui integration.

- The Vulkan context creates the one native device/queue and NVRHI device, then creates an `Engine::RHI::Device` wrapper around that NVRHI handle. It creates no second native device or raw Vulkan scene command path. Core supports NVRHI buffers, RGBA8/depth textures, output clear, explicit texture transitions, debug markers, graphics command submission/wait/garbage collection, buffer update, and RGBA8 staging readback. Shader/pipeline/draw/query methods reject clearly.
- `--vulkan-rhi-core-smoke`, enabled by both `TestVulkan` scripts, creates an upload buffer plus 16x12 RGBA8/depth targets, clears through `Engine::RHI`, reads RGBA8 through an NVRHI staging texture, validates extent/row pitch/every pixel (one byte tolerance), and emits `VulkanRHICoreV1` with adapter class and submission status.
- The D3D12 adapter creates texture-owned persistent CPU-only RTV/DSV descriptors at output-texture initialization. View binding validates usage, transitions texture state, and binds those existing non-shader-visible handles without per-frame descriptor-heap allocation; renderer-owned texture destruction releases the views. Scene marker lifetime is scope-bound, including output-bind/clear failure returns.
- The scene renderer now receives only `RHI::CommandList` and `RHI::Texture` references. It retains snapshot/raster behavior, per-draw constant-buffer lifetime, and shader validation. Presentation retains native command-list, swapchain, SRV/ImGui, and capture/readback ownership.

## Evidence

- MSVC Debug build: passed with zero warnings/errors.
- MSVC `EngineTests`: 35/35 passed.
- `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022`: passed with the core marker on the local selected Windows Vulkan device.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022`: passed. A/C captures were byte-identical; B shifted right by 196.24 pixels with a 13.20% non-background ratio.
- `Scripts/CheckCodeStyle.ps1`: passed.

This is real Windows x86_64/MSVC D3D12 viewport-output evidence. It is not Vulkan scene rendering or Vulkan handoff evidence.

## Limits And Next Work

The first core item remains unchecked until hosted Windows, Ubuntu lavapipe, and macOS Apple Paravirtual/MoltenVK execute the new marker. The next implementation item consumes SPIR-V and adds reflected pipeline/binding/input-layout plus deterministic indexed offscreen draw; it must not claim Scene or ImGui integration.

## Working State

Baseline includes documentation commit `842929f` on top of `3c1ac41`; preserve both. Implementation commit `7cd657b` is pushed to `main`. GitHub Actions run `29357129469` is queued for that push; it is the pending Windows/Ubuntu/macOS core-marker evidence. The working tree should otherwise be clean; generated build output is ignored.
