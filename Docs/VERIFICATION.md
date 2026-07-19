# Verification Guide

Status: Living contract
Date: 2026-07-18

Verification must exercise the behavior claimed by the change. Compilation proves build compatibility; it does not prove a runtime or editor workflow.

[TESTING_STRATEGY.md](TESTING_STRATEGY.md) owns how tests are designed: stable behavior boundaries, implementation-informed failure hypotheses, explicit invariants/oracles, reproducible generated inputs, test speed tiers, sanitizer/coverage policy, and the required AI test brief. This document owns the concrete commands and evidence matrix. A command listed here does not excuse a weak oracle, irrelevant inputs, an unreproducible random failure, or a test placed in the wrong feedback tier.

Headed child processes launched by a verification script must stream stdout/stderr to the active console while retaining the lines needed for assertions, and each launch must have a finite runtime-only timeout. Timeout diagnostics must name the invocation, elapsed time, PID, recent useful output, dump availability/capture result, and successful process-tree cleanup. A cancelled or timed-out hosted run is failure evidence, never a passing qualification.

## Baseline Checks

Run for every scoped change:

```powershell
.\Scripts\CheckCodeStyle.ps1
git diff --check
```

For Markdown changes, also verify that every relative Markdown link resolves. New documents must be listed in [README.md](README.md), and architecture documents must also be listed in [Architecture/README.md](Architecture/README.md).

PowerShell link check from the repository root:

```powershell
$Broken = @()
Get-ChildItem -Recurse -Filter *.md |
    Where-Object { $_.FullName -notmatch '\\Vendor\\|\\bin\\|\\bin-int\\' } |
    ForEach-Object {
        $File = $_
        $Text = Get-Content $File.FullName -Raw
        [regex]::Matches($Text, '\[[^\]]+\]\(([^)]+\.md(?:#[^)]*)?)\)') | ForEach-Object {
            $Target = $_.Groups[1].Value.Split('#')[0]
            if ($Target -notmatch '^(https?://|/)' -and
                -not (Test-Path -LiteralPath ([IO.Path]::GetFullPath((Join-Path $File.DirectoryName $Target))))) {
                $Broken += "$($File.FullName): $Target"
            }
        }
    }
if ($Broken.Count) { $Broken; throw 'Broken Markdown links found.' }
```

## Test Selection And Generated Failures

Classify new tests as Fast contract, Integration/headless, Headed/platform, or Stress/fuzz/soak using [TESTING_STRATEGY.md](TESTING_STRATEGY.md). `EngineTests --tier fast` selects only deterministic in-memory contract tests under a 60-second execution budget. `EngineTests --tier integration` is the complete ordered registry, including filesystem and shader/toolchain cases, under a 300-second execution budget; it is also the compatibility default when no selector is supplied. `--list`, exact `--test`, substring `--filter`, positive `--budget-ms`, and schema-1 `--report-json` provide discovery, focused reruns, explicit overrides, per-test/total durations, and machine-readable evidence. Zero matches and invalid/conflicting selectors fail.

After building, exercise the runner contract itself with:

```powershell
.\Scripts\TestEngineTestRunner.ps1 -Configuration Debug -Action vs2022
```

```bash
bash Scripts/TestEngineTestRunner.sh "$PWD/bin/Debug-<system>-x86_64-gmake/EngineTests/EngineTests"
```

Both scripts must emit `EngineTestRunnerV1 result=pass`, prove both tiers are nonempty, verify exact/filter/generated-replay selection and JSON schema/order/status/budgets/seed, require default Integration to retain the complete registry, and reject zero/invalid/conflicting selectors plus malformed or non-exact replay. These are runner-contract checks, not sanitizer, fuzz, headed workflow, or backend qualification.

Every generated or randomized failure report must retain the seed, serialized input or operation trace, exact rerun command, tier/timeout, and sanitizer mode under an ignored `output/` path. Reduce it to a deterministic regression fixture or curated corpus entry before completion. A varying-seed campaign is additional stress evidence; it never replaces the stable fast replay.

The complete Integration tier includes `Tests/Corpus/Fuzz/*.case` replay. It exercises valid structure generation and field-aware corruption, truncation, version changes, bit flips, and crossover for transactional Scene loading and portable shader-package loading/validation. Run the same harness as a bounded local campaign with:

```powershell
.\bin\Debug-windows-x86_64\EngineFuzzTests\EngineFuzzTests.exe --iterations 256 --seed 42
```

The command must emit `EngineFuzzSummaryV1 ... result=pass`. `--replay <case-or-input>` replays one curated CSV case or exact raw failure input; a failure writes the exact bytes and rerun manifest under ignored `output/fuzz-failures/`.

Linux Clang ASan+UBSan and coverage-guided structured fuzzing use the single canonical command:

```bash
bash Scripts/TestSanitizers.sh
```

It generates the isolated `Debug-linux-x86_64-gmake-asan-ubsan` tree, instruments only project-owned Engine/test/fuzz code, leaves vendored GLFW/ImGui/NVRHI/Slang libraries uninstrumented, enables llvm-symbolizer plus fail-fast ASan/UBSan diagnostics and leak detection, runs the complete Integration tier, and runs libFuzzer for 512 inputs from the checked corpus. Success requires `SanitizerLaneV1 ... result=pass`. The CI `Linux ASan UBSan And Structured Fuzz` job is the admitted hosted evidence. Initial run `29650901373` failed honestly before the lane at a pre-existing GCC/Clang rejection of RenderGraph's nested `ExecuteOptions` default argument; the behavior-preserving explicit overload repair then allowed replacement run `29651182554` to pass the sanitizer/fuzz job, Ubuntu and macOS portable builds/tests, Windows D3D12, and style. This lane makes no TSan, race-freedom, vendor-memory-safety, coverage-percentage, graphics-backend, or physical-device claim; TSan is deferred pending a separately qualified concurrency/toolchain/suppression boundary.

libFuzzer writes a crashing raw input beneath `output/fuzz-failures/`; rerun it by passing that file as the sole positional input to the sanitized executable. Minimize only after reproducing, for example `EngineFuzzTests -minimize_crash=1 -exact_artifact_path=output/fuzz-failures/minimized.input output/fuzz-failures/crash-*`, then add the equivalent small CSV operation case to `Tests/Corpus/Fuzz/` and verify the normal Integration replay.

## Build And Contract Tests

When up to three independent roadmap slices are intentionally batched, do not launch overlapping full builds against the same output tree. Perform cheap slice-specific checks during implementation, integrate and review all diffs, then run the applicable build matrix once against the combined state. After that shared build, run each slice's focused behavior test separately so a passing compile or unrelated smoke cannot mask an incomplete item. Push the integrated batch once and monitor the complete CI run; if it fails, identify the responsible slice before changing roadmap status.

Windows/MSVC:

```powershell
.\Scripts\Build.ps1 -Configuration Debug -Action vs2022
.\bin\Debug-windows-x86_64\EngineTests\EngineTests.exe
```

Windows/MinGW portability:

```powershell
.\Scripts\Build.ps1 -Configuration Debug -Action gmake
.\bin\Debug-windows-x86_64-gmake\EngineTests\EngineTests.exe
```

Linux/macOS:

```bash
./Scripts/Build.sh Debug gmake
./bin/Debug-<system>-x86_64-gmake/EngineTests/EngineTests
```

Use the actual generated system/architecture path on the host.

## Portable Slang Runtime And Snapshot Publication

Actions run `29350139365` passed Windows D3D12 but failed before portable
runtime qualification: Ubuntu `EngineTests` could not load the staged
`libslang-compiler.so.0.2026.13.1`, and macOS libc++ rejected
`std::atomic<std::shared_ptr<const SceneRenderSnapshot/SceneRasterFrame>>` at
compile time. Generated Linux Editor, Sandbox, and EngineTests executables must
contain `RUNPATH=$ORIGIN`; generated macOS counterparts must contain
`LC_RPATH @loader_path`. Verify those generated loader records with the native
platform inspection tool and run the staged executable with no
`LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` override. A disposable WSL probe has
already established the Linux binary behavior; it is not Linux hosted
qualification.

Renderer immutable snapshot publication must use release store and acquire load
on every supported standard-library implementation. Where the C++ library
advertises `__cpp_lib_atomic_shared_ptr`, it may use the specialization;
otherwise it must use the standard atomic `shared_ptr` free functions with the
same orderings. The two headless scene-render-snapshot smoke commands below
exercise the retained-epoch behavior. Final-head Actions run `29354068102`
passed this implementation through the Ubuntu and macOS portable jobs.

Replacement run `29352633796` proved the loader and atomic-publication repairs
by building and launching both portable `EngineTests` jobs, then failed exactly
one of 35 tests on each host: `Slang compiler emits validated portable shader
packages` requested DXIL+SPIR-V even though DXC is admitted only on Windows
x86_64. Portable-host verification therefore compiles a real SPIR-V-only
package, validates its reflection/layout/conventions, confirms deterministic
disk-cache reuse and input invalidation, and verifies that an explicit DXIL
request returns the unavailable-target diagnostic. It must not skip the
integration test, use a system DXC, or infer Vulkan scene execution.
Final-head run `29354068102` passed all 35 tests on both Ubuntu and macOS with
that contract; the same run also passed Linux Vulkan and macOS MoltenVK
presentation, which remains presentation rather than Vulkan scene evidence.

