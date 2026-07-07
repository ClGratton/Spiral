param(
    [string]$PremakeVersion = "5.0.0-beta8"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$PremakeDir = Join-Path $Root "Vendor/premake/bin"
$PremakeExe = Join-Path $PremakeDir "premake5.exe"

New-Item -ItemType Directory -Force -Path $PremakeDir | Out-Null

if (!(Test-Path $PremakeExe)) {
    $Archive = Join-Path $PremakeDir "premake-$PremakeVersion-windows.zip"
    $Url = "https://github.com/premake/premake-core/releases/download/v$PremakeVersion/premake-$PremakeVersion-windows.zip"

    Write-Host "Downloading Premake $PremakeVersion..."
    Invoke-WebRequest -Uri $Url -OutFile $Archive

    Write-Host "Extracting Premake..."
    Expand-Archive -Path $Archive -DestinationPath $PremakeDir -Force
}

if (!(Test-Path $PremakeExe)) {
    throw "Premake executable was not found after setup: $PremakeExe"
}

Write-Host "Setup complete."
Write-Host "Premake: $PremakeExe"
