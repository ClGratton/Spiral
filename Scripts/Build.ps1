param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [string]$Action = ""
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Find-CommandPath($Name) {
    $Command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($Command) {
        return $Command.Source
    }

    return $null
}

function Find-MSBuildPath {
    $MSBuild = Find-CommandPath "msbuild"
    if ($MSBuild) {
        return $MSBuild
    }

    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $Resolved = & $VsWhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\Current\Bin\amd64\MSBuild.exe" | Select-Object -First 1
        if ($Resolved -and (Test-Path $Resolved)) {
            return $Resolved
        }
    }

    $Candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )

    foreach ($Candidate in $Candidates) {
        if (Test-Path $Candidate) {
            return $Candidate
        }
    }

    return $null
}

if ([string]::IsNullOrWhiteSpace($Action)) {
    if (Find-MSBuildPath) {
        $Action = "vs2022"
    } else {
        $Action = "gmake"
    }
}

& (Join-Path $PSScriptRoot "GenerateProjects.ps1") -Action $Action

if ($Action -like "vs*") {
    $Solution = Join-Path $Root "Spiral.sln"
    $MSBuild = Find-MSBuildPath
    if (!$MSBuild) {
        throw "MSBuild was not found on PATH. Install Visual Studio Build Tools or use -Action gmake."
    }

    & $MSBuild $Solution /m /p:Configuration=$Configuration /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
} elseif ($Action -eq "gmake" -or $Action -eq "gmake2") {
    $Make = Find-CommandPath "mingw32-make"
    if (!$Make) {
        $Make = Find-CommandPath "make"
    }

    if (!$Make) {
        throw "No make executable found. Install MSYS2 mingw32-make, GNU make, or use Visual Studio Build Tools."
    }

    $ConfigName = $Configuration.ToLowerInvariant()
    & $Make -C $Root "config=$ConfigName"
    if ($LASTEXITCODE -ne 0) {
        throw "Make failed with exit code $LASTEXITCODE."
    }
} else {
    throw "Unsupported build action '$Action'. Try gmake or vs2022."
}

Write-Host "Build complete: $Configuration ($Action)"