`EngineTests` verifies the large-world coordinate foundation by subtracting an exact double-precision per-view origin before float conversion at trillion-unit coordinates, ensuring the exact-camera-origin float view contains no absolute-world translation. It verifies the version-1 world-grid primitives at negative and exact centered boundaries, immediately adjacent representable values, trillion-unit positions, multi-sector carries, checked invalid inputs, min-inclusive/max-exclusive ranges, and bounded oversized classification. Canonical relative conversion rejects signed-sector subtraction overflow, non-finite sector products, and finite results outside translated float range transactionally. For Scene format 4, it verifies canonical signed-sector/local transform and complete `WorldGridPolicy` persistence (including non-default policy data), absence of serialized `MainCamera.Transform`, legacy v1-v3 absolute-double migration through the default policy with selected-entity camera-transform precedence, rejection of noncanonical/invalid v4 world state, and preservation of the destination Scene after a rejected load. It also verifies stable-ID per-view origin tracking: exact-camera default behavior, exact positive/negative sector-snapped hysteresis boundaries and no-flap behavior, direct destination-sector teleport handling, failed-request transactional state, adjacent extreme signed-sector local-detail preservation, independent multi-view state, and retained immutable complete view epochs. It further verifies deterministic backend-neutral render extraction, main-camera authority, stable source/entity and asset handles, hidden-mesh omission, copied light/camera/canonical-transform values, arbitrary-origin raster invariance, mesh-only motion, camera-plus-mesh origin transition invariance, and tracker-derived nonzero local mesh-camera deltas at extreme positive and negative sectors where approximate absolute doubles alias. The MSVC Debug build completed with zero warnings/errors and the focused executable passed all 31 cases. These checks establish canonical sector/local snapshot/raster propagation for the current snapshot and raster-preparation path; they do not qualify real mesh/material GPU resources, culling, coordinate debug views, physics, or ray-tracing consumers.

`EngineTests --test "Cooked mesh artifacts validate and resolve transactionally"` is the focused Integration boundary after the shared build. It proves schema-1 `SpiralMeshArtifact` round-trip and atomic replacement for `PositionColorUV32F` vertices, explicit primitive byte ranges, and `UInt32` triangle indices; malformed/unsupported headers and out-of-range-index rejection without mutating a destination artifact; registry resolution that rejects missing artifacts, wrong asset types, and mismatched source provenance without partial publication; and Renderer publication of an immutable copied registry catalog. The resolver test proves a published catalog remains valid after the mutable registry changes, a replacement snapshot rejects a removed mesh handle, and that failure preserves the caller's prior artifact. The bounded `Editor.exe --smoke-test --gltf-import-smoke` additionally writes a minimal glTF triangle, imports it through the real editor workflow, publishes the renderer resolver, resolves the registered cooked artifact through that boundary, and requires three vertices and three indices. The headless form currently fails an unrelated frame-input lifecycle invariant before import and is not accepted evidence for this slice. This validates artifact publication/resolution only; it does not detect changed source dependencies, upload buffers, replace the viewport prototype, create descriptors/materials/textures, or qualify any renderer backend/device.

`EngineTests --test "Mesh GPU resource cache preserves exact immutable generations"` is the focused deterministic prerequisite evidence before viewport integration. Its fake `Engine::RHI::Device` checks exact artifact identity reuse, identity-change generation, immutable vertex/index descriptions and byte-for-byte uploads, primitive byte-to-element conversion with zero base vertex, exact-device separation, deterministic least-recent eviction, accepted external-`Ref` survival, and upload failure atomicity. The final MSVC Debug build completed with zero warnings/errors and the full suite passed 81/81. It does not execute a native backend, resolve snapshots, bind or draw a mesh, retain a submitted frame, or qualify a device/backend.

For a real editor persistence check, run the existing save/reload smoke and the scene-authoring save/reopen smoke after the focused `EngineTests` case:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --smoke-test --save-scene-smoke
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --scene-authoring-smoke
```

They are workflow checks for editor save/reload and project save/reopen, respectively; they do not establish sector/local snapshot or raster propagation.

For the entity Inspector, select a camera-bearing entity and confirm Transform exposes Position and Rotation but no Scale row; select an entity without a Camera component and confirm Scale remains editable. Camera zoom/projection remains controlled by Field of View or the applicable projection settings. This visual check is required because compilation and the headless scene smokes cannot prove control visibility.

`EngineTests` also verifies worker-local nested-job stealing, submitted/completed/stolen scheduler statistics, stable deterministic dependency order, typed immutable publication, retained task failures, dependent skipping, independent-branch progress, cycle rejection, and frame/task/thread/worker profiler identities. Exercise the real Application frame graph in both execution modes:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke --frame-task-single-thread
```

Both commands must publish the frame input, complete every caller-affine layer task, emit one terminal profile event per task, and print the matching `CPU frame task graph smoke passed` marker. These tests do not prove future workerized simulation, visibility/render preparation, translated-origin/raster snapshot consumption, command recording, priorities/cancellation, or Profiler-panel lane visualization.

## Phase 3C Prerequisite P1 Scene Raster Preparation

The current real viewport consumer proves the first split multithreaded-render prerequisite with the same Application frame task in both modes:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --scene-raster-preparation-smoke --scene-viewport-render-graph-smoke
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-single-thread --scene-raster-preparation-smoke --scene-viewport-render-graph-smoke
```

`SceneRasterPreparationV1` must report `Frame.PrepareSceneRaster`, the current snapshot frame, immutable instance count, and `worker=<index>` in parallel versus `worker=caller` in deterministic mode. Both runs must also emit the backend's existing `SceneViewportRenderGraphV1 ... comparator=exact-byte-pass` marker: the graph consumes the prepared frame and still matches the direct recorder byte-for-byte. This proves one real worker-safe CPU preparation consumer and its inline oracle; it does not claim simultaneous preparation/recording, worker-safe callbacks, parallel RHI command-list recording, or changed GPU submission/retirement policy.

## Phase 3C Expected-Before RHI Prerequisite And P2 Worker Recording

Run the headed backend harnesses in both recording modes:

```powershell
.\Scripts\TestRender.ps1
.\Scripts\TestVulkan.ps1
```

Each parallel run must emit `RenderGraphRecordingV1 backend=<D3D12|Vulkan> mode=worker workerPasses=2 overlap=yes submitted=3 result=pass`; its deterministic-inline companion must emit the same marker with `mode=inline`, `overlap=no`, and three submissions. Both modes also require `SceneRasterPreparationV1` for the corresponding lane and `SceneViewportRenderGraphV1 ... comparator=exact-byte-pass`. The bounded deterministic `EngineTests` contract separately requires caller-affine default callbacks, two explicitly eligible independent callbacks overlapping without a sleep gate, identical inline callback recording, same-effective producer-token submission order, cross-queue acquire deferral to caller recording, false/throw accepted-prefix handling with unsubmitted suffix disposal, stale expected-before rejection with no destination-state publication, fail-closed unsupported expected-before adapters, and exact-token context retirement/reuse. The same headed harness runs retain `RHIQueueDependencySmokeV1 ... cpuWaitBetween=no`, while the executor submits/publishes only in compiled order. Together this is the local D3D12/Vulkan evidence for the completed multithreaded render-preparation/recording line; cross-queue ownership acquire/release remains caller-recorded.

Exercise real Editor-to-Renderer scene snapshot publication and retained epoch lifetime in both frame-task modes:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke --scene-render-snapshot-smoke
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke --frame-task-single-thread --scene-render-snapshot-smoke
```

Both commands must publish the stable-ID editor viewport view after mutable layer update, validate counts and authoritative main-camera identity against the active Scene, retain the previous immutable epoch while publishing the next, consume the initial one-shot discontinuity only on a valid view epoch, and print both `CPU frame task graph smoke passed` and `Scene render snapshot smoke passed`. This is portable extraction/publication evidence; the D3D12-specific raster behavior is exercised separately below.

## Phase 3 GPU Timing P1 Query Lifecycle

P1 is deterministic backend-neutral contract evidence. Run the canonical Debug build and the complete deterministic suite:

```powershell
.\Scripts\Build.ps1 -Configuration Debug -Action vs2022
.\bin\Debug-windows-x86_64\EngineTests\EngineTests.exe
```

The `RHI timestamp query lifecycle preserves generation-safe nonblocking results` case must prove timestamp-only nonzero bounded creation, exact-device ownership, overflow-safe ranges, reset/write/resolve order, poisoned failed recordings, failed-submission rollback, accepted exact-token `Pending` publication, no-CPU-wait completion-gated reuse, `Ready`/`Disjoint`, stale generation and duplicate-token/completion rejection, and a four-generation history bound. One logical pool is caller-serialized and has at most one pending generation; parallel command contexts use distinct bounded logical pools. Current D3D12/Vulkan query creation and query commands must continue to reject explicitly and timestamp capabilities must remain unimplemented until P2 native translation is qualified. A passing P1 test is not native timestamp, frame/pass timing, Profiler, or GPU-headroom evidence.

