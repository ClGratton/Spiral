# Verification Guide

Status: Living contract
Date: 2026-07-14

Verification must exercise the behavior claimed by the change. Compilation proves build compatibility; it does not prove a runtime or editor workflow.

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

Exercise real Editor-to-Renderer scene snapshot publication and retained epoch lifetime in both frame-task modes:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke --scene-render-snapshot-smoke
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke --frame-task-single-thread --scene-render-snapshot-smoke
```

Both commands must publish the stable-ID editor viewport view after mutable layer update, validate counts and authoritative main-camera identity against the active Scene, retain the previous immutable epoch while publishing the next, consume the initial one-shot discontinuity only on a valid view epoch, and print both `CPU frame task graph smoke passed` and `Scene render snapshot smoke passed`. This is portable extraction/publication evidence; the D3D12-specific raster behavior is exercised separately below.

## Renderer Verification

`EngineTests` includes GPU-independent renderer-capability policy coverage. It proves lifecycle invariants, deterministic candidate ranking, retained rejection reasons, required format-usage validation, compatible queue fallbacks, strict selection by stable adapter ID, and `Phase3FrameTimingV1` selection of usable GPU timestamps versus the CPU steady-clock fallback. These tests do not prove a physical adapter or backend runtime path.

Windows D3D12 viewport behavior and non-blank capture:

```powershell
.\Scripts\TestRender.ps1 -Configuration Debug -Action vs2022
```

This smoke requires the D3D12 versioned-profile selection marker before accepting the capture. It also requires exactly one successful `PortableShaderTerminalV1` marker for each vertex/pixel stage and one complete `D3D12PortablePipelineV1 status=active` marker. Those structured markers must agree on valid 64-hex package keys, terminal success/cache-hit state, cache mode/source, nonzero reflected bindings, vertex-input counts, compiler `Slang-2026.13.1`, backend identity, exact targets `DXIL+SPIR-V`, convention schema 1 (row-major, zero-to-one D3D clip depth, inverted SPIR-V Y, clockwise front face, D3D register-space bindings), and `legacySourceCompile=false`. Pending-only, failed, cancelled, unknown, duplicate-stage, key/status/cache-mismatched, or legacy compile evidence fails the smoke.

The same run launches `--scene-origin-raster-smoke`, waits for a stable editor-viewport projection, and captures cases A/B/C at trillion-unit coordinates around a real 4096-unit sector boundary. A and C place camera/origin and mesh together on opposite sides of that boundary and must be byte-identical; B moves only the mesh across the boundary and must differ, retain comparable foreground area, and move the foreground centroid right. Renderer diagnostics identify the current snapshot frame/entity/canonical sector-local position/origin/relative position and exactly one issued draw in each case. The measured MSVC D3D12 run produced byte-identical A/C images, a 196.24-pixel rightward B centroid shift, and a 13.20% non-background ratio. It proves the selected Windows x86_64/MSVC D3D12 viewport consumed Slang-generated DXIL and issued the real prototype draw. The paired SPIR-V output is compiled, reflected, convention-validated package evidence, not Vulkan scene execution. Normal editor startup compiles through job-system fire-and-poll; the smoke intentionally selects deterministic-inline execution. Slang v2026.13.1 and Windows x86_64 DXC v1.9.2602 are exact admitted pins; other Slang host archives are acquisition targets only, and redistribution remains blocked by the binary-component/notice audit in [DEPENDENCIES.md](DEPENDENCIES.md). This does not qualify Vulkan scene raster, Linux, macOS, MinGW, Windows ARM64, real mesh/material resources, culling, coordinate debug views, physics, or ray/TLAS/query consumers.

To exercise the strict runtime failure path, launch a missing adapter and require a nonzero exit plus per-candidate strict-preference rejection diagnostics:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --smoke-test "--renderer-adapter=Definitely Missing Spiral Adapter" --renderer-adapter-strict
```

Vulkan device, NVRHI wrapping, native ImGui presentation, resize, and successful post-resize present:

```powershell
.\Scripts\TestVulkan.ps1 -Configuration Debug -Action vs2022
.\Scripts\TestVulkan.ps1 -Configuration Debug -Action gmake
```

Linux/macOS use:

```bash
bash Scripts/TestVulkan.sh Debug gmake
```

The Vulkan smokes require the versioned-profile selection marker in addition to device/NVRHI creation and presentation evidence.

Native Apple Silicon project-generation, build, or MoltenVK presentation claims require a completed run on a native arm64 macOS environment. Project generation on another platform, source inspection, x86_64 macOS results, cross-compiled artifacts, and hosted CI jobs that execute zero steps are not native Apple Silicon evidence.

Repeat the same full launch contract when investigating presentation reliability:

```bash
VULKAN_SMOKE_ITERATIONS=3 bash Scripts/TestVulkan.sh Debug gmake --skip-build
```

The Vulkan smoke requests a resize once, then exits successfully only when the currently tracked recreated swapchain generation has completed an actual present. Its frame limit is a bounded failure deadline, not the success condition. Hosted macOS CI runs three complete launches and uploads every attempt log.

The D3D12 render smoke also requires the selected Bootstrap capability identity/state markers before accepting its viewport capture. Vulkan smokes require the Bootstrap profile, timeline lifecycle, and buffer-device-address lifecycle markers in addition to NVRHI device creation and presentation. These markers verify that the real startup path published the report; they do not by themselves qualify formats, future optional features, or production hardware.

The D3D12 and Vulkan scripts also launch with `--renderer-capability-smoke`. The editor validates the renderer-owned capability snapshot, executes the Profiler diagnostics drawing path, and emits required markers containing the exact Bootstrap profile, adapter, device qualification, format/feature/group/candidate counts, and `Phase3FrameTimingV1` path/lifecycle. The group marker is emitted only after a real frame uses the selected timing path and a native present succeeds. Current D3D12 and Vulkan smokes must report `GpuTimestamps` as preferred, `CpuSteadyClock` as selected, `exercised=yes`, group qualification `Presentation`, and device qualification `Bootstrap`. This exercises the current portable timing fallback on the named backend/device; it does not implement GPU timestamp recording/resolve, prove a future group's fallback, or qualify Scene/Production rendering. Inspect the full editor panel visually when changing its layout or interaction rather than treating the viewport-only BMP as a panel screenshot.

A backend claim requires that backend's smoke or a stronger representative scene/capture test. A presentation smoke does not qualify scene-resource rendering. A WARP/lavapipe/llvmpipe/Apple-Paravirtual result must be labeled as that device class and must not be generalized to physical production hardware.

Future frame-pacing/profiler completion requires a deterministic marker trace that carries one frame ID through engine `FrameStart`, input/simulation, render submission, `Present` begin/end, GPU completion, and display feedback where available. Tests must prove that the primary in-game frametime series is consecutive engine-start cadence, that intentional pacing wait and CPU active work are separate, and that injected inter-frame and pre-`Present` delays appear at their actual control points. A flat limiter/Afterburner/RTSS hook-local graph, average FPS, or present cadence alone is not completion evidence. When display feedback is unavailable, diagnostics must report it unavailable rather than substituting `Present` cadence.

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
```

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
