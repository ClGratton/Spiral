#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

PIN_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ShaderToolchainPins.env"
# shellcheck source=ShaderToolchainPins.env
source "$PIN_FILE"
[[ "$SHADER_TOOLCHAIN_PIN_FORMAT" == "1" ]] || { echo "Unsupported shader toolchain pin format." >&2; exit 1; }
SLANG_RELEASE_URL="https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLANG_ROOT="$ROOT/Vendor/Slang/v${SLANG_VERSION}"
CACHE_ROOT="$ROOT/Vendor/Slang/.cache"

host_platform=""
host_architecture=""
force=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-platform) host_platform="$2"; shift 2 ;;
        --host-architecture) host_architecture="$2"; shift 2 ;;
        --force) force=true; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$host_platform" ]]; then
    case "$(uname -s)" in
        Linux*) host_platform="linux" ;;
        Darwin*) host_platform="macos" ;;
        MINGW*|MSYS*|CYGWIN*) host_platform="windows" ;;
        *) echo "Unsupported host operating system: $(uname -s)" >&2; exit 1 ;;
    esac
fi

if [[ -z "$host_architecture" ]]; then
    case "$(uname -m)" in
        x86_64|amd64) host_architecture="x86_64" ;;
        arm64|aarch64) host_architecture="aarch64" ;;
        *) echo "Unsupported host architecture: $(uname -m)" >&2; exit 1 ;;
    esac
fi

package_key="${host_platform}-${host_architecture}"
pin_prefix="SLANG_$(printf '%s' "$package_key" | tr '[:lower:]-' '[:upper:]_')"
archive_variable="${pin_prefix}_ARCHIVE"
hash_variable="${pin_prefix}_SHA256"
archive_name="${!archive_variable:-}"
expected_sha256="${!hash_variable:-}"
[[ -n "$archive_name" && -n "$expected_sha256" ]] || { echo "No pinned Slang package is declared for $package_key." >&2; exit 1; }

destination="$SLANG_ROOT/$package_key"
archive="$CACHE_ROOT/$archive_name"
manifest_name=".spiral-package-manifest"
expected_manifest() {
    printf '%s\n' \
        "format=1" \
        "name=Slang" \
        "version=$SLANG_VERSION" \
        "package=$package_key" \
        "archive=$archive_name" \
        "sha256=$expected_sha256"
}
test_package() {
    local path="$1" module_directory
    [[ -f "$path/include/slang.h" && -f "$path/LICENSE" && -f "$path/$manifest_name" ]] || return 1
    diff -u <(expected_manifest) "$path/$manifest_name" >/dev/null || return 1
    if [[ "$host_platform" == "windows" ]]; then
        module_directory="bin/slang-standard-module-$SLANG_VERSION"
        [[ -f "$path/lib/slang.lib" && -f "$path/bin/slang.dll" && -f "$path/bin/slang-compiler.dll" && -f "$path/bin/slang-glslang.dll" && -f "$path/$module_directory/experimental/workgraph.slang-module" && -f "$path/$module_directory/slang/neural.slang-module" ]]
    elif [[ "$host_platform" == "linux" ]]; then
        module_directory="lib/slang-standard-module-$SLANG_VERSION"
        glslang_files=("$path"/lib/libslang-glslang*.so*)
        [[ -f "$path/lib/libslang.so" && -f "$path/lib/libslang-compiler.so" && ${#glslang_files[@]} -gt 0 && -f "$path/$module_directory/experimental/workgraph.slang-module" && -f "$path/$module_directory/slang/neural.slang-module" ]]
    else
        module_directory="lib/slang-standard-module-$SLANG_VERSION"
        glslang_files=("$path"/lib/libslang-glslang*.dylib)
        [[ -f "$path/lib/libslang.dylib" && -f "$path/lib/libslang-compiler.dylib" && ${#glslang_files[@]} -gt 0 && -f "$path/$module_directory/experimental/workgraph.slang-module" && -f "$path/$module_directory/slang/neural.slang-module" ]]
    fi
}

if test_package "$destination" && [[ "$force" != true ]]; then
    echo "Pinned Slang $SLANG_VERSION is already staged at $destination"
    exit 0
fi

mkdir -p "$CACHE_ROOT"
if [[ ! -f "$archive" || "$force" == true ]]; then
    echo "Downloading pinned Slang $SLANG_VERSION package for $package_key..."
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location "$SLANG_RELEASE_URL/$archive_name" --output "$archive"
    elif command -v wget >/dev/null 2>&1; then
        wget "$SLANG_RELEASE_URL/$archive_name" --output-document="$archive"
    else
        echo "curl or wget is required to download Slang." >&2
        exit 1
    fi
fi

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    actual_sha256="$(shasum -a 256 "$archive" | awk '{print $1}')"
else
    echo "sha256sum or shasum is required to verify the Slang archive." >&2
    exit 1
fi
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    rm -f "$archive"
    echo "Slang package hash mismatch for $archive_name; the archive was removed." >&2
    exit 1
fi

temporary="$(mktemp -d "${TMPDIR:-/tmp}/spiral-slang-${SLANG_VERSION}-${package_key}-XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
# shellcheck source=archive_safety.sh
source "$ROOT/Scripts/archive_safety.sh"
if [[ "$archive_name" == *.zip ]]; then
    assert_safe_zip_archive "$archive"
    unzip -q "$archive" -d "$temporary"
else
    assert_safe_tar_archive "$archive"
    tar -xzf "$archive" -C "$temporary"
fi

candidate_list="$(find "$temporary" -type f -path '*/include/slang.h' -exec dirname {} \; | sed 's#/include$##' | sort -u)"
candidate_count="$(printf '%s\n' "$candidate_list" | sed '/^$/d' | wc -l | tr -d ' ')"
if [[ "$candidate_count" != "1" ]]; then
    echo "Pinned Slang package has an unexpected layout or lacks its LICENSE notice." >&2
    exit 1
fi
package_root="$candidate_list"
[[ -f "$package_root/LICENSE" ]] || { echo "Pinned Slang package has an unexpected layout or lacks its LICENSE notice." >&2; exit 1; }

staging="${destination}.staging-$$"
mkdir -p "$(dirname "$destination")"
mv "$package_root" "$staging"
expected_manifest > "$staging/$manifest_name"
test_package "$staging" || { echo "Pinned Slang package is missing a required header, import library, runtime library, or standard module." >&2; exit 1; }
[[ "$destination" == "$SLANG_ROOT/"* ]] || { echo "Refusing to replace an unsafe destination." >&2; exit 1; }
rm -rf "$destination"
mv "$staging" "$destination"
test_package "$destination" || { echo "Slang staging verification failed at $destination." >&2; exit 1; }
echo "Pinned Slang $SLANG_VERSION staged at $destination"
