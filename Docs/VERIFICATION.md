# Verification Guide

Status: Living contract
Date: 2026-07-12

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

## Renderer Verification

Windows D3D12 viewport behavior and non-blank capture:

```powershell
.\Scripts\TestRender.ps1 -Configuration Debug -Action vs2022
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

A backend claim requires that backend's smoke or a stronger representative scene/capture test. A presentation smoke does not qualify scene-resource rendering. A WARP/lavapipe/llvmpipe/Apple-Paravirtual result must be labeled as that device class and must not be generalized to physical production hardware.

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
