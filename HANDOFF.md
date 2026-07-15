# Current Handoff

Updated 2026-07-15. This is a recovery aid; `PLAN.md` remains the sole roadmap and checkbox authority. A new agent must begin with `AGENTS.md`, `PLAN.md`, `Docs/README.md`, `Docs/ROADMAP_GOVERNANCE.md`, `Docs/VERIFICATION.md`, `Docs/Architecture/README.md`, and `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`.

## Objective And Stop Condition

This continuation began at 1% weekly model allowance used and was asked to complete as many coherent roadmap features as practical near 50%, without stopping mid-slice. Native `terra_medium` owners implemented bounded vertical slices while the root agent reconciled architecture, reviewed synchronization/publication risk, ran one consolidated acceptance gate, and handled CI. The final checkpoint was 52% used; the small overrun completed and qualified the active cross-queue RenderGraph slice rather than leaving its CI failures unresolved. Do not start another large slice from this handoff without a fresh allowance check.

## Token-Efficiency Baseline And Required Next-Session Audit (2026-07-15)

This baseline is scoped only to the 2026-07-15 12:58:27Z-15:08:04Z goal window (2h10m). Goal telemetry reported 732,808 tokens and 7,777 seconds; weekly allowance moved from about 1% to 52%. Five coherent features used nine exact `terra_medium` owner sessions. Root telemetry recorded 214 model turns, 206 tool calls, and 48 explicit waits (33 `wait_agent`, 15 `functions.wait`), with median input context 138,427, maximum 220,812, and cache share 97.67%. Terra recorded 445 turns; combined cache share was 97.06%. All nine delegated session metadata records reported `agent_role` `terra_medium` and model `gpt-5.6-terra`. Cached/raw token totals are diagnostic measures and cannot prove exact weekly-quota weighting.

Verdict: role routing, evidence collection, and defect discovery were correct and useful, but root resumptions/waits, cold continuation owners, and context growth were excessive. For the next completed medium/large slice, compare actual results with the `AGENTS.md` targets (at most 20 root resumptions through pre-CI acceptance, at most two explicit waits per owner/process, and owner sessions equal coherent slices unless justified). Report whether orchestration/tool telemetry or role routing changed, record exception reasons, and update `AGENTS.md` and this handoff if those rules prove stale. In practice: resume the same owner with `followup_task`, use one longest event/process wait, use the full commit SHA with compact CI status/log excerpts, and compact at the safe threshold rather than carrying an overgrown root context.

## Completed Roadmap Work

The following commits are on `main` and `origin/main`:

- `0cc9b6f` — real D3D12 buffer queue-ownership qualification, including independent-queue bytes, final owner/state, recovery, retirement, and forced-Graphics rejection.
- `f891342` — whole-resource texture ownership parity across deterministic, D3D12, and Vulkan paths.
- `0b6d72a` — Vulkan Compute/Copy queue admission and truthful Graphics fallback.
- `e8e7878` — Vulkan different-family buffer/texture release/acquire translation with NVRHI GPU waits and no CPU wait between forward submissions.
- `093bdae` — compiled RenderGraph cross-queue execution, ownership translation, accepted-prefix state publication, per-pass bounded context retirement/reuse, focused tests, architecture/verification documentation, and the checked `PLAN.md` item at line 260.
- `7e5e80b` — portable Vulkan harness parity so Linux/macOS actually invoke every marker they require, including RenderGraph execution.

`PLAN.md` is complete through the cross-queue execution item. The first unchecked item is the representative Scene-viewport integration at line 261.

## Accepted Mechanism And Current Behavior

- Each compiled pass records and submits on its resolved effective RHI queue. Producer completion tokens are deduplicated and supplied as consumer dependencies; distinct queues become GPU waits, while unavailable queue classes preserve deterministic ordered Graphics fallback without fabricated ownership transfers.
- Resources crossing distinct effective queues receive paired whole-resource release/acquire operations. Imported initial/final state is retained, and accepted submissions publish the exact prefix of state and ownership changes. Missing producer tokens fail before the dependent callback/submission while preserving prior accepted work.
- Recording contexts are keyed by effective queue plus compiled pass identity. Each key has a three-context bound; an incomplete token blocks only its exact context, and a retired context is reused with the same command-list object.
- D3D12 and Vulkan command lists retain ordered vectors of buffer and texture ownership operations. Every operation is validated before native submission. Duplicate resources reject before tracker, transition, or native command-list mutation, so a failed record cannot leave hidden GPU work.
- A source command list may stage the portable transition needed for release while the ownership tracker still validates against its committed baseline. Native acceptance commits staged state before release publication; pending/recovery remains tied to the declared pre-release state.
- The completion smoke accepts either a complete or incomplete first nonblocking query. It no longer assumes an incomplete fence remains incomplete across a second observation; actual same-list reuse is checked at `Begin` and then proved after bounded retirement.

## Evidence

Local Windows/MSVC Debug evidence on an RTX 3080 Ti:

- Full solution build passed with zero warnings and zero errors.
- `EngineTests` passed 53/53.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022 -SkipBuild` passed. `RenderGraphExecutionSmokeV1` exercised independent D3D12 Graphics-to-Copy execution, a GPU wait, exact readback, and retired same-context reuse.
- `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022 -SkipBuild` passed on split families Graphics 0, Copy 1, and Compute 2. The same RenderGraph smoke exercised an independent Copy queue and GPU wait.
- `bash -n Scripts/TestVulkan.sh`, `Scripts/CheckCodeStyle.ps1`, and `git diff --check` passed.

Hosted evidence for behavior/harness head `7e5e80b`:

- [CI run 29426061727](https://github.com/ClGratton/Spiral/actions/runs/29426061727) passed Windows D3D12, Ubuntu Vulkan/lavapipe Graphics fallback, macOS Intel MoltenVK, and code style.
- [Dependency submission 29426061613](https://github.com/ClGratton/Spiral/actions/runs/29426061613) passed.
- Earlier run `29422498828` exposed a racy D3D12 completion-smoke assertion on Microsoft Basic Render Driver; `093bdae` corrected it and the final Windows job passed.
- Earlier run `29425229645` exposed that `TestVulkan.sh` required the RenderGraph marker without invoking its flag; `7e5e80b` corrected the invocation and the final Ubuntu/macOS jobs passed.

Hosted Vulkan evidence is regression/fallback evidence unless the selected runner reports independent queues. The native independent-family qualification remains the local RTX 3080 Ti run. No new Apple Silicon claim was made; the hosted macOS job is Intel MoltenVK coverage only.

## Next Ordered Work

The first unchecked `PLAN.md` item is:

> Frame/render graph real-workflow integration: drive a representative multi-pass Scene viewport through the execution path on every backend claimed, with graph/pass capture labels and output equivalence against the pre-graph path before removing that bootstrap path.

Start by classifying and bounding that item under `AGENTS.md`. Assign one exact `terra_medium` owner the vertical slice with `PLAN.md` line 261, `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`, renderer ownership files, the existing D3D12/Vulkan viewport scene renderers, and the required backend captures. Do not remove the bootstrap path until output equivalence is demonstrated on every claimed backend. Do not select speculative native Apple Silicon work without an execution environment that can run its focused verification.

## Working State

Behavior and harness commits through `7e5e80b` are pushed and have green exact-head hosted evidence. This file is the only deliberate post-evidence change; after its documentation commit, the working tree must be clean. Generated CI artifact downloads are under ignored `output/`. Preserve the first unchecked `PLAN.md` item exactly as the next scope.
