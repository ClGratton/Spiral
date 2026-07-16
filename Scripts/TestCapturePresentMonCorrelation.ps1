$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Supervisor = Join-Path $PSScriptRoot "CapturePresentMonCorrelation.ps1"
$FakeEditor = Join-Path $PSScriptRoot "TestSupport\FakePresentMonAttachmentEditor.ps1"
$FakeCollector = Join-Path $PSScriptRoot "TestSupport\FakePresentMonCollector.ps1"
$PowerShell = (Get-Process -Id $PID).Path
$PowerShellHash = (Get-FileHash -LiteralPath $PowerShell -Algorithm SHA256).Hash
$Temporary = Join-Path ([IO.Path]::GetTempPath()) "spiral-presentmon-supervisor-$([guid]::NewGuid().ToString('N'))"

function Invoke-Supervisor([string]$Name, [hashtable]$Overrides = @{}) {
    $output = Join-Path $Temporary $Name
    $parameters = @{
        PresentMonPath = $PowerShell; ExpectedPresentMonSha256 = $PowerShellHash; EditorPath = $PowerShell; OutputDirectory = $output
        TestMode = $true; TestEditorScriptPath = $FakeEditor; TestCollectorScriptPath = $FakeCollector
        ReadinessTimeoutSeconds = 3; CollectorReadyTimeoutSeconds = 3; EditorTimeoutSeconds = 5; PresentMonTimeoutSeconds = 5
        TestCollectorSettleMilliseconds = 50
        FinalQpcTolerance = 2000
    }
    foreach ($key in $Overrides.Keys) { $parameters[$key] = $Overrides[$key] }
    & $Supervisor @parameters
    return $output
}

function Assert-Fails([scriptblock]$Action, [string]$Needle) {
    try { & $Action } catch { if ($_.Exception.Message -like "*$Needle*") { return }; throw "Expected '$Needle', got '$($_.Exception.Message)'" }
    throw "Expected failure containing '$Needle'"
}

function Assert-FailureReceipt([string]$Output, [string]$ErrorNeedle) {
    $receiptPath = Join-Path $Output "capture-receipt.json"
    if (!(Test-Path -LiteralPath $receiptPath)) { throw "Failure receipt was not preserved: $Output" }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ($receipt.success -or $receipt.error -notlike "*$ErrorNeedle*" -or $receipt.process.joined -or
        !$receipt.process.cleanup.verified -or (Test-Path -LiteralPath $receipt.paths.report)) {
        throw "Failure receipt did not retain fail-closed join/cleanup evidence: $Output"
    }
    return $receipt
}

