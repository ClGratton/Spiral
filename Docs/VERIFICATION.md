# Verification Guide

Status: Living contract
Date: 2026-07-13

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

`EngineTests` verifies the large-world coordinate foundation by subtracting an exact double-precision per-view origin before float conversion at trillion-unit coordinates, ensuring the float view matrix contains no absolute-world translation, and round-tripping high-magnitude double positions through the versioned scene format. It also verifies deterministic backend-neutral render extraction, main-camera authority, stable source/entity and asset handles, hidden-mesh omission, copied light/camera/transform values, and retained older immutable epochs after Scene mutation. The Windows D3D12 render smoke verifies only that camera-relative transform remains integrated into the current raster prototype. These checks do not qualify translated-origin snapshot propagation, actual scene-raster consumption, culling, debug, sector/origin transitions, physics, or ray-tracing consumers.

`EngineTests` also verifies worker-local nested-job stealing, submitted/completed/stolen scheduler statistics, stable deterministic dependency order, typed immutable publication, retained task failures, dependent skipping, independent-branch progress, cycle rejection, and frame/task/thread/worker profiler identities. Exercise the real Application frame graph in both execution modes:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --frame-task-graph-smoke --frame-task-single-thread
```

Both commands must publish the frame input, complete every caller-affine layer task, emit one terminal profile event per task, and print the matching `CPU frame task graph smoke passed` marker. These tests do not prove future workerized simulation, visibility/render preparation, translated-origin/raster snapshot consumption, command recording, priorities/cancellation, or Profiler-panel lane visualization.

Exercise real Editor-to-Renderer scene snapshot publication and retained epoch lifetime in both frame-task modes:

```powershell
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --scene-render-snapshot-smoke
.\bin\Debug-windows-x86_64\Editor\Editor.exe --headless --scene-render-snapshot-smoke --frame-task-single-thread
```

Both commands must publish after mutable layer update, validate counts and authoritative main-camera identity against the active Scene, retain the previous immutable epoch while publishing the next, and print `Scene render snapshot smoke passed`. This is extraction/publication evidence only: the current D3D12 viewport still renders its built-in prototype rather than snapshot mesh instances.

## Renderer Verification

`EngineTests` includes GPU-independent renderer-capability policy coverage. It proves lifecycle invariants, deterministic candidate ranking, retained rejection reasons, required format-usage validation, compatible queue fallbacks, strict selection by stable adapter ID, and `Phase3FrameTimingV1` selection of usable GPU timestamps versus the CPU steady-clock fallback. These tests do not prove a physical adapter or backend runtime path.

Windows D3D12 viewport behavior and non-blank capture:

```powershell
.\Scripts\TestRender.ps1 -Configuration Debug -Action vs2022
```

This smoke requires the D3D12 versioned-profile selection marker before accepting the capture. To exercise the strict runtime failure path, launch a missing adapter and require a nonzero exit plus per-candidate strict-preference rejection diagnostics:

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

Repeat the same full launch contract when investigating presentation reliability:

```bash
VULKAN_SMOKE_ITERATIONS=3 bash Scripts/TestVulkan.sh Debug gmake --skip-build
```

The Vulkan smoke requests a resize once, then exits successfully only when the currently tracked recreated swapchain generation has completed an actual present. Its frame limit is a bounded failure deadline, not the success condition. Hosted macOS CI runs three complete launches and uploads every attempt log.

The D3D12 render smoke also requires the selected Bootstrap capability identity/state markers before accepting its viewport capture. Vulkan smokes require the Bootstrap profile, timeline lifecycle, and buffer-device-address lifecycle markers in addition to NVRHI device creation and presentation. These markers verify that the real startup path published the report; they do not by themselves qualify formats, future optional features, or production hardware.

The D3D12 and Vulkan scripts also launch with `--renderer-capability-smoke`. The editor validates the renderer-owned capability snapshot, executes the Profiler diagnostics drawing path, and emits required markers containing the exact Bootstrap profile, adapter, device qualification, format/feature/group/candidate counts, and `Phase3FrameTimingV1` path/lifecycle. The group marker is emitted only after a real frame uses the selected timing path and a native present succeeds. Current D3D12 and Vulkan smokes must report `GpuTimestamps` as preferred, `CpuSteadyClock` as selected, `exercised=yes`, group qualification `Presentation`, and device qualification `Bootstrap`. This exercises the current portable timing fallback on the named backend/device; it does not implement GPU timestamp recording/resolve, prove a future group's fallback, or qualify Scene/Production rendering. Inspect the full editor panel visually when changing its layout or interaction rather than treating the viewport-only BMP as a panel screenshot.

A backend claim requires that backend's smoke or a stronger representative scene/capture test. A presentation smoke does not qualify scene-resource rendering. A WARP/lavapipe/llvmpipe/Apple-Paravirtual result must be labeled as that device class and must not be generalized to physical production hardware.

Future frame-pacing/profiler completion requires a deterministic marker trace that carries one frame ID through engine `FrameStart`, input/simulation, render submission, `Present` begin/end, GPU completion, and display feedback where available. Tests must prove that the primary in-game frametime series is consecutive engine-start cadence, that intentional pacing wait and CPU active work are separate, and that injected inter-frame and pre-`Present` delays appear at their actual control points. A flat limiter/Afterburner/RTSS hook-local graph, average FPS, or present cadence alone is not completion evidence. When display feedback is unavailable, diagnostics must report it unavailable rather than substituting `Present` cadence.

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
