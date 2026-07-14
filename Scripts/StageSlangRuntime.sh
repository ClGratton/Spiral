#!/usr/bin/env bash
set -euo pipefail

source_path="$1"
destination="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[[ -f "$source_path/include/slang.h" && -f "$source_path/LICENSE" && -f "$source_path/.spiral-package-manifest" ]] || { echo "Pinned Slang package is unavailable or lacks its installed manifest at '$source_path'. Run Scripts/Setup.sh first." >&2; exit 1; }
mkdir -p "$destination"

shopt -s nullglob
case "$(uname -s)" in
    Linux*)
        proxy_files=("$source_path"/lib/libslang.so "$source_path"/lib/libslang.so.*)
        compiler_files=("$source_path"/lib/libslang-compiler.so "$source_path"/lib/libslang-compiler.so.*)
        optimizer_files=("$source_path"/lib/libslang-glslang*.so*)
        ;;
    Darwin*)
        proxy_files=("$source_path"/lib/libslang.dylib "$source_path"/lib/libslang.*.dylib)
        compiler_files=("$source_path"/lib/libslang-compiler.dylib "$source_path"/lib/libslang-compiler.*.dylib)
        optimizer_files=("$source_path"/lib/libslang-glslang*.dylib)
        ;;
    *) echo "StageSlangRuntime.sh supports the Linux and macOS Slang packages; use StageSlangRuntime.ps1 on Windows." >&2; exit 1 ;;
esac
[[ ${#proxy_files[@]} -gt 0 && ${#compiler_files[@]} -gt 0 && ${#optimizer_files[@]} -gt 0 ]] || { echo "Pinned Slang package at '$source_path' lacks the exact proxy/compiler/SPIR-V-optimizer shared runtime closure." >&2; exit 1; }
runtime_files=("${proxy_files[@]}" "${compiler_files[@]}" "${optimizer_files[@]}")
module_directories=("$source_path"/lib/slang-standard-module-*)
[[ ${#module_directories[@]} -eq 1 && -d "${module_directories[0]}" ]] || { echo "Pinned Slang package at '$source_path' must contain exactly one standard module directory." >&2; exit 1; }
[[ -f "${module_directories[0]}/experimental/workgraph.slang-module" && -f "${module_directories[0]}/slang/neural.slang-module" ]] || { echo "Pinned Slang standard module is incomplete." >&2; exit 1; }

rm -f "$destination"/libgfx* "$destination"/libslang-glsl-module* "$destination"/libslang-glslang* "$destination"/libslang-llvm* "$destination"/libslang-rt* \
    "$destination"/libslang.so* "$destination"/libslang-compiler.so* "$destination"/libslang.dylib "$destination"/libslang.*.dylib \
    "$destination"/libslang-compiler.dylib "$destination"/libslang-compiler.*.dylib "$destination"/gfx.slang "$destination"/slang.slang \
    "$destination"/Slang-LICENSE.txt "$destination"/Slang-THIRD_PARTY_NOTICE.md "$destination"/ShaderToolchainRuntimeManifest.txt
rm -rf "$destination"/slang-standard-module-*

cp -pPR "${runtime_files[@]}" "$destination/"
cp -pPR "${module_directories[0]}" "$destination/"
cp -f "$source_path/LICENSE" "$destination/Slang-LICENSE.txt"
cp -f "$root/Vendor/Slang/THIRD_PARTY_NOTICE.md" "$destination/Slang-THIRD_PARTY_NOTICE.md"

slang_hash="$(sed -n 's/^sha256=//p' "$source_path/.spiral-package-manifest")"
[[ -n "$slang_hash" ]] || { echo "Pinned Slang installed manifest has no archive hash." >&2; exit 1; }
cat > "$destination/ShaderToolchainRuntimeManifest.txt" <<EOF
format=1
slang_sha256=$slang_hash
slang_runtime=$(printf '%s,' "${runtime_files[@]##*/}" | sed 's/,$//'),${module_directories[0]##*/}
notices=Slang-LICENSE.txt,Slang-THIRD_PARTY_NOTICE.md
distribution_status=blocked-pending-slang-binary-notice-audit
EOF