## Phase 3 GPU Timing P2A1 Transaction And Retirement

Use the same canonical Debug build and complete deterministic suite as P1. The `RHI timestamp query transactions retain native state through exact-token retirement` case must prove logical validation precedes every fake-native callback; native failure and a fully recorded but failed submission publish no generation; an accepted exact-device token publishes both logical generation and opaque shared state; invalid, cross-device, stale, incomplete, and duplicate-state operations reject; public-pool destruction after accepted publication cannot release the shared state before terminal completion; reuse requires both result completion and exact-token retirement; and reserved plus unresolved state is bounded at four. D3D12/Vulkan query creation and recording must still reject and capability state must remain unusable. P2A1 evidence does not qualify pre-submit public-wrapper destruction, pre-submit publication feasibility, a native timestamp, frame/pass scope, Profiler duration, or pacing headroom.

The same deterministic case also proves P2A2: the transaction retains logical state when the public wrapper is destroyed after recording but before submit; unfinished/poisoned/stale/unreserved state fails a non-mutating publication preflight before the simulated native-submit callback; a successful preflight freezes further recording; invalid/cross-device and same-pool duplicate tokens reject while one submission token remains valid across distinct logical pools; per-state values complete both pools; and retirement rejects until every matching generation is terminal. Backend query methods and capability state remain unavailable.

## Phase 3 GPU Timing P2B D3D12 Native Translation

Run the canonical zero-warning Debug build, complete deterministic suite, and focused bounded native smoke:

```powershell
.\Scripts\Build.ps1 -Configuration Debug -Action vs2022
.\bin\Debug-windows-x86_64\EngineTests\EngineTests.exe
.\Scripts\TestD3D12Timestamps.ps1 -Configuration Debug -Action vs2022 -SkipBuild
```

The focused script must exit successfully, leave no Editor child alive, report D3D12 timestamps advertised/enabled/implemented with a nonzero graphics-queue frequency, and emit `RHITimestampQuerySmokeV1 ... allocation=pass ... writeResolve=pass, pending=pass, readback=pass, reuse=retired-pass, destruction=retained-pass, result=pass` with a positive `periodNanoseconds`. `Pending` is observed after accepted publication and before completion collection; mapping and logical completion occur only when the exact completion fence is terminal. This qualifies the D3D12 native RHI bridge only. `Phase3FrameTimingV1` must remain on `CpuSteadyClock` until P3 adds whole-frame/pass identity and asynchronous Profiler publication; the smoke is not GPU-headroom or pacing-selection evidence.

## Phase 3 GPU Timing P2C Vulkan Native Translation

Run the same zero-warning Debug build and deterministic suite, then the focused backend smoke:

```powershell
.\Scripts\TestVulkanTimestamps.ps1 -Configuration Debug -Action vs2022 -SkipBuild
```

The script must exit successfully with no surviving Editor, report timestamps advertised/enabled/implemented with nonzero graphics `timestampValidBits` and positive physical `timestampPeriod`, and emit the same `RHITimestampQuerySmokeV1` pass fields for `backend=Vulkan`. The adapter must record reset/write only through the live NVRHI command buffer, call `vkGetQueryPoolResults` only after the exact NVRHI submission is complete and without `VK_QUERY_RESULT_WAIT_BIT`, keep `VK_NOT_READY` nonterminal, mask values to valid bits, and publish other native errors as failed/disjoint. This qualifies the Vulkan native RHI bridge only; P3 renderer instrumentation and GPU headroom remain separate.

## Renderer Verification

For the Windows D3D12 viewport live-pipeline slice, run the zero-warning Debug build and complete deterministic suite, then:

```powershell
.\Scripts\TestLiveD3D12PipelineRebuild.ps1 -Configuration Debug -TimeoutSeconds 60
```

The script copies `Engine/Shaders/EditorViewport.hlsl` beneath ignored `output/`, launches one bounded hidden Editor against that disposable source, and requires three published generations: initial source, a valid source edit, and recovery after restoring the source. An intervening invalid Slang edit must report failure while explicitly retaining the last valid D3D12 viewport pipeline. Success additionally requires the process to end without forced cleanup after the bounded application-stop marker and complete engine log shutdown; Windows PowerShell 5 does not expose an exit code for this redirected `Start-Process` path. The deterministic `D3D12 viewport shader reload` test independently proves unchanged-revision suppression, stale-ticket rejection, failure retention, single publication, epoch invalidation, and a fresh replacement epoch. This evidence is D3D12 viewport-only; it does not qualify a generic hot-reload system or another backend.

## Render Graph Construction Verification

The construction compiler is verified by deterministic `EngineTests`, not a renderer capture: tests must prove stable topological ordering (including independent passes), RAW/WAR/WAW edges, transient read-before-write rejection, invalid declaration diagnostics, explicit dependency-cycle rejection, lifetimes in compiled rather than registration order, state barriers, and cross-queue transition records paired with a resource dependency. This evidence verifies only the logical plan. Physical/imported binding, RHI barrier emission, queue signal/wait submission, callback execution, GPU retirement, transient allocation/reuse, and viewport integration require the next execution/integration item and its runtime evidence.

The single-queue executor has two complementary evidence layers. Deterministic `EngineTests` use a minimal fake RHI device/list/resource set to prove that foreign/description-invalid bindings and imported committed-state mismatches fail before `Begin` or any callback; undeclared and wrong-kind context lookup returns null; callback failure prevents every later callback and submission; invalid/failed completion prevents acquisition work; compiled texture/buffer barriers and callbacks retain exact order while pending states stay unpublished; successful submission commits final states; three incomplete tokens exhaust the bounded pool; and completing only the second token reuses only the second context and same command-list object.

The current Scene viewport consumer builds three imported-resource Graphics passes per render on D3D12 and Vulkan: `Scene Viewport Graph Clear`, `Scene Viewport Graph Raster`, and `Scene Viewport Graph Output Handoff`. The executor records those stable pass names as RHI debug markers around the restricted callbacks; the output handoff finishes the color target in `ShaderResource` for the unchanged native presentation/ImGui bridges. Under `--scene-viewport-render-graph-smoke`, both backends also create separate renderer-owned reference color/depth outputs, replay the former direct recorder with the same immutable prepared frame, clear, pipeline/resources, constants, and draw order, and compare the graph/reference RHI readbacks for equal extent, row pitch, and exact bytes. The graph output is restored to `ShaderResource`; the reference output is never exported or presented. `SceneViewportRenderGraphV1` reports the backend, exact-byte result, dimensions, and byte count, and `TestRender.ps1`, `TestVulkan.ps1`, and `TestVulkan.sh` require `reference=direct comparator=exact-byte-pass`. Exact-head CI run `29430304670` passed those earlier comparator markers on Windows D3D12, Ubuntu Vulkan/lavapipe, and macOS Intel/MoltenVK; dependency submission `29430304809` passed.

Production graph lifetime is now asynchronous. `SubmittedRenderGraphFrameOwner` retains a bounded frame's graph contexts, exact accepted tokens, frame/pass identity, and external per-frame constant resources until nonblocking completion; ordinary frames no longer call `WaitForCompletion` between graph submission and presentation. Deterministic `EngineTests` must prove payload retention, stable labels and exact tokens, accepted-prefix failure retention, explicit four-frame capacity, truthful failed-token state, and zero calls to the fake device's wait method. `TestRender.ps1` and `TestVulkan.ps1` require at least two `ProductionRenderGraphRetirementV1 backend=<backend> frame=<id> passes=3 cpuWaitBetween=no pending=<n> result=pass` markers plus the existing exact-byte comparator. The 2026-07-16 local RTX 3080 Ti runs passed both scripts; the final zero-warning Debug build passed 67/67 tests. Device-idle release is a shutdown/device-replacement rule, not a normal-frame pacing mechanism.

P3A instruments each accepted effective-Graphics production RenderGraph pass with a separate two-query transaction and retains its pool/generation through the same asynchronous owner. Deterministic `EngineTests` prove reset/start/work/end/resolve ordering in caller and worker lanes, exact frame/pass/token/generation identity, failed-token disjoint publication without release, independent-queue rejection before recording, the 12-state retirement bound, and zero wait calls. `TestRender.ps1`, `TestVulkan.ps1`, and `TestVulkan.sh` require at least two `RenderGraphTimestampScopesV1 backend=<backend> frame=<id> scopes=3 raw=ready cpuWaitBetween=no result=pass` markers. The 2026-07-16 local RTX 3080 Ti D3D12 and Vulkan scripts passed with the exact-byte comparator; the final zero-warning Debug build passed 68/68 tests. These are raw endpoints only: P3B owns tick conversion, exact-frame Profiler publication, capability-path promotion, and CPU fallback evidence.

