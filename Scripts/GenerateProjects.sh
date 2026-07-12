#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-gmake}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

bash "$ROOT/Scripts/Setup.sh"

PREMAKE_EXE="$ROOT/Vendor/premake/bin/premake5"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) PREMAKE_EXE="$ROOT/Vendor/premake/bin/premake5.exe" ;;
esac

echo "Generating projects with Premake action '$ACTION'..."
"$PREMAKE_EXE" --file="$ROOT/premake5.lua" "$ACTION"
