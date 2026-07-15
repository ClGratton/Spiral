# Current Handoff

Updated 2026-07-15. This is a recovery aid; `PLAN.md` remains the sole roadmap and checkbox authority. A new agent must begin with `AGENTS.md`, `PLAN.md`, `Docs/README.md`, `Docs/ROADMAP_GOVERNANCE.md`, `Docs/VERIFICATION.md`, `Docs/Architecture/README.md`, and `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`.

## Objective And Stop Condition

The continuation preserved the user-set 23%-remaining weekly-allowance floor and used one exact persistent `terra_medium` owner for the medium transient-resource capability prerequisite. Reliable allowance telemetry was unavailable in this task session (`get_goal` returned no allowance state), so do not infer an exact remaining percentage from token counts. Commit `02c86e0` is locally accepted and pushed; its exact-head dependency submission passed and its CI workflow was in progress at the single permitted status check. The next ordered item is a large physical-allocation/reuse slice, so do not begin it without reliable allowance telemetry showing that doing so preserves the 23% floor.

## Token-Efficiency Baseline And Required Next-Session Audit (2026-07-15)

This baseline is scoped only to the 2026-07-15 12:58:27Z-15:08:04Z goal window (2h10m). Goal telemetry reported 732,808 tokens and 7,777 seconds; weekly allowance moved from about 1% to 52%. Five coherent features used nine exact `terra_medium` owner sessions. Root telemetry recorded 214 model turns, 206 tool calls, and 48 explicit waits (33 `wait_agent`, 15 `functions.wait`), with median input context 138,427, maximum 220,812, and cache share 97.67%. Terra recorded 445 turns; combined cache share was 97.06%. All nine delegated session metadata records reported `agent_role` `terra_medium` and model `gpt-5.6-terra`. Cached/raw token totals are diagnostic measures and cannot prove exact weekly-quota weighting.

Verdict: role routing, evidence collection, and defect discovery were correct and useful, but root resumptions/waits, cold continuation owners, and context growth were excessive. For the next completed medium/large slice, compare actual results with the `AGENTS.md` targets (at most 20 root resumptions through pre-CI acceptance, at most two explicit waits per owner/process, and owner sessions equal coherent slices unless justified). Report whether orchestration/tool telemetry or role routing changed, record exception reasons, and update `AGENTS.md` and this handoff if those rules prove stale. In practice: resume the same owner with `followup_task`; use one longest event/process wait only for an active Terra owner or genuinely blocking hosted/platform acceptance gate; otherwise check the exact head once, detach or record CI pending, and return control. Use compact CI status/log excerpts and compact at the safe threshold rather than carrying an overgrown root context.

Wall-clock audit: the 7,777-second window was partitioned from rollout session timestamps and GitHub run `createdAt`/`updatedAt`, clipping to the window and taking interval unions rather than summing overlap. Exact Terra owner-session coverage was 5,898.1 s (75.8%); CI workflow coverage for heads `0cc9b6faf8770d8dd00724529348ad837add3852`, `f89134228946d1c5e6b1e35da1f17e5fd721476a`, `0b6d72a06e0aeb0a21a4dcdacbbd9747e29d2c64`, `e8e7878880513457489d228e7b0cec009d744662`, `093bdaedcc32159e8085778826c89693ed8d9dfa`, `7e5e80b082686949b0bd2697ffe263f1c65ac616`, and the window-clipped start of `589192148f916c6644a8cde7bbc55f6cb2479051` was 2,481.0 s (31.9%). The exclusive partition is 4,555.9 s owner-only (58.6%), 1,342.2 s overlapping owner/CI (17.3%), 1,138.8 s CI-only (14.6%), and 740.1 s neither (9.5%).

Hosted CI was therefore not most of the wall time; owner-session lifetime and orchestration/model gaps were dominant. Active recorded tool durations were only 548.2 s local build/test, 131.7 s explicit waits, and 43.7 s GitHub queries; session lifetime includes reasoning and unobserved idle gaps, so it cannot be called active work, and wall time cannot be converted into token/allowance cost. Next medium/large-slice handoff must record the exact head, whether local evidence already accepted the slice, one initial CI check, monitor/pending-handoff choice, any synchronous-wait blocking-gate justification, and the resulting compact status.

### Scene-Viewport Slice Audit (2026-07-15)

The audit snapshot covers 2026-07-15 15:41:11Z-16:00:47Z (about 19m36s). Weekly allowance moved from 58% to 64% used, leaving 36%, so this large slice consumed about six percentage points and respected the 23%-remaining floor. One coherent feature used one exact `terra_medium` session running `gpt-5.6-terra`; the incomplete first report was resumed through one `followup_task`, with no cold replacement or optional reviewer. The owner had two active turns totaling about 669.2 seconds. This meets the one-session-per-slice rule.