P3B converts only a complete exact-frame set of P3A scopes on one effective Graphics clock. Deterministic `EngineTests` prove named-pass end-minus-start conversion, the non-summed first-start-to-last-end whole-frame interval, exact retained-frame amendment, cross-queue/generation rejection, and explicit ready/pending/unavailable/disjoint states. They also launch 12 simultaneous per-pass reservations against one retirement queue, require all distinct reservations to survive, reject the thirteenth, and prove destruction releases the bound; this is regression coverage for the worker-recording race exposed by the Vulkan headed smoke. `TestRender.ps1`, `TestVulkan.ps1`, and `TestVulkan.sh` require `RendererGpuTimingV1 backend=<NVRHI D3D12|NVRHI Vulkan> frame=<id> passes=3 wholeMs=<positive> status=Ready capability=GpuTimestamps result=pass`; the PowerShell D3D12 harness requires at least two ready publications and Vulkan requires at least one before its close gate. An initialization-smoke result with no retained application timing frame emits `RendererGpuTimingDropV1 ... status=Unavailable ... result=ignored` and must not fail rendering or amend another frame. The 2026-07-16 local RTX 3080 Ti build completed with zero warnings/errors, 70/70 tests passed, both headed scripts passed, and no Editor process survived.

P3C calls `FramePacingBenchmarkCapture::AmendGpuTiming` only after P3B has identified a publication's exact frame. The deterministic benchmark test proves a ready matching frame receives its duration and `1000 / effective Smooth target FPS - duration` headroom, while an evicted ID, absent ID, nonpositive target, pending/unavailable/disjoint status, or invalid duration leaves headroom unavailable. Schema-3 CSV/JSON fixtures require explicit `gpuTimingStatus`, `gpuDurationMs`, and `gpuHeadroom`; schema-2 PresentMon fixtures remain accepted for backward compatibility. `Scripts/TestFramePacingBenchmark.ps1 -Backend D3D12` and `-Backend Vulkan` each run the complete 60/120 FPS Responsive/InterFrame/SubmissionGate matrix, require 512 retained frames and at least one ready exact-frame duration per artifact, verify every eligible numeric headroom row against its own duration, and require Responsive headroom to stay unavailable. The 2026-07-16 local RTX 3080 Ti runs passed both matrices with 511 eligible headroom rows in each final condition and no surviving Editor. `TestPresentMonCorrelation.ps1` and the fake supervisor matrix also passed after the compatible schema admission change. This proves engine GPU-budget evidence only, not display/input delivery or a selected pacing behavior.

`TestRender.ps1`, `TestVulkan.ps1`, and `TestVulkan.sh` require topology-adaptive `RenderGraphExecutionSmokeV1`: a Graphics clear crosses to Copy for deterministic 3x2 RGBA8 readback. Independent queues require `topology=independent-copy, dependency=gpu-wait`; unavailable Copy requires `graphics-fallback, ordered-elided`. Fresh imported bindings drive a second run through the same graph so the aggregate accepted tokens retire and the same context is reused. `RenderGraphTransientAllocationSmokeV1` additionally creates real 64-byte transient buffers, proves compatible sequential lifetime sharing, waits the exact accepted tokens, and requires retired reuse with the non-aliased mode and estimated logical cost marker. Deterministic `EngineTests` separately proves an incomplete earlier token blocks reuse even if the final token is complete, including a later-failure accepted prefix. No native placed-resource/alias-barrier evidence is claimed.

The deterministic cross-queue executor test uses two buffers and one texture on one Graphics-to-Copy edge. It verifies all ordered release/acquire operations, one deduplicated producer dependency token, final state/owner publication, and that one invalid operation rejects the complete command-list batch before fake-native acceptance or partial publication. A separate four-pass same-effective-queue case proves the three-context limit is keyed by effective queue plus compiled pass identity: four earlier incomplete pass tokens do not block distinct pass identities, while three incomplete submissions for one identity exhaust only that key; completing its exact token reuses that identity's command-list object. A corrupted declared dependency with no producer token fails before the consumer callback/submission and retains the accepted prefix.

The buffer-transition prerequisite is verified separately from graph execution. `EngineTests` checks the backend-neutral state/usage contract: copy, read-only vertex/index/constant/structured, storage write, CPU-visible, texture-only, and unknown-state cases. `TestRender.ps1` and `TestVulkan.ps1` require `RHIBufferTransitionSmokeV1` to reject out-of-recording, incompatible, and post-close calls before a valid whole-buffer CopyDest-to-CopySource recording is closed and synchronously submitted. Local Windows/MSVC Debug evidence passed D3D12 and Vulkan/NVRHI validation paths. This does not bind graph resources, invoke callbacks, implement cross-queue ownership, or prove hosted physical-device breadth.

The imported-resource state-query prerequisite is verified separately from graph execution. Deterministic `EngineTests` prove exact-device ownership and rejection of null, foreign-backend, same-backend-other-device, and `Unknown` values. `TestRender.ps1` and `TestVulkan.ps1` require the real-device `RHIResourceStateSmokeV1`: an exact-owned texture and buffer initially query `CopyDest`; valid transitions stay invisible while recording and after an invalid `Unknown` recording attempt; after successful synchronous submission both query `CopySource`; null and an explicitly unknown wrapper state are rejected. The one-device headed smokes do not claim real two-device evidence. This is wrapper-state observation only: no state adoption, graph execution, cross-queue scheduling, transient allocation, or viewport adoption is exercised.

The D3D12 texture-readback prerequisite is verified by `TestRender.ps1`'s real-device `RHITextureReadbackSmokeV1`. It creates a three-by-two RGBA8 color/depth offscreen target, clears it through `Engine::RHI`, transitions only the color target to `CopySource`, and requires `ReadbackTexture` to return the exact clear bytes with extent `3x2`, `RowPitchBytes=12`, and exactly 24 bytes. It also requires rejection before submission for a same-device target in the wrong state and for a CopySource R8 target. The marker must report `invalidState=rejected`, `unsupportedFormat=rejected`, `submit=pass`, `readback=pass`, `layout=tight`, and `result=pass`. The smoke uses no native presentation/capture bridge. `TestVulkan.ps1` retains the existing Vulkan/NVRHI clear/readback regression; neither result proves graph execution or cross-queue scheduling.

The queue-topology/GPU-dependency prerequisite is verified separately from graph scheduling. Deterministic `EngineTests` prove requested/effective/independent resolution, dependency-token rejection, and Vulkan same/different-family policy. `TestRender.ps1` retains its D3D12 normal and forced-fallback evidence. `TestVulkan.ps1` adapts to selected topology: independent Copy/Compute must report real GPU waits and queue-local retirement, while unavailable classes report ordered Graphics fallback. On the local split-family RTX 3080 Ti it also requires rejection of a Graphics-owned resource on Copy before recording/submission. These smokes do not qualify RenderGraph cross-queue translation or Vulkan different-family ownership barriers.

The buffer queue-ownership contract has deterministic and native evidence. `EngineTests` drives the same `BufferOwnershipTracker` used by both production adapters through private release/acquire recording, submission validation, accepted-release pending publication, exact-token acquire, accepted destination publication, immutable pending snapshots, and explicit recovery. It rejects null/unregistered resources, incompatible usage/state (including every CPU-visible buffer), wrong owner/before-state, same-effective or wrong list queue, zero/foreign/mismatched tokens, missing/duplicate dependencies, duplicate operations, and second release. Tests also prove that failed release acceptance publishes nothing; failed acquire preserves pending; pending rejects ordinary transition, copy, state update, owner/state observation, and destruction; and independently completed tokens recover only their matching buffer. D3D12 maps release to no barrier and acquire to a portable transition. Vulkan maps different-family pairs to matched native release/acquire barriers and reseeds NVRHI after the raw barrier without publishing pending wrapper state. `TestVulkan.ps1` requires the topology-adaptive paired lifecycle: an independent queue proves GPU-only source and validation-destination movement, exact bytes after a Graphics copy to CPU readback, final Copy ownership/state, recovery, and retirement without a CPU wait between forward release/acquire submissions; Graphics fallback rejects without pending publication. The cross-queue RenderGraph executor now consumes this contract as described below.

`EngineTests` includes GPU-independent renderer-capability policy coverage. It proves lifecycle invariants, deterministic candidate ranking, retained rejection reasons, required format-usage validation, compatible queue fallbacks, strict selection by stable adapter ID, and `Phase3FrameTimingV1` selection of usable GPU timestamps versus the CPU steady-clock fallback. These tests do not prove a physical adapter or backend runtime path.

Windows D3D12 viewport behavior and non-blank capture:

```powershell
.\Scripts\TestRender.ps1 -Configuration Debug -Action vs2022
```

This smoke requires the D3D12 versioned-profile selection marker before accepting the capture. It also requires exactly one successful `PortableShaderTerminalV1` marker for each vertex/pixel stage and one complete `D3D12PortablePipelineV1 status=active` marker. Those structured markers must agree on valid 64-hex package keys, terminal success/cache-hit state, cache mode/source, nonzero reflected bindings, vertex-input counts, compiler `Slang-2026.13.1`, backend identity, exact targets `DXIL+SPIR-V`, convention schema 1 (row-major, zero-to-one D3D clip depth, inverted SPIR-V Y, clockwise front face, D3D register-space bindings), and `legacySourceCompile=false`. Pending-only, failed, cancelled, unknown, duplicate-stage, key/status/cache-mismatched, or legacy compile evidence fails the smoke.

