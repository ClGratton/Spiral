# Workspace Agent Instructions

These instructions are the persistent operating contract for any AI or human agent working in this repository. Do not rely on chat memory, screenshots, or another agent's summary when repository evidence is available.

## Start Here

Before roadmap, architecture, or implementation work:

1. Read this file.
2. Read [PLAN.md](PLAN.md) for current state and execution order.
3. Read [Docs/README.md](Docs/README.md) for the complete documentation catalog, authority rules, and update responsibilities.
4. Read [Docs/ROADMAP_GOVERNANCE.md](Docs/ROADMAP_GOVERNANCE.md) before changing roadmap wording or checkboxes.
5. Read [Docs/VERIFICATION.md](Docs/VERIFICATION.md) before claiming behavior is complete.
6. Read the task-relevant architecture contracts linked by [Docs/Architecture/README.md](Docs/Architecture/README.md) and the nearest `OWNERSHIP.md` before editing a subsystem.

For a broad architecture audit, read every document listed in `Docs/README.md`. For a bounded implementation, read the mandatory set above plus every contract named for that subsystem; do not substitute a stale “first implementation order” in a research document for `PLAN.md`.

## Documentation Authority And Conflicts

- `PLAN.md` alone owns current implementation state, roadmap order, and completion checkboxes.
- `PRODUCT.md` owns product goals and user-facing principles. `DESIGN.md` owns editor visual and interaction rules.
- `Docs/Architecture/*` owns technical decisions and implementation contracts. ADRs record choices and their consequences; research/reference documents do not override accepted contracts.
- `Docs/DEPENDENCIES.md` owns admitted dependency versions, licenses, and integration boundaries.
- `OWNERSHIP.md` files own directory/module scope and forbidden dependencies.
- Source code, tests, runtime captures, and CI are the evidence of actual behavior. When prose disagrees with evidence, do not rationalize the mismatch: report it and update the relevant current-state or contract document in the same scoped change.
- A user instruction can change project direction. If that change is intended to persist, update the repository documents that future agents will read; do not leave the decision only in chat.
- If two authoritative documents conflict and the intended resolution cannot be established from code, tests, roadmap order, or an accepted ADR, stop and flag the decision instead of silently choosing one.

## Research Fidelity And Mechanism Preservation

- Treat user-supplied chats, transcripts, screenshots, diagrams, and external critiques as source material. Before compressing them into a decision, extract the named mechanisms, operation order, timing/control point, defaults, alternatives, trade-offs, caveats, and unresolved claims.
- Do not replace a specific mechanism with a broader label when that loses the reason it was supplied. For example, an RTSS-ASYNC-inspired inter-frame wait, a submission gate, Front Edge Sync immediately before `Present`, and Reflex-style latency control are distinct hypotheses; “frame pacing” is not an adequate substitute for documenting their different control points.
- Separate four evidence levels in durable docs: the source's claim, the agent's inference, the accepted project decision, and behavior verified in this repository. Community posts and assistant chats may motivate a bake-off, but they are not implementation facts until confirmed by primary documentation, code inspection, or engine-owned measurement.
- When a supplied source changes project direction, write the mechanism and its provenance into the authoritative architecture contract and put implementation/verification work in `PLAN.md`. Do not leave the only recoverable version in chat or reduce it to a generic summary future agents can misread.
- Before handoff, compare the edited contract element-by-element with the supplied source. If a named mode, ordering constraint, default, or warning was intentionally rejected, record that rejection and why instead of silently omitting it.

## Workspace Scope Map

