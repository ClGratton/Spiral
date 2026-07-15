# Current Handoff

Updated 2026-07-15. This is a recovery aid; `PLAN.md` remains the sole roadmap and checkbox authority. A new agent must begin with `AGENTS.md`, `PLAN.md`, `Docs/README.md`, `Docs/ROADMAP_GOVERNANCE.md`, `Docs/VERIFICATION.md`, `Docs/Architecture/README.md`, and `Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md`.

## Completed Slice

The Windows D3D12 render-smoke harness now launches each headed Editor invocation through `InvokeBoundedProcess.ps1`. It uses inherited child environment and exact argument arrays, asynchronously collects stdout/stderr while printing new lines immediately, retains merged output for existing marker assertions, and applies a configurable 180-second per-child runtime deadline (build time excluded). A timeout prints the invocation label, elapsed time, PID, recent output, dump availability, and then terminates and verifies the discovered Windows process tree. No repository-admitted Windows dump utility is configured, so that condition is explicit rather than silently omitting dump evidence. `TestInvokeBoundedProcess.ps1` proves successful stdout/stderr and nonzero-exit propagation plus deliberate timeout/tree cleanup. This is verification-harness reliability work only; it does not establish an engine deadlock or hosted queue-gate qualification.

Phase 3's RHI queue-topology/GPU-dependency prerequisite is implemented and checked. `Engine::RHI` publishes requested/effective queue resolution plus independent/fallback identity and accepts an ordered list of prior opaque completion tokens on submission. The shared monotonic validator rejects zero, foreign, absent, duplicate, prospective-self, and forward identities before native submission; accepting only issued smaller identities makes cycles unrepresentable.

D3D12 creates command lists for the effective queue. Distinct enabled Copy/Compute queues use GPU queue fence waits against already-signaled producer values; same-effective Graphics fallback relies on ordered submission and emits no wait. Queue identity is based on distinct created objects rather than non-null aliases. The forced diagnostic mode suppresses independent queue creation and exercises the same portable command/dependency path through Graphics. Vulkan queue creation remains unchanged and resolves requested Copy/Compute work to the one enabled Graphics queue.

This slice does not change RenderGraph execution, add Vulkan multi-queue admission or ownership transfers, allocate transients, or migrate viewport work.

## Evidence

- Windows/MSVC Debug solution build passed with zero warnings/errors.
- `EngineTests`: 47/47 passed. The two added tests cover requested/effective/independent resolution; one and multiple dependencies in caller order; same-effective elision versus distinct-effective waits; zero, foreign, unissued, duplicate, forward/cycle-forming, and self rejection; no native acceptance/token/state publication after validation failure; valid later publication; and independent token retirement/queryability. The prior 45 tests remain.
- `Scripts/TestInvokeBoundedProcess.ps1` passed: stdout/stderr and zero exit code propagated, then a two-second deliberate parent/child hang printed its PID/recent output/dump-unavailable diagnostic and verified both processes terminated.
- `Scripts/TestRender.ps1 -Configuration Debug -Action vs2022 -SkipBuild` passed through the updated harness. On the local RTX 3080 Ti, normal D3D12 reported independent Copy and Compute, Copy-to-Graphics and Graphics-to-Compute `gpu-wait`, no CPU wait between submissions, exact 4 KiB output, committed `CopySource`, and retirement. Its second forced-fallback launch reported both classes as `graphics-fallback` and both dependencies `ordered-elided` with identical bytes/state.
- `Scripts/TestVulkan.ps1 -Configuration Debug -Action vs2022` passed. Vulkan reported truthful Copy/Compute Graphics fallback, both dependencies `ordered-elided`, no CPU wait, and token retirement; buffer-copy bytes/state are explicitly not claimed.
- `Scripts/CheckCodeStyle.ps1`, `git diff --check`, and tracked-Markdown link validation passed.
- Exact-head CI [run 29409715948](https://github.com/ClGratton/Spiral/actions/runs/29409715948) passed Windows D3D12, Ubuntu Vulkan/lavapipe, macOS MoltenVK, and code style for harness commit `373300e`, which contains queue-dependency implementation `556281b`; dependency submission [run 29409715908](https://github.com/ClGratton/Spiral/actions/runs/29409715908) passed.
- Cancelled run `29404884243` built successfully at 09:35:45Z but emitted no child output before cancellation at 10:44:06Z; it remains failed/cancelled evidence, not a pass. The replacement completed Windows in 2m55s, and local NVIDIA/WARP repetitions pass, so an engine deadlock was not established. The bounded harness remains the permanent reliability correction.

## Next Ordered Work

Vulkan multi-queue evaluation found that vendored NVRHI's Vulkan state tracker always uses ignored queue-family indices, so shared-resource ownership across families cannot be delegated to it. The work is split into three bounded gates. First is the portable RHI paired release/acquire state machine with deterministic and real D3D12 global-resource evidence. Second admits selected Vulkan Compute/Copy queues for queue-local work and same-family/graphics-fallback shared work. Third translates the portable pair to native Vulkan different-family barriers and qualifies actual shared-resource movement. RenderGraph translation follows all three; transient allocation and viewport adoption remain separate.

## Working State

Queue-dependency implementation `556281b` and bounded-harness correction `373300e` are on `origin/main` with completed hosted evidence. The next scoped change begins from the evidence documentation commit following them; the working tree must be clean at handoff.