try {
    [IO.Directory]::CreateDirectory($Temporary) | Out-Null

    $successOutput = Invoke-Supervisor "success"
    $success = Get-Content -LiteralPath (Join-Path $successOutput "capture-receipt.json") -Raw | ConvertFrom-Json
    $report = Get-Content -LiteralPath $success.paths.report -Raw | ConvertFrom-Json
    if (!$success.success -or !$success.process.joined -or !$success.process.cleanup.verified -or $report.counts.pairedRows -ne 3 -or
        $success.identity.editorPid -ne $success.identity.readinessPid -or $success.identity.qpcFrequency -le 0 -or
        [DateTimeOffset]$success.timing.collectorReadyAt -gt [DateTimeOffset]$success.timing.releasedAt -or
        $success.timing.collectorSettleMilliseconds -ne 50 -or
        [DateTimeOffset]$success.timing.collectorSettledAt -lt [DateTimeOffset]$success.timing.collectorReadyAt -or
        [DateTimeOffset]$success.timing.releasedAt -lt [DateTimeOffset]$success.timing.collectorSettledAt -or
        $success.process.editorExitCode -ne 0 -or $success.process.presentMonExitCode -ne 0 -or
        $success.sessionName -notmatch '^GameEngine-PM-[0-9a-f]{32}$' -or $success.tool.arguments -notcontains "-SessionName" -or
        [string]::IsNullOrWhiteSpace($success.hashes.engineJsonSha256) -or [string]::IsNullOrWhiteSpace($success.hashes.presentMonCsvSha256) -or
        $success.hashes.engineJsonSha256 -eq $success.hashes.presentMonCsvSha256 -or
        $report.rawInputs.engineJsonSha256 -ne $success.hashes.engineJsonSha256 -or $report.rawInputs.presentMonCsvSha256 -ne $success.hashes.presentMonCsvSha256 -or
        ($report.presentMonHeaders -join ",") -cne "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,QPCTime" -or
        [IO.Path]::GetFullPath($success.paths.engineJson).Equals([IO.Path]::GetFullPath($success.paths.presentMonCsv), [StringComparison]::OrdinalIgnoreCase) -or
        $success.condition.monitor -ne "unknown" -or $success.condition.vrr -ne "unknown" -or $success.condition.rtss -ne "unavailable" -or
        $success.condition.fes -ne "unavailable" -or $success.condition.inputLatency -ne "unavailable" -or $success.condition.gpuHeadroom -ne "unavailable") {
        throw "Successful fake supervision violated ordering, identity, condition, join, or cleanup invariants"
    }

    $supervisorSource = Get-Content -LiteralPath $Supervisor -Raw
    if ($supervisorSource -notmatch '"-output_file", \$presentMonCsv, "-qpc_time"' -or
        $supervisorSource -notmatch '"-terminate_on_proc_exit", "-no_top"' -or
        $supervisorSource -match '"-output_stdout"|"-terminate_after_timed"|"-timed"') {
        throw "Production PresentMon argument contract regressed"
    }

    $teardownOutput = Invoke-Supervisor "owned-session-teardown" @{ TestExactSessionTeardown = "success" }
    $teardown = Get-Content -LiteralPath (Join-Path $teardownOutput "capture-receipt.json") -Raw | ConvertFrom-Json
    if (!$teardown.success -or !$teardown.process.cleanup.verified -or $teardown.process.cleanup.exactSessionCleanup -ne "terminated" -or
        ($teardown.process.cleanup.exactSessionHelperArguments -join " ") -cne "-session_name $($teardown.sessionName) -terminate_existing" -or
        $teardown.sessionName -notmatch '^GameEngine-PM-[0-9a-f]{32}$') { throw "Exact owned-session teardown did not retain the sole generated target and success state" }
    foreach ($behavior in @("failure", "survives", "timeout")) {
        $output = Join-Path $Temporary "owned-session-$behavior"
        Assert-Fails { Invoke-Supervisor "owned-session-$behavior" @{ TestExactSessionTeardown = $behavior } } "exact-session cleanup failed"
        $receipt = Get-Content -LiteralPath (Join-Path $output "capture-receipt.json") -Raw | ConvertFrom-Json
        if ($receipt.process.cleanup.verified -or $receipt.process.cleanup.exactSessionCleanup -notlike "$(if ($behavior -eq 'survives') { 'survived' } else { 'failed:*' })") { throw "Exact session teardown $behavior did not fail closed" }
    }

    $lingerOutput = Invoke-Supervisor "linger-after-capture" @{ TestCollectorBehavior = "linger-after-capture" }
    $linger = Get-Content -LiteralPath (Join-Path $lingerOutput "capture-receipt.json") -Raw | ConvertFrom-Json
    if (!$linger.success -or !$linger.process.joined -or !$linger.process.cleanup.verified -or
        $linger.process.presentMonTerminationMode -ne "owned-job-after-complete-capture" -or $null -ne $linger.process.presentMonExitCode -or
        $linger.hashes.presentMonCsvSha256 -ne $linger.hashes.presentMonCsvSha256AfterCleanup) {
        throw "Lingering collector was not joined, owned-job terminated, and rehashed after complete capture"
    }

    Assert-Fails {
        & $Supervisor -PresentMonPath $PowerShell -ExpectedPresentMonSha256 $PowerShellHash -EditorPath $PowerShell `
            -OutputDirectory (Join-Path $Temporary "forbidden-test-hooks") -TestEditorScriptPath $FakeEditor -TestCollectorScriptPath $FakeCollector
    } "Fake process hooks require explicit TestMode"
    Assert-Fails {
        & $Supervisor -PresentMonPath $PowerShell -ExpectedPresentMonSha256 $PowerShellHash -EditorPath $PowerShell `
            -OutputDirectory (Join-Path $Temporary "forbidden-session-hook") -TestExactSessionTeardown success
    } "Fake process hooks require explicit TestMode"

    $collisionOutput = Join-Path $Temporary "collision"
    [IO.Directory]::CreateDirectory($collisionOutput) | Out-Null
    [IO.File]::WriteAllText((Join-Path $collisionOutput "owner-marker.txt"), "preserve")
    Assert-Fails { Invoke-Supervisor "collision" } "must not already exist"
    if ((Get-Content -LiteralPath (Join-Path $collisionOutput "owner-marker.txt") -Raw) -cne "preserve") { throw "Collision handling changed existing output" }

    $staleOutput = Join-Path $Temporary "stale-readiness"
    Assert-Fails { Invoke-Supervisor "stale-readiness" @{ TestEditorBehavior = "stale-readiness" } } "identity or condition"
    [void](Assert-FailureReceipt $staleOutput "identity or condition")
    if (Test-Path -LiteralPath (Join-Path $staleOutput "release.txt")) { throw "Stale readiness was released" }

    $versionOutput = Join-Path $Temporary "bad-version"
    Assert-Fails { Invoke-Supervisor "bad-version" @{ TestPresentMonVersion = "1.9.0" } } "exactly 1.10.0"
    [void](Assert-FailureReceipt $versionOutput "exactly 1.10.0")

    $headerOutput = Join-Path $Temporary "bad-header"
    Assert-Fails { Invoke-Supervisor "bad-header" @{ TestCollectorBehavior = "bad-header" } } "actual header"
    [void](Assert-FailureReceipt $headerOutput "actual header")

    $failureOutput = Join-Path $Temporary "collector-failure"
    Assert-Fails { Invoke-Supervisor "collector-failure" @{ TestCollectorBehavior = "failure-before-ready" } } "exited before collector readiness"
    [void](Assert-FailureReceipt $failureOutput "exited before collector readiness")
    if (Test-Path -LiteralPath (Join-Path $failureOutput "release.txt")) { throw "Failed collector was released" }

    $settleExitOutput = Join-Path $Temporary "collector-settle-exit"
    Assert-Fails { Invoke-Supervisor "collector-settle-exit" @{ TestCollectorBehavior = "exit-during-settle"; TestCollectorSettleMilliseconds = 1000 } } "exited during collector settle"
    [void](Assert-FailureReceipt $settleExitOutput "exited during collector settle")
    if (Test-Path -LiteralPath (Join-Path $settleExitOutput "release.txt")) { throw "Collector that died during settle was released" }

    $editorFailureOutput = Join-Path $Temporary "editor-failure"
    Assert-Fails { Invoke-Supervisor "editor-failure" @{ TestEditorBehavior = "failure" } } "Editor failed with exit code 7"
    [void](Assert-FailureReceipt $editorFailureOutput "Editor failed with exit code 7")

    $missingRawOutput = Join-Path $Temporary "collector-missing-raw-after-editor"
    Assert-Fails { Invoke-Supervisor "collector-missing-raw-after-editor" @{ TestCollectorBehavior = "timeout"; PresentMonTimeoutSeconds = 2 } } "PresentMon raw CSV was not produced"
    [void](Assert-FailureReceipt $missingRawOutput "PresentMon raw CSV was not produced")

    $timeoutOutput = Join-Path $Temporary "collector-timeout"
    Assert-Fails { Invoke-Supervisor "collector-timeout" @{ TestEditorBehavior = "linger-after-release"; TestCollectorBehavior = "timeout"; PresentMonTimeoutSeconds = 2 } } "PresentMon timed out"
    $timeout = Assert-FailureReceipt $timeoutOutput "PresentMon timed out"
    $childPid = [int](Get-Content -LiteralPath (Join-Path $timeoutOutput "collector-child.pid") -Raw)
    for ($attempt = 0; $attempt -lt 20 -and (Get-Process -Id $childPid -ErrorAction SilentlyContinue); ++$attempt) { Start-Sleep -Milliseconds 25 }
    if (!$timeout.process.presentMonTimedOut -or (Get-Process -Id $childPid -ErrorAction SilentlyContinue)) {
        throw "Collector timeout did not terminate and verify its fake child process tree"
    }

    Write-Host "PresentMon supervisor deterministic tests passed: readiness-plus-settle ordering, production argument contract, collision/stale, version/header, missing-raw, child failure/timeout, no-join failure, exact-session teardown, and process-tree cleanup."
} finally {
    Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction SilentlyContinue
}
