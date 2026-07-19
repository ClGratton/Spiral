param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [string]$Action = "vs2022",

    [string]$CapturePath = "output/captures/editor-viewport.bmp",

    [ValidateRange(1, 3600)]
    [int]$ChildTimeoutSeconds = 180,

    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "InvokeBoundedProcess.ps1")
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

$RenderResult = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--capture-viewport", "--smoke-test", "--frame-lifecycle-telemetry-smoke", "--renderer-capability-smoke", "--scene-origin-raster-smoke", "--scene-raster-preparation-smoke", "--scene-viewport-render-graph-smoke", "--rhi-buffer-transition-smoke", "--rhi-completion-smoke", "--rhi-timestamp-query-smoke", "--rhi-queue-dependency-smoke", "--rhi-buffer-ownership-smoke", "--rhi-texture-ownership-smoke", "--rhi-resource-ownership-smoke", "--rhi-resource-state-smoke", "--rhi-texture-readback-smoke", "--rhi-texture-upload-smoke", "--render-graph-execution-smoke") -Label "Editor render smoke" -TimeoutSeconds $ChildTimeoutSeconds
$Output = $RenderLog = $RenderResult.Output
if ($RenderResult.TimedOut) {
    throw "Editor render smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($RenderResult.ExitCode -ne 0) {
    throw "Editor render smoke run failed with exit code $($RenderResult.ExitCode)."
}

foreach ($Candidate in @("inter-frame", "submission-gate")) {
    $PacingResult = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--smoke-test", "--smooth-frametime-candidate-smoke", "--smooth-frametime-candidate=$Candidate", "--smooth-frametime-target-fps=5") -Label "D3D12 Smooth Frametime $Candidate" -TimeoutSeconds $ChildTimeoutSeconds
    if ($PacingResult.TimedOut -or $PacingResult.ExitCode -ne 0) { throw "D3D12 Smooth Frametime $Candidate smoke failed." }
    $PacingLog = $PacingResult.Output -join "`n"
    $CandidateMarker = if ($Candidate -eq "inter-frame") { "InterFrame" } else { "SubmissionGate" }
    if (($PacingLog -notmatch "SmoothFrametimeCandidateSmokeV1 backend=NVRHI D3D12") -or ($PacingLog -notmatch "candidate=$CandidateMarker")) { throw "D3D12 pacing smoke did not emit candidate telemetry for $Candidate." }
    if ($Candidate -eq "submission-gate" -and $PacingLog -notmatch "SmoothFrametimeNativeV1 backend=D3D12 candidate=SubmissionGate control=pre-ExecuteCommandLists") { throw "D3D12 submission-gate smoke did not prove the pre-submit seam." }
}

foreach ($TargetChange in @(@{ Name = "below"; NewTarget = 10 }, @{ Name = "above"; NewTarget = 1000 })) {
    $ChangeResult = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--smoke-test", "--smooth-frametime-candidate-smoke", "--smooth-frametime-target-change-smoke", "--smooth-frametime-candidate=inter-frame", "--smooth-frametime-target-fps=5", "--smooth-frametime-target-change-fps=$($TargetChange.NewTarget)") -Label "D3D12 Smooth Frametime target change $($TargetChange.Name)" -TimeoutSeconds $ChildTimeoutSeconds
    if ($ChangeResult.TimedOut -or $ChangeResult.ExitCode -ne 0) { throw "D3D12 Smooth Frametime target-change $($TargetChange.Name) smoke failed." }
    $ChangeLog = $ChangeResult.Output -join "`n"
    if (($ChangeLog -notmatch "SmoothFrametimeTargetChangeV1 state=published oldTargetFps=5") -or ($ChangeLog -notmatch "newTargetFps=$($TargetChange.NewTarget).+firstControlPoint=next-valid-frame-boundary") -or ($ChangeLog -notmatch "targetChange=applied.+newTargetFps=$($TargetChange.NewTarget)")) { throw "D3D12 target-change smoke did not prove the live policy transition for $($TargetChange.Name)." }
    if ($TargetChange.Name -eq "below") {
        if ($ChangeLog -notmatch "frame=(\d+).+effectiveLimiter=RequestedTargetCadence.+limitingSourceFrame=\1") { throw "D3D12 below-target smoke did not report an InterFrame requested-cadence source on its terminal cadence frame." }
    } else {
        $Match = [regex]::Match($ChangeLog, "frame=(\d+).+effectiveLimiter=(CpuActiveWork|GpuWork|D3D12SynchronizedPresent|Unresolved).+limitingSourceFrame=(\d+|unavailable)")
        if (!$Match.Success -or $ChangeLog -match "effectiveLimiter=RequestedTargetCadence" -or ($Match.Groups[2].Value -eq "Unresolved" -and $Match.Groups[3].Value -ne "unavailable") -or ($Match.Groups[2].Value -ne "Unresolved" -and [UInt64]$Match.Groups[3].Value -ne ([UInt64]$Match.Groups[1].Value - 1))) { throw "D3D12 above-target smoke did not report a source-qualified non-target result aligned to its cadence interval." }
    }
}

