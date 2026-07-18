#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "$ROOT/Scripts/Setup.sh"
PREMAKE="$ROOT/Vendor/premake/bin/premake5"
"$PREMAKE" --file="$ROOT/premake5.lua" --cc=clang --sanitize=address-undefined gmake
make -C "$ROOT" config=debug EngineTests EngineFuzzTests

export ASAN_SYMBOLIZER_PATH="${ASAN_SYMBOLIZER_PATH:-$(command -v llvm-symbolizer || true)}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-symbolize=1:detect_leaks=1:halt_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
mkdir -p "$ROOT/output/test-results" "$ROOT/output/fuzz-failures"

ENGINE_TESTS="$ROOT/bin/Debug-linux-x86_64-gmake-asan-ubsan/EngineTests/EngineTests"
ENGINE_FUZZ="$ROOT/bin/Debug-linux-x86_64-gmake-asan-ubsan/EngineFuzzTests/EngineFuzzTests"
"$ENGINE_TESTS" --tier integration --report-json "$ROOT/output/test-results/engine-tests-asan-ubsan.json"
"$ENGINE_FUZZ" -runs=512 -artifact_prefix="$ROOT/output/fuzz-failures/" "$ROOT/Tests/Corpus/Fuzz"
echo "SanitizerLaneV1 mode=asan-ubsan engineTests=pass structuredFuzz=pass runs=512 vendorInstrumentation=excluded result=pass"
