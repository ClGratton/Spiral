param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [string]$Action = "vs2022",

    [string]$CapturePath = "output/captures/editor-viewport.bmp",

    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$ResolvedCapturePath = Join-Path $Root $CapturePath

if ($Action -notlike "vs*") {
    throw "Render smoke capture currently requires the Visual Studio/MSVC D3D12 build path. Use -Action vs2022."
}

if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration -Action $Action
}

$Editor = Join-Path $Root "bin/$Configuration-windows-x86_64/Editor/Editor.exe"
if (!(Test-Path $Editor)) {
    throw "Editor executable not found: $Editor"
}

if (Test-Path $ResolvedCapturePath) {
    Remove-Item -LiteralPath $ResolvedCapturePath
}

& $Editor --capture-viewport --smoke-test
if ($LASTEXITCODE -ne 0) {
    throw "Editor render smoke run failed with exit code $LASTEXITCODE."
}

if (!(Test-Path $ResolvedCapturePath)) {
    throw "Viewport capture was not produced: $ResolvedCapturePath"
}

$Bytes = [System.IO.File]::ReadAllBytes($ResolvedCapturePath)
if ($Bytes.Length -lt 54) {
    throw "Viewport capture is too small to be a BMP: $($Bytes.Length) bytes"
}

$Signature = "$([char]$Bytes[0])$([char]$Bytes[1])"
$PixelOffset = [BitConverter]::ToInt32($Bytes, 10)
$Width = [BitConverter]::ToInt32($Bytes, 18)
$HeightRaw = [BitConverter]::ToInt32($Bytes, 22)
$BitsPerPixel = [BitConverter]::ToUInt16($Bytes, 28)
$Height = [Math]::Abs($HeightRaw)

if ($Signature -ne "BM") {
    throw "Viewport capture is not a BMP file. Signature: $Signature"
}

if ($Width -lt 64 -or $Height -lt 64) {
    throw "Viewport capture dimensions are unexpectedly small: ${Width}x${HeightRaw}"
}

if ($BitsPerPixel -ne 32) {
    throw "Viewport capture must be 32-bit BGRA. Found $BitsPerPixel bpp."
}

$ExpectedBytes = $PixelOffset + ($Width * $Height * 4)
if ($Bytes.Length -lt $ExpectedBytes) {
    throw "Viewport capture is truncated. Expected at least $ExpectedBytes bytes, found $($Bytes.Length)."
}

$BackgroundB = $Bytes[$PixelOffset + 0]
$BackgroundG = $Bytes[$PixelOffset + 1]
$BackgroundR = $Bytes[$PixelOffset + 2]
$DifferentPixels = 0
$UniqueColors = New-Object 'System.Collections.Generic.HashSet[int]'

for ($Pixel = 0; $Pixel -lt ($Width * $Height); ++$Pixel) {
    $Index = $PixelOffset + ($Pixel * 4)
    $B = $Bytes[$Index + 0]
    $G = $Bytes[$Index + 1]
    $R = $Bytes[$Index + 2]
    $A = $Bytes[$Index + 3]
    [void]$UniqueColors.Add(($A -shl 24) -bor ($R -shl 16) -bor ($G -shl 8) -bor $B)

    $Delta = [Math]::Abs([int]$B - [int]$BackgroundB) + [Math]::Abs([int]$G - [int]$BackgroundG) + [Math]::Abs([int]$R - [int]$BackgroundR)
    if ($Delta -gt 12) {
        ++$DifferentPixels
    }
}

$DifferentRatio = $DifferentPixels / [double]($Width * $Height)
if ($UniqueColors.Count -lt 4 -or $DifferentRatio -lt 0.01) {
    throw "Viewport capture appears blank. UniqueColors=$($UniqueColors.Count), DifferentRatio=$('{0:P2}' -f $DifferentRatio)."
}

Write-Host "Render smoke passed: $ResolvedCapturePath"
Write-Host "Dimensions: ${Width}x${HeightRaw}, UniqueColors=$($UniqueColors.Count), DifferentRatio=$('{0:P2}' -f $DifferentRatio)"
