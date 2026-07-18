param(
    [ValidateSet("Debug", "Release", "Dist")]
    [string]$Configuration = "Debug",

    [ValidateSet("vs2022", "gmake", "gmake2")]
    [string]$Action = "vs2022",

    [ValidateRange(1, 3600)]
    [int]$ChildTimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

& (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration -Action $Action
. (Join-Path $PSScriptRoot "InvokeBoundedProcess.ps1")

$OutputSuffix = if ($Action -like "gmake*") { "-$Action" } else { "" }
$Executable = Join-Path $Root "bin\$Configuration-windows-x86_64$OutputSuffix\Editor\Editor.exe"
if (!(Test-Path $Executable)) {
    throw "Vulkan smoke executable was not found: $Executable"
}

$VulkanResult = Invoke-BoundedProcess -FilePath $Executable -Arguments @("--vulkan-render-smoke", "--frame-lifecycle-telemetry-smoke", "--renderer-capability-smoke", "--scene-raster-preparation-smoke", "--scene-viewport-render-graph-smoke", "--vulkan-rhi-core-smoke", "--vulkan-rhi-indexed-draw-smoke", "--vulkan-scene-viewport-raster-smoke", "--rhi-buffer-transition-smoke", "--rhi-completion-smoke", "--rhi-timestamp-query-smoke", "--rhi-queue-dependency-smoke", "--rhi-buffer-ownership-smoke", "--rhi-texture-ownership-smoke", "--rhi-resource-ownership-smoke", "--rhi-resource-state-smoke", "--render-graph-execution-smoke") -Label "Editor Vulkan render smoke" -TimeoutSeconds $ChildTimeoutSeconds
$Output = $VulkanLog = $VulkanResult.Output
if ($VulkanResult.TimedOut) {
    throw "Vulkan render smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($VulkanResult.ExitCode -ne 0) {
    throw "Vulkan render smoke failed with exit code $($VulkanResult.ExitCode)."
}

foreach ($Candidate in @("inter-frame", "submission-gate")) {
    $PacingResult = Invoke-BoundedProcess -FilePath $Executable -Arguments @("--vulkan-render-smoke", "--smooth-frametime-candidate-smoke", "--smooth-frametime-candidate=$Candidate", "--smooth-frametime-target-fps=5") -Label "Vulkan Smooth Frametime $Candidate" -TimeoutSeconds $ChildTimeoutSeconds
    if ($PacingResult.TimedOut -or $PacingResult.ExitCode -ne 0) { throw "Vulkan Smooth Frametime $Candidate smoke failed." }
    $PacingLog = $PacingResult.Output -join "`n"
    $CandidateMarker = if ($Candidate -eq "inter-frame") { "InterFrame" } else { "SubmissionGate" }
    if (($PacingLog -notmatch "SmoothFrametimeCandidateSmokeV1 backend=NVRHI Vulkan") -or ($PacingLog -notmatch "candidate=$CandidateMarker")) { throw "Vulkan pacing smoke did not emit candidate telemetry for $Candidate." }
    if ($Candidate -eq "submission-gate" -and $PacingLog -notmatch "SmoothFrametimeNativeV1 backend=Vulkan candidate=SubmissionGate control=pre-vkQueueSubmit") { throw "Vulkan submission-gate smoke did not prove the pre-submit seam." }
}

foreach ($TargetChange in @(@{ Name = "below"; NewTarget = 10 }, @{ Name = "above"; NewTarget = 1000 })) {
    $ChangeResult = Invoke-BoundedProcess -FilePath $Executable -Arguments @("--vulkan-render-smoke", "--smooth-frametime-candidate-smoke", "--smooth-frametime-target-change-smoke", "--smooth-frametime-candidate=inter-frame", "--smooth-frametime-target-fps=5", "--smooth-frametime-target-change-fps=$($TargetChange.NewTarget)") -Label "Vulkan Smooth Frametime target change $($TargetChange.Name)" -TimeoutSeconds $ChildTimeoutSeconds
    if ($ChangeResult.TimedOut -or $ChangeResult.ExitCode -ne 0) { throw "Vulkan Smooth Frametime target-change $($TargetChange.Name) smoke failed." }
    $ChangeLog = $ChangeResult.Output -join "`n"
    if (($ChangeLog -notmatch "SmoothFrametimeTargetChangeV1 state=published oldTargetFps=5") -or ($ChangeLog -notmatch "newTargetFps=$($TargetChange.NewTarget).+firstControlPoint=next-valid-frame-boundary") -or ($ChangeLog -notmatch "targetChange=applied.+newTargetFps=$($TargetChange.NewTarget)")) { throw "Vulkan target-change smoke did not prove the live policy transition for $($TargetChange.Name)." }
    if ($TargetChange.Name -eq "below") {
        if ($ChangeLog -notmatch "frame=(\d+).+effectiveLimiter=RequestedTargetCadence.+limitingSourceFrame=\1") { throw "Vulkan below-target smoke did not report an InterFrame requested-cadence source on its terminal cadence frame." }
    } else {
        $Match = [regex]::Match($ChangeLog, "frame=(\d+).+effectiveLimiter=(CpuActiveWork|GpuWork|VulkanFifoPresent|Unresolved).+limitingSourceFrame=(\d+|unavailable)")
        if (!$Match.Success -or $ChangeLog -match "effectiveLimiter=RequestedTargetCadence" -or ($Match.Groups[2].Value -eq "Unresolved" -and $Match.Groups[3].Value -ne "unavailable") -or ($Match.Groups[2].Value -ne "Unresolved" -and [UInt64]$Match.Groups[3].Value -ne ([UInt64]$Match.Groups[1].Value - 1))) { throw "Vulkan above-target smoke did not report a source-qualified non-target result aligned to its cadence interval." }
    }
}

$InlineRecordingResult = Invoke-BoundedProcess -FilePath $Executable -Arguments @("--vulkan-render-smoke", "--scene-raster-preparation-smoke", "--scene-viewport-render-graph-smoke", "--frame-task-single-thread") -Label "Editor Vulkan inline recording smoke" -TimeoutSeconds $ChildTimeoutSeconds
$InlineRecordingLog = $InlineRecordingResult.Output
if ($InlineRecordingResult.TimedOut) {
    throw "Vulkan inline recording smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($InlineRecordingResult.ExitCode -ne 0) {
    throw "Vulkan inline recording smoke failed with exit code $($InlineRecordingResult.ExitCode)."
}

$RequiredMarkers = @(
    "NVRHI Vulkan device created on adapter:",
    "Selected Vulkan adapter:",
    "Vulkan capability profile: Phase 3 Vulkan Bootstrap Presentation V1, qualification=Bootstrap",
    "Vulkan capability state: Timeline Synchronization advertised=yes, enabled=yes, implemented=yes",
    "Vulkan capability state: Timestamps advertised=yes, enabled=yes, implemented=yes, exercised=no",
    "Vulkan capability state: Buffer Device Address advertised=",
    "Editor renderer capability diagnostics ready: profile=Phase 3 Vulkan Bootstrap Presentation V1, qualification=Bootstrap",
    "Renderer capability group: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=no",
    "Renderer capability group: group=Phase3TransientResourcesV1, profile=Phase 3 Transient Resources V1, preferredPath=PlacedAliasedTransient, selectedPath=NonAliasedGpuRetiredPool, implemented=yes, exercised=no",
    "Editor renderer capability group exercised: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps",
    "Editor renderer capability group exercised: group=Phase3TransientResourcesV1, profile=Phase 3 Transient Resources V1, preferredPath=PlacedAliasedTransient, selectedPath=NonAliasedGpuRetiredPool, implemented=yes, exercised=yes, qualification=Presentation, deviceQualification=Bootstrap",
    "Renderer initialized with backend: NVRHI Vulkan",
    "FrameLifecycleTelemetryV1 backend=NVRHI Vulkan frame=",
    "gpuCompletion=observed completedFrame=",
    "completionSwapchainGeneration=2",
    "mandatoryWaits=vulkan-acquire+fence",
    "Vulkan swapchain and ImGui presentation initialized",
    "Vulkan render smoke requested window resize",
    "Vulkan swapchain recreated after window resize",
    "Vulkan render smoke verified native ImGui presentation after resize",
    "RHIBufferTransitionSmokeV1 backend=Vulkan, invalid=rejected, lifecycle=pass, submission=pass, result=pass",
    "RHICompletionSmokeV1 backend=Vulkan, tokenValidation=pass, query=nonblocking-",
    "RHITimestampQuerySmokeV1 backend=Vulkan, allocation=pass, periodNanoseconds=",
    "RHIQueueDependencySmokeV1 backend=Vulkan,",
    "RHIBufferOwnershipSmokeV1 backend=Vulkan,",
    "RHITextureOwnershipSmokeV1 backend=Vulkan,",
    "RHIResourceOwnershipSmokeV1 backend=Vulkan, owned=pass, null=rejected, result=pass",
    "RHIResourceStateSmokeV1 backend=Vulkan, initial=pass, pending=hidden, invalid=rejected, submission=pass, final=pass, result=pass",
    "VulkanRHICoreV1",
    "lifecycle=pass, cpuMapNone=pass, markers=executed-balanced",
    "VulkanRHIIndexedDrawV1 package=pass reflection=pass pipeline=pass constants=pass draw=pass submit=pass readback=pass interior=pass background=pass"
    "VulkanSceneViewportRasterV1 snapshot=pass pipeline=pass raster=pass readback=pass geometry=pass background=pass resize=pass"
    "VulkanSceneOutputCaptureV1 outputGeneration="
    "VulkanSceneOutputHandoffV1 producer=pass"
    "SceneRasterPreparationV1 mode=parallel task=Frame.PrepareSceneRaster worker="
    "SceneViewportRenderGraphV1 backend=Vulkan passes=3 labels=clear,raster,output-handoff execution=pass reference=direct comparator=exact-byte-pass"
    "ProductionRenderGraphRetirementV1 backend=Vulkan"
    "RenderGraphTimestampScopesV1 backend=Vulkan"
    "RendererGpuTimingV1 backend=NVRHI Vulkan"
)

$JoinedLog = $VulkanLog -join "`n"
$PresentationResult = Invoke-BoundedProcess -FilePath $Executable -Arguments @("--renderer-vulkan", "--presentation-policy-smoke") -Label "Editor Vulkan presentation-policy smoke" -TimeoutSeconds $ChildTimeoutSeconds
if ($PresentationResult.TimedOut -or $PresentationResult.ExitCode -ne 0) { throw "Vulkan presentation-policy smoke failed." }
$PresentationLog = $PresentationResult.Output -join "`n"
if ($PresentationLog -notmatch 'PresentationPolicySmokeV1 state=tearing-resolved requested=TearingAllowed\s+capability=(?:immediate-supported|immediate-unsupported|fifo-required-by-vulkan)\s+actual=Vulkan(?:Immediate|Fifo)\s+fallback=.*?\s+generation=\d+\s+effectiveFrame=\d+' -or
    $PresentationLog -notmatch 'PresentationPolicySmokeV1 state=stable-request generation=\d+\s+resize=requested restore=Synchronized' -or
    $PresentationLog -notmatch 'PresentationPolicySmokeV1 state=pass requested=Synchronized\s+capability=(?:fifo-supported|fifo-required-by-vulkan)\s+actual=VulkanFifo\s+fallback=.*?\s+generation=(\d+)\s+effectiveFrame=\d+\s+lastPresentGeneration=\1\s+lastPresentFrame=\d+\s+resize=pass imgui=pass present=pass') {
    throw "Vulkan presentation-policy smoke did not prove ordered transition, stable FIFO fallback, resize, and final synchronized ImGui/present survival."
}
foreach ($Marker in $RequiredMarkers) {
    if (!$JoinedLog.Contains($Marker)) {
        throw "Vulkan render smoke did not emit required marker: $Marker"
    }
}
if ($JoinedLog -notmatch 'FrameLifecycleTelemetryV1 backend=NVRHI Vulkan frame=\d+ phases=frame-start,input-sample,input-simulation,render-submission,present-begin,present-end .*mandatoryWaits=vulkan-acquire\+fence') {
    throw "Vulkan render smoke did not prove the ordered input-sample lifecycle boundary while retaining mandatory acquire/fence waits."
}
$InputLatencyMatch = [regex]::Match($JoinedLog, 'FrameLifecycleTelemetryV1 backend=NVRHI Vulkan frame=(\d+).*?inputLatencySourceFrame=(\d+).*?inputToSimulationMs=([0-9.eE+-]+).*?inputToSubmitMs=([0-9.eE+-]+).*?inputToPresentMs=([0-9.eE+-]+).*?inputToDisplay=unavailable clickToPhoton=unavailable', [Text.RegularExpressions.RegexOptions]::Singleline)
if (!$InputLatencyMatch.Success -or [uint64]$InputLatencyMatch.Groups[2].Value -ne [uint64]$InputLatencyMatch.Groups[1].Value -or [double]$InputLatencyMatch.Groups[3].Value -lt 0.0 -or [double]$InputLatencyMatch.Groups[4].Value -lt [double]$InputLatencyMatch.Groups[3].Value -or [double]$InputLatencyMatch.Groups[5].Value -lt [double]$InputLatencyMatch.Groups[4].Value) {
    throw "Vulkan render smoke did not publish ordered exact-frame input-to-simulation/submit/Present intervals."
}
if ($JoinedLog -notmatch 'RenderGraphRecordingV1 backend=Vulkan mode=worker workerPasses=2 overlap=(yes|no) submitted=3 result=pass') {
    throw "Vulkan render smoke did not prove the parallel RenderGraph recording marker."
}
$RetirementMatches = [regex]::Matches($JoinedLog, 'ProductionRenderGraphRetirementV1 backend=Vulkan frame=\d+ passes=3 cpuWaitBetween=no pending=\d+ result=pass')
if ($RetirementMatches.Count -lt 2) {
    throw "Vulkan render smoke did not prove asynchronous RenderGraph retirement across consecutive frames."
}
$TimestampScopeMatches = [regex]::Matches($JoinedLog, 'RenderGraphTimestampScopesV1 backend=Vulkan frame=\d+ scopes=3 raw=ready cpuWaitBetween=no result=pass')
if ($TimestampScopeMatches.Count -lt 2) {
    throw "Vulkan render smoke did not prove completion-gated raw timestamp scopes across consecutive frames."
}
$GpuTimingMatches = [regex]::Matches($JoinedLog, 'RendererGpuTimingV1 backend=NVRHI Vulkan frame=\d+ passes=3 wholeMs=[0-9]+(?:\.[0-9]+)? status=Ready capability=GpuTimestamps result=pass')
if ($GpuTimingMatches.Count -lt 1) {
    throw "Vulkan render smoke did not publish exact-frame GPU durations and promote the exercised capability path."
}
$InlineRecordingJoinedLog = $InlineRecordingLog -join "`n"
if (($InlineRecordingJoinedLog -notmatch 'SceneRasterPreparationV1 mode=single-thread task=Frame.PrepareSceneRaster worker=caller') -or ($InlineRecordingJoinedLog -notmatch 'RenderGraphRecordingV1 backend=Vulkan mode=inline workerPasses=2 overlap=no submitted=3 result=pass') -or ($InlineRecordingJoinedLog -notmatch 'SceneViewportRenderGraphV1 backend=Vulkan passes=3 labels=clear,raster,output-handoff execution=pass reference=direct comparator=exact-byte-pass') -or ($InlineRecordingJoinedLog -notmatch 'ProductionRenderGraphRetirementV1 backend=Vulkan frame=\d+ passes=3 cpuWaitBetween=no pending=\d+ result=pass')) {
    throw "Vulkan inline recording smoke did not preserve the deterministic recording and exact-byte viewport markers."
}
if ($JoinedLog -notmatch 'RHICompletionSmokeV1 backend=Vulkan, tokenValidation=pass, query=nonblocking-(incomplete|complete), wait=pass, reuse=pass, result=pass') {
    throw "Vulkan render smoke did not prove completion-token retirement and recording reuse."
}
if ($JoinedLog -notmatch 'RHITimestampQuerySmokeV1 backend=Vulkan, allocation=pass, periodNanoseconds=[0-9]+(?:\.[0-9]+)?, writeResolve=pass, pending=pass, readback=pass, reuse=retired-pass, destruction=retained-pass, result=pass') {
    throw "Vulkan render smoke did not prove native timestamp allocation, resolve, retirement, reuse, and destruction."
}
if ($JoinedLog -notmatch 'RenderGraphExecutionSmokeV1 backend=Vulkan, barriers=3, callbacks=ordered-pass, undeclared=rejected, submission=pass, topology=(independent-copy|graphics-fallback), dependency=(gpu-wait|ordered-elided), readback=pass, reuse=retired-same-context, result=pass') {
    throw "Vulkan render smoke did not prove topology-adaptive RenderGraph queue execution, readback, and aggregate retirement."
}
if ($JoinedLog -notmatch 'RenderGraphTransientAllocationSmokeV1 backend=Vulkan, mode=NonAliasedGpuRetiredPool, lifetime=compatible-sequential-pass, estimatedLogicalAllocatedBytes=64, estimatedLogicalPooledBytes=64, retirement=exact-token-pass, reuse=retired-pass, result=pass') {
    throw "Vulkan render smoke did not prove transient lifetime allocation, exact-token retirement, and pooled reuse."
}
if ($JoinedLog -notmatch 'RHIQueueDependencySmokeV1 backend=Vulkan, copy=(independent|graphics-fallback), compute=(independent|graphics-fallback), copyToGraphics=(gpu-wait|ordered-elided), graphicsToCompute=(gpu-wait|ordered-elided), cpuWaitBetween=no, queueLocal=yes, sharedResources=(rejected|permitted-or-elided), retirement=pass, result=pass') {
    throw "Vulkan smoke did not prove topology-adaptive queue-local dependency retirement."
}
if ($JoinedLog -notmatch 'RHIBufferOwnershipSmokeV1 backend=Vulkan, mode=(independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=pass, finalOwner=Copy, finalState=CopySource, recovery=pass, retirement=pass, result=pass|graphics-fallback, transfer=rejected, pending=no, result=pass)') {
    throw "Vulkan smoke did not prove topology-adaptive buffer ownership transfer or truthful fallback rejection."
}
if ($JoinedLog -notmatch 'RHITextureOwnershipSmokeV1 backend=Vulkan, mode=(independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=pass, finalOwner=Copy, finalState=CopySource, recovery=pass, retirement=pass, result=pass|graphics-fallback, transfer=rejected, pending=no, result=pass)') {
    throw "Vulkan smoke did not prove topology-adaptive texture ownership transfer or truthful fallback rejection."
}
$DiagnosticsPattern = "Editor renderer capability diagnostics rendered: profile=Phase 3 Vulkan Bootstrap Presentation V1, adapter=.+, qualification=Bootstrap, formats=[1-9][0-9]*, features=11, groups=2, candidates=[1-9][0-9]*"
if ($JoinedLog -notmatch $DiagnosticsPattern) {
    throw "Vulkan render smoke did not emit a complete editor capability diagnostics marker."
}
if ($JoinedLog -notmatch 'VulkanSceneOutputCaptureV1 outputGeneration=[2-9][0-9]* capture=pass') {
    throw "Vulkan render smoke did not capture the post-resize renderer-owned Scene output."
}
if ($JoinedLog -notmatch 'VulkanSceneOutputHandoffV1 producer=pass outputGeneration=[2-9][0-9]* descriptor=registered descriptorGeneration=[2-9][0-9]* imgui=queued present=pass swapchainGeneration=[2-9][0-9]*') {
    throw "Vulkan render smoke did not prove post-resize ImGui consumption and swapchain presentation of the Scene output."
}

Write-Host "Vulkan render smoke passed: $Configuration ($Action)"
