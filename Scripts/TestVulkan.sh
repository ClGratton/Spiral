#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Debug}"
ACTION="${2:-gmake}"
BUILD_MODE="${3:-build}"
ITERATIONS="${VULKAN_SMOKE_ITERATIONS:-1}"
CHILD_TIMEOUT_SECONDS="${VULKAN_SMOKE_TIMEOUT_SECONDS:-180}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$CONFIGURATION" in
    Debug|Release|Dist) ;;
    *) echo "Unsupported configuration: $CONFIGURATION" >&2; exit 1 ;;
esac
case "$ACTION" in
    gmake|gmake2) ;;
    *) echo "Unsupported action: $ACTION" >&2; exit 1 ;;
esac
if [[ "$BUILD_MODE" != "build" && "$BUILD_MODE" != "--skip-build" ]]; then
    echo "Unsupported build mode: $BUILD_MODE" >&2
    exit 1
fi
if [[ ! "$ITERATIONS" =~ ^[1-9][0-9]*$ ]]; then
    echo "VULKAN_SMOKE_ITERATIONS must be a positive integer: $ITERATIONS" >&2
    exit 1
fi
if [[ ! "$CHILD_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
    echo "VULKAN_SMOKE_TIMEOUT_SECONDS must be a positive integer: $CHILD_TIMEOUT_SECONDS" >&2
    exit 1
fi
if ! command -v perl >/dev/null 2>&1; then
    echo "Perl is required for portable Vulkan smoke timeout/process-group cleanup." >&2
    exit 1
fi

if [[ "$BUILD_MODE" != "--skip-build" ]]; then
    bash "$ROOT/Scripts/Build.sh" "$CONFIGURATION" "$ACTION"
fi

case "$(uname -s)" in
    Linux*) SYSTEM_DIR="linux" ;;
    Darwin*) SYSTEM_DIR="macosx" ;;
    MINGW*|MSYS*|CYGWIN*) SYSTEM_DIR="windows" ;;
    *) echo "Unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac

SUFFIX=""
if [[ "$ACTION" != "vs2022" ]]; then
    SUFFIX="-$ACTION"
fi

EDITOR="$ROOT/bin/${CONFIGURATION}-${SYSTEM_DIR}-x86_64${SUFFIX}/Editor/Editor"
if [[ "$SYSTEM_DIR" == "windows" ]]; then
    EDITOR="$EDITOR.exe"
fi
if [[ ! -x "$EDITOR" ]]; then
    echo "Vulkan smoke executable was not found: $EDITOR" >&2
    exit 1
fi

LOG_DIR="$ROOT/output/test-logs"
LOG_BASE="$LOG_DIR/vulkan-smoke-${SYSTEM_DIR}-${ACTION}"
mkdir -p "$LOG_DIR"

if [[ "$SYSTEM_DIR" == "macosx" ]]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is required for the macOS MoltenVK smoke." >&2
        exit 1
    fi
    VULKAN_LOADER_PREFIX="$(brew --prefix vulkan-loader)"
    MOLTENVK_PREFIX="$(brew --prefix molten-vk)"
    export DYLD_LIBRARY_PATH="$VULKAN_LOADER_PREFIX/lib:$MOLTENVK_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export VK_DRIVER_FILES="$MOLTENVK_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json"
    export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0
    export MVK_CONFIG_USE_MTLHEAP=0
    if [[ ! -f "$VK_DRIVER_FILES" ]]; then
        echo "MoltenVK ICD manifest was not found: $VK_DRIVER_FILES" >&2
        exit 1
    fi
fi

