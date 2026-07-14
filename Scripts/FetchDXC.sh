#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIN_FILE="$ROOT/Scripts/ShaderToolchainPins.env"
# shellcheck source=ShaderToolchainPins.env
source "$PIN_FILE"
[[ "$SHADER_TOOLCHAIN_PIN_FORMAT" == "1" ]] || { echo "Unsupported shader toolchain pin format." >&2; exit 1; }
ARCHIVE_NAME="$DXC_WINDOWS_X86_64_ARCHIVE"
EXPECTED_SHA256="$DXC_WINDOWS_X86_64_SHA256"
DXC_ROOT="$ROOT/Vendor/DXC/v${DXC_VERSION}"
CACHE_ROOT="$ROOT/Vendor/DXC/.cache"
DESTINATION="$DXC_ROOT/windows-x86_64"
ARCHIVE="$CACHE_ROOT/$ARCHIVE_NAME"
RELEASE_URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${DXC_VERSION}/${ARCHIVE_NAME}"
MANIFEST_NAME=".spiral-package-manifest"
expected_manifest() {
    printf '%s\n' \
        "format=1" \
        "name=DXC" \
        "version=$DXC_VERSION" \
        "package=windows-x86_64" \
        "archive=$ARCHIVE_NAME" \
        "sha256=$EXPECTED_SHA256"
}

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "The admitted DXC package is currently Windows x86_64 only. Other hosts remain unqualified." >&2; exit 1 ;;
esac
[[ "$(uname -m)" == "x86_64" ]] || { echo "The admitted DXC package is currently Windows x86_64 only." >&2; exit 1; }

test_payload() {
    [[ -f "$1/bin/x64/dxcompiler.dll" && -f "$1/bin/x64/dxil.dll" && -f "$1/LICENSE-LLVM.txt" && -f "$1/LICENSE-MIT.txt" && -f "$1/LICENSE-MS.txt" ]]
}
test_package() {
    test_payload "$1" && [[ -f "$1/$MANIFEST_NAME" ]] || return 1
    diff -u <(expected_manifest) "$1/$MANIFEST_NAME" >/dev/null
}
if test_package "$DESTINATION"; then
    echo "Pinned DXC v$DXC_VERSION is already staged at $DESTINATION"
    exit 0
fi

mkdir -p "$CACHE_ROOT"
if [[ ! -f "$ARCHIVE" ]]; then
    echo "Downloading pinned DXC v$DXC_VERSION package for Windows x86_64..."
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location "$RELEASE_URL" --output "$ARCHIVE"
    elif command -v wget >/dev/null 2>&1; then
        wget "$RELEASE_URL" --output-document="$ARCHIVE"
    else
        echo "curl or wget is required to download DXC." >&2
        exit 1
    fi
fi

if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL_SHA256="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
else
    echo "sha256sum or shasum is required to verify the DXC archive." >&2
    exit 1
fi
if [[ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]]; then
    rm -f "$ARCHIVE"
    echo "DXC package hash mismatch; the archive was removed." >&2
    exit 1
fi

TEMPORARY="$(mktemp -d "${TMPDIR:-/tmp}/spiral-dxc-${DXC_VERSION}-XXXXXX")"
trap 'rm -rf "$TEMPORARY"' EXIT
# shellcheck source=archive_safety.sh
source "$ROOT/Scripts/archive_safety.sh"
assert_safe_zip_archive "$ARCHIVE"
unzip -q "$ARCHIVE" -d "$TEMPORARY"
PACKAGE_ROOT="$TEMPORARY"
test_payload "$PACKAGE_ROOT" || { echo "Pinned DXC package is missing compiler, validator, or its LLVM/MIT/Microsoft license notices." >&2; exit 1; }

STAGING="${DESTINATION}.staging-$$"
mkdir -p "$(dirname "$DESTINATION")"
mv "$PACKAGE_ROOT" "$STAGING"
expected_manifest > "$STAGING/$MANIFEST_NAME"
test_package "$STAGING" || { echo "Pinned DXC package failed installed-manifest validation." >&2; exit 1; }
rm -rf "$DESTINATION"
mv "$STAGING" "$DESTINATION"
test_package "$DESTINATION" || { echo "DXC staging verification failed." >&2; exit 1; }
echo "Pinned DXC v$DXC_VERSION staged at $DESTINATION"
