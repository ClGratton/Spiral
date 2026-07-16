param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [string]$Action = "vs2022",

    [ValidateRange(1, 300)]
    [int]$ChildTimeoutSeconds = 30,

    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "InvokeBoundedProcess.ps1")

if ($Action -notlike "vs*") {
    throw "D3D12 timestamp smoke requires the Visual Studio/MSVC build path. Use -Action vs2022."
}

if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration -Action $Action
}

$Editor = Join-Path $Root "bin/$Configuration-windows-x86_64/Editor/Editor.exe"
if (!(Test-Path $Editor)) {
    throw "Editor executable not found: $Editor"
}

$Result = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--smoke-test", "--rhi-timestamp-query-smoke") -Label "D3D12 timestamp query smoke" -TimeoutSeconds $ChildTimeoutSeconds
if ($Result.TimedOut) {
    throw "D3D12 timestamp query smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($Result.ExitCode -ne 0) {
    throw "D3D12 timestamp query smoke failed with exit code $($Result.ExitCode)."
}

$Output = $Result.Output -join "`n"
if ($Output -notmatch 'D3D12 capability state: Timestamps advertised=yes, enabled=yes, implemented=yes, exercised=no') {
    throw "D3D12 timestamp capability was not reported as usable."
}
if ($Output -notmatch 'RHITimestampQuerySmokeV1 backend=D3D12, allocation=pass, periodNanoseconds=[0-9]+(?:\.[0-9]+)?, writeResolve=pass, pending=pass, readback=pass, reuse=retired-pass, destruction=retained-pass, result=pass') {
    throw "D3D12 native timestamp allocation, resolve, retirement, reuse, or destruction evidence was incomplete."
}

Write-Host "D3D12 timestamp query smoke passed: $Configuration ($Action)"
