param(
    [ValidateSet("Debug", "Release", "Dist")][string]$Configuration = "Debug",
    [ValidateRange(10, 180)][int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Editor = Join-Path $Root "bin/$Configuration-windows-x86_64/Editor/Editor.exe"
if (!(Test-Path -LiteralPath $Editor)) { throw "Editor executable not found: $Editor" }

$temporary = Join-Path $Root ("output/live-d3d12-pipeline-rebuild-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporary | Out-Null
$source = Join-Path $Root "Engine/Shaders/EditorViewport.hlsl"
$copy = Join-Path $temporary "EditorViewport.live.hlsl"
$stdout = Join-Path $temporary "stdout.txt"
$stderr = Join-Path $temporary "stderr.txt"
Copy-Item -LiteralPath $source -Destination $copy
$original = [IO.File]::ReadAllText($copy)

function Require-Marker([string]$Log, [string]$Pattern, [string]$Description) {
    if ($Log -notmatch $Pattern) { throw "Missing $Description. Evidence: $temporary" }
}

$process = $null
try {
    $quotedShaderArgument = '"--viewport-shader-path=' + $copy + '"'
    $process = Start-Process -FilePath $Editor -ArgumentList @(
        "--live-d3d12-pipeline-rebuild-smoke",
        "--smooth-frametime-candidate-smoke",
        "--smooth-frametime-target-fps=60",
        $quotedShaderArgument
    ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -WindowStyle Hidden

    # The smoke is deliberately paced to 60 FPS, so these two-second windows
    # give each asynchronous compile a stable opportunity to publish before the
    # next source revision supersedes it. Final assertions use engine markers.
    Start-Sleep -Seconds 2
    if ($process.HasExited) { throw "Editor exited before the valid source edit. Evidence: $temporary" }
    [IO.File]::WriteAllText($copy, $original.Replace('0.86f', '0.72f'))

    Start-Sleep -Seconds 2
    if ($process.HasExited) { throw "Editor exited before the invalid source edit. Evidence: $temporary" }
    [IO.File]::WriteAllText($copy, 'this is deliberately invalid Slang')

    Start-Sleep -Seconds 2
    if ($process.HasExited) { throw "Editor exited before source recovery. Evidence: $temporary" }
    [IO.File]::WriteAllText($copy, $original)

    if (!$process.WaitForExit($TimeoutSeconds * 1000)) { throw "Editor did not exit within $TimeoutSeconds seconds. Evidence: $temporary" }
    $process.WaitForExit()

    $log = [IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)
    Require-Marker $log 'D3D12LivePipelineRebuildV1 status=published requestedRevision=1 generation=1' 'initial live pipeline publication'
    Require-Marker $log 'D3D12LivePipelineRebuildV1 status=published requestedRevision=2 generation=2' 'valid live pipeline replacement'
    Require-Marker $log 'PortableShaderTerminalV1 status=failure' 'invalid Slang rejection'
    Require-Marker $log 'Last valid D3D12 viewport pipeline remains active' 'last-known-good retention'
    Require-Marker $log 'D3D12LivePipelineRebuildV1 status=published requestedRevision=4 generation=3' 'restored live pipeline publication'
    Require-Marker $log 'Application stopped after 900 frame\(s\)' 'bounded application-managed stop'
    Require-Marker $log '\[Engine\] Log shutdown' 'complete application teardown'
    Write-Host "Live D3D12 pipeline rebuild smoke passed: $temporary"
}
finally {
    if (Test-Path -LiteralPath $copy) { [IO.File]::WriteAllText($copy, $original) }
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force; $process.WaitForExit() }
}
