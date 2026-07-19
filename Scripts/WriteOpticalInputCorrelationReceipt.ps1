param(
    [Parameter(Mandatory = $true)][string]$MarkerPath,
    [Parameter(Mandatory = $true)][string]$ObservationPath,
    [Parameter(Mandatory = $true)][string]$ReceiptPath,
    [Parameter(Mandatory = $true)][ValidateSet("LDAT", "Photodiode", "HighSpeedCamera")][string]$Method,
    [Parameter(Mandatory = $true)][string]$Device,
    [Parameter(Mandatory = $true)][string]$Calibration,
    [Parameter(Mandatory = $true)][string]$Monitor,
    [Parameter(Mandatory = $true)][string]$Connection,
    [Parameter(Mandatory = $true)][string]$OsRefresh,
    [Parameter(Mandatory = $true)][string]$DriverDisplayState,
    [ValidateRange(100, 60000)][int]$TimeoutMilliseconds = 30000
)

$ErrorActionPreference = "Stop"
function Write-AtomicJson([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if ([string]::IsNullOrWhiteSpace($parent)) { throw "Receipt path needs a parent directory." }
    if (Test-Path -LiteralPath $Path) { throw "Receipt collision: destination already exists." }
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try { [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 8), [Text.Encoding]::UTF8); Move-Item -LiteralPath $temporary -Destination $Path }
    finally { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
}

$receipt = [ordered]@{ schema = 1; status = "failed"; method = $Method; device = $Device; calibration = $Calibration; monitor = $Monitor; connection = $Connection; osRefresh = $OsRefresh; driverDisplayState = $DriverDisplayState; requestedPresentationMode = "unavailable"; actualPresentationMode = "unavailable"; marker = $null; observation = $null; rawObservationSha256 = "unavailable"; unavailable = [ordered]@{ panelCadence = "unavailable"; vrrActive = "unavailable"; peripheralInput = "unavailable"; clickToPhoton = "unavailable" }; cleanup = [ordered]@{ ownedTemporaryFiles = "removed"; operatorOwnedDevice = "not-owned"; operatorOwnedRawObservation = "preserved" }; failure = $null }
try {
    $marker = Get-Content -Raw -LiteralPath $MarkerPath | ConvertFrom-Json
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while (!(Test-Path -LiteralPath $ObservationPath)) {
        if ($deadline.ElapsedMilliseconds -ge $TimeoutMilliseconds) { throw "Optical observation timed out before the bounded deadline." }
        Start-Sleep -Milliseconds 10
    }
    $observationBytes = [IO.File]::ReadAllBytes($ObservationPath)
    $observation = [Text.Encoding]::UTF8.GetString($observationBytes) | ConvertFrom-Json
    if ($marker.schema -ne 1 -or [string]::IsNullOrWhiteSpace([string]$marker.runId) -or [uint64]$marker.processId -le 0 -or [string]::IsNullOrWhiteSpace([string]$marker.executablePath) -or [uint64]$marker.qpcFrequency -le 0 -or [uint64]$marker.qpcTick -le 0 -or [string]::IsNullOrWhiteSpace([string]$marker.markerId) -or [string]::IsNullOrWhiteSpace([string]$marker.triggerId) -or [uint64]$marker.frame -ne [uint64]$marker.inputFrame -or [uint64]$marker.inputQpc -lt 0 -or [string]::IsNullOrWhiteSpace([string]$marker.backend) -or [string]::IsNullOrWhiteSpace([string]$marker.requestedPresentationMode) -or [string]::IsNullOrWhiteSpace([string]$marker.actualPresentationMode) -or [uint64]$marker.presentationGeneration -lt 1) { throw "Marker readiness schema, identity, trigger, or presentation binding is malformed." }
    if ($observation.source -notin @("LDAT", "Photodiode", "HighSpeedCamera") -or $observation.source -ne $Method) { throw "Optical observation source is not the declared hardware method." }
    if ([string]$observation.runId -ne [string]$marker.runId -or [uint64]$observation.processId -ne [uint64]$marker.processId -or [string]$observation.executablePath -ne [string]$marker.executablePath -or [uint64]$observation.qpcFrequency -ne [uint64]$marker.qpcFrequency -or $observation.duplicate -eq $true -or $observation.ambiguous -eq $true -or [string]$observation.markerId -ne [string]$marker.markerId -or [string]$observation.triggerId -ne [string]$marker.triggerId -or [uint64]$observation.frame -ne [uint64]$marker.frame -or [uint64]$observation.inputFrame -ne [uint64]$marker.inputFrame -or [string]::IsNullOrWhiteSpace([string]$observation.timingDomain) -or [double]$observation.observedMilliseconds -lt 0.0 -or [double]::IsNaN([double]$observation.observedMilliseconds) -or [double]::IsInfinity([double]$observation.observedMilliseconds)) { throw "Optical observation is stale, duplicate, ambiguous, noncausal, or does not bind the exact process/QPC/trigger/marker/input/frame in a declared timing domain." }
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $receipt.rawObservationSha256 = ([BitConverter]::ToString($hash.ComputeHash($observationBytes))).Replace("-", "").ToLowerInvariant() }
    finally { $hash.Dispose() }
    $receipt.status = "completed"; $receipt.marker = $marker; $receipt.observation = $observation
    $receipt.requestedPresentationMode = if ($marker.requestedPresentationMode) { [string]$marker.requestedPresentationMode } else { "unavailable" }
    $receipt.actualPresentationMode = if ($marker.actualPresentationMode) { [string]$marker.actualPresentationMode } else { "unavailable" }
} catch { $receipt.failure = $_.Exception.Message } finally { Write-AtomicJson $ReceiptPath $receipt }
if ($receipt.status -ne "completed") { throw "Optical input correlation failed closed: $($receipt.failure)" }
Write-Host "OpticalInputCorrelationReceiptV1 result=pass method=$Method markerId=$($receipt.marker.markerId) frame=$($receipt.marker.frame)"
