param(
    [string]$Action = "gmake"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
& (Join-Path $PSScriptRoot "Setup.ps1")

$PremakeExe = Join-Path $Root "Vendor/premake/bin/premake5.exe"
$PremakeFile = Join-Path $Root "premake5.lua"
Write-Host "Generating projects with Premake action '$Action'..."
& $PremakeExe "--file=$PremakeFile" $Action

if ($LASTEXITCODE -ne 0) {
    throw "Premake project generation failed with exit code $LASTEXITCODE."
}
