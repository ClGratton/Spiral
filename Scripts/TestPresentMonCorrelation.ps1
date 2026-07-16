$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Joiner = Join-Path $PSScriptRoot "JoinPresentMonCorrelation.ps1"
$Headers = "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,QPCTime"
$Temporary = Join-Path ([IO.Path]::GetTempPath()) "spiral-presentmon-correlation-$([guid]::NewGuid().ToString('N'))"

function New-Fixture([string]$Name, [int[]]$EngineQpcs = @(100, 200, 300), [int[]]$PresentMonQpcs = @(90, 110, 210, 310, 400), [string]$Header = $Headers, [int]$ProcessId = 42, [int]$PresentMonProcessId = 42) {
    $directory = Join-Path $Temporary $Name; New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $engine = [ordered]@{ schema = 2; condition = [ordered]@{ runId = "fixture-$Name"; processId = $ProcessId; qpcFrequency = 10000000 }; frames = @() }
    for ($index = 0; $index -lt $EngineQpcs.Count; ++$index) { $engine.frames += [ordered]@{ frame = $index; lifecycle = @([ordered]@{ phase = "PresentBegin"; qpc = $EngineQpcs[$index] }) } }
    $enginePath = Join-Path $directory "engine.json"; $engine | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $enginePath -Encoding utf8
    $csvPath = Join-Path $directory "presentmon.csv"; $lines = [Collections.Generic.List[string]]::new(); $lines.Add($Header)
    foreach ($qpc in $PresentMonQpcs) { $lines.Add("Editor.exe,$PresentMonProcessId,0x1,DXGI,1,0,0,0.1,0.1,1.0,0,Hardware Composed: Independent Flip,0.1,1.0,1.0,$qpc") }
    [IO.File]::WriteAllLines($csvPath, $lines, [Text.UTF8Encoding]::new($false))
    return [pscustomobject]@{ Engine = $enginePath; Csv = $csvPath; Report = (Join-Path $directory "report.json") }
}

function Assert-Fails([scriptblock]$Action, [string]$Needle) {
    try { & $Action } catch { if ($_.Exception.Message -like "*$Needle*") { return }; throw "Expected '$Needle', got '$($_.Exception.Message)'" }
    throw "Expected failure containing '$Needle'"
}

try {
    $success = New-Fixture "success"
    & $Joiner -EngineJsonPath $success.Engine -PresentMonCsvPath $success.Csv -OutputPath $success.Report -FinalQpcTolerance 20
    $report = Get-Content -Raw $success.Report | ConvertFrom-Json
    if ($report.schema -ne 1 -or $report.counts.pairedRows -ne 3 -or $report.counts.warmupRows -ne 1 -or $report.counts.trailingRows -ne 1 -or @($report.pairs | Select-Object -ExpandProperty presentMonQpc -Unique).Count -ne 3) { throw "Success report violated schema/count/one-to-one invariants" }
    Assert-Fails { & $Joiner $success.Engine $success.Csv $success.Engine -FinalQpcTolerance 20 } "distinct from both raw input"

    $wrongPid = New-Fixture "wrong-pid" -PresentMonProcessId 7; Assert-Fails { & $Joiner $wrongPid.Engine $wrongPid.Csv $wrongPid.Report -FinalQpcTolerance 20 } "ProcessID"
    $badHeader = New-Fixture "bad-header" -Header ($Headers -replace "Dropped", "DroppedX"); Assert-Fails { & $Joiner $badHeader.Engine $badHeader.Csv $badHeader.Report -FinalQpcTolerance 20 } "header"
    $missingHeader = New-Fixture "missing-header" -Header ""; Assert-Fails { & $Joiner $missingHeader.Engine $missingHeader.Csv $missingHeader.Report -FinalQpcTolerance 20 } "header"
    $missingRows = New-Fixture "missing-rows" -PresentMonQpcs @(90, 110, 210); Assert-Fails { & $Joiner $missingRows.Engine $missingRows.Csv $missingRows.Report -FinalQpcTolerance 20 } "final candidates"
    $missingInterval = New-Fixture "missing-interval" -PresentMonQpcs @(90, 110, 310); Assert-Fails { & $Joiner $missingInterval.Engine $missingInterval.Csv $missingInterval.Report -FinalQpcTolerance 20 } "interval candidates"
    $duplicateQpc = New-Fixture "duplicate-qpc" -PresentMonQpcs @(90, 110, 210, 210, 310); Assert-Fails { & $Joiner $duplicateQpc.Engine $duplicateQpc.Csv $duplicateQpc.Report -FinalQpcTolerance 20 } "monotonic"
    $reorderedQpc = New-Fixture "reordered-qpc" -PresentMonQpcs @(90, 110, 250, 210, 310); Assert-Fails { & $Joiner $reorderedQpc.Engine $reorderedQpc.Csv $reorderedQpc.Report -FinalQpcTolerance 20 } "monotonic"
    $extraInterval = New-Fixture "extra-interval" -PresentMonQpcs @(90, 110, 120, 210, 310); Assert-Fails { & $Joiner $extraInterval.Engine $extraInterval.Csv $extraInterval.Report -FinalQpcTolerance 20 } "interval candidates"
    $nonCausalFinal = New-Fixture "noncausal-final" -EngineQpcs @(100) -PresentMonQpcs @(90); Assert-Fails { & $Joiner $nonCausalFinal.Engine $nonCausalFinal.Csv $nonCausalFinal.Report -FinalQpcTolerance 20 } "final candidates"
    $ambiguousFinal = New-Fixture "ambiguous-final" -EngineQpcs @(100) -PresentMonQpcs @(110, 115); Assert-Fails { & $Joiner $ambiguousFinal.Engine $ambiguousFinal.Csv $ambiguousFinal.Report -FinalQpcTolerance 20 } "final candidates"
    Write-Host "PresentMon correlation synthetic tests passed: schema, headers, PID, missing/extra, monotonic/unique, causal/final ambiguity, and one-to-one reuse invariant"
} finally {
    Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction SilentlyContinue
}
