#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "Python 3 was not found on PATH. Install Python 3 to run the code style check." >&2
    exit 1
fi

"$PYTHON" "$ROOT/tools/check_code_style.py"
