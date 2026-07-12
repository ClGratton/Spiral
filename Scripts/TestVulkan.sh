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

set +e
(cd "$ROOT" && "$EDITOR" --vulkan-render-smoke) 2>&1 | tee "$LOG_FILE"
STATUS=${PIPESTATUS[0]}
set -e
if [[ $STATUS -ne 0 ]]; then
    echo "Vulkan render smoke failed with exit code $STATUS." >&2
    exit "$STATUS"
fi

REQUIRED_MARKERS=(
    "NVRHI Vulkan device created on adapter:"
    "Renderer initialized with backend: NVRHI Vulkan"
    "Vulkan swapchain and ImGui presentation initialized"
    "Vulkan render smoke requested window resize"
    "Vulkan render smoke verified native ImGui presentation after resize"
)

for MARKER in "${REQUIRED_MARKERS[@]}"; do
    if ! grep -Fq "$MARKER" "$LOG_FILE"; then
        echo "Vulkan render smoke did not emit required marker: $MARKER" >&2
        exit 1
    fi
done

echo "Vulkan render smoke passed: $CONFIGURATION ($ACTION)"