| Path | Scope |
| --- | --- |
| `Engine/src/Engine/Core` | Application lifecycle, layers, windows, logging, assertions, command-line handling, and lightweight utilities. Must not depend on renderer, scene, assets, scripting, or editor code. |
| `Engine/src/Engine/RHI` | Backend-neutral GPU device/resource/command contracts and NVRHI-backed implementations. No scene, material, or editor policy. |
| `Engine/src/Engine/RenderGraph` | Pass/resource declarations, dependency compilation, lifetime/state planning, execution scheduling, and transient-resource policy as those stages are implemented. |
| `Engine/src/Engine/Renderer` | High-level rendering, presentation bridges, shader management, scene rendering, and renderer diagnostics. Depends on RHI/render graph; does not own gameplay entities. |
| `Engine/src/Engine/Scene` | Entity/component authoring facade, scene data, serialization, cameras, and future runtime extraction. No editor panels or backend-native GPU types. |
| `Engine/src/Engine/Assets` | Asset identity, import, metadata, cooked artifacts, dependency tracking, reimport, and streaming inputs. It does not render. |
| `Engine/src/Engine/Jobs` | Worker scheduling and task dependencies. It does not own renderer, scene, asset, or editor policy. |
| `Engine/src/Engine/Terrain` (planned; absent until Phase 7) | Project-selectable terrain topology and source contracts, canonical tile identity/artifacts, generation scheduling, caches, edit layers, provenance, and diagnostics publication. It must not own renderer passes, the physics world, Scene entities, editor UI, or native graphics types. |
| `Engine/src/Engine/Physics` (planned; absent until Phase 11) | Backend-neutral fixed-step physics world, handles, commands/results, collision/query contracts, state capabilities, and diagnostics publication. It must not own Scene entities, asset importing, editor UI, renderer passes, or native graphics types. |
| `Engine/src/Engine/Platform` | GLFW/headless and future OS services hidden behind engine interfaces. |
| `Engine/src/Engine/UI` | Engine/editor tool UI integration such as ImGui. Native graphics access is limited to documented UI/presentation bridges. |
| `Engine/src/Engine/Diagnostics` | Crash reporting, profiling contracts, logs, captures, and diagnostic data surfaces. |
| `Engine/src/Engine/Automation` | Deterministic workflow contracts and future agent/editor automation through normal engine APIs. |
| `Editor` | Panels, inspectors, viewports, authoring workflows, settings UI, and editor orchestration. It is a client of Engine. |
| `Sandbox` | Public engine API proving ground; no editor-private access. |
| `Tests` | Deterministic contract and integration tests. Tests may consume public/test-facing APIs but must not become production behavior. |
| `Scripts` | Reproducible setup, generation, build, style, smoke, and validation entry points. |
| `.github/workflows` | Hosted verification matching the scripts; do not invent CI-only behavior when a reusable script is practical. |
| `Vendor` | Pinned third-party source. Do not edit casually or admit a dependency without updating `Docs/DEPENDENCIES.md`. |

More detailed engine/editor/sandbox boundaries live in their nearest `OWNERSHIP.md`. When a new top-level module or materially different responsibility is added, update this table, `Docs/README.md`, and the nearest ownership document in the same change.

## Verification

- Follow [Docs/VERIFICATION.md](Docs/VERIFICATION.md) and use the smallest test set that exercises the changed behavior plus proportionate regression coverage.
- For frame pacing or latency work, never accept one hook-local frametime graph as evidence. Carry one frame ID through engine frame start, input/simulation, render submission, `Present`, GPU completion, and display feedback; report start-to-start cadence, active work, intentional pacing wait, present cadence, and display cadence separately. A limiter/overlay graph sampled near its own delay point may look flat while visible delivery still stutters.
- When a completed feature can be exercised locally, test the new behavior itself rather than relying only on compilation.
- For editor-facing changes, run the editor and inspect a screenshot when practical. Use an existing automated smoke test when it covers the interaction; otherwise add focused coverage that does.
- For platform/backend claims, run that backend. If local hardware is unavailable and the repository has matching hosted CI, push the scoped change and use the completed job as evidence.
- Agent reports, source inspection, successful compilation, and a matching log marker are not substitutes for the required runtime behavior.
- Report verification that could not be performed and the reason.

## Roadmap Integrity