The current prototype's visible shader demonstration is a procedural checker driven by stable 0-1 UVs duplicated per cube face. A headed capture must show alternating light/dark cells inside the non-background prototype geometry without one-pixel triangle-diagonal striping; the 2026-07-14 local `editor-viewport.bmp` capture was visually inspected after both `TestRender.ps1` and `TestVulkan.ps1` passed. Exact-head run `29363501290` passed the corrected UV input/reflection and shader on Windows D3D12, Ubuntu Vulkan, and macOS MoltenVK, together with portable tests and style. This demonstrates interpolator and pixel-shader execution through the portable package, not sampled textures, samplers, material descriptors, or texture-asset upload.

For the Phase 3C viewport-output contract, this same real D3D12 capture is the focused behavior test: the scene renderer binds renderer-owned RHI color/depth targets, clears them, records viewport/scissor/pipeline/prototype draws, and transitions the color output to shader-resource state without receiving a native D3D12 command list. Presentation may still wrap its currently recording list and owns swapchain/ImGui/SRV/capture mechanics. This evidence is Windows x86_64/MSVC D3D12 only; it does not exercise the later Vulkan NVRHI-output-to-native-presentation/ImGui handoff.

The same run launches `--scene-origin-raster-smoke`, waits for a stable editor-viewport projection, and captures cases A/B/C at trillion-unit coordinates around a real 4096-unit sector boundary. A and C place camera/origin and mesh together on opposite sides of that boundary and must be byte-identical; B moves only the mesh across the boundary and must differ, retain comparable foreground area, and move the foreground centroid right. Renderer diagnostics identify the current snapshot frame/entity/canonical sector-local position/origin/relative position and exactly one issued draw in each case. The measured MSVC D3D12 run produced byte-identical A/C images, a 196.24-pixel rightward B centroid shift, and a 13.20% non-background ratio. It proves the selected Windows x86_64/MSVC D3D12 viewport consumed Slang-generated DXIL and issued the real prototype draw. The paired SPIR-V output is consumed separately by the Vulkan indexed-draw and immutable-Scene viewport smokes described below; it is not evidence that this D3D12 capture itself exercised Vulkan. Normal editor startup compiles through job-system fire-and-poll; the smoke intentionally selects deterministic-inline execution. Slang v2026.13.1 and Windows x86_64 DXC v1.9.2602 are exact admitted pins; other Slang host archives are acquisition targets only, and redistribution remains blocked by the binary-component/notice audit in [DEPENDENCIES.md](DEPENDENCIES.md). This D3D12-only capture does not qualify Vulkan scene raster, Linux, macOS, MinGW, Windows ARM64, real mesh/material resources, culling, coordinate debug views, physics, or ray/TLAS/query consumers.

