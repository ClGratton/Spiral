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

if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration -Action $Action
}

$OutputSuffix = if ($Action -like "gmake*") { "-$Action" } else { "" }
$Editor = Join-Path $Root "bin/$Configuration-windows-x86_64$OutputSuffix/Editor/Editor.exe"
if (!(Test-Path $Editor)) {
    throw "Vulkan timestamp smoke executable was not found: $Editor"
}

$Result = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--vulkan-render-smoke", "--rhi-timestamp-query-smoke") -Label "Vulkan timestamp query smoke" -TimeoutSeconds $ChildTimeoutSeconds
if ($Result.TimedOut) {
    throw "Vulkan timestamp query smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($Result.ExitCode -ne 0) {
    throw "Vulkan timestamp query smoke failed with exit code $($Result.ExitCode)."
}

$Output = $Result.Output -join "`n"
if ($Output -notmatch 'Vulkan capability state: Timestamps advertised=yes, enabled=yes, implemented=yes, exercised=no') {
    throw "Vulkan timestamp capability was not reported as usable."
}
if ($Output -notmatch 'RHITimestampQuerySmokeV1 backend=Vulkan, allocation=pass, periodNanoseconds=[0-9]+(?:\.[0-9]+)?, writeResolve=pass, pending=pass, readback=pass, reuse=retired-pass, destruction=retained-pass, result=pass') {
    throw "Vulkan native timestamp allocation, resolve, retirement, reuse, or destruction evidence was incomplete."
}

Write-Host "Vulkan timestamp query smoke passed: $Configuration ($Action)"
