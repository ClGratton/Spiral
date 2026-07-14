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
$SceneOriginCapturePaths = @(
    (Join-Path $Root "output/captures/scene-origin-a.bmp"),
    (Join-Path $Root "output/captures/scene-origin-b.bmp"),
    (Join-Path $Root "output/captures/scene-origin-c.bmp")
)

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

foreach ($Path in @($ResolvedCapturePath) + $SceneOriginCapturePaths) {
    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path
    }
}

$Output = & $Editor --capture-viewport --smoke-test --renderer-capability-smoke --scene-origin-raster-smoke 2>&1 | Tee-Object -Variable RenderLog
if ($LASTEXITCODE -ne 0) {
    throw "Editor render smoke run failed with exit code $LASTEXITCODE."
}

$RequiredMarkers = @(
    "NVRHI D3D12 device created on adapter:",
    "Selected D3D12 adapter for profile 'Phase 3 D3D12 Bootstrap V1':",
    "D3D12 capability state: Ray Tracing advertised=",
    "D3D12 capability state: Timestamps advertised=yes, enabled=no, implemented=no, exercised=no",
    "Editor renderer capability diagnostics ready: profile=Phase 3 D3D12 Bootstrap V1, qualification=Bootstrap",
    "Renderer capability group: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=no",
    "Editor renderer capability group exercised: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=yes, qualification=Presentation, deviceQualification=Bootstrap",
    "Renderer initialized with backend: NVRHI D3D12",
    "D3D12 scene origin raster case A:",
    "D3D12 scene origin raster case B:",
    "D3D12 scene origin raster case C:",
    "D3D12 scene origin raster smoke passed"
)
$JoinedLog = $RenderLog -join "`n"
foreach ($Marker in $RequiredMarkers) {
    if (!$JoinedLog.Contains($Marker)) {
        throw "D3D12 render smoke did not emit required marker: $Marker"
    }
}
$CanonicalOriginPatterns = @(
    "D3D12 scene origin raster case A:.*sectorX=244140625, localX=2047\.5, originSectorX=244140625, originLocalX=2047\.5,",
    "D3D12 scene origin raster case B:.*sectorX=244140626, localX=-2047\.5, originSectorX=244140625, originLocalX=2047\.5,",
    "D3D12 scene origin raster case C:.*sectorX=244140626, localX=-2047\.5, originSectorX=244140626, originLocalX=-2047\.5,"
)
foreach ($Pattern in $CanonicalOriginPatterns) {
    if ($JoinedLog -notmatch $Pattern) {
        throw "D3D12 scene-origin diagnostics did not prove the expected canonical mesh/origin boundary transition: $Pattern"
    }
}
$DiagnosticsPattern = "Editor renderer capability diagnostics rendered: profile=Phase 3 D3D12 Bootstrap V1, adapter=.+, qualification=Bootstrap, formats=[1-9][0-9]*, features=7, groups=1, candidates=[1-9][0-9]*"
if ($JoinedLog -notmatch $DiagnosticsPattern) {
    throw "D3D12 render smoke did not emit a complete editor capability diagnostics marker."
}

