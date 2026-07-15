# Current Handoff

Updated 2026-07-15. This is a recovery aid; `PLAN.md` remains the sole roadmap and checkbox authority. A new agent must begin with `AGENTS.md`, `PLAN.md`, `Docs/README.md`, `Docs/ROADMAP_GOVERNANCE.md`, `Docs/VERIFICATION.md`, `Docs/Architecture/README.md`, and `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`.

## Completed Slice

Phase 3's frame/render-graph execution core is implemented, checked, committed as `c8468e9`, and pushed to `main`. The backend-neutral graphics-only executor:

- validates every explicit physical texture/buffer binding for exact device ownership, kind, full description/usage/extent or size, and committed imported initial state before recording;
- exposes only declared resources and the assigned `Engine::RHI::CommandList` through a restricted per-pass context;
- records compiler-derived same-queue texture/buffer transitions and callbacks in deterministic compiled order;
- stops before later callbacks or success on validation, access, callback, recording, submission, or completion failure;
- publishes imported final states only after `Device::Submit` accepts the list and returns a valid completion token; and
- manages a bounded three-context pool, reports exhaustion while every token is incomplete, and reuses only the exact context/list whose GPU token retired.

The executor stays on `Engine::RHI`. It does not implement cross-queue graph signal/wait submission, transient resource allocation/reuse, or Scene viewport adoption.

## Evidence

- Windows/MSVC Debug build: passed with zero warnings/errors.
- `EngineTests`: 45/45 passed. Focused cases cover binding/state rejection before recording, undeclared and wrong-kind access, callback/completion failure propagation, deterministic barriers/callbacks/final states, three-token exhaustion, and exact retired-context reuse.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022`: passed real D3D12 `RenderGraphExecutionSmokeV1`.
- `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022`: passed real Vulkan/NVRHI `RenderGraphExecutionSmokeV1`.
- Both real-device smokes execute two imported-texture clear/finalize passes twice, require three graph-derived transitions and ordered callbacks, reject undeclared access, validate compact deterministic 3x2 RGBA8 readback, observe token retirement, and reuse the same retired context.
- `Scripts/CheckCodeStyle.ps1`, `git diff --check`, and Markdown-link validation passed.
- Exact-head implementation CI [run 29378695587](https://github.com/ClGratton/Spiral/actions/runs/29378695587) passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, and code style for `c8468e9`. Dependency submission [run 29378695596](https://github.com/ClGratton/Spiral/actions/runs/29378695596) passed.

The immediate RHI prerequisites are also complete and documented in `PLAN.md` and `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`: portable buffer transitions, asynchronous completion tokens and recording reuse, exact resource/device ownership validation, D3D12 RHI texture-readback parity, and committed portable resource-state queries.

## Next Ordered Work

The cross-queue item was evaluated after the single-queue executor completed and split into dependency-ordered gates. The first unchecked `PLAN.md` item is now the RHI queue-topology and GPU-dependency prerequisite: distinguish independent queues from graphics fallback and add same-device GPU-side submission dependencies, first exercised on D3D12 without changing RenderGraph execution. The following Vulkan/NVRHI admission gate enables and qualifies real compute/copy queues and same-family versus cross-family semantics while preserving graphics-only fallback. Only then does the graph consumer translate compiled dependencies into multi-queue submissions. Do not silently combine these gates with transient allocation or Scene viewport adoption.

All 45 current deterministic `EngineTests` remain intentional. The render-graph/compiler/executor cases cover different ordering, declaration, binding, failure, state-publication, pool-exhaustion, and exact-retirement invariants; no touched test was found redundant. Add focused queue-resolution, dependency, fallback, partial-prefix, and multi-queue retirement coverage rather than weakening existing contracts.

## Working State

Implementation commit `c8468e9` is on `origin/main`. The evidence-only documentation commit made from this handoff should be the current head, and the working tree must be clean before further work.