- In `PLAN.md`, `[x]` means the exact behavior written on that line is implemented, integrated into its real workflow, and verified. Compilation alone is not enough for a runtime or editor-facing feature.
- Never check an item whose delivered artifact is only a stub, placeholder, skeleton, plan, scaffold, interface, or unused implementation. Put foundations in current-state prose and keep the behavioral item unchecked.
- Before changing a roadmap checkbox, update current-state prose, add or identify focused verification, run `Scripts/CheckCodeStyle`, and confirm the checked wording does not overstate platform, backend, device, or workflow coverage.
- Phase completion means every required item and exit criterion is demonstrably met. A phase may have useful checked foundations without being complete.
- Do not reorder unrelated roadmap items silently. Record new prerequisites immediately before their consumers and explain cross-phase dependencies in the relevant architecture contract.

## Agentic Workflow

- For requests to continue the roadmap, inspect the first unchecked `PLAN.md` item in order and implement that item rather than substituting an invented slice. If it must be split for honest platform or verification scope, preserve the original intent with precise completed and remaining wording.
- Evaluate architecture and roadmap integrity before implementation. If the next item depends on missing infrastructure, document and schedule the prerequisite first; do not build dependent behavior on an implicit subsystem.
- Use parallel agents when the user asks for agentic work and the task has independent bounded concerns. Typical renderer assignments are architecture/backend audit, editor/runtime integration audit, and verification/roadmap audit. The primary agent owns edits, reconciles findings, prevents overlap, and remains responsible for evidence.
- A user request to "continue," take the "next step," or otherwise continue the roadmap is standing authorization to evaluate and, when safe, use parallel agents for a batch of up to three small, independently verifiable roadmap slices before the expensive shared build/CI cycle; do not ask for separate batching permission. Three is a ceiling, not a quota. Select the first unchecked item and only the immediately following eligible work whose prerequisites are already satisfied; never skip a blocking item merely to fill the batch.
- Parallelize a batch only when agents have non-overlapping file/module ownership and no slice consumes another slice's unfinished API, data format, lifecycle, or architecture decision. Coupled work stays sequential under one owner even when that produces a batch of one. Assign every agent an explicit path scope, behavior contract, verification target, and forbidden overlap before work begins.
- Agents sharing this workspace must not run concurrent generators, compilers, formatters, or tests that write the same build/output tree. Each agent performs safe read-only analysis and the smallest non-conflicting checks for its slice; the primary agent reconciles all diffs, then runs one consolidated generation/build/regression pass and one CI push for the integrated batch.
- Amortizing a build never amortizes evidence. Every slice needs its own focused behavior test or capture and independently supportable roadmap wording. If the consolidated build or a focused test fails, isolate the responsible slice and fix or remove it from the batch without checking it off; do not weaken tests or hold unrelated verified slices hostage.
- Prefer separate commits per independently reviewable slice after the integrated batch passes, followed by a documentation/checkmark commit when hosted evidence is required. A single commit is acceptable when the slices form one atomic contract, but the handoff must still report each slice's evidence separately.
- Prefer GPT-5.6-Terra for agentic engine work when model choice is available. Use Luna only for bounded, low-risk, mechanical work; do not use it as the lead for renderer architecture, cross-platform backend work, roadmap governance, or broad refactors. Use the frontier model when Terra cannot safely carry the complexity.
- Agents may accelerate analysis and verification, but their reports are not evidence by themselves. Inspect real call sites and run the required behavior.
- Roadmap-continuation requests authorize staging the scoped changes, committing on the current branch, pushing to the configured remote, monitoring resulting CI, and fixing/retrying in-scope failures. This does not authorize unrelated changes, destructive history edits, releases, or deployment outside existing CI.
- Before handoff, ensure the working tree contains no accidental generated files and that every durable decision from the task is present in the appropriate Markdown file.

## Renderer Portability

