param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [string]$Action = "vs2022",

    [ValidateRange(1, 600)]
    [int]$ChildTimeoutSeconds = 360
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "InvokeBoundedProcess.ps1")

$Suffix = if ($Action -like "vs*") { "" } else { "-$Action" }
$Executable = Join-Path $Root "bin/$Configuration-windows-x86_64$Suffix/EngineTests/EngineTests.exe"
if (!(Test-Path -LiteralPath $Executable)) {
    throw "EngineTests executable not found; build it before this script: $Executable"
}

$Temporary = Join-Path ([IO.Path]::GetTempPath()) "spiral-engine-test-runner-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $Temporary | Out-Null

function Invoke-Runner([string]$Label, [string[]]$Arguments, [int]$ExpectedExitCode = 0) {
    $Result = Invoke-BoundedProcess -FilePath $Executable -Arguments $Arguments -Label $Label -TimeoutSeconds $ChildTimeoutSeconds
    if ($Result.TimedOut) { throw "$Label timed out." }
    if ($Result.ExitCode -ne $ExpectedExitCode) {
        throw "$Label returned $($Result.ExitCode); expected $ExpectedExitCode."
    }
    return $Result
}

try {
    $List = Invoke-Runner "EngineTests list" @("--list")
    $Entries = @($List.Output | Where-Object { $_ -match '^(Fast|Integration)\t(.+)$' } | ForEach-Object {
        [pscustomobject]@{ Tier = $Matches[1]; Name = $Matches[2] }
    })
    if ($Entries.Count -eq 0) { throw "--list returned no tiered tests." }
    $FastEntries = @($Entries | Where-Object Tier -eq "Fast")
    $IntegrationEntries = @($Entries | Where-Object Tier -eq "Integration")
    if ($FastEntries.Count -eq 0 -or $IntegrationEntries.Count -eq 0) {
        throw "Both Fast and Integration metadata must be represented."
    }

    $ExactPath = Join-Path $Temporary "exact.json"
    [void](Invoke-Runner "EngineTests exact selection" @("--test", $FastEntries[0].Name, "--report-json", $ExactPath))
    $Exact = Get-Content -Raw -LiteralPath $ExactPath | ConvertFrom-Json
    if ($Exact.schema -ne 1 -or $Exact.selectedCount -ne 1 -or $Exact.failures -ne 0 -or
        $Exact.results[0].name -ne $FastEntries[0].Name -or $Exact.results[0].tier -ne "Fast") {
        throw "Exact selection JSON violated schema, count, order, tier, or status."
    }

    $FilterPath = Join-Path $Temporary "filter.json"
    [void](Invoke-Runner "EngineTests filter selection" @("--filter", "Frame timing capability group", "--report-json", $FilterPath))
    $Filter = Get-Content -Raw -LiteralPath $FilterPath | ConvertFrom-Json
    if ($Filter.selectedCount -lt 2 -or @($Filter.results | Where-Object { $_.name -notlike '*Frame timing capability group*' }).Count -ne 0) {
        throw "Filter selection did not return only the expected deterministic substring matches."
    }

    $FastPath = Join-Path $Temporary "fast.json"
    [void](Invoke-Runner "EngineTests fast tier" @("--tier", "fast", "--report-json", $FastPath))
    $Fast = Get-Content -Raw -LiteralPath $FastPath | ConvertFrom-Json
    if ($Fast.selection -ne "tier:fast" -or $Fast.selectedCount -ne $FastEntries.Count -or
        @($Fast.results | Where-Object tier -ne "Fast").Count -ne 0 -or $Fast.budgetMs -ne 60000) {
        throw "Fast tier selection, count, metadata, or default budget was incorrect."
    }

    $ReplayName = "Generated world-grid normalization preserves canonical reference results"
    $ReplayPath = Join-Path $Temporary "generated-replay.json"
    [void](Invoke-Runner "EngineTests generated replay" @("--test", $ReplayName, "--seed", "42", "--replay-trace", "0,0", "--report-json", $ReplayPath))
    $Replay = Get-Content -Raw -LiteralPath $ReplayPath | ConvertFrom-Json
    if ($Replay.seed -ne 42 -or $Replay.selectedCount -ne 1 -or $Replay.failures -ne 0 -or
        $Replay.results[0].name -ne $ReplayName) {
        throw "Generated replay did not preserve the requested seed, exact selection, and passing result."
    }

    $AllPath = Join-Path $Temporary "integration-default.json"
    [void](Invoke-Runner "EngineTests default integration tier" @("--report-json", $AllPath))
    $All = Get-Content -Raw -LiteralPath $AllPath | ConvertFrom-Json
    $ListedNames = @($Entries | ForEach-Object Name)
    $ReportedNames = @($All.results | ForEach-Object name)
    if ($All.selection -ne "tier:integration" -or $All.selectedCount -ne $Entries.Count -or
        $All.budgetMs -ne 300000 -or (Compare-Object $ListedNames $ReportedNames -SyncWindow 0)) {
        throw "Default integration selection did not retain the complete registry in stable order."
    }

    [void](Invoke-Runner "EngineTests zero selection rejection" @("--filter", "__no_such_spiral_test__") 2)
    [void](Invoke-Runner "EngineTests invalid tier rejection" @("--tier", "slow") 2)
    [void](Invoke-Runner "EngineTests conflicting selector rejection" @("--tier", "fast", "--test", $FastEntries[0].Name) 2)
    [void](Invoke-Runner "EngineTests unknown argument rejection" @("--unknown") 2)
    [void](Invoke-Runner "EngineTests replay without exact selection rejection" @("--tier", "fast", "--replay-trace", "0,0") 2)
    [void](Invoke-Runner "EngineTests invalid replay trace rejection" @("--test", $ReplayName, "--replay-trace", "0,,1") 2)

    Write-Host "EngineTestRunnerV1 result=pass total=$($Entries.Count) fast=$($FastEntries.Count) integration=$($IntegrationEntries.Count)"
} finally {
    Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction SilentlyContinue
}
