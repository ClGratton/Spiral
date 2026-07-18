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
    [switch]$ExternalAttachment,
    [ValidateRange(10, 600)]
    [int]$ChildTimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "InvokeBoundedProcess.ps1")
$Executable = Join-Path $Root "bin\Debug-windows-x86_64\Editor\Editor.exe"
if (!(Test-Path $Executable)) { throw "Build Debug first: $Executable" }

function Write-AttachmentRelease([string]$Path, [string]$RunId, [int]$ProcessId) {
    [System.IO.File]::WriteAllText($Path, "schema=1`nrunId=$RunId`npid=$ProcessId`n", [System.Text.UTF8Encoding]::new($false))
}

function Get-ProcessTreeIds([int]$RootProcessId) {
    $Processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
    $ChildrenByParent = @{}
    foreach ($Candidate in $Processes) {
        $Parent = [int]$Candidate.ParentProcessId
        if (!$ChildrenByParent.ContainsKey($Parent)) { $ChildrenByParent[$Parent] = [System.Collections.Generic.List[int]]::new() }
        $ChildrenByParent[$Parent].Add([int]$Candidate.ProcessId)
    }
    $Seen = [System.Collections.Generic.HashSet[int]]::new()
    $Pending = [System.Collections.Generic.Queue[int]]::new()
    $Pending.Enqueue($RootProcessId)
    while ($Pending.Count -gt 0) {
        $Current = $Pending.Dequeue()
        if (!$Seen.Add($Current)) { continue }
        if ($ChildrenByParent.ContainsKey($Current)) {
            foreach ($Child in $ChildrenByParent[$Current]) { $Pending.Enqueue($Child) }
        }
    }
    return @($Seen)
}

function Capture-OwnedProcessTree([Diagnostics.Process]$Process, [System.Collections.Generic.HashSet[int]]$OwnedProcessIds) {
    foreach ($ProcessId in @(Get-ProcessTreeIds $Process.Id)) { [void]$OwnedProcessIds.Add([int]$ProcessId) }
}

