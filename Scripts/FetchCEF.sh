#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

PIN_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/BrowserRuntimePins.env"
# shellcheck source=BrowserRuntimePins.env
source "$PIN_FILE"
[[ "$BROWSER_RUNTIME_PIN_FORMAT" == "1" ]] || { echo "Unsupported browser runtime pin format." >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CEF_ROOT="$ROOT/Vendor/CEF/v${CEF_VERSION}"
CACHE_ROOT="$ROOT/Vendor/CEF/.cache"

host_platform=""
host_architecture=""
force=false
keep_archive=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-platform) host_platform="$2"; shift 2 ;;
        --host-architecture) host_architecture="$2"; shift 2 ;;
        --force) force=true; shift ;;
        --keep-archive) keep_archive=true; shift ;;
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
pin_prefix="CEF_$(printf '%s' "$package_key" | tr '[:lower:]-' '[:upper:]_')"
archive_variable="${pin_prefix}_ARCHIVE"
hash_variable="${pin_prefix}_SHA256"
size_variable="${pin_prefix}_SIZE"
prestrip_variable="${pin_prefix}_LIBCEF_PRESTRIP_SHA256"
archive_name="${!archive_variable:-}"
archive_size="${!size_variable:-0}"
expected_sha256="${!hash_variable:-}"
expected_prestrip_sha256="${!prestrip_variable:-}"
[[ -n "$archive_name" && -n "$expected_sha256" ]] || { echo "No pinned CEF package is declared for $package_key." >&2; exit 1; }
if [[ "$host_platform" == "linux" ]]; then
    [[ -n "$expected_prestrip_sha256" ]] || { echo "The pinned Linux CEF package declares no libcef.so pre-strip hash." >&2; exit 1; }
fi

destination="$CEF_ROOT/$package_key"
archive="$CACHE_ROOT/$archive_name"
manifest_name=".spiral-package-manifest"
expected_manifest() {
    printf '%s\n' \
        "format=1" \
        "name=CEF" \
        "version=$CEF_VERSION" \
        "build=$CEF_BUILD" \
        "package=$package_key" \
        "archive=$archive_name" \
        "sha256=$expected_sha256"
    if [[ "$host_platform" == "linux" ]]; then
        printf '%s\n' "libcef_prestrip_sha256=$expected_prestrip_sha256"
    fi
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "sha256sum or shasum is required to verify the CEF archive." >&2
        return 1
    fi
}

# The installed libcef.so is the stripped file; its hash is the final manifest
# line and is rechecked on every idempotent run so an edited library is repaired.
test_package() {
    local path="$1" expected_lines actual_lines recorded
    [[ -f "$path/include/cef_version.h" && -f "$path/include/cef_app.h" && -d "$path/libcef_dll" && -f "$path/LICENSE.txt" && -f "$path/CREDITS.html" && -f "$path/$manifest_name" ]] || return 1
    grep -q "^#define CEF_VERSION \"$CEF_BUILD\"\$" "$path/include/cef_version.h" || return 1
    expected_lines="$(expected_manifest | wc -l | tr -d ' ')"
    diff -u <(expected_manifest) <(head -n "$expected_lines" "$path/$manifest_name") >/dev/null || return 1
    if [[ "$host_platform" == "windows" ]]; then
        [[ -f "$path/Release/libcef.dll" && -f "$path/Release/libcef.lib" && -f "$path/Release/v8_context_snapshot.bin" && -f "$path/Resources/icudtl.dat" && -f "$path/Resources/resources.pak" && -f "$path/Resources/locales/en-US.pak" ]]
    elif [[ "$host_platform" == "linux" ]]; then
        [[ -f "$path/Release/libcef.so" && -f "$path/Release/v8_context_snapshot.bin" && -f "$path/Resources/icudtl.dat" && -f "$path/Resources/resources.pak" && -f "$path/Resources/chrome_100_percent.pak" && -f "$path/Resources/locales/en-US.pak" && ! -e "$path/Release/chrome-sandbox" ]] || return 1
        actual_lines="$(wc -l < "$path/$manifest_name" | tr -d ' ')"
        [[ "$actual_lines" == "$((expected_lines + 1))" ]] || return 1
        recorded="$(sed -n 's/^libcef_sha256=//p' "$path/$manifest_name")"
        [[ -n "$recorded" && "$(sha256_of "$path/Release/libcef.so")" == "$recorded" ]]
    else
        return 1
    fi
}

if test_package "$destination" && [[ "$force" != true ]]; then
    echo "Pinned CEF $CEF_VERSION is already staged at $destination"
    exit 0