$InlineRecordingResult = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--smoke-test", "--scene-raster-preparation-smoke", "--scene-viewport-render-graph-smoke", "--frame-task-single-thread") -Label "Editor render graph inline recording smoke" -TimeoutSeconds $ChildTimeoutSeconds
$InlineRecordingLog = $InlineRecordingResult.Output
if ($InlineRecordingResult.TimedOut) {
    throw "Editor render graph inline recording smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($InlineRecordingResult.ExitCode -ne 0) {
    throw "Editor render graph inline recording smoke failed with exit code $($InlineRecordingResult.ExitCode)."
}

$FallbackResult = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--smoke-test", "--rhi-queue-dependency-smoke", "--rhi-buffer-ownership-smoke", "--rhi-texture-ownership-smoke", "--rhi-force-graphics-queue-fallback") -Label "Editor forced graphics-queue fallback smoke" -TimeoutSeconds $ChildTimeoutSeconds
$FallbackOutput = $FallbackLog = $FallbackResult.Output
if ($FallbackResult.TimedOut) {
    throw "Editor forced graphics-queue fallback smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($FallbackResult.ExitCode -ne 0) {
    throw "Editor forced graphics-queue fallback smoke failed with exit code $($FallbackResult.ExitCode)."
}

$RequiredMarkers = @(
    "NVRHI D3D12 device created on adapter:",
    "Selected D3D12 adapter for profile 'Phase 3 D3D12 Bootstrap V1':",
    "D3D12 capability state: Ray Tracing advertised=",
    "D3D12 capability state: Timestamps advertised=yes, enabled=yes, implemented=yes, exercised=no",
    "Editor renderer capability diagnostics ready: profile=Phase 3 D3D12 Bootstrap V1, qualification=Bootstrap",
    "Renderer capability group: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=no",
    "Renderer capability group: group=Phase3TransientResourcesV1, profile=Phase 3 Transient Resources V1, preferredPath=PlacedAliasedTransient, selectedPath=NonAliasedGpuRetiredPool, implemented=yes, exercised=no",
    "Editor renderer capability group exercised: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps",
    "Editor renderer capability group exercised: group=Phase3TransientResourcesV1, profile=Phase 3 Transient Resources V1, preferredPath=PlacedAliasedTransient, selectedPath=NonAliasedGpuRetiredPool, implemented=yes, exercised=yes, qualification=Presentation, deviceQualification=Bootstrap",
    "Renderer initialized with backend: NVRHI D3D12",
    "FrameLifecycleTelemetryV1 backend=NVRHI D3D12 frame=",
    "gpuCompletion=observed completedFrame=",
    "mandatoryWaits=dxgi-latency",
    "D3D12 scene origin raster case A:",
    "D3D12 scene origin raster case B:",
    "D3D12 scene origin raster case C:",
    "D3D12 scene origin raster smoke passed",
    "RHIBufferTransitionSmokeV1 backend=D3D12, invalid=rejected, lifecycle=pass, submission=pass, result=pass"
    "RHICompletionSmokeV1 backend=D3D12, tokenValidation=pass, query=nonblocking-"
    "RHITimestampQuerySmokeV1 backend=D3D12, allocation=pass, periodNanoseconds="
    "RHIQueueDependencySmokeV1 backend=D3D12,"
    "RHIBufferOwnershipSmokeV1 backend=D3D12,"
    "RHITextureOwnershipSmokeV1 backend=D3D12,"
    "RHIResourceOwnershipSmokeV1 backend=D3D12, owned=pass, null=rejected, result=pass"
    "RHIResourceStateSmokeV1 backend=D3D12, initial=pass, pending=hidden, invalid=rejected, submission=pass, final=pass, result=pass"
    "RHITextureReadbackSmokeV1 backend=D3D12, invalidState=rejected, unsupportedFormat=rejected, submit=pass, readback=pass, layout=tight, result=pass"
    "SceneRasterPreparationV1 mode=parallel task=Frame.PrepareSceneRaster worker="
    "SceneViewportRenderGraphV1 backend=D3D12 passes=3 labels=clear,raster,output-handoff execution=pass reference=direct comparator=exact-byte-pass"
    "SceneMeshGpuIntegrationV1 backend=D3D12 snapshot=pass resolver=pass cache=pass indexFormat=UInt32 baseVertex=0"
    "ProductionRenderGraphRetirementV1 backend=D3D12"
    "RenderGraphTimestampScopesV1 backend=D3D12"
    "RendererGpuTimingV1 backend=NVRHI D3D12"
)
$JoinedLog = $RenderLog -join "`n"
$PresentationResult = Invoke-BoundedProcess -FilePath $Editor -Arguments @("--presentation-policy-smoke") -Label "Editor D3D12 presentation-policy smoke" -TimeoutSeconds $ChildTimeoutSeconds
if ($PresentationResult.TimedOut -or $PresentationResult.ExitCode -ne 0) { throw "D3D12 presentation-policy smoke failed." }
$PresentationLog = $PresentationResult.Output -join "`n"
if ($PresentationLog -notmatch 'PresentationPolicySmokeV1 state=tearing-resolved requested=TearingAllowed\s+capability=allow-tearing-(?:supported|unsupported)\s+actual=D3D12Flip(?:Synchronized|TearingAllowed)\s+fallback=.*?\s+generation=\d+\s+effectiveFrame=\d+' -or
    $PresentationLog -notmatch 'PresentationPolicySmokeV1 state=stable-request generation=\d+\s+resize=requested restore=Synchronized' -or
    $PresentationLog -notmatch 'PresentationPolicySmokeV1 state=pass requested=Synchronized\s+capability=allow-tearing-(?:supported|unsupported)\s+actual=D3D12FlipSynchronized\s+fallback=.*?\s+generation=(\d+)\s+effectiveFrame=\d+\s+lastPresentGeneration=\1\s+lastPresentFrame=\d+\s+resize=pass imgui=pass present=pass') {
    throw "D3D12 presentation-policy smoke did not prove ordered transition, stable fallback, resize, and final synchronized ImGui/present survival."
}
foreach ($Marker in $RequiredMarkers) {
    if (!$JoinedLog.Contains($Marker)) {
        throw "D3D12 render smoke did not emit required marker: $Marker"
    }
}
if ($JoinedLog -notmatch 'FrameLifecycleTelemetryV1 backend=NVRHI D3D12 frame=\d+ phases=frame-start,input-sample,input-simulation,render-submission,present-begin,present-end .*mandatoryWaits=dxgi-latency') {
    throw "D3D12 render smoke did not prove the ordered input-sample lifecycle boundary while retaining the mandatory DXGI wait."
}
$InputLatencyMatch = [regex]::Match($JoinedLog, 'FrameLifecycleTelemetryV1 backend=NVRHI D3D12 frame=(\d+).*?inputLatencySourceFrame=(\d+).*?inputToSimulationMs=([0-9.eE+-]+).*?inputToSubmitMs=([0-9.eE+-]+).*?inputToPresentMs=([0-9.eE+-]+).*?inputToDisplay=unavailable clickToPhoton=unavailable', [Text.RegularExpressions.RegexOptions]::Singleline)
if (!$InputLatencyMatch.Success -or [uint64]$InputLatencyMatch.Groups[2].Value -ne [uint64]$InputLatencyMatch.Groups[1].Value -or [double]$InputLatencyMatch.Groups[3].Value -lt 0.0 -or [double]$InputLatencyMatch.Groups[4].Value -lt [double]$InputLatencyMatch.Groups[3].Value -or [double]$InputLatencyMatch.Groups[5].Value -lt [double]$InputLatencyMatch.Groups[4].Value) {
    throw "D3D12 render smoke did not publish ordered exact-frame input-to-simulation/submit/Present intervals."
}
if ($JoinedLog -notmatch 'RenderGraphRecordingV1 backend=D3D12 mode=worker workerPasses=2 overlap=(yes|no) submitted=3 result=pass') {
    throw "D3D12 render smoke did not prove the parallel RenderGraph recording marker."
}
$RetirementMatches = [regex]::Matches($JoinedLog, 'ProductionRenderGraphRetirementV1 backend=D3D12 frame=\d+ passes=3 cpuWaitBetween=no pending=\d+ result=pass')
if ($RetirementMatches.Count -lt 2) {
    throw "D3D12 render smoke did not prove asynchronous RenderGraph retirement across consecutive frames."
}
$TimestampScopeMatches = [regex]::Matches($JoinedLog, 'RenderGraphTimestampScopesV1 backend=D3D12 frame=\d+ scopes=3 raw=ready cpuWaitBetween=no result=pass')
if ($TimestampScopeMatches.Count -lt 2) {
    throw "D3D12 render smoke did not prove completion-gated raw timestamp scopes across consecutive frames."
}
$GpuTimingMatches = [regex]::Matches($JoinedLog, 'RendererGpuTimingV1 backend=NVRHI D3D12 frame=\d+ passes=3 wholeMs=[0-9]+(?:\.[0-9]+)? status=Ready capability=GpuTimestamps result=pass')
if ($GpuTimingMatches.Count -lt 2) {
    throw "D3D12 render smoke did not publish exact-frame GPU durations and promote the exercised capability path."
}
$InlineRecordingJoinedLog = $InlineRecordingLog -join "`n"
if (($InlineRecordingJoinedLog -notmatch 'SceneRasterPreparationV1 mode=single-thread task=Frame.PrepareSceneRaster worker=caller') -or ($InlineRecordingJoinedLog -notmatch 'RenderGraphRecordingV1 backend=D3D12 mode=inline workerPasses=2 overlap=no submitted=3 result=pass') -or ($InlineRecordingJoinedLog -notmatch 'SceneViewportRenderGraphV1 backend=D3D12 passes=3 labels=clear,raster,output-handoff execution=pass reference=direct comparator=exact-byte-pass') -or ($InlineRecordingJoinedLog -notmatch 'ProductionRenderGraphRetirementV1 backend=D3D12 frame=\d+ passes=3 cpuWaitBetween=no pending=\d+ result=pass')) {
    throw "D3D12 inline recording smoke did not preserve the deterministic recording and exact-byte viewport markers."
}
if ($JoinedLog -notmatch 'RHICompletionSmokeV1 backend=D3D12, tokenValidation=pass, query=nonblocking-(incomplete|complete), wait=pass, reuse=pass, result=pass') {
    throw "D3D12 render smoke did not prove completion-token retirement and recording reuse."
}
if ($JoinedLog -notmatch 'RHITimestampQuerySmokeV1 backend=D3D12, allocation=pass, periodNanoseconds=[0-9]+(?:\.[0-9]+)?, writeResolve=pass, pending=pass, readback=pass, reuse=retired-pass, destruction=retained-pass, result=pass') {
    throw "D3D12 render smoke did not prove native timestamp allocation, resolve, retirement, reuse, and destruction."
}
if ($JoinedLog -notmatch 'RenderGraphExecutionSmokeV1 backend=D3D12, barriers=3, callbacks=ordered-pass, undeclared=rejected, submission=pass, topology=(independent-copy|graphics-fallback), dependency=(gpu-wait|ordered-elided), readback=pass, reuse=retired-same-context, result=pass') {
    throw "D3D12 render smoke did not prove topology-adaptive RenderGraph queue execution, readback, and aggregate retirement."
}
if ($JoinedLog -notmatch 'RenderGraphTransientAllocationSmokeV1 backend=D3D12, mode=NonAliasedGpuRetiredPool, lifetime=compatible-sequential-pass, estimatedLogicalAllocatedBytes=64, estimatedLogicalPooledBytes=64, retirement=exact-token-pass, reuse=retired-pass, result=pass') {
    throw "D3D12 render smoke did not prove transient lifetime allocation, exact-token retirement, and pooled reuse."
}
$QueueDependencyPattern = 'RHIQueueDependencySmokeV1 backend=D3D12, copy=(?<copy>independent|graphics-fallback), compute=(?<compute>independent|graphics-fallback), copyToGraphics=(?<copyMode>gpu-wait|ordered-elided), graphicsToCompute=(?<computeMode>gpu-wait|ordered-elided), cpuWaitBetween=no, bytes=pass, finalState=CopySource, retirement=pass, result=pass'
$QueueDependencyMatch = [regex]::Match($JoinedLog, $QueueDependencyPattern)
if (!$QueueDependencyMatch.Success) {
    throw "D3D12 render smoke did not prove deterministic GPU dependency output and retirement."
}
$CopyTopologyMatchesMode = ($QueueDependencyMatch.Groups['copy'].Value -eq 'independent') -eq ($QueueDependencyMatch.Groups['copyMode'].Value -eq 'gpu-wait')
$ComputeTopologyMatchesMode = ($QueueDependencyMatch.Groups['compute'].Value -eq 'independent') -eq ($QueueDependencyMatch.Groups['computeMode'].Value -eq 'gpu-wait')
if (!$CopyTopologyMatchesMode -or !$ComputeTopologyMatchesMode) {
    throw "D3D12 queue topology and wait/elision evidence disagree."
}
if ($JoinedLog -notmatch 'RHIBufferOwnershipSmokeV1 backend=D3D12, mode=independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=pass, finalOwner=Copy, finalState=CopySource, recovery=pass, retirement=pass, result=pass') {
    throw "D3D12 render smoke did not prove real independent-queue buffer ownership transfer, recovery, and retirement."
}
if ($JoinedLog -notmatch 'RHITextureOwnershipSmokeV1 backend=D3D12, mode=independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=pass, finalOwner=Copy, finalState=CopySource, recovery=pass, retirement=pass, result=pass') { throw "D3D12 texture ownership smoke failed." }
$FallbackJoinedLog = $FallbackLog -join "`n"
if ($FallbackJoinedLog -notmatch 'RHIQueueDependencySmokeV1 backend=D3D12, copy=graphics-fallback, compute=graphics-fallback, copyToGraphics=ordered-elided, graphicsToCompute=ordered-elided, cpuWaitBetween=no, bytes=pass, finalState=CopySource, retirement=pass, result=pass') {
    throw "D3D12 forced fallback smoke did not prove same-effective-graphics ordered dependencies."
}
if ($FallbackJoinedLog -notmatch 'RHIBufferOwnershipSmokeV1 backend=D3D12, mode=graphics-fallback, transfer=rejected, pending=no, result=pass') {
    throw "D3D12 forced fallback smoke did not reject buffer ownership transfer without publishing pending state."
}
if ($FallbackJoinedLog -notmatch 'RHITextureOwnershipSmokeV1 backend=D3D12, mode=graphics-fallback, transfer=rejected, pending=no, result=pass') { throw "D3D12 forced fallback texture ownership smoke failed." }

