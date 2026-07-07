#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Debug}"
ACTION="${2:-gmake}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$ROOT/Scripts/Build.sh" "$CONFIGURATION" "$ACTION"

EDITOR="$(find "$ROOT/bin" -path "*/Editor/Editor" -type f | sort | head -n 1 || true)"
if [[ -z "$EDITOR" ]]; then
    echo "Editor executable was not found after build." >&2
    exit 1
fi

"$EDITOR" --headless --smoke-test

cat <<'MESSAGE'
Render smoke passed for the portable headless path.
Native image capture is currently implemented for the Windows/MSVC D3D12 path.
Use Scripts/TestRender.ps1 on Windows until the Vulkan viewport backend lands.
MESSAGE
