# Current Handoff

Updated 2026-07-15. This is a recovery aid; `PLAN.md` remains the sole roadmap and checkbox authority. A new agent must begin with `AGENTS.md`, `PLAN.md`, `Docs/README.md`, `Docs/ROADMAP_GOVERNANCE.md`, `Docs/VERIFICATION.md`, `Docs/Architecture/README.md`, and `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`.

## Completed Slice

Phase 3's RHI queue-topology/GPU-dependency prerequisite is implemented and checked. `Engine::RHI` publishes requested/effective queue resolution plus independent/fallback identity and accepts an ordered list of prior opaque completion tokens on submission. The shared monotonic validator rejects zero, foreign, absent, duplicate, prospective-self, and forward identities before native submission; accepting only issued smaller identities makes cycles unrepresentable.

D3D12 creates command lists for the effective queue. Distinct enabled Copy/Compute queues use GPU queue fence waits against already-signaled producer values; same-effective Graphics fallback relies on ordered submission and emits no wait. Queue identity is based on distinct created objects rather than non-null aliases. The forced diagnostic mode suppresses independent queue creation and exercises the same portable command/dependency path through Graphics. Vulkan queue creation remains unchanged and resolves requested Copy/Compute work to the one enabled Graphics queue.

This slice does not change RenderGraph execution, add Vulkan multi-queue admission or ownership transfers, allocate transients, or migrate viewport work.

## Evidence

- Windows/MSVC Debug solution build passed with zero warnings/errors.
- `EngineTests`: 47/47 passed. The two added tests cover requested/effective/independent resolution; one and multiple dependencies in caller order; same-effective elision versus distinct-effective waits; zero, foreign, unissued, duplicate, forward/cycle-forming, and self rejection; no native acceptance/token/state publication after validation failure; valid later publication; and independent token retirement/queryability. The prior 45 tests remain.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022 -SkipBuild` passed. On the local RTX 3080 Ti, normal D3D12 reported independent Copy and Compute, Copy-to-Graphics and Graphics-to-Compute `gpu-wait`, no CPU wait between submissions, exact 4 KiB output, committed `CopySource`, and retirement. Its second forced-fallback launch reported both classes as `graphics-fallback` and both dependencies `ordered-elided` with identical bytes/state.
- `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022` passed. Vulkan reported truthful Copy/Compute Graphics fallback, both dependencies `ordered-elided`, no CPU wait, and token retirement; buffer-copy bytes/state are explicitly not claimed.
- `Scripts/CheckCodeStyle.ps1`, `git diff --check`, and tracked-Markdown link validation passed.
- Hosted CI evidence is pending until the scoped implementation commit is pushed.

## Next Ordered Work

The first unchecked `PLAN.md` item is Vulkan/NVRHI multi-queue admission: enumerate and create selected Compute/Copy queues/families, retain Graphics fallback, distinguish same-family ordering from cross-family release/acquire ownership transfer, and qualify only exercised support. The following item translates compiled RenderGraph dependencies into multi-queue RHI submissions. Do not silently combine either gate with transient allocation or Scene viewport adoption.

## Working State

The scoped implementation is ready for commit and push after the final focused test rebuild caused by the last assertion-only edit. After push, record the exact hosted implementation/dependency run IDs and outcomes here. The working tree must be clean at handoff.
