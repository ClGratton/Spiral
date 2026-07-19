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
    "VulkanSceneOutputCaptureV1 outputGeneration="
    "VulkanSceneOutputHandoffV1 producer=pass"
    "SceneViewportRenderGraphV1 backend=Vulkan passes=3 labels=clear,raster,output-handoff execution=pass reference=direct comparator=exact-byte-pass"
    "SceneMeshGpuIntegrationV1 backend=Vulkan snapshot=pass resolver=pass cache=pass indexFormat=UInt32 baseVertex=0"
    "ProductionRenderGraphRetirementV1 backend=Vulkan"
    "RenderGraphTimestampScopesV1 backend=Vulkan"
    "RendererGpuTimingV1 backend=NVRHI Vulkan"
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
    ' "$CHILD_TIMEOUT_SECONDS" "$EDITOR" --vulkan-render-smoke --renderer-capability-smoke --scene-viewport-render-graph-smoke --vulkan-rhi-core-smoke --vulkan-rhi-indexed-draw-smoke --vulkan-scene-viewport-raster-smoke --rhi-buffer-transition-smoke --rhi-completion-smoke --rhi-queue-dependency-smoke --rhi-buffer-ownership-smoke --rhi-texture-ownership-smoke --rhi-resource-ownership-smoke --rhi-resource-state-smoke --rhi-texture-upload-smoke --render-graph-execution-smoke) 2>&1 | tee "$LOG_FILE"
    STATUS=${PIPESTATUS[0]}
    set -e
    if [[ $STATUS -ne 0 ]]; then
        for CRASH_REPORT in "$ROOT"/output/crashes/*.txt; do
            if [[ -f "$CRASH_REPORT" ]]; then
                echo "Vulkan smoke crash report: $CRASH_REPORT" >&2
                sed -n '1,240p' "$CRASH_REPORT" >&2
            fi
        done
        echo "Vulkan render smoke failed with exit code $STATUS on attempt $ATTEMPT/$ITERATIONS." >&2
        exit "$STATUS"
    fi

    for MARKER in "${REQUIRED_MARKERS[@]}"; do
        if ! grep -Fq "$MARKER" "$LOG_FILE"; then
            echo "Vulkan render smoke did not emit required marker on attempt $ATTEMPT/$ITERATIONS: $MARKER" >&2
            exit 1
        fi
    done
    TIMESTAMP_SCOPE_COUNT=$(grep -Ec 'RenderGraphTimestampScopesV1 backend=Vulkan frame=[0-9]+ scopes=3 raw=ready cpuWaitBetween=no result=pass' "$LOG_FILE" || true)
    if [[ "$TIMESTAMP_SCOPE_COUNT" -lt 2 ]]; then
        echo "Vulkan render smoke did not prove completion-gated raw timestamp scopes across consecutive frames on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    GPU_TIMING_COUNT=$(grep -Ec 'RendererGpuTimingV1 backend=NVRHI Vulkan frame=[0-9]+ passes=3 wholeMs=[0-9]+([.][0-9]+)? status=Ready capability=GpuTimestamps result=pass' "$LOG_FILE" || true)
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
