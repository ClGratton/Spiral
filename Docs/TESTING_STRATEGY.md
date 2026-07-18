# Testing Strategy

Status: Required living contract
Date: 2026-07-18

This document owns how Spiral tests are designed. [VERIFICATION.md](VERIFICATION.md) owns the commands and evidence matrix, while [PLAN.md](../PLAN.md) alone owns implementation order and completion state.

The goal is not to accumulate tests or maximize a coverage percentage. A test earns its maintenance cost when it can expose a plausible defect in a current contract, prevent that defect from returning, or make a future change safer. Test design must maximize useful state and failure-mode coverage within an explicit feedback-time budget.

## Research Basis And Project Decision

Salvatore Sanfilippo's 2026 video ["I test nel software: le tecniche che uso"](https://www.youtube.com/watch?v=521V3S3QmnQ) is source material, not project authority. Its useful mechanisms were cross-checked against the original [QuickCheck paper](https://doi.org/10.1145/351240.351266), LLVM's [libFuzzer](https://llvm.org/docs/LibFuzzer.html) and [structured-fuzzing](https://llvm.org/docs/FuzzingLLVM.html) guidance, Clang's [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html) and [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html) documentation, Google's guidance to [test behavior rather than implementation](https://testing.googleblog.com/2013/08/testing-on-toilet-test-behavior-not.html) and treat [coverage as diagnostic rather than sufficient](https://testing.googleblog.com/2020/08/code-coverage-best-practices.html), and Bazel's requirements for [deterministic, hermetic, reentrant tests](https://bazel.build/versions/9.2.0/reference/test-encyclopedia?hl=en).

| Source claim | Spiral decision | Current repository evidence |
| --- | --- | --- |
| Prefer tests through the exported/user API so implementation changes do not invalidate correct behavior tests. | Accepted as the default assertion boundary. Assert public or deliberately test-facing contracts; do not freeze private layout or call order unless that detail is itself a performance, synchronization, persistence, or compatibility contract. | `EngineTests` exercises public/test-facing RHI, RenderGraph, Scene, Jobs, shader, and pacing contracts; headed scripts exercise editor and backend workflows. |
| Use knowledge gained from implementation to target fragile internal states and exact boundaries. | Accepted. Black-box assertion boundaries do not require ignorant input selection. Test `B-1`, `B`, and `B+1`, empty/full, invalid/valid transitions, overflow, lifetime, ownership, failure publication, and recovery paths suggested by the implementation. | Sector boundaries, completion tokens, queue ownership, serialization rejection, render-graph hazards, and accepted-prefix failure paths already follow this pattern. |
| Prefer randomized stress/property tests with invariants or a simpler reference implementation over many fixed examples. | Accepted where a stable property or independent oracle exists. Random bytes alone are insufficient for structured inputs; generators must reach semantically interesting states. | `GeneratedTest.h` supplies deterministic choice traces and shrinking; numeric world-grid and stateful RenderGraph properties use independent reference models. `StructuredFuzz.h` generates and mutates Scene/shader-package structures and retains a checked corpus. |
| Run stress under leak/memory diagnostics and do not treat line coverage as proof of state coverage. | Accepted. Sanitizer and coverage results are additional evidence, never replacements for behavioral assertions, real workflows, or platform qualification. | Clang ASan+UBSan instruments Spiral Engine/test code in a dedicated Linux lane while admitted vendor libraries remain uninstrumented. TSan and coverage thresholds remain deliberately unclaimed. |
| Keep the useful suite fast enough to run frequently; use AI to build demanding tests by naming the mechanisms, not by saying only "write tests." | Accepted. Test type and runtime budget are explicit, and AI receives a complete test brief below. | Verification commands are bounded, but the 70-case `EngineTests` registry is one roughly 5,000-line executable with no filter/tier contract or per-test timing publication. |
| Never use TDD or isolated unit tests. | Rejected as a universal rule. TDD is optional. A small isolated test is appropriate when an isolated contract/invariant gives faster, clearer fault localization; an internal-detail test is inappropriate when it merely mirrors the implementation. | Current deterministic RHI/graph/state tests are valuable foundations and remain required beside integration and headed evidence. |

## Test Design Contract

Every nontrivial test or test change must state, in code, its task packet, or the relevant verification entry:

1. **Behavioral contract:** the consumer-visible behavior or deliberately test-facing invariant being protected.
2. **Failure hypothesis:** the plausible bug, fragile state, boundary, ordering, lifetime, or malformed input the test is designed to expose.
3. **Oracle:** exact expected output, invariant, metamorphic relation, state model, or simpler independent reference implementation. "Does not crash" is enough only for a robustness contract.
4. **Input strategy:** fixed examples, boundary table, generated operations, corpus mutation, or structure-aware fuzzing, including why it reaches the risky states.
5. **Reproduction:** deterministic fixture and, for generated work, seed, operation trace/input artifact, and exact rerun command.
6. **Tier and budget:** one tier below, its finite timeout, and why that tier is the smallest one that proves the claim.
7. **Evidence and non-claims:** the backend/platform/workflow actually exercised and what remains unqualified.

Prefer assertions at the highest stable API that still localizes the failure usefully. Direct inspection of private state is justified only when that state is the contract or when no public observation can distinguish a safety failure; expose the smallest read-only test seam rather than making tests a second production authority.

### Boundary and state selection

Implementation knowledge selects inputs even when assertions remain black-box. At minimum consider:

- zero, one, empty, null, missing, duplicate, stale, maximum, and capacity-plus-one;
- `B-1`, `B`, and `B+1` around representation, allocation, queue, timeout, serialization-version, and numeric transitions;
- negative coordinates, overflow/underflow, NaN/infinity, precision loss, and signed/unsigned conversion where applicable;
- valid operation sequences plus reorder, repeat, cancel, failure, partial acceptance, recovery, destruction, and reuse;
- different thread counts, execution order, cache state, device capability, and fallback route when the contract claims independence from them.

Do not add a large combinatorial matrix blindly. Name the state transition or invariant each dimension can falsify.

### Reproducible generated testing

- The fast suite uses a stable default seed and prints it on failure.
- Extended campaigns may vary seeds, but every failure retains the exact seed plus the serialized input or operation trace under an ignored `output/` path before triage.
- A fixed seed alone is not enough when scheduling or external state can vary; record every operation needed to replay the failure.
- Minimize a failure through shrinking, delta debugging, corpus minimization, or a manual reduction before promoting it to a permanent regression fixture. Preserve the original artifact until the reduced case is proven equivalent.
- A found counterexample becomes a small deterministic regression test or curated corpus entry; do not rely on rediscovering it randomly.
- Use a simple independent model or differential oracle when practical. Do not share enough implementation with the system under test that both can reproduce the same defect.

For parsers, importers, serialization, shader packages, cooked assets, and future network/save formats, seed from small valid and invalid examples; generate valid structures; then apply field-aware edits, truncation, duplication, version changes, byte/bit flips, and crossover at meaningful boundaries. Raw uniform bytes are only a surface robustness input, not the complete campaign.

### Dynamic diagnostics and coverage

Memory-unsafe C++ boundaries and long generated campaigns should run under the supported sanitizer combination for that host. ASan/UBSan are the first portable candidates; TSan is a separate, slower concurrency lane with its own qualification and suppressions. A sanitizer lane is not claimed until its compiler/runtime, vendor boundary, timeout, symbolization, suppressions, and complete command are documented in [VERIFICATION.md](VERIFICATION.md) and run in CI or equivalent admitted evidence.

Coverage may identify unexecuted regions or generator starvation. It does not prove assertions, state coverage, boundary coverage, race coverage, or correctness. Do not set a completion checkbox from a line-coverage percentage.

## Test Types And Speed Tiers

Budgets below measure test execution after required binaries exist; build/generation time is reported separately and amortized only as allowed by `AGENTS.md`. They are initial ceilings, not permission to consume the whole interval.

| Tier | Intended evidence | Default execution ceiling | Rules |
| --- | --- | --- | --- |
| **Fast contract** | Pure or deterministic engine contract, property replay, small reference oracle | 60 seconds for the selected command | No network, headed window, real sleep, or undeclared machine state. Run after the coherent source patch and before shared integration. |
| **Integration/headless** | Filesystem, jobs, multiple modules, shader toolchain, or headless editor/backend integration | 5 minutes per bounded command | Isolated temp/output state, finite child lifetime, exact assertion output, no leaked processes. Run at a coherent milestone. |
| **Headed/platform** | Real editor interaction, graphics backend/device, capture, presentation, or external tool | 15 minutes per bounded script excluding a separately reported build | Stream output, preserve diagnostics, inspect the real artifact, and state exact hardware/platform scope. |
| **Stress/fuzz/soak** | Many generated states, sanitizer campaign, concurrency schedule exploration, long performance/reliability run | Explicit iteration/time/memory budget; normally scheduled or manually requested | Persist failures and corpus deltas. Keep a short deterministic corpus replay in a faster tier; do not put an unbounded campaign in the default developer gate. |

Visual golden/reference and performance tests use the smallest applicable integration/headed tier plus domain-specific tolerances, raw artifacts, condition metadata, and non-claims. Timing noise or image tolerance must never be widened merely to make a regression pass.

If a fast command becomes build-scale or flaky, split selection/executables or move only the inherently expensive evidence to a slower tier. Do not stop running the whole useful suite simply because one unrelated test is slow.

## AI-Assisted Test Brief

An agent asked to create or review tests must receive, or derive and restate before editing, this packet:

```text
Behavior/public contract:
Known implementation/control boundaries:
Fragile states and exact limits:
Invariants, metamorphic relations, or independent oracle:
Generator/corpus shape and boundary weighting:
Invalid/corrupted operation strategy:
Seed, replay, minimization, and failure-artifact path:
Test tier, execution budget, and sanitizer mode:
Required command, platform/backend evidence, and non-claims:
```

"Write tests for this change" is not a sufficient assignment. AI may propose missing properties, generators, or oracles, but the owner must inspect whether assertions can actually fail, whether the oracle is independent, whether generated inputs reach meaningful states, and whether the test ran against the real changed behavior. Compilation, plausible test code, line coverage, or an agent report is not acceptance evidence.

## Current Infrastructure And Adoption Rule

Phase 3 owns the shared loop because every later implementation slice consumes it; Phase 15 owns broad product-validation suites, not first admission of basic mechanics. `EngineTests` provides Fast and complete Integration selection, exact/filter/list discovery, stable order, measured budgets, exact reruns, and schema-1 JSON. `GeneratedTest.h` provides stable/caller seeds, serialized traces, boundary weighting, campaign replay, delta/value shrinking, and atomic failure artifacts. `StructuredFuzz.h` and `EngineFuzzTests` provide deterministic checked-corpus replay plus a coverage-guided libFuzzer entry point for structure-producing Scene and portable-shader mutations.

The admitted dynamic-diagnostic configuration is Linux Clang ASan+UBSan. It instruments project-owned Engine, deterministic test, and fuzz-target code; GLFW, ImGui, NVRHI, Slang, and other vendored libraries are linked but explicitly not instrumented so vendor defects and suppressions cannot silently become an engine completion claim. `Scripts/TestSanitizers.sh` enables symbolization, fail-fast diagnostics, leak detection, the complete Integration tier, and a bounded 512-run libFuzzer campaign. TSan remains deferred until the job/renderer concurrency boundary, third-party suppressions, runtime budget, and a non-conflicting toolchain lane are independently qualified. There is no coverage percentage gate: coverage is diagnostic only.

Every new feature still adds the smallest behavior test at its implementation step. Use the Fast tier for in-memory contracts, Integration for filesystem/toolchain interactions, and a focused headed/backend script for physical runtime claims. Promote every discovered generated failure to a minimized deterministic regression or curated corpus case; do not rely on rediscovery.