REQUIRED_MARKERS=(
    "NVRHI Vulkan device created on adapter:"
    "Selected Vulkan adapter:"
    "Vulkan capability profile: Phase 3 Vulkan Bootstrap Presentation V1, qualification=Bootstrap"
    "Vulkan capability state: Timeline Synchronization advertised=yes, enabled=yes, implemented=yes"
    "Vulkan capability state: Buffer Device Address advertised="
    "Editor renderer capability diagnostics ready: profile=Phase 3 Vulkan Bootstrap Presentation V1, qualification=Bootstrap"
    "Renderer capability group: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=no"
    "Editor renderer capability group exercised: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps"
    "Renderer initialized with backend: NVRHI Vulkan"
    "Vulkan swapchain and ImGui presentation initialized"
    "Vulkan render smoke requested window resize"
    "Vulkan swapchain recreated after window resize"
    "Vulkan render smoke verified native ImGui presentation after resize"
    "VulkanRHICoreV1"
    "lifecycle=pass, cpuMapNone=pass, markers=executed-balanced"
    "VulkanRHIIndexedDrawV1 package=pass reflection=pass pipeline=pass constants=pass draw=pass submit=pass readback=pass interior=pass background=pass"
    "RHIVertexStrideV1 backend=Vulkan attributes=4 stride=44 fetch=exact result=pass"
    "RHIFixedStructuredBufferV1 backend=Vulkan declaration=exact pixel=t0-space3-uint4 stride=16 malformedReflection=rejected malformedBuffer=rejected missing=rejected wrongUsage=rejected pipelineInvalidation=rejected-stale coexistence=sampled-table-preserved readback=33,82,154,255 expected=33,82,154,255 result=pass"
    "VulkanSceneOutputCaptureV1 outputGeneration="
    "VulkanSceneOutputHandoffV1 producer=pass"
    "ScenePhotometricLightPublicationV1 backend=Vulkan directional=1 local=1 directionalUnit=lux localUnit=lm snapshot=typed grid=typed effectiveExposureEV100=0 exposureScale=1 shaderConsumption=no result=pass"
    "SceneViewportRenderGraphV1 backend=Vulkan passes=4 labels=clear,raster,tone-map,output-handoff execution=pass reference=direct comparator=exact-byte-pass"
    "SceneColorPipelineV1 backend=Vulkan sceneLinear=RGBA16F manualExposureEV100=0"
    "SceneExposureControlV1 backend=Vulkan ev100=-2,0,+2 graph=exact-byte-pass monotonic=pass constants=immutable-retained-cached"
    "calibrated=camera-fnumber-shutter-iso-pass"
    "calibratedCache=same-ev-distinct-settings-pass"
    "ColorPipelineSettingsSmokeV1 default=pass bounds=pass nonfinite=pass v3Migration=pass v4GradingMigration=pass v5SaveReopen=pass historyUndoRedo=pass restoreFailureAtomic=pass rendererPublication=pass"
    "v6Calibration=pass"
    "orderIndependent=pass"
    "roundTripPrecision=pass"
    "ScenePostToneMapGradingV1 backend=Vulkan identity=pass controls=saturation-contrast order=after-tone-map graph=exact-byte-pass constants=immutable-retained-cached result=pass"
    "SceneMeshGpuIntegrationV1 backend=Vulkan snapshot=pass resolver=pass cache=pass indexFormat=UInt32 baseVertex=0"
    "SceneMaterialTextureIntegrationV1 backend=Vulkan material=immutable texture=sRGB-base-color sampler=declared table=bound mips=implicit fallbacks=semantic retained=exact-raster-token result=pass"
    "ProductionRenderGraphRetirementV1 backend=Vulkan"
    "VulkanCompletedSubmissionCollectionV1 collections=8 result=pass"
    "VulkanCompletionHistoryV1 issued="
    "RenderGraphTimestampScopesV1 backend=Vulkan"
    "RendererGpuTimingV1 backend=NVRHI Vulkan"
    "RHITextureUploadSmokeV2 backend=Vulkan, mips=4, bc5Bytes=pass, bc7Bytes=pass, bc7SrgbBytes=pass, finalState=ShaderResource, result=pass"
    "TextureGpuResourceCacheSmokeV1 backend=Vulkan, preferred=pass, reuse=exact, replacement=pass, fallback=RGBA8, shaderResource=pass, cacheCleared=pass, retained=pass, result=pass"
    "TextureRuntimePublicationSmokeV1 backend=Vulkan, catalog=pass, upload=pass, table=pass, replacement=exact-token-pass, removal=exact-token-pass, failure=error-resource, idleRelease=pass, result=pass"
    "SceneMaterialTextureShaderReadbackV1 backend=Vulkan roles=exact-pass colorSpace=sRGB-linear-pass samplers=declared-pass mip1=pass missing=semantic-defaults-pass invalid=error-resource-pass retention=exact-token-pass result=pass"
    "RenderGraphExecutionSmokeV1 backend=Vulkan, barriers=3, callbacks=ordered-pass, undeclared=rejected, submission=pass, topology="
    "RenderGraphTransientAllocationSmokeV1 backend=Vulkan, mode=NonAliasedGpuRetiredPool, lifetime=compatible-sequential-pass, estimatedLogicalAllocatedBytes=64, estimatedLogicalPooledBytes=64, retirement=exact-token-pass, reuse=retired-pass, result=pass"
)