To exercise the strict runtime failure path, launch a missing adapter and require a nonzero exit plus per-candidate strict-preference rejection diagnostics:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --smoke-test "--renderer-adapter=Definitely Missing Spiral Adapter" --renderer-adapter-strict
```

Vulkan device, NVRHI wrapping, native ImGui presentation, resize, and successful post-resize present:

```powershell
.\Scripts\TestVulkan.ps1 -Configuration Debug -Action vs2022
.\Scripts\TestVulkan.ps1 -Configuration Debug -Action gmake
```

The Vulkan smoke also runs `--vulkan-rhi-indexed-draw-smoke` and `--vulkan-scene-viewport-raster-smoke`. The indexed draw requires Vulkan 1.3 dynamic rendering and synchronization2 to be selected before device creation, compiles and validates the admitted `EditorViewport.hlsl` SPIR-V package, creates the reflected constant-buffer and vertex-input layout, records an indexed triangle into a 32x24 offscreen RGBA8/depth framebuffer, uploads vertex/index/identity-constant data through the RHI, and reads the color target back. Acceptance requires the structured `VulkanRHIIndexedDrawV1` marker with package, reflection, pipeline, constants, draw, submit, readback, interior, and background all `pass`; the deterministic local Windows reference produced interior RGBA `210,59,40,255` and 172 non-background pixels. The Scene smoke publishes one immutable camera/mesh snapshot, consumes that published epoch through the renderer, rasters the shared prototype cube into renderer-owned color/depth outputs at 48x36 and then 64x48, and requires `VulkanSceneViewportRasterV1` to report pass for raster, readback, geometry, background, and resize. The real viewport smoke additionally requires `VulkanSceneOutputCaptureV1` for the renderer-owned post-resize output and `VulkanSceneOutputHandoffV1` for its matching descriptor registration, Editor ImGui queueing, and successful post-resize swapchain present. The scripts reject a capture/handoff generation below 2, so a clear-only presentation or pre-resize descriptor cannot satisfy this evidence. This is current Scene-output-to-presentation evidence on the named local/hosted Vulkan device classes, not physical-device breadth or Production qualification.

The final zero-warning MSVC Debug build and `EngineTests --test "Cooked mesh artifacts validate and resolve transactionally"` prove the default-scene constructor rejects an invalid handle without mutating its output and otherwise creates/resolves the supported 24-vertex/36-`UInt32` single-primitive artifact; the full suite passed 81/81. `TestVulkan.ps1` proves the fixture publishes that artifact through a copied resolver before its immutable snapshot and reports `VulkanSceneViewportRasterV1 ... artifact=pass`. `TestRender.ps1` proves D3D12 default/origin-raster setup uses the normal resolvable handle while retaining its exact-byte graph/reference and origin-shift evidence. Vulkan completion validation parses the reported swapchain generation and requires at least generation 2; it no longer mistakes a valid later generation such as 3 for failure. These markers establish fixture/default artifact publication only, not GPU mesh binding, descriptor/material behavior, asset freshness, or new backend qualification.

Linux/macOS use:

```bash
bash Scripts/TestVulkan.sh Debug gmake
```

The Vulkan smokes require the versioned-profile selection marker in addition to device/NVRHI creation and presentation evidence. `TestVulkan.ps1 -Action vs2022` is local Windows Vulkan evidence when run on a Windows machine with an admitted Vulkan implementation. The hosted workflow's Windows job runs the D3D12 render smoke only; hosted Vulkan runtime evidence is limited to its Ubuntu lavapipe and macOS Intel Apple-Paravirtual/MoltenVK smoke jobs. Exact-head run `29369950355` passed the strengthened Scene-output capture and ImGui/swapchain handoff smoke on both hosted Vulkan jobs (the macOS job completes three launches), alongside Code Style and Windows D3D12 regression. A `VulkanRHICoreV1 markers=executed-balanced` result proves that the balanced marker calls were issued through NVRHI during the smoke and that close rejected an unbalanced marker; it is not a GPU-capture or external-debugger label-readability claim.

Native Apple Silicon project-generation, build, or MoltenVK presentation claims require a completed run on a native arm64 macOS environment. Project generation on another platform, source inspection, x86_64 macOS results, cross-compiled artifacts, and hosted CI jobs that execute zero steps are not native Apple Silicon evidence.

Repeat the same full launch contract when investigating presentation reliability:

```bash
VULKAN_SMOKE_ITERATIONS=3 bash Scripts/TestVulkan.sh Debug gmake --skip-build
```

The Vulkan smoke requests a resize once, then exits successfully only when the currently tracked recreated swapchain generation has completed an actual present. Its frame limit is a bounded failure deadline, not the success condition. Hosted macOS CI runs three complete launches and uploads every attempt log.

The D3D12 render smoke also requires the selected Bootstrap capability identity/state markers before accepting its viewport capture. Vulkan smokes require the Bootstrap profile, timeline lifecycle, and buffer-device-address lifecycle markers in addition to NVRHI device creation and presentation. These markers verify that the real startup path published the report; they do not by themselves qualify formats, future optional features, or production hardware.

The D3D12 and Vulkan scripts also launch with `--renderer-capability-smoke`. The editor validates the renderer-owned capability snapshot, executes the Profiler diagnostics drawing path, and emits required markers containing the exact Bootstrap profile, adapter, device qualification, format/feature/group/candidate counts, and `Phase3FrameTimingV1` path/lifecycle. The group marker is emitted only after a real frame uses the selected timing path and a native present succeeds. Current D3D12 and Vulkan smokes must report `GpuTimestamps` as preferred, `CpuSteadyClock` as selected, `exercised=yes`, group qualification `Presentation`, and device qualification `Bootstrap`. This exercises the current portable timing fallback on the named backend/device; it does not implement GPU timestamp recording/resolve, prove a future group's fallback, or qualify Scene/Production rendering. Inspect the full editor panel visually when changing its layout or interaction rather than treating the viewport-only BMP as a panel screenshot.

A backend claim requires that backend's smoke or a stronger representative scene/capture test. A presentation smoke does not qualify scene-resource rendering. A WARP/lavapipe/llvmpipe/Apple-Paravirtual result must be labeled as that device class and must not be generalized to physical production hardware.

Frame-pacing/profiler completion requires a deterministic marker trace that carries one frame ID through engine `FrameStart`, input/simulation, render submission, `Present` begin/end, GPU completion, and display feedback where available. Tests must prove that the primary in-game frametime series is consecutive released engine-start cadence, that intentional pacing wait and CPU active work are separate, and that injected inter-frame and submission-gate delays appear at their actual control points. A flat limiter/Afterburner/RTSS hook-local graph, average FPS, or present cadence alone is not completion evidence. When display or input-to-photon feedback is unavailable, diagnostics must report it unavailable rather than substituting engine or `Present` cadence.

The current prerequisite is exercised by `FrameLifecycleTelemetryV1` in both headed scripts. It waits for an actual GPU-completion observation (including valid completed frame 0), requires the application frame ID, ordered start/input/submission/present markers, `intentionalWait=not-applied:0`, backend-specific mandatory waits, and explicit unavailable input/display/replacement status; D3D12 latency-object and Vulkan acquire/fence waits are mandatory classifications, not pacing evidence. Presentation-policy changes are exercised through the same `TestRender.ps1 -Configuration Debug -Action vs2022` and `TestVulkan.ps1 -Configuration Debug -Action vs2022` paths: initial/transition/resize markers must retain requested/capability/actual/fallback/generation and every mandatory lifecycle wait. A truthful synchronized/FIFO fallback is admissible; VRR-active or displayed-cadence claims are not.

`TestRender.ps1` and `TestVulkan.ps1` additionally run bounded `--smooth-frametime-candidate-smoke` launches for `--smooth-frametime-candidate=inter-frame` and `submission-gate` at `--smooth-frametime-target-fps=5`. Each launch must emit `SmoothFrametimeCandidateSmokeV1` with the selected candidate, a nonzero intentional wait, released `startToStartMs`, separate `cpuActiveMs`, GPU-completion observation, backend-specific mandatory waits, and explicit `inputLatency=unavailable display=unavailable replacementDrop=unavailable`. Submission-gate runs must also emit `SmoothFrametimeNativeV1` at `pre-ExecuteCommandLists` on D3D12 or `pre-vkQueueSubmit` on Vulkan. The low target makes the control point visible; it does not qualify production timer precision, displayed smoothness, latency, or candidate selection.

The same scripts also run same-process `--smooth-frametime-target-change-smoke` transitions on InterFrame. Both 5-to-10 and 5-to-1000 FPS cases require the published old/new targets, the `next-valid-frame-boundary` contract, a completed frame with the new requested target, GPU-completion observation, bounded exit, and process-tree cleanup. Below-target evidence requires `RequestedTargetCadence` sourced by the terminal cadence frame `N`; above-target evidence requires CPU/GPU/exact-backend-present sourced by `N-1`, or qualified `Unresolved` with no source. Deterministic tests separately prove SubmissionGate uses `N-1`, current CPU `N` cannot claim the preceding interval, simultaneous qualifying sources stay unresolved, exact present identity is required, the measured 110.84 ms WARP release remains an InterFrame source for a 100 ms target, and delayed exact GPU `N-1` reclassifies its dependent cadence `N`. Neither marker proves monitor refresh, actual displayed cadence, or a production pacing choice.

`FrameLifecycleTelemetryV1` requires `frame-start,input-sample,input-simulation,render-submission,present-begin,present-end` for one exact application frame. The active GLFW poll completes after `Renderer::BeginFrame` and before immutable frame-input publication; minimized windows poll without inventing a timing sample. Deterministic validation accepts one finite nonnegative same-frame sample and proves rejection is failure-atomic. The latency resolver progressively publishes exact-source input-to-simulation/submit/Present intervals and rejects duplicate, missing, regressing, or wrong-frame endpoints without mutation. `TestRender.ps1` and `TestVulkan.ps1` require source frame equal to trace frame and `0 <= simulation <= submit <= Present`, retain DXGI latency or Vulkan acquire/fence waits separately, and require input-to-display/click-to-photon unavailable. D3D12/Vulkan benchmark scripts enforce the same relation in every retained schema-5 frame; attachment QPC checks include the sample row, while normal runs may retain `qpc=0`.

The benchmark-capture contract has deterministic tests for bounded retention/eviction, frame-ID continuity, percentile and 1%/0.1% fixtures, deadline-miss/overshoot aggregation, raw-spike preservation, unavailable-field preservation, exact-frame GPU amendment, and stable CSV/JSON schema/order. Bounded D3D12/Vulkan launches qualify engine trace completeness, condition metadata, and same-frame GPU target-budget headroom only; they cannot claim display cadence, VRR state, or input latency.

`Scripts/TestFramePacingBenchmark.ps1 -Backend D3D12` and `-Backend Vulkan` run bounded 60/120 FPS Responsive, InterFrame, and SubmissionGate conditions and write ignored schema-7 CSV/JSON artifacts. Each condition discards 30 warm-up frames before retaining 512 raw frames. The frozen engine-owned condition records requested presentation policy, capability, actual mode, fallback, sync, tearing, and generation; caller strings cannot spoof those values and VRR remains `unavailable`. The engine retains lifecycle/wait/control-point rows, cadence/limiter source identity, exact input-stage source/intervals, and deterministic cadence/CPU/wait summaries. Applied intentional waits additionally retain the selected deadline primitive, fallback/reason, timer and portable wait durations, at-most-0.5 ms active-tail budget plus measured tail, process-CPU/wall proxy, requested deadline, release, and overshoot. Requested/effective targets and exact-frame GPU duration/headroom remain separate. Display, replacement/drop, input-to-display, generic input latency, power/energy, and click-to-photon remain unavailable. These artifacts do not prove displayed smoothness, external RTSS behavior, VRR state, end-to-end latency, or a selected candidate.

The external-capture attachment prerequisite runs independently of a collector. `Scripts/TestFramePacingBenchmark.ps1 -Backend D3D12 -ExternalAttachment` and `-Backend Vulkan -ExternalAttachment` own readiness/release files and run one released condition plus stale-run and timeout failures. After renderer/window initialization, the editor publishes schema-1 readiness with exact run/process/QPC identity and waits for bounded release. Released schema-6 artifacts carry nonzero Windows QPC ticks for every lifecycle marker; normal rows retain `qpc=0`. The runner proves exact identity, 512 released rows, stale-release rejection, timeout failure without an artifact, and process cleanup.

The implemented deterministic parser is `Scripts/JoinPresentMonCorrelation.ps1`. It accepts schema-2 through schema-7 engine JSON and PresentMon CSV paths, hashes them before and after parsing, and preserves the same exact identity/QPC/one-to-one causal join contract. Schema-5 through schema-7 compatibility are deterministic test evidence only; engine-stage input and deadline-wait fields are not consumed as display or end-to-end latency evidence.

`Scripts/CapturePresentMonCorrelation.ps1` is the Windows live supervisor. The caller supplies an explicit PresentMon executable and expected SHA-256; the script requires exactly version 1.10.0, owns a collision-safe capture directory and unique ETW session, validates the exact Editor readiness identity, and starts PresentMon for that PID/QPC domain. After the bounded ready-and-alive gate and released Editor exit, PresentMon must exit naturally with code zero, leave no known owned descendant, and relinquish its exact ETW session before the supervisor reads, stabilizes, hashes, validates, or joins its CSV. A lingering writer, nonzero exit, timeout, descendant, or surviving session fails before raw acceptance; `finally` still verifies exact owned cleanup and publishes the failed receipt. Stable schema-2 through schema-7 engine input, exact-header/PID collector CSV, immutable hashes, and the one-to-one join remain unchanged acceptance boundaries. Schema-5 engine-stage input, schema-6 deadline-wait fields, and schema-7 engine-owned requested/capability/actual/fallback/generation/synchronization/tearing presentation conditions do not change collector correlation or become VRR-active/display/end-to-end latency evidence. The supervisor no longer accepts caller-supplied presentation labels.

The 2026-07-18 deadline-precision acceptance used the zero-warning Debug build, 76/76 `EngineTests`, code style/diff checks, schema-5/schema-6 parser compatibility, the complete fake PresentMon supervisor matrix, and both complete local RTX 3080 Ti D3D12/Vulkan schema-6 benchmark matrices. Deterministic tests prove early-wake re-arm, the at-most-0.5 ms tail budget, creation/arm/wait fallback reasons, handle cleanup, and the pre-existing 60/120 target/candidate/reset/late/no-drop pacer state. Every applied native Windows wait identified `WindowsHighResolutionWaitableTimer`, reported zero fallback, and retained nonnegative timer/tail/process-CPU/wall fields. D3D12 waited-release overshoot across 60/120 InterFrame/SubmissionGate was 0.0010-0.0014 ms p50 and 0.0195-0.3891 ms p99, versus the pre-fix roughly 14.3/15.5 ms. D3D12 60 FPS cadence returned to approximately 16.67 ms p50; 120 FPS conditions often had fewer applicable waits because real frame work exceeded 8.33 ms. This verifies the deadline primitive and its evidence only, not a physical display cadence or candidate winner.

The completed automated production bundle is `output/production-smooth-schema6-20260718`. Its `engine-matrices` directory freezes all twelve schema-6 D3D12/Vulkan 60/120 Responsive/InterFrame/SubmissionGate condition artifacts. Every one of the six D3D12 PresentMon conditions joined 512 exact-PID/QPC pairs with 29 warm-up and zero trailing rows, reported zero drops, exited Editor and collector naturally with code zero, retained matching before/after collector hashes, and verified cleanup. The report hashes for 60-responsive/inter-frame/submission-gate are `3912c253178c0425ec213b2f225502e1d7e8a85f630b091abe6cf54f633740f5`, `ba91a86c4412c250edc9da564041d6459abf4802e909727b9f1e730b1327e936`, and `0447c9a2dfd5c2af79eb02f74cb652449b074e004e24609268a739f330106beb`; the 120 counterparts are `40efac8248367f0aed7d269e559c015bdae39df0451a6608ddffbdd7499ab7af`, `7c7d2685a664249543b35c078fb72470ac0433a393610bcba62d63f82df93c91`, and `40594fb65760e80cd5faf26b0dc01cc77699b47a05dce02b83df476bde9f1259`. D3D12 60 FPS InterFrame engine/present/PresentMon-display-change p50 is 16.668/16.629/16.667 ms and p99 is 16.718/18.831/22.240 ms. At 120 FPS InterFrame the corresponding p50 is 11.160/11.208/11.111 ms, truthfully above the requested 8.33 ms because the workload is over budget. SubmissionGate retained 51.706 ms and 96.296 ms engine p99 spikes at 60/120 rather than smoothing them away. The Vulkan 60-responsive attempt failed before report publication because frame 31 had two causal collector candidates; its receipt records `success=false`, `joined=false`, both zero natural exits, cleanup verified, engine hash `eebcc2f52abab5dad33bd5373d1037c96b7fcf94051ff5195ec98076dd682e94`, collector hash `fa2cbfb0d1f407a16e35bee9e64af56175afd1a1065ab5befdc6314ef33acba7`, and null report hash. This is accepted fail-closed evidence, not a Vulkan display correlation. None of these fields establishes a physical panel cadence, monitor/VRR state, RTSS/FES configuration, or input-to-photon latency.

RTSS `ASYNC` is not an automated capture condition on the audited RTSS 7.3.5.28001 installation. Its bundled SDK does not document an ASYNC/limiter-mode profile property or a transactional crash-recovery protocol, so scripts must not guess profile-file fields, drive the UI, or use telemetry shared memory as control authority. A manual RTSS reference run is admissible only when an operator completes one bounded receipt containing: the exact `Editor.exe` absolute path and exact RTSS profile; RTSS version and executable hash; before, applied, and restored captures for limiter target/mode, FES/scanline, frame-generation, and global-versus-application scope; the benchmark run ID and fixed timeout; restoration after success, failure, or timeout; and operator sign-off that only the exact Editor profile changed. Any global/other-profile/driver/FES/frame-generation mutation, missing restored capture, or unsigned receipt invalidates the RTSS condition. This receipt qualifies the external condition only; comparison and production selection still require the full engine/PresentMon evidence above.

The operator completed this manual gate on 2026-07-18 for RTSS 7.3.5.28001 (`RTSS.exe` SHA-256 `786d9c786c085d4bf14c6e2f08ab1779e01741ceeeb367f8c11db4ab99f9f68d`). Screenshot evidence captured the initial and after-each-target application list containing only `Global`, global limiter mode `Async`, passive waiting enabled, benchmark mode disabled, and RTSS frametime calculation at `frame start`; those global values were observed and left unchanged. For 60 and 120 FPS, the operator temporarily added only the exact Debug `Editor.exe` profile with Low detection, the named limit, Scanline Sync `0`, OSD/statistics off, stealth off, and custom Direct3D off, ran D3D12 then Vulkan, deleted the profile, captured restoration, and signed that Global/other profiles/driver/FES/Reflex/frame-generation state did not change. The ignored `output/production-smooth-rtss-async-passive-20260718/operator-evidence/manual-receipt.md` has SHA-256 `eaa0e1d10ca39a540d89af9cbc89a086941ae4a696c4ac388c64de672f85eb5e` and retains hashes for seven screenshots and every raw capture/report.

All four exact-PID PresentMon 1.10.0 runs used engine `responsive` so intentional engine wait remained zero while RTSS owned the external wait. Each joined 512/512 retained frames with 29 warm-up and zero trailing rows, exited Editor and PresentMon naturally with code zero, retained stable raw/report hashes, and verified process/session cleanup. D3D12 engine cadence p50/p95/p99 was 16.684/18.063/19.314 ms at 60 FPS and 11.402/17.487/25.615 ms at 120 FPS. Vulkan was 16.681/42.491/63.161 ms and 9.047/109.028/125.351 ms respectively; these large tails are retained evidence, not filtered as capture noise. RTSS's `frame start` statistics setting is condition metadata, not a substitute for engine lifecycle or PresentMon observations. Monitor, physical display cadence, VRR/tearing, input-to-display, and photon latency remain unknown, and this prerequisite selects no production default.

Run the script-only gates with:

```powershell
.\Scripts\TestInvokeBoundedProcess.ps1
.\Scripts\TestPresentMonCorrelation.ps1
.\Scripts\TestCapturePresentMonCorrelation.ps1
```

The supervisor test uses explicit `-TestMode` fake Editor/collector processes and proves collector-ready-plus-settle-before-release ordering, natural collector finalization before raw acceptance, exclusive CSV ownership after Editor exit, exact production argument contract, output collision preservation, stale readiness rejection, version/header rejection, liveness failure during settle, child/finalization timeout behavior, null hashes and no join/report on pre-acceptance failure, exact owned-session teardown success/failure/survival/timeout receipt states, and bounded descendant cleanup after kill-on-close. Those fake hooks are rejected outside `-TestMode` and are not native collector evidence. The final non-elevated local D3D12 capture is `output/presentmon-correlation/native-d3d12-desktop-20260716-191008`: PresentMon 1.10.0 and both exact children exited 0; the report retains 512 engine frames, 541 exact-PID collector rows, 512 pairs, 29 warm-up rows, zero trailing rows, and 158/207/1926 minimum/p50/maximum QPC deltas. The collector SHA-256 remained `3f296e3a32b01cde45efee875b152c731d4fcade9d9bea52b7db1c9dc3982259` after cleanup, the report SHA-256 is `dd1601d4d2653d616f178b8a1ed81cfd3c64a72f590997b315fba948bdbc2230`, no owned process survived, and the unique ETW session was absent. That historical capture predates the stricter finalization order and is not reused as production-selection evidence; the current automated bundle must be recaptured. Do not add `-restart_as_admin`, weaken readiness, or infer monitor/VRR/RTSS/FES/input/GPU-headroom evidence from the joined stream.

## Editor Perspective Viewport Navigation

Run the focused global-settings and navigation checks with:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --editor-settings-smoke --editor-viewport-navigation-smoke
```