function Stop-OwnedProcessTree([System.Collections.Generic.HashSet[int]]$OwnedProcessIds, [string]$Label) {
    $Running = @($OwnedProcessIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    foreach ($ProcessId in @($Running | Sort-Object -Descending)) { Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue }
    $Remaining = @($OwnedProcessIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    if ($Remaining.Count -ne 0) { throw "$Label process-tree cleanup failed: PIDs=$($Remaining -join ',')" }
}

function Invoke-AttachmentCase([ValidateSet("release", "mismatch", "timeout")][string]$Mode) {
    $Output = Join-Path $Root "output\frame-pacing-attachment\$Backend-$Mode"
    if (Test-Path $Output) { Remove-Item -LiteralPath $Output -Recurse -Force }
    New-Item -ItemType Directory -Path $Output | Out-Null
    $Ready = Join-Path $Output "readiness.json"; $Release = Join-Path $Output "release.txt"; $Artifact = Join-Path $Output "engine"
    $Stdout = Join-Path $Output "editor.stdout.log"; $Stderr = Join-Path $Output "editor.stderr.log"
    $Args = @("--frame-pacing-benchmark", "--frame-pacing-benchmark-responsive", "--smooth-frametime-target-fps=60",
        "--frame-pacing-benchmark-output=$Artifact", "--frame-pacing-benchmark-attachment-readiness=$Ready",
        "--frame-pacing-benchmark-attachment-release=$Release", "--frame-pacing-benchmark-attachment-timeout-ms=750")
    if ($Backend -eq "Vulkan") { $Args += "--renderer-vulkan" }
    $Process = Start-Process -FilePath $Executable -ArgumentList (($Args | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ') -PassThru -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr
    $OwnedProcessIds = [System.Collections.Generic.HashSet[int]]::new()
    Capture-OwnedProcessTree $Process $OwnedProcessIds
    try {
        $Deadline = [Diagnostics.Stopwatch]::StartNew()
        while (!(Test-Path $Ready) -and !$Process.HasExited -and $Deadline.Elapsed.TotalSeconds -lt 15) { Start-Sleep -Milliseconds 25; $Process.Refresh(); Capture-OwnedProcessTree $Process $OwnedProcessIds }
        if (!(Test-Path $Ready)) { throw "Attachment readiness was not published: $Backend $Mode" }
        $Readiness = Get-Content -Raw $Ready | ConvertFrom-Json
        $ExpectedPath = [IO.Path]::GetFullPath($Executable)
        if ($Readiness.schema -ne 1 -or [string]::IsNullOrWhiteSpace($Readiness.runId) -or $Readiness.processId -ne $Process.Id -or
            ![IO.Path]::GetFullPath($Readiness.executablePath).Equals($ExpectedPath, [StringComparison]::OrdinalIgnoreCase) -or
            $Readiness.qpcFrequency -le 0 -or $Readiness.qpcTick -le 0 -or $Readiness.benchmarkArtifactPath -ne [IO.Path]::GetFullPath($Artifact) -or
            $Readiness.condition.backend -ne $(if ($Backend -eq "Vulkan") { "NVRHI Vulkan" } else { "NVRHI D3D12" }) -or $Readiness.condition.targetFps -ne 60 -or
            $Readiness.condition.presentationMode -ne "unknown" -or $Readiness.condition.sync -ne "unknown" -or $Readiness.condition.vrr -ne "unknown" -or $Readiness.condition.tearing -ne "unknown") {
            throw "Attachment readiness identity/condition mismatch: $Backend $Mode"
        }
        if (Test-Path (Join-Path $Artifact "frame-pacing-benchmark.json")) { throw "Measurement artifact existed before supervisor release: $Backend $Mode" }
        if ($Mode -eq "release") { Write-AttachmentRelease $Release $Readiness.runId $Readiness.processId }
        elseif ($Mode -eq "mismatch") { Write-AttachmentRelease $Release "stale-$($Readiness.runId)" $Readiness.processId }
        $ChildDeadline = [Diagnostics.Stopwatch]::StartNew()
        while (!$Process.WaitForExit(100)) {
            Capture-OwnedProcessTree $Process $OwnedProcessIds
            if ($ChildDeadline.Elapsed.TotalSeconds -ge $ChildTimeoutSeconds) {
                Capture-OwnedProcessTree $Process $OwnedProcessIds
                throw "Attachment child timed out: $Backend $Mode"
            }
        }
        Capture-OwnedProcessTree $Process $OwnedProcessIds
        $Log = (Get-Content $Stdout -Raw) + "`n" + (Get-Content $Stderr -Raw)
        $SurvivingProcessIds = @($OwnedProcessIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
        if ($SurvivingProcessIds.Count -ne 0) { throw "Attachment child process tree survived reported exit: PIDs=$($SurvivingProcessIds -join ',')" }
        if ($Mode -eq "release") {
            if ($Log -notmatch "FramePacingAttachmentV1 state=released" -or $Log -notmatch "FramePacingBenchmarkV1 frames=512") { throw "Attachment release launch failed: $Backend" }
            $Json = Get-Content -Raw (Join-Path $Artifact "frame-pacing-benchmark.json") | ConvertFrom-Json
            if ($Json.schema -ne 6 -or $Json.condition.runId -ne $Readiness.runId -or $Json.condition.processId -ne $Process.Id -or $Json.condition.qpcFrequency -ne $Readiness.qpcFrequency -or $Json.frames.Count -ne 512 -or @($Json.frames | Where-Object { @($_.lifecycle | Where-Object { $_.qpc -le 0 }).Count -ne 0 }).Count -ne 0) { throw "Attachment release artifact did not retain QPC/run identity: $Backend" }
            Write-Host "Frame pacing attachment passed: $Backend release runId=$($Readiness.runId)"
        } else {
            $Expected = if ($Mode -eq "mismatch") { "state=rejected" } else { "state=timeout" }
            if ($Log -notmatch $Expected -or (Test-Path (Join-Path $Artifact "frame-pacing-benchmark.json"))) { throw "Attachment $Mode did not fail closed: $Backend" }
            Write-Host "Frame pacing attachment passed: $Backend $Mode cleanup=verified"
        }
    } finally {
        Capture-OwnedProcessTree $Process $OwnedProcessIds
        Stop-OwnedProcessTree $OwnedProcessIds "Attachment $Backend $Mode"
    }
}

if ($ExternalAttachment) {
    foreach ($Mode in @("release", "mismatch", "timeout")) { Invoke-AttachmentCase $Mode }
    exit 0
}
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
        if ($Json.schema -ne 6 -or $Json.condition.backend -ne $ExpectedBackend -or $Json.condition.targetFps -ne $Target -or $EffectiveTargetMismatch -or $Json.condition.warmupFrames -ne 30 -or $Json.condition.mode -ne $ExpectedMode -or $Json.condition.candidate -ne $ExpectedCandidate -or $Json.condition.presentationMode -ne $PresentationMode -or $Json.condition.sync -ne $SyncMode -or $Json.condition.vrr -ne $VrrMode -or $Json.condition.tearing -ne $TearingMode -or $Json.frames.Count -ne 512 -or $Csv.Count -ne 512 -or $null -eq $Json.summary.p50Ms -or $null -eq $Json.summary.p95Ms -or $null -eq $Json.summary.p99Ms -or $null -eq $Json.summary.cpuActiveP50Ms -or $null -eq $Json.summary.cpuActiveP95Ms -or $null -eq $Json.summary.cpuActiveP99Ms -or $null -eq $Json.summary.intentionalWaitP50Ms -or $null -eq $Json.summary.intentionalWaitP95Ms -or $null -eq $Json.summary.intentionalWaitP99Ms -or $null -eq $Json.summary.deadlineMisses -or $null -eq $Json.summary.deadlineOvershootP99Ms -or $null -eq $Json.summary.onePercentLowFps -or $null -eq $Json.summary.pointOnePercentLowFps) {
            throw "Benchmark condition manifest did not retain ${Candidate}: $Backend $Target"
        }
        $AppliedPacingWaits = @($Json.frames | ForEach-Object { @($_.waits) } | Where-Object { $_.kind -eq "IntentionalPacing" -and $_.applied })
        if ($Candidate -eq "responsive") {
            if ($AppliedPacingWaits.Count -ne 0) { throw "Responsive benchmark applied an intentional deadline wait: $Backend $Target" }
        } elseif ($AppliedPacingWaits.Count -eq 0) {
            throw "Smooth benchmark retained no applied deadline wait: $Backend $Target $Candidate"
        } else {
            foreach ($Wait in $AppliedPacingWaits) {
                $Telemetry = $Wait.deadlineWait
                if ($null -eq $Telemetry -or [string]::IsNullOrWhiteSpace([string]$Telemetry.primitive) -or
                    [double]$Telemetry.timerWaitMs -lt 0.0 -or [double]$Telemetry.portableWaitMs -lt 0.0 -or
                    [double]$Telemetry.activeTailBudgetMs -lt 0.0 -or [double]$Telemetry.activeTailBudgetMs -gt 0.5 -or
                    [double]$Telemetry.activeTailMs -lt 0.0 -or [double]$Telemetry.processCpuTimeMs -lt 0.0 -or
                    [double]$Telemetry.wallTimeMs -lt 0.0) {
                    throw "Smooth benchmark deadline-wait telemetry is invalid: $Backend $Target $Candidate frame-wait"
                }
            }
        }
        $ReadyGpuFrames = @($Json.frames | Where-Object { $_.gpuTimingStatus -eq "Ready" -and $_.gpuDurationMs -ne "unavailable" })
        if ($ReadyGpuFrames.Count -eq 0) { throw "Benchmark retained no ready exact-frame GPU duration: $Backend $Target $Candidate" }
        for ($FrameIndex = 0; $FrameIndex -lt $Json.frames.Count; ++$FrameIndex) {
            $Frame = $Json.frames[$FrameIndex]
            if ($FrameIndex -gt 0 -and [uint64]$Frame.frame -ne ([uint64]$Json.frames[$FrameIndex - 1].frame + 1)) {
                throw "Benchmark frame IDs are not continuous: $Backend $Target $Candidate"
            }
            $Phases = @($Frame.lifecycle | ForEach-Object { $_.phase })
            foreach ($RequiredPhase in @("FrameStart", "InputSample", "InputSimulation", "RenderSubmission", "PresentBegin", "PresentEnd")) {
                if ($RequiredPhase -notin $Phases) { throw "Benchmark lifecycle is incomplete: $Backend $Target $Candidate frame=$($Frame.frame) phase=$RequiredPhase" }
            }
            $RequiredLifecycle = @("FrameStart", "InputSample", "InputSimulation", "RenderSubmission", "PresentBegin", "PresentEnd")
            $RequiredLifecycleIndexes = @($RequiredLifecycle | ForEach-Object { [array]::IndexOf($Phases, $_) })
            $RequiredLifecycleOrder = $RequiredLifecycleIndexes -join ','
            $SortedLifecycleOrder = (@($RequiredLifecycleIndexes | Sort-Object)) -join ','
            if ($RequiredLifecycleIndexes -contains -1 -or $SortedLifecycleOrder -ne $RequiredLifecycleOrder) {
                throw "Benchmark lifecycle phases are out of order: $Backend $Target $Candidate frame=$($Frame.frame)"
            }
            if ($null -eq $Frame.waits -or $Frame.display -ne "unavailable" -or $Frame.replacementDrop -ne "unavailable" -or $Frame.inputLatency -ne "unavailable" -or $null -eq $Frame.gpuTimingStatus -or $null -eq $Frame.gpuDurationMs -or $null -eq $Frame.gpuHeadroom) {
                throw "Benchmark raw fields are incomplete: $Backend $Target $Candidate frame=$($Frame.frame)"
            }
            if ([uint64]$Frame.inputLatencySourceFrame -ne [uint64]$Frame.frame -or $null -eq $Frame.inputToSimulationMs -or $null -eq $Frame.inputToSubmitMs -or $null -eq $Frame.inputToPresentMs -or $Frame.inputToDisplay -ne "unavailable" -or $Frame.clickToPhoton -ne "unavailable" -or [double]$Frame.inputToSimulationMs -lt 0.0 -or [double]$Frame.inputToSubmitMs -lt [double]$Frame.inputToSimulationMs -or [double]$Frame.inputToPresentMs -lt [double]$Frame.inputToSubmitMs) {
                throw "Benchmark input-stage intervals are not exact-frame ordered evidence: $Backend $Target $Candidate frame=$($Frame.frame)"
            }
            if ($Frame.gpuTimingStatus -eq "Ready") {
                if ($Frame.gpuDurationMs -eq "unavailable") { throw "Ready GPU timing omitted its duration: $Backend $Target $Candidate frame=$($Frame.frame)" }
                if ($Candidate -eq "responsive") {
                    if ($Frame.gpuHeadroom -ne "unavailable") { throw "Responsive frame invented target-budget headroom: $Backend $Target frame=$($Frame.frame)" }
                } else {
                    $ExpectedHeadroom = 1000.0 / $Target - [double]$Frame.gpuDurationMs
                    if ($Frame.gpuHeadroom -eq "unavailable" -or [Math]::Abs([double]$Frame.gpuHeadroom - $ExpectedHeadroom) -gt 0.001) {
                        throw "GPU headroom did not use the matching-frame target budget and duration: $Backend $Target $Candidate frame=$($Frame.frame)"
                    }
                }
            } elseif ($Frame.gpuDurationMs -ne "unavailable" -or $Frame.gpuHeadroom -ne "unavailable") {
                throw "Non-ready GPU timing published duration/headroom: $Backend $Target $Candidate frame=$($Frame.frame)"
            }
        }
        $CsvLifecycle = @($Csv[0].lifecycleJson | ConvertFrom-Json)
        $CsvWaits = @($Csv[0].waitsJson | ConvertFrom-Json)
        $ReadyGpuCsv = @($Csv | Where-Object { $_.gpuTimingStatus -eq "Ready" -and $_.gpuDurationMs -ne "unavailable" })
        if ($CsvLifecycle.Count -eq 0 -or $null -eq $CsvWaits -or $Csv[0].display -ne "unavailable" -or $Csv[0].replacementDrop -ne "unavailable" -or $Csv[0].inputLatency -ne "unavailable" -or $ReadyGpuCsv.Count -eq 0) {
            throw "Benchmark CSV raw fields are incomplete: $Backend $Target $Candidate"
        }
        foreach ($Frame in $Csv) {
            if ([uint64]$Frame.inputLatencySourceFrame -ne [uint64]$Frame.frame -or [double]$Frame.inputToSimulationMs -lt 0.0 -or [double]$Frame.inputToSubmitMs -lt [double]$Frame.inputToSimulationMs -or [double]$Frame.inputToPresentMs -lt [double]$Frame.inputToSubmitMs) {
                throw "Benchmark CSV input-stage intervals are not exact-frame ordered evidence: $Backend $Target $Candidate frame=$($Frame.frame)"
            }
        }
        foreach ($Frame in $ReadyGpuCsv) {
            if ($Candidate -eq "responsive") {
                if ($Frame.gpuHeadroom -ne "unavailable") { throw "Responsive CSV invented target-budget headroom: $Backend $Target" }
            } else {
                $ExpectedHeadroom = 1000.0 / $Target - [double]$Frame.gpuDurationMs
                if ($Frame.gpuHeadroom -eq "unavailable" -or [Math]::Abs([double]$Frame.gpuHeadroom - $ExpectedHeadroom) -gt 0.001) {
                    throw "CSV GPU headroom did not match its frame duration: $Backend $Target $Candidate"
                }
            }
        }
    }
}
Write-Host "Frame pacing benchmark passed: $Backend"
