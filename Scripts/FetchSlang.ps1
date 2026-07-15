param(
    [ValidateSet("windows", "linux", "macos")]
    [string]$HostPlatform,
    [ValidateSet("x86_64", "aarch64")]
    [string]$HostArchitecture,
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
$SlangVersion = $Pins.SLANG_VERSION
$SlangReleaseUrl = "https://github.com/shader-slang/slang/releases/download/v$SlangVersion"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$SlangRoot = Join-Path $Root "Vendor/Slang/v$SlangVersion"
$CacheRoot = Join-Path $Root "Vendor/Slang/.cache"

function Get-DetectedPlatform {
    $isWindowsHost = (Get-Variable IsWindows -ValueOnly -ErrorAction SilentlyContinue) -eq $true
    $isLinuxHost = (Get-Variable IsLinux -ValueOnly -ErrorAction SilentlyContinue) -eq $true
    $isMacOSHost = (Get-Variable IsMacOS -ValueOnly -ErrorAction SilentlyContinue) -eq $true
    if ($isWindowsHost -or $env:OS -eq "Windows_NT") { return "windows" }
    if ($isLinuxHost) { return "linux" }
    if ($isMacOSHost) { return "macos" }
    throw "Unsupported host operating system. Specify a supported Slang host package explicitly."
}

function Get-DetectedArchitecture {
    $architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    switch ($architecture) {
        "x64" { return "x86_64" }
        "arm64" { return "aarch64" }
        default { throw "Unsupported host architecture '$architecture'." }
    }
}

if ([string]::IsNullOrWhiteSpace($HostPlatform)) { $HostPlatform = Get-DetectedPlatform }
if ([string]::IsNullOrWhiteSpace($HostArchitecture)) { $HostArchitecture = Get-DetectedArchitecture }

$packageKey = "$HostPlatform-$HostArchitecture"
$pinPrefix = "SLANG_$($packageKey.Replace('-', '_').ToUpperInvariant())"
$archiveName = $Pins["${pinPrefix}_ARCHIVE"]
$expectedSha256 = $Pins["${pinPrefix}_SHA256"]
if ([string]::IsNullOrWhiteSpace($archiveName) -or [string]::IsNullOrWhiteSpace($expectedSha256)) {
    throw "No pinned Slang package is declared for '$packageKey'."
}
$destination = Join-Path $SlangRoot $packageKey
$archive = Join-Path $CacheRoot $archiveName
$manifestName = ".spiral-package-manifest"
$expectedManifest = @(
    "format=1",
    "name=Slang",
    "version=$SlangVersion",
    "package=$packageKey",
    "archive=$archiveName",
    "sha256=$expectedSha256"
)

function Test-SlangPackage {
    param([string]$Path)
    if (!(Test-Path (Join-Path $Path "include/slang.h") -PathType Leaf) -or
        !(Test-Path (Join-Path $Path "LICENSE") -PathType Leaf)) { return $false }

    $manifest = Join-Path $Path $manifestName
    if (!(Test-Path $manifest -PathType Leaf)) { return $false }
    $actualManifest = @(Get-Content -LiteralPath $manifest)
    if ($actualManifest.Count -ne $expectedManifest.Count) { return $false }
    for ($index = 0; $index -lt $expectedManifest.Count; ++$index) {
        if ($actualManifest[$index] -cne $expectedManifest[$index]) { return $false }
    }

    $moduleDirectory = if ($HostPlatform -eq "windows") { "bin/slang-standard-module-$SlangVersion" } else { "lib/slang-standard-module-$SlangVersion" }
    if (!(Test-Path (Join-Path $Path "$moduleDirectory/experimental/workgraph.slang-module") -PathType Leaf) -or
        !(Test-Path (Join-Path $Path "$moduleDirectory/slang/neural.slang-module") -PathType Leaf)) { return $false }
    if ($HostPlatform -eq "windows") {
        return (Test-Path (Join-Path $Path "lib/slang.lib") -PathType Leaf) -and
            (Test-Path (Join-Path $Path "bin/slang.dll") -PathType Leaf) -and
            (Test-Path (Join-Path $Path "bin/slang-compiler.dll") -PathType Leaf) -and
            (Test-Path (Join-Path $Path "bin/slang-glslang.dll") -PathType Leaf)
    }
    if ($HostPlatform -eq "linux") {
        return (Test-Path (Join-Path $Path "lib/libslang.so") -PathType Leaf) -and
            (Test-Path (Join-Path $Path "lib/libslang-compiler.so") -PathType Leaf) -and
            @(Get-ChildItem -LiteralPath (Join-Path $Path "lib") -Filter "libslang-glslang*.so*" -File).Count -gt 0
    }
    return (Test-Path (Join-Path $Path "lib/libslang.dylib") -PathType Leaf) -and
        (Test-Path (Join-Path $Path "lib/libslang-compiler.dylib") -PathType Leaf) -and
        @(Get-ChildItem -LiteralPath (Join-Path $Path "lib") -Filter "libslang-glslang*.dylib" -File).Count -gt 0
}

if ((Test-SlangPackage $destination) -and !$Force) {
    Write-Host "Pinned Slang $SlangVersion is already staged at $destination"
    exit 0
}

New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
if (!(Test-Path $archive) -or $Force) {
    Write-Host "Downloading pinned Slang $SlangVersion package for $packageKey..."
    try {
        Invoke-WebRequest -Uri "$SlangReleaseUrl/$archiveName" -OutFile $archive
    }
    catch {
        Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
        throw
    }
}

$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedSha256) {
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    throw "Slang package hash mismatch for $archiveName. Expected $expectedSha256, got $actualHash. The archive was removed."
}

$temporary = Join-Path ([System.IO.Path]::GetTempPath()) "spiral-slang-$SlangVersion-$packageKey-$([guid]::NewGuid().ToString('N'))"
try {
    New-Item -ItemType Directory -Path $temporary | Out-Null
    . (Join-Path $PSScriptRoot "ArchiveSafety.ps1")
    if ($archiveName.EndsWith(".zip")) {
        Assert-SafeZipArchive $archive
        Expand-Archive -LiteralPath $archive -DestinationPath $temporary -Force
    }
    else {
        Assert-SafeTarArchive $archive
        & tar -xzf $archive -C $temporary
        if ($LASTEXITCODE -ne 0) { throw "tar extraction failed for $archiveName." }
    }

    $candidates = @($temporary) + @(Get-ChildItem -LiteralPath $temporary -Directory -Recurse | Select-Object -ExpandProperty FullName)
    $packageRoot = @($candidates | Where-Object { Test-Path (Join-Path $_ "include/slang.h") })
    if ($packageRoot.Count -ne 1) { throw "Expected exactly one Slang package root with include/slang.h, found $($packageRoot.Count)." }
    if (!(Test-Path (Join-Path $packageRoot[0] "LICENSE") -PathType Leaf)) { throw "Pinned Slang package is missing its required LICENSE notice." }

    $staging = "$destination.staging-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $staging | Out-Null
    Get-ChildItem -LiteralPath $packageRoot[0] -Force | Move-Item -Destination $staging
    Set-Content -LiteralPath (Join-Path $staging $manifestName) -Value $expectedManifest -Encoding ascii

    if (!(Test-SlangPackage $staging)) { throw "Pinned Slang package is missing a required header, import library, runtime library, or standard module." }

    if (Test-Path $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    Move-Item -LiteralPath $staging -Destination $destination
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}

if (!(Test-SlangPackage $destination)) { throw "Slang staging verification failed at $destination." }
Write-Host "Pinned Slang $SlangVersion staged at $destination"
