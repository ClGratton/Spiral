param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [ValidateSet("vs2022", "gmake", "gmake2")]
    [string]$Action = "vs2022"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

& (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration -Action $Action

$OutputSuffix = if ($Action -like "gmake*") { "-$Action" } else { "" }
$Executable = Join-Path $Root "bin\$Configuration-windows-x86_64$OutputSuffix\Editor\Editor.exe"
if (!(Test-Path $Executable)) {
    throw "Vulkan smoke executable was not found: $Executable"
}

$Output = & $Executable --vulkan-render-smoke 2>&1 | Tee-Object -Variable VulkanLog
if ($LASTEXITCODE -ne 0) {
    throw "Vulkan render smoke failed with exit code $LASTEXITCODE."
}

$RequiredMarkers = @(
    "NVRHI Vulkan device created on adapter:",
    "Selected Vulkan adapter:",
    "Vulkan capability profile: Phase 3 Vulkan Bootstrap Presentation V1, qualification=Bootstrap",
    "Vulkan capability state: Timeline Synchronization advertised=yes, enabled=yes, implemented=yes",
    "Vulkan capability state: Buffer Device Address advertised=",
    "Renderer initialized with backend: NVRHI Vulkan",
    "Vulkan swapchain and ImGui presentation initialized",
    "Vulkan render smoke requested window resize",
    "Vulkan swapchain recreated after window resize",
    "Vulkan render smoke verified native ImGui presentation after resize"
)

$JoinedLog = $VulkanLog -join "`n"
foreach ($Marker in $RequiredMarkers) {
    if (!$JoinedLog.Contains($Marker)) {
        throw "Vulkan render smoke did not emit required marker: $Marker"
    }
}

Write-Host "Vulkan render smoke passed: $Configuration ($Action)"
