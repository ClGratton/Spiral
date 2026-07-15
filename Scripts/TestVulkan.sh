#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Debug}"
ACTION="${2:-gmake}"
BUILD_MODE="${3:-build}"
ITERATIONS="${VULKAN_SMOKE_ITERATIONS:-1}"
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
    "Editor renderer capability group exercised: group=Phase3FrameTimingV1, profile=Phase 3 Frame Timing V1, preferredPath=GpuTimestamps, selectedPath=CpuSteadyClock, implemented=yes, exercised=yes, qualification=Presentation, deviceQualification=Bootstrap"
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
    (cd "$ROOT" && "$EDITOR" --vulkan-render-smoke --renderer-capability-smoke --vulkan-rhi-core-smoke --vulkan-rhi-indexed-draw-smoke --rhi-completion-smoke --rhi-buffer-ownership-smoke) 2>&1 | tee "$LOG_FILE"
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
    DIAGNOSTICS_PATTERN='Editor renderer capability diagnostics rendered: profile=Phase 3 Vulkan Bootstrap Presentation V1, adapter=.+, qualification=Bootstrap, formats=[1-9][0-9]*, features=9, groups=1, candidates=[1-9][0-9]*'
    if ! grep -Eq "$DIAGNOSTICS_PATTERN" "$LOG_FILE"; then
        echo "Vulkan render smoke did not emit a complete editor capability diagnostics marker on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Eq 'RHICompletionSmokeV1 backend=Vulkan, tokenValidation=pass, query=nonblocking-(incomplete|complete), wait=pass, reuse=pass, result=pass' "$LOG_FILE"; then
        echo "Vulkan render smoke did not prove completion-token retirement and recording reuse on attempt $ATTEMPT/$ITERATIONS." >&2
        exit 1
    fi
    if ! grep -Fq 'RHIBufferOwnershipSmokeV1 backend=Vulkan, mode=graphics-fallback, transfer=rejected, pending=no, result=pass' "$LOG_FILE"; then
        echo "Vulkan smoke did not reject buffer ownership transfer without publishing pending state on attempt $ATTEMPT/$ITERATIONS." >&2
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
