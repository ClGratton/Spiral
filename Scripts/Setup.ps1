param(
    [string]$PremakeVersion = "5.0.0-beta8"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$PremakeDir = Join-Path $Root "Vendor/premake/bin"
$PremakeExe = Join-Path $PremakeDir "premake5.exe"
$Architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
if ($Architecture -ne "X64") {
    throw "This workspace currently generates x86_64 projects only; host architecture '$Architecture' is unsupported. FetchSlang.ps1 may be used separately to audit a pinned ARM64 package, but Setup will not fetch an unusable build toolchain."
}

New-Item -ItemType Directory -Force -Path $PremakeDir | Out-Null

if (!(Test-Path $PremakeExe)) {
    $Archive = Join-Path $PremakeDir "premake-$PremakeVersion-windows.zip"
    $Url = "https://github.com/premake/premake-core/releases/download/v$PremakeVersion/premake-$PremakeVersion-windows.zip"

    Write-Host "Downloading Premake $PremakeVersion..."
    Invoke-WebRequest -Uri $Url -OutFile $Archive

    Write-Host "Extracting Premake..."
    Expand-Archive -Path $Archive -DestinationPath $PremakeDir -Force
}

# PowerShell child scripts propagate terminating errors directly. LASTEXITCODE
# belongs to native commands and may be unset or stale in a clean session.
& (Join-Path $PSScriptRoot "FetchSlang.ps1")
& (Join-Path $PSScriptRoot "FetchDXC.ps1")
& (Join-Path $PSScriptRoot "FetchDependencies.ps1") -IncludeKtxSoftware

if (!(Test-Path $PremakeExe)) {
    throw "Premake executable was not found after setup: $PremakeExe"
}

Write-Host "Setup complete."
Write-Host "Premake: $PremakeExe"
