param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = "Stop"

$manifestPath = Join-Path $Source ".spiral-package-manifest"
$noticePath = Join-Path $PSScriptRoot "../Vendor/CEF/THIRD_PARTY_NOTICE.md"
foreach ($required in @("include/cef_version.h", "LICENSE.txt", "CREDITS.html", ".spiral-package-manifest")) {
    if (!(Test-Path (Join-Path $Source $required) -PathType Leaf)) {
        throw "Pinned CEF package is unavailable or lacks '$required' at '$Source'. Run Scripts/FetchCEF.ps1 first."
    }
}
if (!(Test-Path $noticePath -PathType Leaf)) { throw "Vendor/CEF/THIRD_PARTY_NOTICE.md is missing." }

# README.txt of the minimal distribution: libcef.dll, chrome_elf.dll, icudtl.dat and v8_context_snapshot.bin
# are required; the rest degrade features. bootstrap.exe/bootstrapc.exe belong to the unresolved Windows
# sandbox design and are never staged here.
$requiredRelease = @("libcef.dll", "chrome_elf.dll", "v8_context_snapshot.bin")
$optionalRelease = @("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll", "vk_swiftshader.dll", "vk_swiftshader_icd.json", "vulkan-1.dll")
$requiredResources = @("icudtl.dat", "resources.pak", "chrome_100_percent.pak", "chrome_200_percent.pak")
$locale = "en-US.pak"

$runtimeFiles = @()
foreach ($name in $requiredRelease) {
    $path = Join-Path $Source "Release/$name"
    if (!(Test-Path $path -PathType Leaf)) { throw "Pinned CEF package lacks Release/$name." }
    $runtimeFiles += $path
}
foreach ($name in $requiredResources) {
    $path = Join-Path $Source "Resources/$name"
    if (!(Test-Path $path -PathType Leaf)) { throw "Pinned CEF package lacks Resources/$name." }
    $runtimeFiles += $path
}
$localePath = Join-Path $Source "Resources/locales/$locale"
if (!(Test-Path $localePath -PathType Leaf)) { throw "Pinned CEF package lacks Resources/locales/$locale." }
foreach ($name in $optionalRelease) {
    $path = Join-Path $Source "Release/$name"
    if (Test-Path $path -PathType Leaf) { $runtimeFiles += $path }
}

$manifestLines = @(Get-Content -LiteralPath $manifestPath)
$cefHash = ($manifestLines | Where-Object { $_ -like "sha256=*" }) -replace '^sha256=', ''
$cefBuild = ($manifestLines | Where-Object { $_ -like "build=*" }) -replace '^build=', ''
if ([string]::IsNullOrWhiteSpace($cefHash) -or [string]::IsNullOrWhiteSpace($cefBuild)) {
    throw "Pinned CEF installed manifest lacks the archive hash or build."
}

$runtimeNames = (($runtimeFiles | ForEach-Object { Split-Path -Leaf $_ }) + "locales/$locale") -join ","
$runtimeManifest = @(
    "format=1",
    "cef_build=$cefBuild",
    "cef_sha256=$cefHash",
    "cef_runtime=$runtimeNames",
    "notices=CEF-LICENSE.txt,CEF-CREDITS.html,CEF-THIRD_PARTY_NOTICE.md",
    "sandbox=undecided-bootstrap-exe-not-staged",
    "distribution_status=blocked-pending-cef-chromium-notice-audit"
)
$runtimeManifestPath = Join-Path $Destination "CefRuntimeManifest.txt"

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# Skip the copy when the previous staging is intact; libcef.dll is about 290 MB.
$current = Test-Path $runtimeManifestPath -PathType Leaf
if ($current) {
    $existing = @(Get-Content -LiteralPath $runtimeManifestPath)
    $current = ($existing.Count -eq $runtimeManifest.Count)
    for ($index = 0; $current -and $index -lt $runtimeManifest.Count; ++$index) {
        if ($existing[$index] -cne $runtimeManifest[$index]) { $current = $false }
    }
}
if ($current) {
    foreach ($file in $runtimeFiles) {
        $staged = Join-Path $Destination (Split-Path -Leaf $file)
        if (!(Test-Path $staged -PathType Leaf) -or (Get-Item -LiteralPath $staged).Length -ne (Get-Item -LiteralPath $file).Length) { $current = $false; break }
    }
}
if ($current -and (Test-Path (Join-Path $Destination "locales/$locale") -PathType Leaf) -and
    (Test-Path (Join-Path $Destination "CEF-LICENSE.txt") -PathType Leaf)) {
    exit 0
}

# Remove only the names this script owns; the helper executable that shares the directory is left alone.
$ownedFiles = @($requiredRelease + $optionalRelease + $requiredResources + @(
        "bootstrap.exe", "bootstrapc.exe", "CEF-LICENSE.txt", "CEF-CREDITS.html", "CEF-THIRD_PARTY_NOTICE.md", "CefRuntimeManifest.txt"))
foreach ($name in $ownedFiles) {
    Remove-Item -LiteralPath (Join-Path $Destination $name) -Force -ErrorAction SilentlyContinue
}
Remove-Item -LiteralPath (Join-Path $Destination "locales") -Recurse -Force -ErrorAction SilentlyContinue

Copy-Item -LiteralPath $runtimeFiles -Destination $Destination -Force
New-Item -ItemType Directory -Force -Path (Join-Path $Destination "locales") | Out-Null
Copy-Item -LiteralPath $localePath -Destination (Join-Path $Destination "locales") -Force
Copy-Item -LiteralPath (Join-Path $Source "LICENSE.txt") -Destination (Join-Path $Destination "CEF-LICENSE.txt") -Force
Copy-Item -LiteralPath (Join-Path $Source "CREDITS.html") -Destination (Join-Path $Destination "CEF-CREDITS.html") -Force
Copy-Item -LiteralPath $noticePath -Destination (Join-Path $Destination "CEF-THIRD_PARTY_NOTICE.md") -Force
Set-Content -LiteralPath $runtimeManifestPath -Value $runtimeManifest -Encoding ascii
