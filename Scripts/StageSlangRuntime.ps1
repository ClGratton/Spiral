param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$DxcSource,
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = "Stop"

if (!(Test-Path (Join-Path $Source "include/slang.h") -PathType Leaf)) {
    throw "Pinned Slang package is unavailable at '$Source'. Run Scripts/Setup.ps1 first."
}

$slangManifest = Join-Path $Source ".spiral-package-manifest"
$dxcManifest = Join-Path $DxcSource ".spiral-package-manifest"
$slangCompiler = Join-Path $Source "bin/slang-compiler.dll"
$slangProxy = Join-Path $Source "bin/slang.dll"
$slangSpirvOptimizer = Join-Path $Source "bin/slang-glslang.dll"
$slangLicense = Join-Path $Source "LICENSE"
$moduleDirectories = @(Get-ChildItem -LiteralPath (Join-Path $Source "bin") -Filter "slang-standard-module-*" -Directory)
if (!(Test-Path $slangManifest -PathType Leaf) -or
    !(Test-Path $slangCompiler -PathType Leaf) -or
    !(Test-Path $slangProxy -PathType Leaf) -or
    !(Test-Path $slangSpirvOptimizer -PathType Leaf) -or
    !(Test-Path $slangLicense -PathType Leaf) -or
    $moduleDirectories.Count -ne 1 -or
    !(Test-Path (Join-Path $moduleDirectories[0].FullName "experimental/workgraph.slang-module") -PathType Leaf) -or
    !(Test-Path (Join-Path $moduleDirectories[0].FullName "slang/neural.slang-module") -PathType Leaf)) {
    throw "Pinned Slang package at '$Source' lacks its validated manifest, exact compiler runtime, license, or standard module."
}

$dxcCompiler = Join-Path $DxcSource "bin/x64/dxcompiler.dll"
$dxcValidator = Join-Path $DxcSource "bin/x64/dxil.dll"
$dxcNotices = @("LICENSE-LLVM.txt", "LICENSE-MIT.txt", "LICENSE-MS.txt") | ForEach-Object { Join-Path $DxcSource $_ }
if (!(Test-Path $dxcManifest -PathType Leaf) -or
    !(Test-Path $dxcCompiler -PathType Leaf) -or
    !(Test-Path $dxcValidator -PathType Leaf) -or
    @($dxcNotices | Where-Object { !(Test-Path $_ -PathType Leaf) }).Count -ne 0) {
    throw "Pinned DXC compiler, validator, installed manifest, or notices are unavailable at '$DxcSource'. Run Scripts/Setup.ps1 first."
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$legacyFiles = @(
    "gfx.dll", "gfx.slang", "slang-glsl-module.dll", "slang-glslang.dll", "slang-llvm.dll",
    "slang-glsl-module.bin", "slang-rt.dll", "slang.slang", "slangc.exe", "slang.dll", "slang-compiler.dll", "dxcompiler.dll", "dxil.dll",
    "Slang-LICENSE.txt", "Slang-THIRD_PARTY_NOTICE.md", "DXC-LICENSE-LLVM.txt", "DXC-LICENSE-MIT.txt",
    "DXC-LICENSE-MS.txt", "DXC-NOTICE.md", "ShaderToolchainRuntimeManifest.txt"
)
foreach ($name in $legacyFiles) {
    Remove-Item -LiteralPath (Join-Path $Destination $name) -Force -ErrorAction SilentlyContinue
}
Get-ChildItem -LiteralPath $Destination -Filter "slang-standard-module-*" -Directory -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force

Copy-Item -LiteralPath $slangProxy, $slangCompiler, $slangSpirvOptimizer, $dxcCompiler, $dxcValidator -Destination $Destination -Force
$module = $moduleDirectories[0]
Copy-Item -LiteralPath $module.FullName -Destination (Join-Path $Destination $module.Name) -Recurse -Force
Copy-Item -LiteralPath $slangLicense -Destination (Join-Path $Destination "Slang-LICENSE.txt") -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../Vendor/Slang/THIRD_PARTY_NOTICE.md") -Destination (Join-Path $Destination "Slang-THIRD_PARTY_NOTICE.md") -Force
Copy-Item -LiteralPath $dxcNotices[0] -Destination (Join-Path $Destination "DXC-LICENSE-LLVM.txt") -Force
Copy-Item -LiteralPath $dxcNotices[1] -Destination (Join-Path $Destination "DXC-LICENSE-MIT.txt") -Force
Copy-Item -LiteralPath $dxcNotices[2] -Destination (Join-Path $Destination "DXC-LICENSE-MS.txt") -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../Vendor/DXC/NOTICE.md") -Destination (Join-Path $Destination "DXC-NOTICE.md") -Force

$slangHash = (Get-Content -LiteralPath $slangManifest | Where-Object { $_ -like "sha256=*" }) -replace '^sha256=', ''
$dxcHash = (Get-Content -LiteralPath $dxcManifest | Where-Object { $_ -like "sha256=*" }) -replace '^sha256=', ''
$runtimeManifest = @(
    "format=1",
    "slang_sha256=$slangHash",
    "dxc_sha256=$dxcHash",
    "slang_runtime=slang.dll,slang-compiler.dll,slang-glslang.dll,$($module.Name)",
    "dxc_runtime=dxcompiler.dll,dxil.dll",
    "notices=Slang-LICENSE.txt,Slang-THIRD_PARTY_NOTICE.md,DXC-LICENSE-LLVM.txt,DXC-LICENSE-MIT.txt,DXC-LICENSE-MS.txt,DXC-NOTICE.md",
    "distribution_status=blocked-pending-slang-binary-notice-audit-and-dxc-license-terms"
)
Set-Content -LiteralPath (Join-Path $Destination "ShaderToolchainRuntimeManifest.txt") -Value $runtimeManifest -Encoding ascii
