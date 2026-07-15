# Current Handoff

Updated 2026-07-15. This is a recovery aid; `PLAN.md` remains the sole roadmap and checkbox authority. A new agent must begin with `AGENTS.md`, `PLAN.md`, `Docs/README.md`, `Docs/ROADMAP_GOVERNANCE.md`, `Docs/VERIFICATION.md`, `Docs/Architecture/README.md`, and `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`.

## Completed Slice

The production buffer queue-ownership lifecycle and deterministic contract are implemented and checked. Portable buffer-only release/acquire descriptors are recorded through `RHI::CommandList`; each production D3D12/Vulkan device owns the shared `BufferOwnershipTracker` exercised by deterministic tests. Recording is private. Accepted release publishes one pending pair tied to its exact returned token without publishing destination owner/state. Acquire must match the exact resource, resolved queues, before/after states, and token, and its accepted submission must include that token exactly once before destination owner/state are published.

Exact-live-device, compatible usage/state, distinct-effective queue, correct-list, duplicate, dependency, and committed owner/state checks occur before native acceptance. Pending buffers reject ordinary transitions, uploads, copies, destruction, and second release. Failed release publishes nothing; failed acquire leaves pending real. Recovery is explicit and succeeds only after the exact release token completes, restoring source owner/before-state without claiming rollback.

D3D12 global-resource semantics are explicit: release emits no ownership barrier; acquire records the portable whole-buffer transition on the destination list, whose execution follows the GPU fence wait. Vulkan queue creation is unchanged and all requests still resolve to Graphics, so the shared same-effective rejection prevents any transfer state or family barrier. This slice adds no headed ownership smoke, texture parity, Vulkan queues/family barriers, RenderGraph execution, transients, or viewport work.

## Evidence

- Windows/MSVC Debug solution build passed with zero warnings/errors.
- `EngineTests`: 49/49 passed. The prior 47 remain; two focused cases cover private recording, exact live resources, state/usage/queue/pair/token validation, failed and accepted publication, duplicate/missing/wrong dependency and operation paths, pending guards, exact completed-token recovery, independent transfer retirement, D3D12 barrier policy, and Vulkan-style same-effective rejection.
- Existing `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022 -SkipBuild` passed on the local RTX 3080 Ti, including its normal independent and forced-Graphics queue-dependency regressions. It contains no ownership-lifecycle marker and is not native ownership qualification.
- Existing `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022` passed on the local Vulkan device. It contains no ownership-lifecycle marker and proves only the prior graphics-fallback regressions.
- `Scripts/TestInvokeBoundedProcess.ps1`, `Scripts/CheckCodeStyle.ps1`, `git diff --check`, and tracked project Markdown link validation passed.
- Root committed the preserved owner changes as `0a4fb2e`. Exact-head CI [run 29415941902](https://github.com/ClGratton/Spiral/actions/runs/29415941902) passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, and code style; dependency submission [run 29415941895](https://github.com/ClGratton/Spiral/actions/runs/29415941895) passed. This is compilation/regression evidence for the checked deterministic-contract gate, not native ownership qualification.

## Next Ordered Work

The first unchecked item is real-device qualification of this exact buffer lifecycle: D3D12 independent queues with no CPU wait, deterministic bytes, final owner/state, recovery, and retirement; forced D3D12 and current Vulkan Graphics fallbacks must prove they create no transfer state. No implementation-only evidence in this handoff completes that gate. Texture parity follows afterward.

## Working State

Implementation `0a4fb2e` is on `origin/main` with completed hosted compilation/regression evidence. The evidence-only documentation commit following it should be current head, and the working tree must be clean. The next owner starts from the real-device buffer queue-ownership qualification item and must not infer that the deterministic contract already proves native transfer behavior.
