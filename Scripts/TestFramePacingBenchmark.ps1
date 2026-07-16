param(
    [ValidateSet("D3D12", "Vulkan")]
    [string]$Backend = "D3D12",
    [ValidateRange(30, 600)]
    [int[]]$TargetFramesPerSecond = @(60, 120),
    [ValidateNotNullOrEmpty()]
    [string]$PresentationMode = "unknown",
    [ValidateNotNullOrEmpty()]
    [string]$SyncMode = "unknown",
    [ValidateNotNullOrEmpty()]
    [string]$VrrMode = "unknown",
    [ValidateNotNullOrEmpty()]
    [string]$TearingMode = "unknown",
    [ValidateRange(10, 600)]
    [int]$ChildTimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "InvokeBoundedProcess.ps1")
$Executable = Join-Path $Root "bin\Debug-windows-x86_64\Editor\Editor.exe"
if (!(Test-Path $Executable)) { throw "Build Debug first: $Executable" }
foreach ($Target in $TargetFramesPerSecond) {
    foreach ($Candidate in @("responsive", "inter-frame", "submission-gate")) {
        $Output = Join-Path $Root "output\frame-pacing-benchmark\$Backend-$Target-$Candidate"
        if (Test-Path $Output) { Remove-Item -LiteralPath $Output -Recurse -Force }
        $Arguments = @("--frame-pacing-benchmark", "--smooth-frametime-target-fps=$Target", "--frame-pacing-benchmark-output=$Output",
            "--frame-pacing-benchmark-presentation=$PresentationMode", "--frame-pacing-benchmark-sync=$SyncMode",
            "--frame-pacing-benchmark-vrr=$VrrMode", "--frame-pacing-benchmark-tearing=$TearingMode")
        if ($Candidate -eq "responsive") { $Arguments += "--frame-pacing-benchmark-responsive" } else { $Arguments += "--smooth-frametime-candidate=$Candidate" }
        if ($Backend -eq "Vulkan") { $Arguments += "--renderer-vulkan" }
        $Result = Invoke-BoundedProcess -FilePath $Executable -Arguments $Arguments -Label "Frame pacing benchmark $Backend $Target $Candidate" -TimeoutSeconds $ChildTimeoutSeconds
        if ($Result.TimedOut -or $Result.ExitCode -ne 0) { throw "Frame pacing benchmark failed: $Backend $Target $Candidate" }
        if (Get-Process -Id $Result.ProcessId -ErrorAction SilentlyContinue) { throw "Frame pacing benchmark child survived its reported exit: PID=$($Result.ProcessId)" }
        $Log = $Result.Output -join "`n"
        if ($Log -notmatch "FramePacingBenchmarkV1 frames=[1-9][0-9]*") { throw "Benchmark marker missing: $Backend $Target $Candidate" }
        foreach ($Artifact in @("frame-pacing-benchmark.csv", "frame-pacing-benchmark.json")) {
            if (!(Test-Path (Join-Path $Output $Artifact))) { throw "Benchmark artifact missing: $Artifact" }
        }
        $Json = Get-Content -Raw (Join-Path $Output "frame-pacing-benchmark.json") | ConvertFrom-Json
        $Csv = @(Import-Csv (Join-Path $Output "frame-pacing-benchmark.csv"))
        $ExpectedMode = if ($Candidate -eq "responsive") { "Responsive" } else { "Smooth Frametime" }
        $ExpectedCandidate = if ($Candidate -eq "responsive") { "InterFrame" } elseif ($Candidate -eq "inter-frame") { "InterFrame" } else { "SubmissionGate" }
        $ExpectedBackend = if ($Backend -eq "Vulkan") { "NVRHI Vulkan" } else { "NVRHI D3D12" }
        $EffectiveTargetMismatch = if ($Candidate -eq "responsive") { $null -ne $Json.condition.effectiveTargetFps } else { $Json.condition.effectiveTargetFps -ne $Target }
        if ($Json.schema -ne 1 -or $Json.condition.backend -ne $ExpectedBackend -or $Json.condition.targetFps -ne $Target -or $EffectiveTargetMismatch -or $Json.condition.warmupFrames -ne 30 -or $Json.condition.mode -ne $ExpectedMode -or $Json.condition.candidate -ne $ExpectedCandidate -or $Json.condition.presentationMode -ne $PresentationMode -or $Json.condition.sync -ne $SyncMode -or $Json.condition.vrr -ne $VrrMode -or $Json.condition.tearing -ne $TearingMode -or $Json.frames.Count -ne 512 -or $Csv.Count -ne 512 -or $null -eq $Json.summary.p50Ms -or $null -eq $Json.summary.p95Ms -or $null -eq $Json.summary.p99Ms -or $null -eq $Json.summary.cpuActiveP50Ms -or $null -eq $Json.summary.cpuActiveP95Ms -or $null -eq $Json.summary.cpuActiveP99Ms -or $null -eq $Json.summary.intentionalWaitP50Ms -or $null -eq $Json.summary.intentionalWaitP95Ms -or $null -eq $Json.summary.intentionalWaitP99Ms -or $null -eq $Json.summary.deadlineMisses -or $null -eq $Json.summary.deadlineOvershootP99Ms -or $null -eq $Json.summary.onePercentLowFps -or $null -eq $Json.summary.pointOnePercentLowFps) {
            throw "Benchmark condition manifest did not retain ${Candidate}: $Backend $Target"
        }
        for ($FrameIndex = 0; $FrameIndex -lt $Json.frames.Count; ++$FrameIndex) {
            $Frame = $Json.frames[$FrameIndex]
            if ($FrameIndex -gt 0 -and [uint64]$Frame.frame -ne ([uint64]$Json.frames[$FrameIndex - 1].frame + 1)) {
                throw "Benchmark frame IDs are not continuous: $Backend $Target $Candidate"
            }
            $Phases = @($Frame.lifecycle | ForEach-Object { $_.phase })
            foreach ($RequiredPhase in @("FrameStart", "InputSimulation", "RenderSubmission", "PresentBegin", "PresentEnd")) {
                if ($RequiredPhase -notin $Phases) { throw "Benchmark lifecycle is incomplete: $Backend $Target $Candidate frame=$($Frame.frame) phase=$RequiredPhase" }
            }
            if ($null -eq $Frame.waits -or $Frame.display -ne "unavailable" -or $Frame.replacementDrop -ne "unavailable" -or $Frame.inputLatency -ne "unavailable" -or $Frame.gpuHeadroom -ne "unavailable") {
                throw "Benchmark raw fields are incomplete: $Backend $Target $Candidate frame=$($Frame.frame)"
            }
        }
        $CsvLifecycle = @($Csv[0].lifecycleJson | ConvertFrom-Json)
        $CsvWaits = @($Csv[0].waitsJson | ConvertFrom-Json)
        if ($CsvLifecycle.Count -eq 0 -or $null -eq $CsvWaits -or $Csv[0].display -ne "unavailable" -or $Csv[0].replacementDrop -ne "unavailable" -or $Csv[0].inputLatency -ne "unavailable" -or $Csv[0].gpuHeadroom -ne "unavailable") {
            throw "Benchmark CSV raw fields are incomplete: $Backend $Target $Candidate"
        }
    }
}
Write-Host "Frame pacing benchmark passed: $Backend"
