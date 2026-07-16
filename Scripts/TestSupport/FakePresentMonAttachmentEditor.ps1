param(
    [Parameter(Mandatory = $true)][string]$ReadinessPath,
    [Parameter(Mandatory = $true)][string]$ReleasePath,
    [Parameter(Mandatory = $true)][string]$EngineDirectory,
    [ValidateSet("success", "stale-readiness", "failure", "timeout", "linger-after-release")][string]$Behavior,
    [ValidateSet("D3D12", "Vulkan")][string]$Backend,
    [ValidateSet("responsive", "inter-frame", "submission-gate")][string]$Candidate,
    [int]$TargetFramesPerSecond
)

$ErrorActionPreference = "Stop"
$frequency = [Diagnostics.Stopwatch]::Frequency
$tick = [Diagnostics.Stopwatch]::GetTimestamp()
$processPath = (Get-Process -Id $PID).Path
$runId = "fake-run-$PID-$tick"
$publishedPid = if ($Behavior -eq "stale-readiness") { $PID + 1 } else { $PID }
$candidateName = if ($Candidate -eq "submission-gate") { "SubmissionGate" } else { "InterFrame" }
$readiness = [ordered]@{
    schema = 1; runId = $runId; processId = $publishedPid; executablePath = [IO.Path]::GetFullPath($processPath)
    qpcFrequency = $frequency; qpcTick = $tick; benchmarkArtifactPath = [IO.Path]::GetFullPath($EngineDirectory)
    condition = [ordered]@{
        backend = if ($Backend -eq "Vulkan") { "NVRHI Vulkan" } else { "NVRHI D3D12" }
        targetFps = $TargetFramesPerSecond; candidate = $candidateName
        presentationMode = "unknown"; sync = "unknown"; vrr = "unknown"; tearing = "unknown"
    }
}
[IO.Directory]::CreateDirectory((Split-Path -Parent $ReadinessPath)) | Out-Null
[IO.File]::WriteAllText($ReadinessPath, (($readiness | ConvertTo-Json -Depth 8) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
Write-Output "FakeAttachment state=ready pid=$publishedPid"

if ($Behavior -eq "stale-readiness") { while ($true) { Start-Sleep -Seconds 1 } }
$deadline = [Diagnostics.Stopwatch]::StartNew()
while (!(Test-Path -LiteralPath $ReleasePath) -and $deadline.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 10 }
if (!(Test-Path -LiteralPath $ReleasePath)) { throw "Fake attachment release timed out" }
$expectedRelease = "schema=1`nrunId=$runId`npid=$PID`n"
if ((Get-Content -LiteralPath $ReleasePath -Raw) -cne $expectedRelease) { throw "Fake attachment release identity mismatch" }
Write-Output "FakeAttachment state=released pid=$PID"

if ($Behavior -eq "failure") { exit 7 }
if ($Behavior -eq "timeout") {
    $child = Start-Process -FilePath $processPath -ArgumentList @("-NoProfile", "-Command", "Start-Sleep -Seconds 60") -PassThru
    [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $ReadinessPath) "editor-child.pid"), [string]$child.Id)
    while ($true) { Start-Sleep -Seconds 1 }
}
if ($Behavior -eq "linger-after-release") { while ($true) { Start-Sleep -Seconds 1 } }

[IO.Directory]::CreateDirectory($EngineDirectory) | Out-Null
$frames = @()
foreach ($offset in @(10000, 20000, 30000)) {
    $frames += [ordered]@{
        frame = $frames.Count + 30
        lifecycle = @([ordered]@{ phase = "PresentBegin"; qpc = $tick + $offset })
        display = "unavailable"; replacementDrop = "unavailable"; inputLatency = "unavailable"; gpuHeadroom = "unavailable"
    }
}
$engine = [ordered]@{
    schema = 2
    condition = [ordered]@{
        runId = $runId; processId = $PID; executablePath = [IO.Path]::GetFullPath($processPath); qpcFrequency = $frequency
        backend = if ($Backend -eq "Vulkan") { "NVRHI Vulkan" } else { "NVRHI D3D12" }
        targetFps = $TargetFramesPerSecond; warmupFrames = 30; mode = "Smooth Frametime"; candidate = $candidateName
        presentationMode = "unknown"; sync = "unknown"; vrr = "unknown"; tearing = "unknown"
    }
    frames = $frames
}
[IO.File]::WriteAllText((Join-Path $EngineDirectory "frame-pacing-benchmark.json"), (($engine | ConvertTo-Json -Depth 10) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $EngineDirectory "frame-pacing-benchmark.csv"), "fake-engine-raw`n", [Text.UTF8Encoding]::new($false))
Write-Output "FramePacingBenchmarkV1 frames=3"