- Keep gameplay, scene, editor, and backend-neutral renderer code on `Engine::RHI`. NVRHI is the first implementation backend; it has not been replaced by raw Vulkan or raw D3D12.
- Vulkan follows NVRHI's ownership model: the engine/platform layer creates the native instance, surface, physical/logical device, and queues, then wraps them with `nvrhi::vulkan::createDevice`. Renderer resources and command submission use the returned `nvrhi::DeviceHandle` behind `Engine::RHI`.
- Native API use is limited to documented backend escape hatches. Window-system swapchain/presentation and Dear ImGui may consume native handles because NVRHI does not own presentation and Dear ImGui has no NVRHI renderer backend. Do not let those bridges expand into scene rendering.
- Backend selection and fallback happen before native device creation. A strict request fails clearly when unavailable; an ordinary portable launch may select a documented fallback. Never report one backend while running another.
- Preserve multi-device and multi-vendor behavior: enumerate adapters, select by required capabilities rather than vendor identity, distinguish supported from enabled features, avoid unconditional optional extensions, and record fallbacks and qualification coverage accurately.
- The renderer capability and fallback contract lives in [Docs/Architecture/RENDERER_CAPABILITY_CONTRACT.md](Docs/Architecture/RENDERER_CAPABILITY_CONTRACT.md). The macOS/MoltenVK decision and measured limitations live in [Docs/Architecture/MACOS_RENDERER_BACKEND_DECISION.md](Docs/Architecture/MACOS_RENDERER_BACKEND_DECISION.md).

## Physics Authority And Research

- The accepted planning contract is [Docs/Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md](Docs/Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md). Read it before physics, character collision, cloth/hair simulation, GPU deformation, determinism, rollback, or collision-asset work.
- No physics backend is selected or admitted yet. Implement the backend-neutral contract and conformance harness before choosing a library; the initial planned bake-off is architecture-fit candidate Box3D versus maturity-control candidate Jolt under identical engine-owned tests. Do not dismiss Box3D because it is new, and do not ignore its explicitly documented alpha/maturity risk.
- CPU fixed-step physics is gameplay authority by default. Optional GPU deformables are visual/secondary and publish through explicit RHI synchronization; they do not silently drive gameplay bodies, hit tests, events, replication, saves, AI, or navigation.
- FEM, PD+barrier, IPC-family methods, and ABD are measured hero/offline candidates, not interchangeable whole-engine backends or universal zero-penetration/performance promises. Preserve portable fallbacks and report algorithm, tolerance, backend, hardware, and failure scope.
- Consumer DX12/Vulkan queues do not guarantee dedicated SM/RT-core shares or free overlap. DMA moves data but does not solve physics. Treat CUDA, L2 policy controls, and RT-assisted collision as optional capability-gated research paths, never the multi-device baseline.

## Terrain Authority And Research

- The accepted planning contract is [Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md](Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md). Read it before terrain generation, infinite-world streaming, terrain LOD, terrain collision, terrain editing, hydrology/erosion, or learned terrain work.
- Terrain sources are project-selectable and produce canonical versioned artifacts. Do not make procedural generation, Terrain Diffusion, a heightfield, or an infinite world mandatory for every game.
- Terrain Diffusion is an evaluation candidate for optional offline or asynchronous macro generation. Do not admit PyTorch, CUDA, its models, or a service dependency without the documented bake-off and a `Docs/DEPENDENCIES.md` update.

## Documentation Maintenance

- Update `PLAN.md` whenever current behavior, roadmap prerequisites, ordering, or verification status changes.
- Update the relevant architecture contract/ADR when ownership, synchronization, data layout, fallback, platform, or technology decisions change.
- Update `Docs/DEPENDENCIES.md` in the same change that admits, removes, upgrades, or changes the use of a dependency.
- Update `PRODUCT.md`, `DESIGN.md`, or `Docs/EDITOR_UI_REVIEW.md` when user workflow or editor interaction rules change.
- Update the nearest `OWNERSHIP.md`, this scope map, and `Docs/README.md` when files or responsibilities move across modules.
- Add every new Markdown document to `Docs/README.md`; add architecture documents to `Docs/Architecture/README.md` as well.
- Keep facts in one authoritative location and link to them elsewhere. Do not create competing copies of version numbers, platform status, completion claims, or execution order.
- When an instruction in this file becomes incomplete because of a project change, update `AGENTS.md` in the same change. Future chats and other AI systems must be able to recover the workflow from the repository alone.
