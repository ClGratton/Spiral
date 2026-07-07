#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Debug}"
ACTION="${2:-gmake}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

bash "$ROOT/Scripts/Build.sh" "$CONFIGURATION" "$ACTION"

SYSTEM_NAME="$(uname -s)"
case "$SYSTEM_NAME" in
    Linux*) SYSTEM_DIR="linux" ;;
    Darwin*) SYSTEM_DIR="macosx" ;;
    MINGW*|MSYS*|CYGWIN*) SYSTEM_DIR="windows" ;;
    *) echo "Unsupported OS: $SYSTEM_NAME" >&2; exit 1 ;;
esac

SUFFIX=""
if [[ "$ACTION" != "vs2022" ]]; then
    SUFFIX="-$ACTION"
fi

EXE="$ROOT/bin/${CONFIGURATION}-${SYSTEM_DIR}-x86_64${SUFFIX}/Sandbox/Sandbox"
if [[ "$SYSTEM_DIR" == "windows" ]]; then
    EXE="$EXE.exe"
fi

"$EXE"
