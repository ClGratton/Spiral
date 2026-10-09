#!/bin/bash
# Thin launcher for scene benchmark: invokes BenchmarkScene.py with appropriate flags.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel 2>/dev/null || cd "$script_dir/.." && pwd)"
python_script="${script_dir}/BenchmarkScene.py"

if [[ ! -f "$python_script" ]]; then
    echo "Python script not found: $python_script" >&2
    exit 1
fi

# Check for --selftest first, before checking Editor existence
for arg in "$@"; do
    if [[ "$arg" == "--selftest" ]]; then
        exec python3 "$python_script" --selftest
    fi
done

editor="${repo_root}/bin/Debug-linux-x86_64-gmake/Editor/Editor"
output_dir="${repo_root}/output/benchmarks/$(date +%s)"

if [[ ! -x "$editor" ]]; then
    echo "Editor not found or not executable: $editor" >&2
    exit 1
fi

mkdir -p "$(dirname "$output_dir")"

# Default arguments
duration=70
fps=60
project=""

# Parse command-line arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)
            duration="$2"
            shift 2
            ;;
        --fps)
            fps="$2"
            shift 2
            ;;
        --project)
            project="$2"
            shift 2
            ;;
        --output)
            output_dir="$2"
            shift 2
            ;;
        --selftest)
            shift 1
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# Invoke Python benchmark
python3 "$python_script" \
    --editor "$editor" \
    --output "$output_dir" \
    --duration "$duration" \
    --fps "$fps" \
    ${project:+--project "$project"}
