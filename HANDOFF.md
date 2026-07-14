# Current Handoff

Updated 2026-07-14. This file is a recovery aid, not roadmap authority; `PLAN.md` remains authoritative.

## Current Slice

Phase 3C's Windows D3D12 viewport-output prerequisite is complete. The Vulkan line was also over-bundled and is now split in `PLAN.md`: first implement and exercise wrapped-NVRHI Vulkan RHI primitives via deterministic offscreen draw/readback; then integrate a Scene snapshot and the narrow completed-output-to-ImGui handoff.

- `RHI::CommandList` binds renderer-owned color/depth outputs, deterministically clears them, and explicitly transitions a texture. Existing RHI viewport/scissor/pipeline/constant-buffer/draw commands complete the recording path.
- The D3D12 adapter creates texture-owned persistent CPU-only RTV/DSV descriptors at output-texture initialization. View binding validates usage, transitions texture state, and binds those existing non-shader-visible handles without per-frame descriptor-heap allocation; renderer-owned texture destruction releases the views. Scene marker lifetime is scope-bound, including output-bind/clear failure returns.
- The scene renderer now receives only `RHI::CommandList` and `RHI::Texture` references. It retains snapshot/raster behavior, per-draw constant-buffer lifetime, and shader validation. Presentation retains native command-list, swapchain, SRV/ImGui, and capture/readback ownership.

## Evidence

- MSVC Debug build: passed with zero warnings/errors.
- MSVC `EngineTests`: 35/35 passed.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022`: passed. A/C captures were byte-identical; B shifted right by 196.24 pixels with a 13.20% non-background ratio.

This is real Windows x86_64/MSVC D3D12 viewport-output evidence. It is not Vulkan scene rendering or Vulkan handoff evidence.

## Limits And Next Work

Vulkan RHI resources/submission are not implemented. The current SPIR-V package is compilation/reflection/convention evidence only. The next item must create an `Engine::RHI::Device` adapter around the context's existing `nvrhi::DeviceHandle`, then prove the exact primitive group through an NVRHI offscreen indexed draw and NVRHI staging readback. Do not create a second Vulkan device/queue, leak native handles into renderer APIs, consume a Scene snapshot, or add an ImGui texture bridge in that prerequisite. Hosted Windows, Ubuntu lavapipe, and macOS MoltenVK evidence is required before checking it.

## Working State

Baseline was clean `main` at `3c1ac41334a31aba9a9a9a3ed02bf326b6d35b64`; user reports CI run `29355748666` green. The current uncommitted documentation-only split changes `PLAN.md`, `Docs/Architecture/RENDERER_CAPABILITY_CONTRACT.md`, and this handoff. No build artifacts were produced.