The settings smoke must prove a missing settings file defaults to Fusion, a valid Unreal selection persists/reopens, malformed/unknown preset input is rejected without mutating the caller's value, and `.spiralproject` contains no navigation field. `ViewportNavigationSmokeV5` must prove both modeled GLFW disabled-cursor callback orders: press then stale pre-transition motion then capture arm then first real motion, and press then transition-time motion then first real motion. Neither stale/transition callback may translate camera/pivot; the first real and subsequent normal drags must equal their exact FOV/depth/viewport-derived camera-plane translations. It must also prove quick release and focus loss while capture is pending leave no capture state, and retain at nonzero pitch/yaw Fusion LMB/RMB isolation, cursor-anchor/no-crossing wheel zoom, MMB-held wheel, and post-zoom Shift+MMB zero-delta/bounded/reversible upright orbit continuity. Unreal must retain alternate LMB/fly behavior and disabled cursor capture. It must prove exact armed-capture cursor restoration, double-precision Scene main-camera movement, camera/snapshot agreement, and one valid discontinuity epoch after F explicitly focuses an existing selected entity origin. These callback orders are modelled at the Editor input boundary; no safe portable native Win32/GLFW injection seam exists in this repository. A bounded headed Windows D3D12 launch must exercise the real viewport and inspect its post-navigation capture; manual native Fusion/Unreal gestures remain a required acceptance gate. This is editor-perspective behavior only; it does not prove picking, multi-select, real geometry bounds, Autodesk custom/reset-pivot UI, bounds-aware Fit/Zoom-to-Fit, MMB-double-click fit, gizmos, orthographic navigation, camera piloting, or configurable bindings.

The 2026-07-16 acceptance combined the passing `ViewportNavigationSmokeV5` deterministic/controller evidence with the user's bounded real Windows D3D12 interaction over the focused viewport. The user exercised the requested Fusion/Unreal movement and cursor-capture path and reported no failure; this closes the native interaction gate without introducing a synthetic-input seam or treating `SendInput`/`PostMessage` as exact native evidence.