if [[ "$SYSTEM_DIR" == "macosx" ]]; then
    REQUIRED_MARKERS+=(
        "Vulkan portability enumeration enabled"
        "Vulkan portability subset device extension enabled"
        "Vulkan portability subset unsupported features:"
    )
fi

for ((ATTEMPT = 1; ATTEMPT <= ITERATIONS; ++ATTEMPT)); do
    LOG_FILE="$LOG_BASE.log"
    if [[ $ITERATIONS -gt 1 ]]; then
        LOG_FILE="$LOG_BASE-attempt-$ATTEMPT.log"
    fi

    echo "Vulkan render smoke attempt $ATTEMPT/$ITERATIONS"
    set +e
    (cd "$ROOT" && perl -e '
        my $timeout = shift;
        my $child = fork();
        die "fork failed: $!\n" unless defined $child;
        if ($child == 0) {
            setpgrp(0, 0) or die "setpgrp failed: $!\n";
            exec @ARGV or die "exec failed: $!\n";
        }
        $SIG{ALRM} = sub {
            warn "Vulkan smoke child timed out after ${timeout}s; terminating process group\n";
            kill "TERM", -$child;
            sleep 1;
            kill "KILL", -$child;
            waitpid($child, 0);
            exit 124;
        };
        alarm $timeout;
        waitpid($child, 0);
        alarm 0;
        my $status = $?;
        exit(128 + ($status & 127)) if $status & 127;
        exit($status >> 8);
    ' "$CHILD_TIMEOUT_SECONDS" "$EDITOR" --vulkan-render-smoke --renderer-capability-smoke --color-pipeline-settings-smoke --scene-viewport-render-graph-smoke --vulkan-rhi-core-smoke --vulkan-rhi-indexed-draw-smoke --vulkan-scene-viewport-raster-smoke --rhi-buffer-transition-smoke --rhi-completion-smoke --rhi-queue-dependency-smoke --rhi-buffer-ownership-smoke --rhi-texture-ownership-smoke --rhi-resource-ownership-smoke --rhi-resource-state-smoke --rhi-texture-upload-smoke --rhi-sampled-table-smoke --rhi-fixed-structured-buffer-smoke --render-graph-execution-smoke) 2>&1 | tee "$LOG_FILE"
    STATUS=${PIPESTATUS[0]}
    set -e
    if [[ $STATUS -ne 0 ]]; then
        for CRASH_REPORT in "$ROOT"/output/crashes/*.txt; do
            if [[ -f "$CRASH_REPORT" ]]; then
                echo "Vulkan smoke rich crash report: $CRASH_REPORT" >&2
                sed -n '1,240p' "$CRASH_REPORT" >&2
            fi
        done
        for SIGNAL_RECEIPT in "$ROOT"/output/crashes/*.receipt; do
            if [[ -s "$SIGNAL_RECEIPT" ]] && grep -Eq '^SpiralFatalSignalReceiptV1 signal=SIG(ABRT|FPE|ILL|SEGV) disposition=reset-reraise enrichment=none$' "$SIGNAL_RECEIPT"; then
                echo "Vulkan smoke minimal fatal-signal receipt: $SIGNAL_RECEIPT" >&2
                sed -n '1p' "$SIGNAL_RECEIPT" >&2
            elif [[ -s "$SIGNAL_RECEIPT" ]]; then
                echo "Vulkan smoke invalid fatal-signal receipt: $SIGNAL_RECEIPT" >&2
            fi
        done
        echo "Vulkan render smoke failed with exit code $STATUS on attempt $ATTEMPT/$ITERATIONS." >&2
        exit "$STATUS"
    fi

    SURFACE_LOG="$LOG_BASE-surface-basis-attempt-$ATTEMPT.log"
    set +e
    (cd "$ROOT" && perl -e '
        my $timeout = shift;
        my $child = fork();
        die "fork failed: $!\n" unless defined $child;
        if ($child == 0) {
            setpgrp(0, 0) or die "setpgrp failed: $!\n";
            exec @ARGV or die "exec failed: $!\n";
        }
        $SIG{ALRM} = sub {
            warn "Vulkan surface-basis child timed out after ${timeout}s; terminating process group\n";
            kill "TERM", -$child;
            sleep 1;
            kill "KILL", -$child;
            waitpid($child, 0);
            exit 124;
        };
        alarm $timeout;
        waitpid($child, 0);
        alarm 0;
        my $status = $?;
        exit(128 + ($status & 127)) if $status & 127;
        exit($status >> 8);
    ' "$CHILD_TIMEOUT_SECONDS" "$EDITOR" --vulkan-render-smoke --vulkan-scene-viewport-raster-smoke --scene-surface-basis-material-id-smoke) 2>&1 | tee "$SURFACE_LOG"
    SURFACE_STATUS=${PIPESTATUS[0]}
    set -e
    if [[ $SURFACE_STATUS -ne 0 ]]; then
        echo "Dedicated Vulkan surface-basis smoke failed with exit code $SURFACE_STATUS on attempt $ATTEMPT/$ITERATIONS." >&2
        exit "$SURFACE_STATUS"
    fi
    if ! grep -Fq 'SceneSurfaceBasisMaterialIdV1 backend=Vulkan invocation=dedicated productionPSMain=preserved authoredNormal=1,2,3-normalized scale=1.3,0.7,2 rotationDegrees=0,0,23' "$SURFACE_LOG" \
        || ! grep -Eq 'expectedDirection=-?[0-9]+([.][0-9]+)?,-?[0-9]+([.][0-9]+)?,-?[0-9]+([.][0-9]+)? readback=[0-9]+,[0-9]+,[0-9]+ expected=[0-9]+,[0-9]+,[0-9]+ tolerance=4 materialId=1 normalTransform=S\^-1\*R interface=production-scene retention=exact-graph-token result=pass' "$SURFACE_LOG"; then
        echo "Dedicated Vulkan surface-basis smoke did not prove oblique normal transformation and material-ID readback." >&2
        exit 1
    fi

    PBR_LOG="$LOG_BASE-basic-pbr-attempt-$ATTEMPT.log"
    set +e
    (cd "$ROOT" && perl -e '
        my $timeout = shift;
        my $child = fork();
        die "fork failed: $!\n" unless defined $child;
        if ($child == 0) {
            setpgrp(0, 0) or die "setpgrp failed: $!\n";
            exec @ARGV or die "exec failed: $!\n";
        }
        $SIG{ALRM} = sub {
            warn "Vulkan basic-PBR child timed out after ${timeout}s; terminating process group\n";
            kill "TERM", -$child;
            sleep 1;
            kill "KILL", -$child;
            waitpid($child, 0);
            exit 124;
        };
        alarm $timeout;
        waitpid($child, 0);
        alarm 0;
        my $status = $?;
        exit(128 + ($status & 127)) if $status & 127;
        exit($status >> 8);
    ' "$CHILD_TIMEOUT_SECONDS" "$EDITOR" --vulkan-render-smoke --vulkan-scene-viewport-raster-smoke --scene-basic-pbr-material-id-smoke) 2>&1 | tee "$PBR_LOG"
    PBR_STATUS=${PIPESTATUS[0]}
    set -e
    if [[ $PBR_STATUS -ne 0 ]]; then
        echo "Dedicated Vulkan basic-PBR smoke failed with exit code $PBR_STATUS on attempt $ATTEMPT/$ITERATIONS." >&2
        exit "$PBR_STATUS"
    fi
    if ! grep -Fq 'SceneBasicPbrMaterialIdV1 backend=Vulkan productionPSMain=exercised brdf=GGX-Smith-Schlick-Burley materialIds=stable rowZero=error view=per-pixel-view-space lighting=neutral-preview-nonphotometric sceneLights=unconsumed hdr=unclamped retention=exact-graph-token result=pass' "$PBR_LOG"; then
        echo "Dedicated Vulkan basic-PBR smoke did not prove the production BRDF/material-ID contract." >&2
        exit 1
    fi

    for MARKER in "${REQUIRED_MARKERS[@]}"; do
        if ! grep -Fq "$MARKER" "$LOG_FILE"; then
            echo "Vulkan render smoke did not emit required marker on attempt $ATTEMPT/$ITERATIONS: $MARKER" >&2
            exit 1
        fi
    done
    HISTORY_LINE=$(grep -Em1 'VulkanCompletionHistoryV1 issued=[0-9]+ compacted=[0-9]+ live=[0-9]+ failed=[0-9]+ incomplete=[0-9]+ result=pass' "$LOG_FILE" || true)
    if [[ "$HISTORY_LINE" =~ issued=([0-9]+)\ compacted=([0-9]+)\ live=([0-9]+)\ failed=([0-9]+)\ incomplete=([0-9]+) ]]; then
        HISTORY_ISSUED="${BASH_REMATCH[1]}"
        HISTORY_COMPACTED="${BASH_REMATCH[2]}"
        HISTORY_LIVE="${BASH_REMATCH[3]}"
        HISTORY_FAILED="${BASH_REMATCH[4]}"
        HISTORY_INCOMPLETE="${BASH_REMATCH[5]}"
        if (( HISTORY_COMPACTED < 8 || HISTORY_ISSUED != HISTORY_COMPACTED + HISTORY_LIVE || HISTORY_LIVE > 8 || HISTORY_FAILED > HISTORY_COMPACTED || HISTORY_INCOMPLETE > HISTORY_LIVE )); then
            echo "Vulkan completion history marker did not prove bounded internally consistent history on attempt $ATTEMPT/$ITERATIONS." >&2
            exit 1
        fi
    else
        echo "Vulkan render smoke did not publish VulkanCompletionHistoryV1 diagnostics on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    TIMESTAMP_SCOPE_COUNT=$(grep -Ec 'RenderGraphTimestampScopesV1 backend=Vulkan frame=[0-9]+ scopes=4 raw=ready cpuWaitBetween=no result=pass' "$LOG_FILE" || true)
    if [[ "$TIMESTAMP_SCOPE_COUNT" -lt 2 ]]; then
        echo "Vulkan render smoke did not prove completion-gated raw timestamp scopes across consecutive frames on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    GPU_TIMING_COUNT=$(grep -Ec 'RendererGpuTimingV1 backend=NVRHI Vulkan frame=[0-9]+ passes=4 wholeMs=[0-9]+([.][0-9]+)? status=Ready capability=GpuTimestamps result=pass' "$LOG_FILE" || true)
    if [[ "$GPU_TIMING_COUNT" -lt 1 ]]; then
        echo "Vulkan render smoke did not publish exact-frame GPU durations and promote the exercised capability path on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    DIAGNOSTICS_PATTERN='Editor renderer capability diagnostics rendered: profile=Phase 3 Vulkan Bootstrap Presentation V1, adapter=.+, qualification=Bootstrap, formats=[1-9][0-9]*, features=12, groups=2, candidates=[1-9][0-9]*'
    if ! grep -Eq "$DIAGNOSTICS_PATTERN" "$LOG_FILE"; then
        echo "Vulkan render smoke did not emit a complete editor capability diagnostics marker on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'RHICompletionSmokeV1 backend=Vulkan, tokenValidation=pass, query=nonblocking-(incomplete|complete), wait=pass, reuse=pass, result=pass' "$LOG_FILE"; then
        echo "Vulkan render smoke did not prove completion-token retirement and recording reuse on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'RenderGraphExecutionSmokeV1 backend=Vulkan, barriers=3, callbacks=ordered-pass, undeclared=rejected, submission=pass, topology=(independent-copy|graphics-fallback), dependency=(gpu-wait|ordered-elided), readback=pass, reuse=retired-same-context, result=pass' "$LOG_FILE"; then
        echo "Vulkan render smoke did not prove topology-adaptive RenderGraph queue execution, readback, and aggregate retirement on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Fq 'RenderGraphTransientAllocationSmokeV1 backend=Vulkan, mode=NonAliasedGpuRetiredPool, lifetime=compatible-sequential-pass, estimatedLogicalAllocatedBytes=64, estimatedLogicalPooledBytes=64, retirement=exact-token-pass, reuse=retired-pass, result=pass' "$LOG_FILE"; then
        echo "Vulkan render smoke did not prove transient lifetime allocation and exact-token pooled reuse on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'RHIQueueDependencySmokeV1 backend=Vulkan, copy=(independent|graphics-fallback), compute=(independent|graphics-fallback), copyToGraphics=(gpu-wait|ordered-elided), graphicsToCompute=(gpu-wait|ordered-elided), cpuWaitBetween=no, queueLocal=yes, sharedResources=(rejected|permitted-or-elided), retirement=pass, result=pass' "$LOG_FILE"; then
        echo "Vulkan smoke did not prove topology-adaptive queue-local dependency retirement on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'RHIBufferOwnershipSmokeV1 backend=Vulkan, mode=(independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=pass, finalOwner=Copy, finalState=CopySource, recovery=pass, retirement=pass, result=pass|graphics-fallback, transfer=rejected, pending=no, result=pass)' "$LOG_FILE"; then
        echo "Vulkan smoke did not prove topology-adaptive buffer ownership transfer or truthful fallback rejection on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'RHITextureOwnershipSmokeV1 backend=Vulkan, mode=(independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=pass, finalOwner=Copy, finalState=CopySource, recovery=pass, retirement=pass, result=pass|graphics-fallback, transfer=rejected, pending=no, result=pass)' "$LOG_FILE"; then
        echo "Vulkan smoke did not prove topology-adaptive texture ownership transfer or truthful fallback rejection on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'VulkanSceneOutputCaptureV1 outputGeneration=[2-9][0-9]* capture=pass' "$LOG_FILE"; then
        echo "Vulkan render smoke did not capture the post-resize renderer-owned Scene output on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'VulkanSceneOutputHandoffV1 producer=pass outputGeneration=[2-9][0-9]* descriptor=registered descriptorGeneration=[2-9][0-9]* imgui=queued present=pass swapchainGeneration=[2-9][0-9]*' "$LOG_FILE"; then
        echo "Vulkan render smoke did not prove post-resize ImGui consumption and swapchain presentation on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
done

echo "Vulkan render smoke passed $ITERATIONS iteration(s): $CONFIGURATION ($ACTION)"
