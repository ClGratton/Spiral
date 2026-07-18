param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$EngineJsonPath,
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PresentMonCsvPath,
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputPath,
    [ValidateRange(1, 10000000)]
    [UInt64]$FinalQpcTolerance = 2000
)

$ErrorActionPreference = "Stop"

$ExpectedHeaders = @(
    "Application", "ProcessID", "SwapChainAddress", "Runtime", "SyncInterval", "PresentFlags", "Dropped", "TimeInSeconds",
    "msInPresentAPI", "msBetweenPresents", "AllowsTearing", "PresentMode", "msUntilRenderComplete", "msUntilDisplayed",
    "msBetweenDisplayChange", "QPCTime"
)

function Get-CanonicalPath([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Input file does not exist: $Path" }
    return [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Convert-ToUInt64([object]$Value, [string]$Label) {
    [UInt64]$parsed = 0
    if ($null -eq $Value -or ![UInt64]::TryParse([string]$Value, [Globalization.NumberStyles]::Integer,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
        throw "$Label is not an unsigned integer"
    }
    return $parsed
}

function Get-NearestRank([UInt64[]]$Values, [double]$Quantile) {
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Ceiling($Quantile * $sorted.Count) - 1
    return $sorted[[Math]::Max(0, [Math]::Min($index, $sorted.Count - 1))]
}

function Write-Atomically([string]$Path, [string]$Text) {
    $directory = Split-Path -Parent $Path
    if (![string]::IsNullOrWhiteSpace($directory)) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    } finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

$enginePath = Get-CanonicalPath $EngineJsonPath
$presentMonPath = Get-CanonicalPath $PresentMonCsvPath
$reportPath = [IO.Path]::GetFullPath($OutputPath)
if ($reportPath.Equals($enginePath, [StringComparison]::OrdinalIgnoreCase) -or
    $reportPath.Equals($presentMonPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Correlation report path must be distinct from both raw input paths"
}
$engineHashBefore = Get-Sha256 $enginePath
$presentMonHashBefore = Get-Sha256 $presentMonPath

$engine = Get-Content -LiteralPath $enginePath -Raw | ConvertFrom-Json
if ($engine.schema -notin @(2, 3, 4) -or $null -eq $engine.condition -or $null -eq $engine.frames) {
    throw "Engine input must be a schema-2, schema-3, or schema-4 frame-pacing benchmark artifact"
}
$processId = Convert-ToUInt64 $engine.condition.processId "Engine condition processId"
$qpcFrequency = Convert-ToUInt64 $engine.condition.qpcFrequency "Engine condition qpcFrequency"
if ($processId -eq 0 -or $qpcFrequency -eq 0) { throw "Engine identity requires nonzero processId and qpcFrequency" }
$frames = @($engine.frames)
if ($frames.Count -eq 0) { throw "Engine input contains no retained frames" }

$presentBegins = [Collections.Generic.List[object]]::new()
$expectedFrame = $null
foreach ($frame in $frames) {
    $frameId = Convert-ToUInt64 $frame.frame "Engine frame id"
    if ($null -ne $expectedFrame -and $frameId -ne $expectedFrame + 1) { throw "Engine frame IDs are not continuous" }
    $expectedFrame = $frameId
    $matches = @($frame.lifecycle | Where-Object { $_.phase -eq "PresentBegin" })
    if ($matches.Count -ne 1) { throw "Engine frame $frameId must contain exactly one PresentBegin" }
    $qpc = Convert-ToUInt64 $matches[0].qpc "Engine PresentBegin QPC for frame $frameId"
    if ($qpc -eq 0) { throw "Engine PresentBegin QPC for frame $frameId must be nonzero" }
    $presentBegins.Add([pscustomobject]@{ Frame = $frameId; Qpc = $qpc })
}
for ($index = 1; $index -lt $presentBegins.Count; ++$index) {
    if ($presentBegins[$index].Qpc -le $presentBegins[$index - 1].Qpc) { throw "Engine PresentBegin QPC values are not strictly monotonic and unique" }
}

$headerLine = Get-Content -LiteralPath $presentMonPath -TotalCount 1
if ([string]::IsNullOrWhiteSpace($headerLine)) { throw "PresentMon CSV is missing its header" }
$actualHeaders = @($headerLine.Split(','))
if ($actualHeaders.Count -ne $ExpectedHeaders.Count -or @($actualHeaders | Where-Object { $_ -notin $ExpectedHeaders }).Count -ne 0 -or
    @($ExpectedHeaders | Where-Object { $_ -notin $actualHeaders }).Count -ne 0 -or
    (@($actualHeaders | Select-Object -Unique).Count -ne $actualHeaders.Count) -or
    -not (($actualHeaders -join "|") -ceq ($ExpectedHeaders -join "|"))) {
    throw "PresentMon CSV header set/order does not match official 1.10.0 x64 output"
}
$presentMonRows = @(Import-Csv -LiteralPath $presentMonPath)
if ($presentMonRows.Count -eq 0) { throw "PresentMon CSV contains no rows" }

$normalizedRows = [Collections.Generic.List[object]]::new()
for ($index = 0; $index -lt $presentMonRows.Count; ++$index) {
    $row = $presentMonRows[$index]
    $rowProcessId = Convert-ToUInt64 $row.ProcessID "PresentMon ProcessID at row $index"
    if ($rowProcessId -ne $processId) { throw "PresentMon row $index has ProcessID $rowProcessId instead of engine processId $processId" }
    $qpc = Convert-ToUInt64 $row.QPCTime "PresentMon QPCTime at row $index"
    $normalizedRows.Add([pscustomobject]@{ Index = $index; Qpc = $qpc; Row = $row })
}
for ($index = 1; $index -lt $normalizedRows.Count; ++$index) {
    if ($normalizedRows[$index].Qpc -le $normalizedRows[$index - 1].Qpc) { throw "PresentMon QPCTime values are not strictly monotonic and unique" }
}

$usedRows = [Collections.Generic.HashSet[int]]::new()
$pairs = [Collections.Generic.List[object]]::new()
for ($engineIndex = 0; $engineIndex -lt $presentBegins.Count; ++$engineIndex) {
    $enginePresent = $presentBegins[$engineIndex]
    $isLast = $engineIndex -eq $presentBegins.Count - 1
    $upperBound = if ($isLast) { $enginePresent.Qpc + $FinalQpcTolerance } else { $presentBegins[$engineIndex + 1].Qpc }
    if ($isLast -and $upperBound -lt $enginePresent.Qpc) { throw "Final QPC tolerance overflows the accepted QPC range" }
    $candidates = @($normalizedRows | Where-Object {
            $_.Qpc -ge $enginePresent.Qpc -and $(if ($isLast) { $_.Qpc -le $upperBound } else { $_.Qpc -lt $upperBound })
        })
    if ($candidates.Count -ne 1) {
        $kind = if ($isLast) { "final" } else { "interval" }
        throw "Engine frame $($enginePresent.Frame) has $($candidates.Count) causal PresentMon $kind candidates; exactly one is required"
    }
    $candidate = $candidates[0]
    if (!$usedRows.Add($candidate.Index)) { throw "PresentMon row $($candidate.Index) would be reused" }
    $source = $candidate.Row
    $pairs.Add([ordered]@{
            frame = $enginePresent.Frame
            engineQpc = $enginePresent.Qpc
            presentMonQpc = $candidate.Qpc
            deltaQpc = $candidate.Qpc - $enginePresent.Qpc
            presentMon = [ordered]@{
                application = $source.Application; processId = $source.ProcessID; swapChainAddress = $source.SwapChainAddress
                runtime = $source.Runtime; syncInterval = $source.SyncInterval; presentFlags = $source.PresentFlags; dropped = $source.Dropped
                timeInSeconds = $source.TimeInSeconds; msInPresentAPI = $source.msInPresentAPI; msBetweenPresents = $source.msBetweenPresents
                allowsTearing = $source.AllowsTearing; presentMode = $source.PresentMode; msUntilRenderComplete = $source.msUntilRenderComplete
                msUntilDisplayed = $source.msUntilDisplayed; msBetweenDisplayChange = $source.msBetweenDisplayChange
            }
        })
}

$firstEngineQpc = $presentBegins[0].Qpc
$lastEngineQpc = $presentBegins[$presentBegins.Count - 1].Qpc
$warmupRows = @($normalizedRows | Where-Object { $_.Qpc -lt $firstEngineQpc }).Count
$trailingRows = @($normalizedRows | Where-Object { $_.Qpc -gt ($lastEngineQpc + $FinalQpcTolerance) }).Count
if ($usedRows.Count -ne $presentBegins.Count) { throw "PresentMon pairing did not retain a one-to-one row count" }

$engineHashAfter = Get-Sha256 $enginePath
$presentMonHashAfter = Get-Sha256 $presentMonPath
if ($engineHashBefore -ne $engineHashAfter -or $presentMonHashBefore -ne $presentMonHashAfter) { throw "Raw input changed while correlation was running" }

$deltas = [UInt64[]]@($pairs | ForEach-Object { [UInt64]$_.deltaQpc })
$report = [ordered]@{
    schema = 1
    rawInputs = [ordered]@{
        engineJsonPath = $enginePath; engineJsonSha256 = $engineHashBefore
        presentMonCsvPath = $presentMonPath; presentMonCsvSha256 = $presentMonHashBefore
    }
    identity = [ordered]@{ runId = $engine.condition.runId; processId = $processId; qpcFrequency = $qpcFrequency }
    presentMonHeaders = $ExpectedHeaders
    finalQpcTolerance = $FinalQpcTolerance
    counts = [ordered]@{ retainedEngineFrames = $presentBegins.Count; presentMonRows = $normalizedRows.Count; pairedRows = $pairs.Count; warmupRows = $warmupRows; trailingRows = $trailingRows }
    summary = [ordered]@{
        deltaQpcMin = ($deltas | Measure-Object -Minimum).Minimum; deltaQpcP50 = Get-NearestRank $deltas 0.5; deltaQpcMax = ($deltas | Measure-Object -Maximum).Maximum
    }
    pairs = @($pairs)
}
Write-Atomically $reportPath (($report | ConvertTo-Json -Depth 16) + [Environment]::NewLine)
Write-Host "PresentMon correlation passed: pairs=$($pairs.Count) warmup=$warmupRows trailing=$trailingRows report=$reportPath"
