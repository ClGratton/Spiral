param(
    [Parameter(Mandatory = $true)][string]$CsvPath,
    [Parameter(Mandatory = $true)][string]$ReadinessPath,
    [Parameter(Mandatory = $true)][string]$ReleasePath,
    [Parameter(Mandatory = $true)][int]$EditorPid,
    [Parameter(Mandatory = $true)][string]$SessionName,
    [ValidateSet("success", "linger-holding-csv", "bad-header", "failure-before-ready", "exit-during-settle", "timeout")][string]$Behavior
)

$ErrorActionPreference = "Stop"
if ($Behavior -eq "failure-before-ready") { [Console]::Error.WriteLine("fake collector failed before readiness"); exit 9 }
Write-Output "Started recording."

if ($Behavior -eq "exit-during-settle") { Start-Sleep -Milliseconds 500; exit 8 }

if ($Behavior -eq "timeout") {
    $processPath = (Get-Process -Id $PID).Path
    $child = Start-Process -FilePath $processPath -ArgumentList @("-NoProfile", "-Command", "Start-Sleep -Seconds 60") -PassThru
    $outputRoot = Split-Path -Parent (Split-Path -Parent $CsvPath)
    [IO.File]::WriteAllText((Join-Path $outputRoot "collector-child.pid"), [string]$child.Id)
    while ($true) { Start-Sleep -Seconds 1 }
}

$deadline = [Diagnostics.Stopwatch]::StartNew()
while (!(Test-Path -LiteralPath $ReleasePath) -and $deadline.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 10 }
if (!(Test-Path -LiteralPath $ReleasePath)) { throw "Fake collector did not observe supervisor release" }
$readiness = Get-Content -LiteralPath $ReadinessPath -Raw | ConvertFrom-Json
if ([int]$readiness.processId -ne $EditorPid) { throw "Fake collector exact PID mismatch" }
$header = "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,QPCTime"
if ($Behavior -eq "bad-header") { $header = $header -replace "Dropped", "DroppedX" }
$lines = [Collections.Generic.List[string]]::new()
$lines.Add($header)
foreach ($offset in @(9000, 10100, 20100, 30100, 40000)) {
    $lines.Add("Editor.exe,$EditorPid,0x1,DXGI,1,0,0,0.1,0.1,1.0,0,Hardware Composed: Independent Flip,0.1,1.0,1.0,$([UInt64]$readiness.qpcTick + $offset)")
}
[IO.Directory]::CreateDirectory((Split-Path -Parent $CsvPath)) | Out-Null
[IO.File]::WriteAllLines($CsvPath, $lines, [Text.UTF8Encoding]::new($false))
while (Get-Process -Id $EditorPid -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 10 }
if ($Behavior -eq "linger-holding-csv") {
    $exclusiveCsv = [IO.File]::Open($CsvPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try { while ($true) { Start-Sleep -Seconds 1 } }
    finally { $exclusiveCsv.Dispose() }
}
Start-Sleep -Milliseconds 100
Write-Output "FakePresentMon state=complete session=$SessionName"