fi

mkdir -p "$CACHE_ROOT"
if [[ ! -f "$archive" || "$force" == true ]]; then
    echo "Downloading pinned CEF $CEF_VERSION package for $package_key (about $((archive_size / 1000000)) MB)..."
    partial="$archive.part-$$"
    rm -f "$partial"
    # The CDN names contain '+', which must be percent-encoded in the URL path.
    url="$CEF_BASE_URL/${archive_name//+/%2B}"
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --proto '=https' --tlsv1.2 --retry 3 "$url" --output "$partial" || { rm -f "$partial"; echo "CEF download failed." >&2; exit 1; }
    elif command -v wget >/dev/null 2>&1; then
        wget --https-only "$url" --output-document="$partial" || { rm -f "$partial"; echo "CEF download failed." >&2; exit 1; }
    else
        echo "curl or wget is required to download CEF." >&2
        exit 1
    fi
    mv -f "$partial" "$archive"
fi

actual_sha256="$(sha256_of "$archive")"
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    rm -f "$archive"
    echo "CEF package hash mismatch for $archive_name; the archive was removed. Expected $expected_sha256, got $actual_sha256." >&2
    exit 1
fi

# Extract beside the destination, not in a possibly RAM-backed /tmp: the minimal
# distribution is about 1.5 GB before libcef.so is stripped.
temporary="$(mktemp -d "$CACHE_ROOT/extract-${package_key}-XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
# shellcheck source=archive_safety.sh
source "$ROOT/Scripts/archive_safety.sh"
assert_safe_tar_archive "$archive" j
tar -xjf "$archive" --no-same-owner -C "$temporary"

if [[ -n "$(find "$temporary" -perm /6000 -print -quit)" ]]; then
    echo "Pinned CEF package contains a setuid or setgid file; refusing to install it." >&2
    exit 1
fi

candidate_list="$(find "$temporary" -type f -path '*/include/cef_version.h' -exec dirname {} \; | sed 's#/include$##' | sort -u)"
candidate_count="$(printf '%s\n' "$candidate_list" | sed '/^$/d' | wc -l | tr -d ' ')"
if [[ "$candidate_count" != "1" ]]; then
    echo "Pinned CEF package has an unexpected layout." >&2
    exit 1
fi
package_root="$candidate_list"
[[ -f "$package_root/LICENSE.txt" && -f "$package_root/CREDITS.html" ]] || { echo "Pinned CEF package lacks its LICENSE.txt or CREDITS.html notice." >&2; exit 1; }

libcef_stripped_sha256=""
if [[ "$host_platform" == "linux" ]]; then
    command -v strip >/dev/null 2>&1 || { echo "strip (binutils) is required to reduce libcef.so from about 1.4 GB to about 270 MB." >&2; exit 1; }
    libcef="$package_root/Release/libcef.so"
    [[ -f "$libcef" ]] || { echo "Pinned CEF package lacks Release/libcef.so." >&2; exit 1; }
    prestrip_sha256="$(sha256_of "$libcef")"
    if [[ "$prestrip_sha256" != "$expected_prestrip_sha256" ]]; then
        echo "libcef.so pre-strip hash mismatch. Expected $expected_prestrip_sha256, got $prestrip_sha256." >&2
        exit 1
    fi
    strip --strip-all "$libcef"
    libcef_stripped_sha256="$(sha256_of "$libcef")"
    # chrome-sandbox is a setuid-root helper that this project never installs;
    # the sandbox is the unprivileged user-namespace one.
    rm -f "$package_root/Release/chrome-sandbox"
fi

staging="${destination}.staging-$$"
mkdir -p "$(dirname "$destination")"
mv "$package_root" "$staging"
{
    expected_manifest
    if [[ -n "$libcef_stripped_sha256" ]]; then
        printf '%s\n' "libcef_sha256=$libcef_stripped_sha256"
    fi
} > "$staging/$manifest_name"
test_package "$staging" || { echo "Pinned CEF package is missing a required header, library, resource, or notice." >&2; exit 1; }
[[ "$destination" == "$CEF_ROOT/"* ]] || { echo "Refusing to replace an unsafe destination." >&2; exit 1; }
rm -rf "$destination"
mv "$staging" "$destination"
test_package "$destination" || { echo "CEF staging verification failed at $destination." >&2; exit 1; }
if [[ "$keep_archive" != true ]]; then
    rm -f "$archive"
fi
echo "Pinned CEF $CEF_VERSION staged at $destination"
