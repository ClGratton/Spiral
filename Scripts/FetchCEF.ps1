param(
    [ValidateSet("windows", "linux", "macos")]
    [string]$HostPlatform,
    [ValidateSet("x86_64", "aarch64")]
    [string]$HostArchitecture,
    [switch]$Force,
    [switch]$KeepArchive
)

$ErrorActionPreference = "Stop"

$PinFile = Join-Path $PSScriptRoot "BrowserRuntimePins.env"
$Pins = @{}
foreach ($line in Get-Content -LiteralPath $PinFile) {
    if ($line -match '^([A-Z0-9_]+)=(.+)$') { $Pins[$Matches[1]] = $Matches[2] }
    elseif (![string]::IsNullOrWhiteSpace($line) -and !$line.StartsWith('#')) { throw "Invalid browser runtime pin line: '$line'." }
}
if ($Pins.BROWSER_RUNTIME_PIN_FORMAT -ne "1") { throw "Unsupported browser runtime pin format." }
$CefVersion = $Pins.CEF_VERSION
$CefBuild = $Pins.CEF_BUILD
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$CefRoot = Join-Path $Root "Vendor/CEF/v$CefVersion"
$CacheRoot = Join-Path $Root "Vendor/CEF/.cache"

function Get-DetectedPlatform {
    $isWindowsHost = (Get-Variable IsWindows -ValueOnly -ErrorAction SilentlyContinue) -eq $true
    $isLinuxHost = (Get-Variable IsLinux -ValueOnly -ErrorAction SilentlyContinue) -eq $true
    $isMacOSHost = (Get-Variable IsMacOS -ValueOnly -ErrorAction SilentlyContinue) -eq $true
    if ($isWindowsHost -or $env:OS -eq "Windows_NT") { return "windows" }
    if ($isLinuxHost) { return "linux" }
    if ($isMacOSHost) { return "macos" }
    throw "Unsupported host operating system. Specify a supported CEF host package explicitly."
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
if ($packageKey -ne "windows-x86_64") {
    throw "FetchCEF.ps1 installs only the windows-x86_64 CEF package; use Scripts/FetchCEF.sh for the Linux package. No other package is pinned."
}
$pinPrefix = "CEF_$($packageKey.Replace('-', '_').ToUpperInvariant())"
$archiveName = $Pins["${pinPrefix}_ARCHIVE"]
$expectedSha256 = $Pins["${pinPrefix}_SHA256"]
$archiveSize = [int64]$Pins["${pinPrefix}_SIZE"]
if ([string]::IsNullOrWhiteSpace($archiveName) -or [string]::IsNullOrWhiteSpace($expectedSha256)) {
    throw "No pinned CEF package is declared for '$packageKey'."
}
$destination = Join-Path $CefRoot $packageKey
$archive = Join-Path $CacheRoot $archiveName
$manifestName = ".spiral-package-manifest"
$expectedManifest = @(
    "format=1",
    "name=CEF",
    "version=$CefVersion",
    "build=$CefBuild",
    "package=$packageKey",
    "archive=$archiveName",
    "sha256=$expectedSha256"
)

function Test-CefPackage {
    param([string]$Path)
    foreach ($required in @(
            "include/cef_version.h", "include/cef_app.h", "LICENSE.txt", "CREDITS.html",
            "Release/libcef.dll", "Release/libcef.lib", "Release/v8_context_snapshot.bin",
            "Resources/icudtl.dat", "Resources/resources.pak", "Resources/locales/en-US.pak")) {
        if (!(Test-Path (Join-Path $Path $required) -PathType Leaf)) { return $false }
    }
    if (!(Test-Path (Join-Path $Path "libcef_dll") -PathType Container)) { return $false }

    $version = Select-String -LiteralPath (Join-Path $Path "include/cef_version.h") -Pattern '^#define CEF_VERSION "(.+)"\s*$' | Select-Object -First 1
    if ($null -eq $version -or $version.Matches[0].Groups[1].Value -cne $CefBuild) { return $false }

    $manifest = Join-Path $Path $manifestName
    if (!(Test-Path $manifest -PathType Leaf)) { return $false }
    $actualManifest = @(Get-Content -LiteralPath $manifest)
    if ($actualManifest.Count -ne $expectedManifest.Count) { return $false }
    for ($index = 0; $index -lt $expectedManifest.Count; ++$index) {
        if ($actualManifest[$index] -cne $expectedManifest[$index]) { return $false }
    }
    return $true
}

if ((Test-CefPackage $destination) -and !$Force) {
    Write-Host "Pinned CEF $CefVersion is already staged at $destination"
    exit 0
}

New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
if (!(Test-Path $archive) -or $Force) {
    Write-Host "Downloading pinned CEF $CefVersion package for $packageKey (about $([int]($archiveSize / 1000000)) MB)..."
    $partial = "$archive.part-$([guid]::NewGuid().ToString('N'))"
    try {
        # The CDN names contain '+', which must be percent-encoded in the URL path.
        Invoke-WebRequest -Uri "$($Pins.CEF_BASE_URL)/$($archiveName.Replace('+', '%2B'))" -OutFile $partial
        Move-Item -LiteralPath $partial -Destination $archive -Force
    }
    catch {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        throw
    }
}

$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedSha256) {
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    throw "CEF package hash mismatch for $archiveName. Expected $expectedSha256, got $actualHash. The archive was removed."
}

# Extract beside the destination so the final move is a rename on one volume. A short
# directory name and --strip-components keep the deepest members under MAX_PATH.
$temporary = Join-Path $CacheRoot "x$([guid]::NewGuid().ToString('N').Substring(0, 8))"
try {
    New-Item -ItemType Directory -Path $temporary | Out-Null
    . (Join-Path $PSScriptRoot "ArchiveSafety.ps1")
    Assert-SafeTarArchive -Archive $archive -CompressionFlag "j"
    & tar -xjf $archive --strip-components 1 -C $temporary
    if ($LASTEXITCODE -ne 0) { throw "tar extraction failed for $archiveName." }

    $candidates = @($temporary) + @(Get-ChildItem -LiteralPath $temporary -Directory -Recurse | Select-Object -ExpandProperty FullName)
    $packageRoot = @($candidates | Where-Object { Test-Path (Join-Path $_ "include/cef_version.h") })
    if ($packageRoot.Count -ne 1) { throw "Expected exactly one CEF package root with include/cef_version.h, found $($packageRoot.Count)." }
    foreach ($notice in @("LICENSE.txt", "CREDITS.html")) {
        if (!(Test-Path (Join-Path $packageRoot[0] $notice) -PathType Leaf)) { throw "Pinned CEF package is missing its required $notice notice." }
    }

    $staging = "$destination.staging-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    New-Item -ItemType Directory -Path $staging | Out-Null
    Get-ChildItem -LiteralPath $packageRoot[0] -Force | Move-Item -Destination $staging
    Set-Content -LiteralPath (Join-Path $staging $manifestName) -Value $expectedManifest -Encoding ascii

    if (!(Test-CefPackage $staging)) { throw "Pinned CEF package is missing a required header, library, resource, or notice." }

    if (Test-Path $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    Move-Item -LiteralPath $staging -Destination $destination
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}

if (!(Test-CefPackage $destination)) { throw "CEF staging verification failed at $destination." }
if (!$KeepArchive) { Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue }
Write-Host "Pinned CEF $CefVersion staged at $destination"
