[CmdletBinding()]
param(
    [string]$ThreadId = $env:CODEX_THREAD_ID,
    [string]$CodexRoot = $(if ($env:CODEX_HOME) { $env:CODEX_HOME } else { Join-Path $env:USERPROFILE ".codex" })
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ThreadId)) {
    throw "CODEX_THREAD_ID is unavailable; pass -ThreadId explicitly instead of guessing from the newest rollout."
}

$sessionsRoot = Join-Path $CodexRoot "sessions"
if (!(Test-Path -LiteralPath $sessionsRoot -PathType Container)) {
    throw "Codex sessions directory does not exist: $sessionsRoot"
}

$rollouts = @(Get-ChildItem -LiteralPath $sessionsRoot -Recurse -File -Filter "*-$ThreadId.jsonl")
if ($rollouts.Count -ne 1) {
    throw "Expected exactly one rollout for thread $ThreadId beneath $sessionsRoot; found $($rollouts.Count)."
}

$sampleLine = Get-Content -LiteralPath $rollouts[0].FullName -Tail 256 |
    Where-Object { $_ -match '"type":"token_count"' -and $_ -match '"rate_limits"' } |
    Select-Object -Last 1
if ([string]::IsNullOrWhiteSpace($sampleLine)) {
    throw "No recent rate-limit sample exists in the current thread rollout. Complete one model response and retry."
}

$sample = $sampleLine | ConvertFrom-Json
$rateLimits = $sample.payload.rate_limits
$weekly = @($rateLimits.primary, $rateLimits.secondary) |
    Where-Object { $null -ne $_ -and [int]$_.window_minutes -eq 10080 } |
    Select-Object -First 1
if ($null -eq $weekly) {
    throw "The current sample has no 10080-minute weekly rate-limit window; do not reinterpret token totals as allowance."
}

$usedPercent = [double]$weekly.used_percent
$resetUtc = [DateTimeOffset]::FromUnixTimeSeconds([long]$weekly.resets_at).UtcDateTime.ToString("o")
[ordered]@{
    schema = 1
    threadId = $ThreadId
    sampleTimestamp = [string]$sample.timestamp
    usedPercent = $usedPercent
    remainingPercent = 100.0 - $usedPercent
    windowMinutes = [int]$weekly.window_minutes
    resetsAtUtc = $resetUtc
    source = $rollouts[0].FullName
} | ConvertTo-Json -Compress
