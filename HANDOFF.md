# Current Handoff

Updated 2026-07-14. This file is a recovery aid, not roadmap authority; `PLAN.md` remains authoritative.

## Current Slice

Phase 3C's Windows D3D12 viewport-output prerequisite is complete. The prior line was over-bundled: a backend-neutral NVRHI-output-to-native-presentation/ImGui handoff cannot be behaviorally exercised before Vulkan scene output exists. That narrow handoff is explicit required scope of the now-first unchecked Vulkan scene item.

- `RHI::CommandList` binds renderer-owned color/depth outputs, deterministically clears them, and explicitly transitions a texture. Existing RHI viewport/scissor/pipeline/constant-buffer/draw commands complete the recording path.
- The D3D12 adapter creates texture-owned persistent CPU-only RTV/DSV descriptors at output-texture initialization. View binding validates usage, transitions texture state, and binds those existing non-shader-visible handles without per-frame descriptor-heap allocation; renderer-owned texture destruction releases the views. Scene marker lifetime is scope-bound, including output-bind/clear failure returns.
- The scene renderer now receives only `RHI::CommandList` and `RHI::Texture` references. It retains snapshot/raster behavior, per-draw constant-buffer lifetime, and shader validation. Presentation retains native command-list, swapchain, SRV/ImGui, and capture/readback ownership.

## Evidence

- MSVC Debug build: passed with zero warnings/errors.
- MSVC `EngineTests`: 35/35 passed.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022`: passed. A/C captures were byte-identical; B shifted right by 196.24 pixels with a 13.20% non-background ratio.

This is real Windows x86_64/MSVC D3D12 viewport-output evidence. It is not Vulkan scene rendering or Vulkan handoff evidence.

## Limits And Next Work

Vulkan scene resources/submission are not implemented. The current SPIR-V package is compilation/reflection/convention evidence only. Implement the now-first unchecked Vulkan scene viewport item through wrapped `nvrhi::DeviceHandle`, including the narrow completed-NVRHI-output-to-native-presentation/ImGui handoff; do not leak raw Vulkan above presentation.

## Working State

Commit `293fb8e` (`Add D3D12 RHI viewport output recording`) is pushed to `main`. Its dependency-submission workflow run [29355459931](https://github.com/ClGratton/Spiral/actions/runs/29355459931) passed. The corresponding main CI run [29355459967](https://github.com/ClGratton/Spiral/actions/runs/29355459967) was still in progress at the prior handoff; record its final result if available. This follow-up correction will trigger a newer main CI run; inspect `git status --short` before continuing and preserve unrelated changes.