$ConventionEvidence = "schema=1\|matrix=row-major\|d3dClipDepth=zero-to-one\|spirvY=inverted\|frontFace=clockwise\|binding=D3DRegisterSpace"
$TerminalPattern = "PortableShaderTerminalV1 status=(?<status>success|cache-hit) request=[1-9][0-9]* stage=(?<stage>vertex|pixel) cacheMode=(?<cacheMode>compiled|cache-hit) cacheSource=(?<cacheSource>compiler|disk|service) compiler=Slang-2026\.13\.1 backend=Slang targets=DXIL\+SPIR-V key=(?<key>[0-9a-f]{64}) bindings=(?<bindings>[1-9][0-9]*) vertexInputs=(?<vertexInputs>[0-9]+) conventions=$ConventionEvidence legacySourceCompile=false"
$TerminalMatches = [regex]::Matches($JoinedLog, $TerminalPattern)
if ($TerminalMatches.Count -ne 2) {
    throw "D3D12 render smoke requires exactly two successful PortableShaderTerminalV1 package markers; found $($TerminalMatches.Count)."
}

$TerminalByStage = @{}
foreach ($Match in $TerminalMatches) {
    $Stage = $Match.Groups["stage"].Value
    if ($TerminalByStage.ContainsKey($Stage)) {
        throw "D3D12 render smoke emitted duplicate PortableShaderTerminalV1 evidence for stage '$Stage'."
    }
    $TerminalByStage[$Stage] = $Match

    $Status = $Match.Groups["status"].Value
    $CacheMode = $Match.Groups["cacheMode"].Value
    $CacheSource = $Match.Groups["cacheSource"].Value
    $ValidCacheEvidence =
        ($Status -eq "success" -and $CacheMode -eq "compiled" -and $CacheSource -eq "compiler") -or
        ($Status -eq "success" -and $CacheMode -eq "cache-hit" -and $CacheSource -eq "disk") -or
        ($Status -eq "cache-hit" -and $CacheMode -eq "cache-hit" -and $CacheSource -eq "service")
    if (!$ValidCacheEvidence) {
        throw "D3D12 portable shader stage '$Stage' emitted inconsistent terminal status/cache evidence."
    }
}
if (!$TerminalByStage.ContainsKey("vertex") -or !$TerminalByStage.ContainsKey("pixel")) {
    throw "D3D12 render smoke requires successful terminal package evidence for both vertex and pixel stages."
}
if ([int]$TerminalByStage["vertex"].Groups["vertexInputs"].Value -lt 1) {
    throw "D3D12 vertex terminal package marker did not contain reflected vertex-input evidence."
}
if ([int]$TerminalByStage["pixel"].Groups["vertexInputs"].Value -ne 0) {
    throw "D3D12 pixel terminal package marker reported unexpected vertex inputs."
}