function Get-BmpMetrics([string]$Path) {
    if (!(Test-Path $Path)) {
        throw "Viewport capture was not produced: $Path"
    }

    $Bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($Bytes.Length -lt 54) {
        throw "Viewport capture is too small to be a BMP: $($Bytes.Length) bytes"
    }

    $Signature = "$([char]$Bytes[0])$([char]$Bytes[1])"
    $PixelOffset = [BitConverter]::ToInt32($Bytes, 10)
    $Width = [BitConverter]::ToInt32($Bytes, 18)
    $HeightRaw = [BitConverter]::ToInt32($Bytes, 22)
    $BitsPerPixel = [BitConverter]::ToUInt16($Bytes, 28)
    $Height = [Math]::Abs($HeightRaw)
    if ($Signature -ne "BM" -or $Width -lt 64 -or $Height -lt 64 -or $BitsPerPixel -ne 32) {
        throw "Invalid viewport BMP '$Path': signature=$Signature, size=${Width}x${HeightRaw}, bpp=$BitsPerPixel"
    }

    $ExpectedBytes = $PixelOffset + ($Width * $Height * 4)
    if ($Bytes.Length -lt $ExpectedBytes) {
        throw "Viewport capture is truncated: $Path"
    }

    $BorderColors = @{}
    foreach ($Y in @(0, ($Height - 1))) {
        for ($X = 0; $X -lt $Width; ++$X) {
            $Index = $PixelOffset + ((($Y * $Width) + $X) * 4)
            $Key = ([int]$Bytes[$Index + 2] -shl 16) -bor ([int]$Bytes[$Index + 1] -shl 8) -bor [int]$Bytes[$Index + 0]
            $BorderColors[$Key] = 1 + [int]$BorderColors[$Key]
        }
    }
    for ($Y = 1; $Y -lt ($Height - 1); ++$Y) {
        foreach ($X in @(0, ($Width - 1))) {
            $Index = $PixelOffset + ((($Y * $Width) + $X) * 4)
            $Key = ([int]$Bytes[$Index + 2] -shl 16) -bor ([int]$Bytes[$Index + 1] -shl 8) -bor [int]$Bytes[$Index + 0]
            $BorderColors[$Key] = 1 + [int]$BorderColors[$Key]
        }
    }
    $BackgroundKey = [int](($BorderColors.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key)
    $BackgroundB = $BackgroundKey -band 255
    $BackgroundG = ($BackgroundKey -shr 8) -band 255
    $BackgroundR = ($BackgroundKey -shr 16) -band 255
    $DifferentPixels = 0
    $SumX = 0.0
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
            $SumX += ($Pixel % $Width)
        }
    }

    $DifferentRatio = $DifferentPixels / [double]($Width * $Height)
    if ($UniqueColors.Count -lt 2 -or $DifferentRatio -lt 0.01) {
        throw "Viewport capture appears blank: $Path; UniqueColors=$($UniqueColors.Count), DifferentRatio=$('{0:P2}' -f $DifferentRatio)"
    }

    return [PSCustomObject]@{
        Path = $Path
        Width = $Width
        Height = $Height
        DifferentPixels = $DifferentPixels
        DifferentRatio = $DifferentRatio
        CentroidX = $SumX / $DifferentPixels
        Hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

$General = Get-BmpMetrics $ResolvedCapturePath
$A = Get-BmpMetrics $SceneOriginCapturePaths[0]
$B = Get-BmpMetrics $SceneOriginCapturePaths[1]
$C = Get-BmpMetrics $SceneOriginCapturePaths[2]
if ($A.Width -ne $B.Width -or $A.Width -ne $C.Width -or $A.Height -ne $B.Height -or $A.Height -ne $C.Height) {
    throw "Scene origin captures do not have matching dimensions."
}
if ($A.Hash -ne $C.Hash) {
    throw "Equivalent scene origin cases A and C produced different images."
}
if ($A.Hash -eq $B.Hash) {
    throw "Moving only the scene mesh did not change the captured image."
}
$MinimumCentroidShift = [Math]::Max(8.0, $A.Width * 0.03)
if ($B.CentroidX -le ($A.CentroidX + $MinimumCentroidShift)) {
    throw "Moving the mesh right did not move the captured foreground right. A=$($A.CentroidX), B=$($B.CentroidX)"
}
$AreaRatio = $B.DifferentPixels / [double]$A.DifferentPixels
if ($AreaRatio -lt 0.5 -or $AreaRatio -gt 1.5) {
    throw "Scene origin moved capture changed foreground area unexpectedly: ratio=$AreaRatio"
}

Write-Host "Render smoke passed: $ResolvedCapturePath"
Write-Host "Dimensions: $($General.Width)x$($General.Height), DifferentRatio=$('{0:P2}' -f $General.DifferentRatio)"
Write-Host "Scene origin captures passed: A/C identical, B centroid shift=$('{0:N2}' -f ($B.CentroidX - $A.CentroidX)) pixels"
