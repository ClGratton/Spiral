param(
    [switch]$IncludeNVRHI,
    [switch]$IncludeNVRHIPlatformHeaders
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

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
        git clone $Url $Destination
    }
    elseif (!(Test-Path (Join-Path $Destination ".git"))) {
        Write-Host "$Name already exists as vendored source; skipping git sync."
        return
    }

    Push-Location $Destination
    try {
        git fetch --all --tags
        git checkout $Commit
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

Write-Host "Dependency fetch complete."
