#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Debug}"
ACTION="${2:-gmake}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$ROOT/Scripts/GenerateProjects.sh" "$ACTION"

CONFIG_LOWER="$(echo "$CONFIGURATION" | tr '[:upper:]' '[:lower:]')"

if command -v make >/dev/null 2>&1; then
    make -C "$ROOT" "config=$CONFIG_LOWER"
else
    echo "make was not found. Install GNU Make or use a generated IDE project." >&2
    exit 1
fi

echo "Build complete: $CONFIGURATION ($ACTION)"
