param(
    [ValidateSet("windows")]
    [string]$HostPlatform = "windows",
    [ValidateSet("x86_64")]
    [string]$HostArchitecture = "x86_64",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$PinFile = Join-Path $PSScriptRoot "ShaderToolchainPins.env"
$Pins = @{}
foreach ($line in Get-Content -LiteralPath $PinFile) {
    if ($line -match '^([A-Z0-9_]+)=(.+)$') { $Pins[$Matches[1]] = $Matches[2] }
    elseif (![string]::IsNullOrWhiteSpace($line) -and !$line.StartsWith('#')) { throw "Invalid shader toolchain pin line: '$line'." }
}
if ($Pins.SHADER_TOOLCHAIN_PIN_FORMAT -ne "1") { throw "Unsupported shader toolchain pin format." }
$DxcVersion = $Pins.DXC_VERSION
$ArchiveName = $Pins.DXC_WINDOWS_X86_64_ARCHIVE
$ExpectedSha256 = $Pins.DXC_WINDOWS_X86_64_SHA256
$ReleaseUrl = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v$DxcVersion/$ArchiveName"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$DxcRoot = Join-Path $Root "Vendor/DXC/v$DxcVersion"
$CacheRoot = Join-Path $Root "Vendor/DXC/.cache"
$Destination = Join-Path $DxcRoot "$HostPlatform-$HostArchitecture"
$Archive = Join-Path $CacheRoot $ArchiveName
$ManifestName = ".spiral-package-manifest"
$ExpectedManifest = @(
    "format=1",
    "name=DXC",
    "version=$DxcVersion",
    "package=windows-x86_64",
    "archive=$ArchiveName",
    "sha256=$ExpectedSha256"
)

if ($env:OS -ne "Windows_NT") {
    throw "The admitted DXC package is currently Windows x86_64 only. Other hosts remain unqualified."
}
if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() -ne "X64") {
    throw "The admitted DXC package is currently Windows x86_64 only; this host architecture is unsupported."
}

function Test-DxcPayload {
    param([string]$Path)
    foreach ($required in @("bin/x64/dxcompiler.dll", "bin/x64/dxil.dll", "LICENSE-LLVM.txt", "LICENSE-MIT.txt", "LICENSE-MS.txt")) {
        if (!(Test-Path (Join-Path $Path $required) -PathType Leaf)) { return $false }
    }
    return $true
}

function Test-DxcPackage {
    param([string]$Path)
    if (!(Test-DxcPayload $Path)) { return $false }
    $manifest = Join-Path $Path $ManifestName
    if (!(Test-Path $manifest -PathType Leaf)) { return $false }
    $actualManifest = @(Get-Content -LiteralPath $manifest)
    if ($actualManifest.Count -ne $ExpectedManifest.Count) { return $false }
    for ($index = 0; $index -lt $ExpectedManifest.Count; ++$index) {
        if ($actualManifest[$index] -cne $ExpectedManifest[$index]) { return $false }
    }
    return $true
}

if ((Test-DxcPackage $Destination) -and !$Force) {
    Write-Host "Pinned DXC v$DxcVersion is already staged at $Destination"
    exit 0
}

New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
if (!(Test-Path $Archive) -or $Force) {
    Write-Host "Downloading pinned DXC v$DxcVersion package for Windows x86_64..."
    try {
        Invoke-WebRequest -Uri $ReleaseUrl -OutFile $Archive
    }
    catch {
        Remove-Item -LiteralPath $Archive -Force -ErrorAction SilentlyContinue
        throw
    }
}

$ActualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash.ToLowerInvariant()
if ($ActualHash -ne $ExpectedSha256) {
    Remove-Item -LiteralPath $Archive -Force -ErrorAction SilentlyContinue
    throw "DXC package hash mismatch for $ArchiveName. Expected $ExpectedSha256, got $ActualHash. The archive was removed."
}

$Temporary = Join-Path ([System.IO.Path]::GetTempPath()) "spiral-dxc-$DxcVersion-windows-x86_64-$([guid]::NewGuid().ToString('N'))"
try {
    New-Item -ItemType Directory -Path $Temporary | Out-Null
    . (Join-Path $PSScriptRoot "ArchiveSafety.ps1")
    Assert-SafeZipArchive $Archive
    Expand-Archive -LiteralPath $Archive -DestinationPath $Temporary -Force

    $Candidates = @($Temporary) + @(Get-ChildItem -LiteralPath $Temporary -Directory -Recurse | Select-Object -ExpandProperty FullName)
    $PackageRoot = @($Candidates | Where-Object { Test-Path (Join-Path $_ "bin/x64/dxcompiler.dll") })
    if ($PackageRoot.Count -ne 1) { throw "Expected exactly one DXC package root, found $($PackageRoot.Count)." }
    if (!(Test-DxcPayload $PackageRoot[0])) { throw "Pinned DXC package is missing compiler, validator, or its LLVM/MIT/Microsoft license notices." }

    $Staging = "$Destination.staging-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $Staging | Out-Null
    Get-ChildItem -LiteralPath $PackageRoot[0] -Force | Move-Item -Destination $Staging
    Set-Content -LiteralPath (Join-Path $Staging $ManifestName) -Value $ExpectedManifest -Encoding ascii

    if (!(Test-DxcPackage $Staging)) { throw "Pinned DXC package failed installed-manifest validation." }

    if (Test-Path $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    Move-Item -LiteralPath $Staging -Destination $Destination
}
finally {
    Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction SilentlyContinue
}

if (!(Test-DxcPackage $Destination)) { throw "DXC staging verification failed at $Destination." }
Write-Host "Pinned DXC v$DxcVersion staged at $Destination"