Root telemetry recorded 29 model turns and 30 tool calls (21 `exec`, four `wait_agent`, three process `wait`, one `spawn_agent`, and one `followup_task`), with 95.37% cache share, median input 89,853, and maximum input 132,849. The root therefore missed the at-most-20-turn target and crossed the 100k compaction threshold. Two user messages interrupted the two intended long owner waits, producing four `wait_agent` calls; the single same-owner correction was required because the first implementation lacked the mandated side-by-side comparator. Additional avoidable churn came from one incorrect local test-binary path and repeated telemetry-parser corrections. The owner recorded 50 model turns and 48 tool calls, with 96.79% cache share, median input 138,443, and maximum input 225,744; future owners should batch repository reads and verification output more aggressively even when prompt caching is high.

Root performed one full-SHA check for `e950d217df23b507a2bc10ba0da59038bce11b0d`, then attempted one synchronous monitor because macOS was the blocking every-claimed-backend gate. That monitor stopped on a transient GitHub HTTP 401 while the run remained active, and root correctly did not poll repeatedly. The later single recovery check confirmed [CI run 29430304670](https://github.com/ClGratton/Spiral/actions/runs/29430304670) and [dependency submission 29430304809](https://github.com/ClGratton/Spiral/actions/runs/29430304809) passed; Windows D3D12, Ubuntu Vulkan/lavapipe, and macOS Intel/MoltenVK logs contain the required `comparator=exact-byte-pass` markers. This is the required next-session comparison result: routing/session reuse and allowance cost improved substantially, but root and owner turn counts still need reduction without weakening the exact-byte/backend evidence.

### Transient-Capability Slice Audit (2026-07-15)

One exact `terra_medium` owner classified the capability prerequisite as a coherent medium slice and made commit `02c86e0` (`Add transient resource capability fallback gate`). The root used more than the preferred two explicit owner waits because the surface limited waits to 60 seconds while the owner was still working; no replacement owner or reviewer was introduced. The owner reported `EngineTests` 55/55 plus local RTX 3080 Ti D3D12 `TestRender.ps1 -SkipBuild`, Vulkan `TestVulkan.ps1`, and code style passing. Root then confirmed `git diff --check`, `bash -n Scripts/TestVulkan.sh`, and `CheckCodeStyle.ps1` on the committed head. The single full-SHA CI check found [dependency submission 29432926998](https://github.com/ClGratton/Spiral/actions/runs/29432926998) successful and [CI run 29432926976](https://github.com/ClGratton/Spiral/actions/runs/29432926976) in progress; local evidence already accepts this capability-only prerequisite, so CI is deliberately detached rather than polled.

## Completed Roadmap Work

The following commits are on `main` and `origin/main`:

- `0cc9b6f` — real D3D12 buffer queue-ownership qualification, including independent-queue bytes, final owner/state, recovery, retirement, and forced-Graphics rejection.
- `f891342` — whole-resource texture ownership parity across deterministic, D3D12, and Vulkan paths.
- `0b6d72a` — Vulkan Compute/Copy queue admission and truthful Graphics fallback.
- `e8e7878` — Vulkan different-family buffer/texture release/acquire translation with NVRHI GPU waits and no CPU wait between forward submissions.
- `093bdae` — compiled RenderGraph cross-queue execution, ownership translation, accepted-prefix state publication, per-pass bounded context retirement/reuse, focused tests, architecture/verification documentation, and the checked `PLAN.md` item at line 260.
- `7e5e80b` — portable Vulkan harness parity so Linux/macOS actually invoke every marker they require, including RenderGraph execution.

Latest accepted slice:

- `e950d21` - live D3D12/Vulkan Scene viewports through named Clear, Raster, and Output Handoff RenderGraph passes, plus a retained direct-recorder oracle that requires exact-byte output equivalence on separate smoke-only outputs.
- `02c86e0` - Phase 3 transient-resource capability prerequisite: separate RHI placed-resource/alias-barrier lifecycle reporting, truthful `PlacedAliasedTransient` versus `NonAliasedGpuRetiredPool` selection, selected-path diagnostics, deterministic coverage, and local backend smoke exercise without premature allocation.

`PLAN.md` is checked through the transient-resource capability group at line 262.

## Accepted Mechanism And Current Behavior

- Each compiled pass records and submits on its resolved effective RHI queue. Producer completion tokens are deduplicated and supplied as consumer dependencies; distinct queues become GPU waits, while unavailable queue classes preserve deterministic ordered Graphics fallback without fabricated ownership transfers.
- Resources crossing distinct effective queues receive paired whole-resource release/acquire operations. Imported initial/final state is retained, and accepted submissions publish the exact prefix of state and ownership changes. Missing producer tokens fail before the dependent callback/submission while preserving prior accepted work.
- Recording contexts are keyed by effective queue plus compiled pass identity. Each key has a three-context bound; an incomplete token blocks only its exact context, and a retired context is reused with the same command-list object.
- D3D12 and Vulkan command lists retain ordered vectors of buffer and texture ownership operations. Every operation is validated before native submission. Duplicate resources reject before tracker, transition, or native command-list mutation, so a failed record cannot leave hidden GPU work.
- A source command list may stage the portable transition needed for release while the ownership tracker still validates against its committed baseline. Native acceptance commits staged state before release publication; pending/recovery remains tied to the declared pre-release state.
- The completion smoke accepts either a complete or incomplete first nonblocking query. It no longer assumes an incomplete fence remains incomplete across a second observation; actual same-list reuse is checked at `Begin` and then proved after bounded retirement.
- The live D3D12 and Vulkan Scene viewport uses three imported-resource graph passes: `Scene Viewport Graph Clear`, `Scene Viewport Graph Raster`, and `Scene Viewport Graph Output Handoff`. The executor surrounds callbacks with those stable debug labels; presentation and ImGui remain native-bridge owned.
- Smoke mode retains the prior direct recorder as an oracle on separate color/depth outputs. It renders the same immutable prepared frame and requires matching extent, row pitch, and every output byte before publishing only the graph result. The graph color output is restored to `ShaderResource` after comparison.
- The per-frame graph currently waits for its final completion because its recording contexts are stack-owned. This is a safe lifetime bridge, not frame pacing or deliberate smoothing; persistent overlap remains later roadmap work.
- `Phase3TransientResourcesV1` selects `PlacedAliasedTransient` only when both RHI `PlacedResources` and `AliasingBarriers` are usable. Current D3D12 and Vulkan/NVRHI adapters truthfully select `NonAliasedGpuRetiredPool` because backend-neutral translations are absent. That fallback defines future reuse as completion-token-gated; it does not allocate resources, publish memory cost, or claim physical reuse.

## Evidence

Local Windows/MSVC Debug evidence on an RTX 3080 Ti:

- Full solution build passed with zero warnings and zero errors.
- `EngineTests` passed 53/53.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022 -SkipBuild` passed. `RenderGraphExecutionSmokeV1` exercised independent D3D12 Graphics-to-Copy execution, a GPU wait, exact readback, and retired same-context reuse.
- `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022 -SkipBuild` passed on split families Graphics 0, Copy 1, and Compute 2. The same RenderGraph smoke exercised an independent Copy queue and GPU wait.
- `bash -n Scripts/TestVulkan.sh`, `Scripts/CheckCodeStyle.ps1`, and `git diff --check` passed.
- `SceneViewportRenderGraphV1` passed exact-byte D3D12 comparison at 1560x822 (5,129,280 bytes) and 850x613 (2,084,200 bytes), while the existing Scene A/C identity and B centroid-shift captures remained green.
- The same marker passed Vulkan comparison at 48x36 (6,912 bytes), 64x48 (12,288 bytes), and the live/resized viewport outputs before native ImGui handoff.
- `Phase3TransientResourcesV1` passed deterministic preferred/fallback policy tests as part of `EngineTests` (55/55), and the named local D3D12/Vulkan presentation smokes exercised selected-path publication. No native placement/alias-barrier, allocator, physical-reuse, memory-cost, or hosted fallback-regression claim is made.

Hosted evidence for behavior/harness head `7e5e80b`:

- [CI run 29426061727](https://github.com/ClGratton/Spiral/actions/runs/29426061727) passed Windows D3D12, Ubuntu Vulkan/lavapipe Graphics fallback, macOS Intel MoltenVK, and code style.
- [Dependency submission 29426061613](https://github.com/ClGratton/Spiral/actions/runs/29426061613) passed.
- Earlier run `29422498828` exposed a racy D3D12 completion-smoke assertion on Microsoft Basic Render Driver; `093bdae` corrected it and the final Windows job passed.
- Earlier run `29425229645` exposed that `TestVulkan.sh` required the RenderGraph marker without invoking its flag; `7e5e80b` corrected the invocation and the final Ubuntu/macOS jobs passed.

Hosted Vulkan evidence is regression/fallback evidence unless the selected runner reports independent queues. The native independent-family qualification remains the local RTX 3080 Ti run. No new Apple Silicon claim was made; the hosted macOS job is Intel MoltenVK coverage only.

Exact-head CI run `29430304670` passed real Windows D3D12, Ubuntu Vulkan/lavapipe, macOS Intel/MoltenVK, and code-style jobs. Its backend logs contain the required `SceneViewportRenderGraphV1 ... comparator=exact-byte-pass` markers. Dependency submission `29430304809` also passed.

## Next Ordered Work

The first unchecked `PLAN.md` item is:

> Transient resource allocation and reuse from render-graph lifetimes.

This is a large multi-backend physical-allocation slice: it must bind resource lifetime intervals, choose the established truthful path, report memory cost, and prove no resource is reused before its exact GPU completion token retires. Recheck reliable weekly allowance telemetry before delegating it. If telemetry remains unavailable, do not start the slice because compliance with the 23%-remaining floor cannot be demonstrated; preserve the clean tree and return control rather than guessing.

## Working State

Implementation commit `02c86e0` is on `main` and `origin/main`. Its dependency-submission workflow passed; CI run `29432926976` was in progress at the one exact-head status check and must be monitored event-driven or checked once in a later task, not repeatedly polled. This handoff update must be committed and pushed before returning control; leave the working tree clean. Generated captures and CI artifacts remain under ignored `output/`.
