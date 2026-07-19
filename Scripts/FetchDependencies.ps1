param(
    [switch]$IncludeNVRHI,
    [switch]$IncludeNVRHIPlatformHeaders,
    [switch]$IncludeKtxSoftware
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Invoke-GitCommand {
    param([string[]]$Arguments)

    # Windows PowerShell surfaces Git's ordinary progress stream as error records.
    # Judge native success by the process exit code while retaining its output.
    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $Output = & git @Arguments 2>&1
        $ExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }

    $Output | ForEach-Object { Write-Host $_ }
    if ($ExitCode -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $ExitCode."
    }
}

function Sync-GitDependency {
    param(
        [string]$Name,
        [string]$Url,
        [string]$Commit,
        [string]$Path
    )

    $Destination = Join-Path $Root $Path
    if (!(Test-Path $Destination)) {
        Write-Host "Cloning $Name..."
        Invoke-GitCommand -Arguments @("clone", $Url, $Destination)
    }
    elseif (!(Test-Path (Join-Path $Destination ".git"))) {
        Write-Host "$Name already exists as vendored source; skipping git sync."
        return
    }

    Push-Location $Destination
    try {
        Invoke-GitCommand -Arguments @("fetch", "--all", "--tags")
        Invoke-GitCommand -Arguments @("checkout", $Commit)
    }
    finally {
        Pop-Location
    }
}

if ($IncludeNVRHI) {
    Sync-GitDependency `
        -Name "NVRHI" `
        -Url "https://github.com/NVIDIA-RTX/NVRHI.git" `
        -Commit "8e8c36e37558acec333204619b95d9d2fcdc4a79" `
        -Path "Vendor/NVRHI"
}

if ($IncludeNVRHIPlatformHeaders) {
    Sync-GitDependency `
        -Name "Vulkan-Headers" `
        -Url "https://github.com/KhronosGroup/Vulkan-Headers.git" `
        -Commit "v1.4.352" `
        -Path "Vendor/Vulkan-Headers"

    Sync-GitDependency `
        -Name "DirectX-Headers" `
        -Url "https://github.com/microsoft/DirectX-Headers.git" `
        -Commit "v1.717.0-preview" `
        -Path "Vendor/DirectX-Headers"
}

if ($IncludeKtxSoftware) {
    $KtxPath = Join-Path $Root "Vendor/KTX-Software"
    $KtxReady = Test-Path -LiteralPath (Join-Path $KtxPath ".git")
    if ($KtxReady) {
        $KtxReady = (& git -C $KtxPath rev-parse HEAD).Trim() -eq "4d6fc70eaf62ad0558e63e8d97eb9766118327a6" `
            -and @(& git -C $KtxPath status --porcelain).Count -eq 0
        foreach ($Notice in @("LICENSE.md", "LICENSES", "NOTICE.md")) {
            $KtxReady = $KtxReady -and (Test-Path -LiteralPath (Join-Path $KtxPath $Notice))
        }
    }
    if (!$KtxReady) {
        Sync-GitDependency `
            -Name "KTX-Software" `
            -Url "https://github.com/KhronosGroup/KTX-Software.git" `
            -Commit "4d6fc70eaf62ad0558e63e8d97eb9766118327a6" `
            -Path "Vendor/KTX-Software"
    }

    $KtxCommit = (& git -C $KtxPath rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $KtxCommit -ne "4d6fc70eaf62ad0558e63e8d97eb9766118327a6") {
        throw "KTX-Software did not resolve to the admitted source commit."
    }
    if (@(& git -C $KtxPath status --porcelain).Count -ne 0) {
        throw "KTX-Software checkout contains local modifications."
    }
    foreach ($Notice in @("LICENSE.md", "LICENSES", "NOTICE.md")) {
        if (!(Test-Path -LiteralPath (Join-Path $KtxPath $Notice))) {
            throw "KTX-Software source is missing required notice path: $Notice"
        }
    }
}

Write-Host "Dependency fetch complete."