`Scripts/TestPresentMonCorrelation.ps1` provides focused synthetic success/failure coverage for wrong PID, missing/mismatched headers, missing rows, duplicate/reordered QPC, missing/ambiguous intervals, noncausal final input, and the successful report's distinct paired-row invariant. The saved ignored parser diagnostic and the complete live supervisor capture both preserve their raw engine/PresentMon files. PresentMon-derived present/display cadence or displayed/dropped classification remains distinct from engine cadence, and monitor identity, VRR, RTSS/FES state, click-to-photon input latency, replacement semantics, and GPU headroom stay unavailable unless separately measured.

`Scripts/TestOpticalInputCorrelation.ps1` exercises schema-1 optical receipt correlation with exact process/QPC/trigger/marker/input/frame identity, exact validated-byte hashing, unavailable-field truthfulness, malformed/stale/duplicate/ambiguous/noncausal and PresentMon-surrogate rejection, collision refusal, and bounded missing-observation timeout cleanup. The receipt owns only its temporary publication file; the instrument and raw observation remain explicitly operator-owned. Run headed `Editor.exe --optical-input-correlation-smoke --optical-input-correlation-marker-output=<new ignored path>` launches on D3D12 and Windows Vulkan; each must atomically create the non-colliding engine marker artifact, emit matching armed/drawn identities, and close naturally after four frames. On 2026-07-19 the zero-warning MSVC Debug build, 78/78 tests, receipt matrix, D3D12 (`D3D12FlipSynchronized`) and Vulkan (`VulkanFifo`) RTX 3080 Ti smokes, code style, and diff checks passed under ignored `output/optical-acceptance-20260719-120249`. Both headed runs used the explicitly labelled synthetic trigger, so this is deterministic control-path evidence only, not proof of physical photons, monitor cadence, VRR-active, peripheral input, click-to-photon latency, or non-Windows support.

## Future AI And Automation Verification

The accepted [AI and automation contract](Architecture/AI_AUTOMATION_ARCHITECTURE.md) is planning, not runtime evidence. Phase 13 tests must carry stable workflow/run/action IDs through preview, permission/precondition checks, apply, validation, the declared commit/rollback/compensation/external-result outcome, undo where supported, and provenance publication. Focused coverage must prove:

- the deterministic non-AI workflow and AI-selected path use the same command semantics and produce the same user-visible result;
- unauthorized, stale, malformed, unknown-version, and unsupported actions are rejected before mutation, while destructive, external, and sensitive actions stop at the specified approval boundary until explicitly approved;
- injected transactional failures completely roll back staged project-local changes, while cancellation, retry, and idempotency produce the documented state without orphaned project-local mutations;
- compensatable actions expose and test their explicit compensating action, record compensation success/failure/pending state, and never label the original external effect as rolled back;
- irreversible actions require just-in-time approval immediately before execution and record successful, failed, or indeterminate external outcomes without claiming reversibility;
- receipts are versioned, complete, tied to editor history, and redact credentials, secrets, and private prompt data;
- validation exercises the edited project through its real public APIs and workflow; an agent report, plausible explanation, log marker, or mocked mutation is not acceptance evidence;
- headless and editor paths agree wherever both are claimed.

Model/provider evaluation is separate from deterministic workflow qualification. Use versioned scenarios and repeated trials to report model/version, plan and tool-selection accuracy, clarification behavior, unsafe-action rate, latency/cost, and final deterministic-tool outcomes. A model score cannot upgrade an unverified tool or workflow.

## Editor And Asset Workflows

Prefer the focused existing smoke that owns the behavior. Current headless flags include:

```text
--headless --smoke-test
--save-scene-smoke
--gltf-import-smoke
--material-asset-smoke
--scene-authoring-smoke
--frame-pacing-policy-smoke
```

`EngineTests` verifies Responsive default resolution, runtime override/inherit resolution, target validation, transactional rejection of an invalid Smooth Frametime override, and that user/game Smooth resolves only to `InterFrame` across runtime changes. The editor `--frame-pacing-policy-smoke` writes and reads a version-1 legacy manifest (defaulting to Responsive), rejects an invalid version-2 target without mutating the caller's manifest value, then saves/reopens an opt-in 144 FPS project policy and emits `FramePacingPolicySmokeV1 ... candidate=InterFrame, behavior=inter-frame ... result=pass`. This smoke verifies settings persistence and product-policy resolution. The explicit `--smooth-frametime-candidate` benchmark/smoke CLI owns named InterFrame and SubmissionGate control-point evidence; it does not make SubmissionGate a user-facing Smooth option.

The root [README.md](../README.md) documents current executable paths and examples. For UI changes without a focused smoke, launch the editor, exercise the interaction, and inspect a screenshot. Add focused automated coverage when the interaction can be made deterministic.

## Roadmap Checkbox Gate

Before changing `[ ]` to `[x]` in `PLAN.md`:

1. Confirm the exact wording is implemented, not merely scaffolded.
2. Confirm it is integrated into the real workflow named by the item.
3. Identify and run focused behavior verification.
4. Run code style and relevant regression tests.
5. Update current-state prose and platform/backend/device limitations.
6. Confirm phase exit criteria are independently met; do not infer phase completion from useful foundations.

If any step cannot be performed, keep the item unchecked and record the missing gate.

## Hosted CI

Use `.github/workflows/ci.yml` for platform evidence unavailable locally. For roadmap-continuation work, push the scoped commit, monitor every job to completion, inspect failing logs, and fix in-scope failures. Record the run URL when it is material evidence for an ADR, platform qualification, or checked roadmap claim.

CI job success proves only the workload and device named by that job. Preserve distinctions between build, headless workflow, presentation, representative scene rendering, capture validation, packaging, and production hardware qualification.

## RHI Completion Tokens And Recording Reuse

The render-graph completion prerequisite requires both deterministic contract coverage and headed backend evidence. Contract tests must verify invalid token shape rejection. Each headed D3D12 and Vulkan/NVRHI smoke must submit a real closed graphics list, query the returned token without waiting, reject an invalid, cross-device, and unissued/stale token, wait to final completion, and re-record/re-submit the same list only after retirement. Fast devices may legitimately report the first query as complete; tests must accept either `nonblocking-incomplete` or `nonblocking-complete`, but must never manufacture an incomplete state with CPU frame counters or destroy/recreate the context. The required `RHICompletionSmokeV1` marker reports token validation, first query state, wait completion, and retired reuse separately.

## Phase 3 Transient-Resource Capability Group

Deterministic tests prove `Phase3TransientResourcesV1` selects `PlacedAliasedTransient` only when both RHI lifecycle states are usable, and otherwise selects `NonAliasedGpuRetiredPool`. The allocation test proves compatible lifetime peak cost and that an incomplete exact earlier token blocks reuse even after a final token completes. Both headed smokes require `RenderGraphTransientAllocationSmokeV1` for the truthful current fallback, real transient resource allocation, 64-byte accounting, exact-token wait, and retired reuse. This does not prove native placement, alias barriers, hosted regression, or macOS qualification.

## RHI Resource Ownership Validation

Graph binding must call the backend-neutral `Device::OwnsResource` overload matching each supplied texture or buffer before command recording. Deterministic contract tests cover owned resources plus null, foreign-backend, and same-backend different-device rejection. Each headed D3D12 and Vulkan/NVRHI smoke creates real buffer and texture wrappers from its active device and requires `RHIResourceOwnershipSmokeV1` to report `owned=pass, null=rejected, result=pass`. The headed environments create one active device, so same-backend different-device rejection is contract/adapter-layer evidence rather than a second-real-device smoke claim.

## Vulkan Multi-Queue Admission

`EngineTests` covers deterministic Vulkan admission fallback, independent Compute/Copy resolution, ordinary same-family resource permission, and different-family policy. `TestVulkan.ps1` and `TestVulkan.sh` adapt to selected topology and require queue-local Copy-to-Graphics and Graphics-to-Compute dependency retirement plus paired buffer/texture ownership evidence: independent queues exercise the full lifecycle; Graphics fallback rejects with no pending publication. The local Windows RTX 3080 Ti selected Graphics family 0/index 0, Copy family 1/index 0, and Compute family 2/index 0; both dependency edges and ownership acquisitions use NVRHI GPU waits with no CPU wait.

## RHI Texture Queue Ownership

Texture queue ownership is whole-resource-only: a transfer requires an exact-device-owned live texture with a nonzero extent, one mip, one array layer, one sample, and usage compatible with both declared portable states. Deterministic `EngineTests` cover unsupported shape rejection, same-effective fallback rejection, exact release-token/dependency validation, publication timing, pending ordinary-use/destruction/state guards, exact completed-token recovery, and immutable compensation snapshots. `RHITextureOwnershipSmokeV1` is required by both Vulkan harnesses and `TestRender.ps1`. The local RTX 3080 Ti D3D12 and split-family Vulkan runs require an independent Graphics-to-Copy lifecycle with exact RGBA8 bytes, final owner/state, recovery, and retirement. Graphics fallback rejects with `transfer=rejected, pending=no`; the cross-queue RenderGraph executor now exercises the same ownership lifecycle for imported textures.
