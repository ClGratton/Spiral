param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [string]$Action = ""
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
& (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration -Action $Action

$Candidates = @(
    "bin/$Configuration-windows-x86_64/Sandbox/Sandbox.exe",
    "bin/$Configuration-windows-x86_64-gmake/Sandbox/Sandbox.exe",
    "bin/$Configuration-windows-x86_64-gmake2/Sandbox/Sandbox.exe"
)

if (![string]::IsNullOrWhiteSpace($Action) -and $Action -ne "vs2022") {
    $Candidates = @("bin/$Configuration-windows-x86_64-$Action/Sandbox/Sandbox.exe") + $Candidates
}

$Executable = $null
foreach ($Candidate in $Candidates) {
    $Path = Join-Path $Root $Candidate
    if (Test-Path $Path) {
        $Executable = $Path
        break
    }
}

if (!$Executable) {
    throw "Sandbox executable was not found in expected build output folders."
}

& $Executable
