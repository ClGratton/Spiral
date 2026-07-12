#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Debug}"
ACTION="${2:-gmake}"
BUILD_MODE="${3:-build}"
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
LOG_FILE="$LOG_DIR/vulkan-smoke-${SYSTEM_DIR}-${ACTION}.log"
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
    if [[ ! -f "$VK_DRIVER_FILES" ]]; then
        echo "MoltenVK ICD manifest was not found: $VK_DRIVER_FILES" >&2
        exit 1
    fi
fi

set +e
(cd "$ROOT" && "$EDITOR" --vulkan-render-smoke) 2>&1 | tee "$LOG_FILE"
STATUS=${PIPESTATUS[0]}
set -e
if [[ $STATUS -ne 0 ]]; then
    for CRASH_REPORT in "$ROOT"/output/crashes/*.txt; do
        if [[ -f "$CRASH_REPORT" ]]; then
            echo "Vulkan smoke crash report: $CRASH_REPORT" >&2
            sed -n '1,240p' "$CRASH_REPORT" >&2
        fi
    done
    echo "Vulkan render smoke failed with exit code $STATUS." >&2
    exit "$STATUS"
fi

REQUIRED_MARKERS=(
    "NVRHI Vulkan device created on adapter:"
    "Renderer initialized with backend: NVRHI Vulkan"
    "Vulkan swapchain and ImGui presentation initialized"
    "Vulkan render smoke requested window resize"
    "Vulkan swapchain recreated after window resize"
    "Vulkan render smoke verified native ImGui presentation after resize"
)

if [[ "$SYSTEM_DIR" == "macosx" ]]; then
    REQUIRED_MARKERS+=(
        "Vulkan portability enumeration enabled"
        "Vulkan portability subset device extension enabled"
    )
fi

for MARKER in "${REQUIRED_MARKERS[@]}"; do
    if ! grep -Fq "$MARKER" "$LOG_FILE"; then
        echo "Vulkan render smoke did not emit required marker: $MARKER" >&2
        exit 1
    fi
done

echo "Vulkan render smoke passed: $CONFIGURATION ($ACTION)"