$ActivePattern = "D3D12PortablePipelineV1 status=active vertexStatus=(?<vertexStatus>success|cache-hit) vertexCacheMode=(?<vertexCacheMode>compiled|cache-hit) vertexCacheSource=(?<vertexCacheSource>compiler|disk|service) vertexKey=(?<vertexKey>[0-9a-f]{64}) vertexBindings=(?<vertexBindings>[1-9][0-9]*) vertexInputs=(?<vertexInputs>[1-9][0-9]*) pixelStatus=(?<pixelStatus>success|cache-hit) pixelCacheMode=(?<pixelCacheMode>compiled|cache-hit) pixelCacheSource=(?<pixelCacheSource>compiler|disk|service) pixelKey=(?<pixelKey>[0-9a-f]{64}) pixelBindings=(?<pixelBindings>[1-9][0-9]*) pixelInputs=0 compiler=Slang-2026\.13\.1 backend=D3D12\+Slang targets=DXIL\+SPIR-V conventions=$ConventionEvidence legacySourceCompile=false"
$ActiveMatches = [regex]::Matches($JoinedLog, $ActivePattern)
if ($ActiveMatches.Count -ne 1) {
    throw "D3D12 render smoke requires exactly one complete D3D12PortablePipelineV1 active marker; found $($ActiveMatches.Count)."
}
$Active = $ActiveMatches[0]
foreach ($Stage in @("vertex", "pixel")) {
    if ($TerminalByStage[$Stage].Groups["key"].Value -ne $Active.Groups["${Stage}Key"].Value) {
        throw "D3D12 portable pipeline $Stage key does not match its terminal package marker."
    }
    if ($TerminalByStage[$Stage].Groups["status"].Value -ne $Active.Groups["${Stage}Status"].Value) {
        throw "D3D12 portable pipeline $Stage status does not match its terminal package marker."
    }
    if ($TerminalByStage[$Stage].Groups["cacheMode"].Value -ne $Active.Groups["${Stage}CacheMode"].Value -or
        $TerminalByStage[$Stage].Groups["cacheSource"].Value -ne $Active.Groups["${Stage}CacheSource"].Value) {
        throw "D3D12 portable pipeline $Stage cache evidence does not match its terminal package marker."
    }
}
if ($JoinedLog -match "PortableShaderTerminalV1 status=(pending|failure|cancelled|unknown)" -or
    $JoinedLog -match "D3D12PortablePipelineV1 status=(pending|failure|cancelled|unknown)" -or
    $JoinedLog -match "legacySourceCompile=true") {
    throw "D3D12 render smoke emitted pending-only, failure, cancellation, unknown, or legacy shader compile evidence."
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
$DiagnosticsPattern = "Editor renderer capability diagnostics rendered: profile=Phase 3 D3D12 Bootstrap V1, adapter=.+, qualification=Bootstrap, formats=[1-9][0-9]*, features=12, groups=2, candidates=[1-9][0-9]*"
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
