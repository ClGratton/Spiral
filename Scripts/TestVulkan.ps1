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

$VulkanResult = Invoke-BoundedProcess -FilePath $Executable -Arguments @("--vulkan-render-smoke", "--renderer-capability-smoke", "--vulkan-rhi-core-smoke", "--vulkan-rhi-indexed-draw-smoke", "--vulkan-scene-viewport-raster-smoke", "--rhi-buffer-transition-smoke", "--rhi-completion-smoke", "--rhi-queue-dependency-smoke", "--rhi-buffer-ownership-smoke", "--rhi-texture-ownership-smoke", "--rhi-resource-ownership-smoke", "--rhi-resource-state-smoke", "--render-graph-execution-smoke") -Label "Editor Vulkan render smoke" -TimeoutSeconds $ChildTimeoutSeconds
$Output = $VulkanLog = $VulkanResult.Output
if ($VulkanResult.TimedOut) {
    throw "Vulkan render smoke timed out after $ChildTimeoutSeconds seconds."
}
if ($VulkanResult.ExitCode -ne 0) {
    throw "Vulkan render smoke failed with exit code $($VulkanResult.ExitCode)."
}

$RequiredMarkers = @(
    "NVRHI Vulkan device created on adapter:",
    "Selected Vulkan adapter:",
    "Vulkan capability profile: Phase 3 Vulkan Bootstrap Presentation V1, qualification=Bootstrap",
    "Vulkan capability state: Timeline Synchronization advertised=yes, enabled=yes, implemented=yes",
    "Vulkan capability state: Buffer Device Address advertised=",
    "Editor renderer capability diagnostics ready: profile=Phase 3 Vulkan Bootstrap Presentation V1, qualification=Bootstrap",
    "Renderer capability group: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=no",
    "Editor renderer capability group exercised: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=yes, qualification=Presentation, deviceQualification=Bootstrap",
    "Renderer initialized with backend: NVRHI Vulkan",
    "Vulkan swapchain and ImGui presentation initialized",
    "Vulkan render smoke requested window resize",
    "Vulkan swapchain recreated after window resize",
    "Vulkan render smoke verified native ImGui presentation after resize",
    "RHIBufferTransitionSmokeV1 backend=Vulkan, invalid=rejected, lifecycle=pass, submission=pass, result=pass",
    "RHICompletionSmokeV1 backend=Vulkan, tokenValidation=pass, query=nonblocking-",
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
)

$JoinedLog = $VulkanLog -join "`n"
foreach ($Marker in $RequiredMarkers) {
    if (!$JoinedLog.Contains($Marker)) {
        throw "Vulkan render smoke did not emit required marker: $Marker"
    }
}
if ($JoinedLog -notmatch 'RHICompletionSmokeV1 backend=Vulkan, tokenValidation=pass, query=nonblocking-(incomplete|complete), wait=pass, reuse=pass, result=pass') {
    throw "Vulkan render smoke did not prove completion-token retirement and recording reuse."
}
if ($JoinedLog -notmatch 'RenderGraphExecutionSmokeV1 backend=Vulkan, barriers=3, callbacks=ordered-pass, undeclared=rejected, submission=pass, topology=(independent-copy|graphics-fallback), dependency=(gpu-wait|ordered-elided), readback=pass, reuse=retired-same-context, result=pass') {
    throw "Vulkan render smoke did not prove topology-adaptive RenderGraph queue execution, readback, and aggregate retirement."
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
$DiagnosticsPattern = "Editor renderer capability diagnostics rendered: profile=Phase 3 Vulkan Bootstrap Presentation V1, adapter=.+, qualification=Bootstrap, formats=[1-9][0-9]*, features=9, groups=1, candidates=[1-9][0-9]*"
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
