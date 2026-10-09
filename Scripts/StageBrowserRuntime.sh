#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

source_path="$1"
destination="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest_name=".spiral-package-manifest"

[[ "$(uname -s)" == "Linux" ]] || { echo "StageBrowserRuntime.sh supports the Linux CEF package; use StageBrowserRuntime.ps1 on Windows." >&2; exit 1; }
[[ -f "$source_path/include/cef_version.h" && -f "$source_path/LICENSE.txt" && -f "$source_path/CREDITS.html" && -f "$source_path/$manifest_name" ]] || { echo "Pinned CEF package is unavailable or lacks its installed manifest at '$source_path'. Run Scripts/FetchCEF.sh first." >&2; exit 1; }
[[ -f "$root/Vendor/CEF/THIRD_PARTY_NOTICE.md" ]] || { echo "Vendor/CEF/THIRD_PARTY_NOTICE.md is missing." >&2; exit 1; }

required_release=(libcef.so v8_context_snapshot.bin)
required_resources=(icudtl.dat resources.pak chrome_100_percent.pak chrome_200_percent.pak)
optional_release=(libvk_swiftshader.so libvulkan.so.1 vk_swiftshader_icd.json libEGL.so libGLESv2.so)
locale="en-US.pak"

runtime_files=()
for name in "${required_release[@]}"; do
    [[ -f "$source_path/Release/$name" ]] || { echo "Pinned CEF package lacks Release/$name." >&2; exit 1; }
    runtime_files+=("$source_path/Release/$name")
done
for name in "${required_resources[@]}"; do
    [[ -f "$source_path/Resources/$name" ]] || { echo "Pinned CEF package lacks Resources/$name." >&2; exit 1; }
    runtime_files+=("$source_path/Resources/$name")
done
[[ -f "$source_path/Resources/locales/$locale" ]] || { echo "Pinned CEF package lacks Resources/locales/$locale." >&2; exit 1; }
for name in "${optional_release[@]}"; do
    [[ ! -f "$source_path/Release/$name" ]] || runtime_files+=("$source_path/Release/$name")
done
# A setuid chrome-sandbox is never staged; the sandbox is the unprivileged user-namespace one.
[[ ! -e "$source_path/Release/chrome-sandbox" ]] || { echo "Pinned CEF package unexpectedly contains chrome-sandbox; reinstall it with Scripts/FetchCEF.sh --force." >&2; exit 1; }

cef_hash="$(sed -n 's/^sha256=//p' "$source_path/$manifest_name")"
prestrip_hash="$(sed -n 's/^libcef_prestrip_sha256=//p' "$source_path/$manifest_name")"
libcef_hash="$(sed -n 's/^libcef_sha256=//p' "$source_path/$manifest_name")"
cef_build="$(sed -n 's/^build=//p' "$source_path/$manifest_name")"
[[ -n "$cef_hash" && -n "$prestrip_hash" && -n "$libcef_hash" && -n "$cef_build" ]] || { echo "Pinned CEF installed manifest lacks the archive, build, or libcef.so hashes." >&2; exit 1; }

runtime_names="$(printf '%s,' "${runtime_files[@]##*/}" | sed 's/,$//'),locales/$locale"
expected_manifest() {
    cat <<EOF
format=1
cef_build=$cef_build
cef_sha256=$cef_hash
libcef_prestrip_sha256=$prestrip_hash
libcef_sha256=$libcef_hash
cef_runtime=$runtime_names
notices=CEF-LICENSE.txt,CEF-CREDITS.html,CEF-THIRD_PARTY_NOTICE.md
sandbox=user-namespace-only-no-chrome-sandbox
distribution_status=blocked-pending-cef-chromium-notice-audit
EOF
}

runtime_is_current() {
    local file
    [[ -f "$destination/CefRuntimeManifest.txt" ]] || return 1
    diff -u <(expected_manifest) "$destination/CefRuntimeManifest.txt" >/dev/null || return 1
    for file in "${runtime_files[@]}"; do
        [[ -f "$destination/${file##*/}" && "$(stat -c %s "$file")" == "$(stat -c %s "$destination/${file##*/}")" ]] || return 1
    done
    [[ -f "$destination/locales/$locale" && -f "$destination/CEF-LICENSE.txt" && -f "$destination/CEF-CREDITS.html" && -f "$destination/CEF-THIRD_PARTY_NOTICE.md" && ! -e "$destination/chrome-sandbox" ]]
}

mkdir -p "$destination"
if runtime_is_current; then
    exit 0
fi

# Remove only the names this script owns; the helper executable that shares the
# directory is left alone.
rm -f "$destination"/libcef.so "$destination"/v8_context_snapshot.bin "$destination"/icudtl.dat "$destination"/resources.pak \
    "$destination"/chrome_100_percent.pak "$destination"/chrome_200_percent.pak "$destination"/libvk_swiftshader.so \
    "$destination"/libvulkan.so.1 "$destination"/vk_swiftshader_icd.json "$destination"/libEGL.so "$destination"/libGLESv2.so \
    "$destination"/chrome-sandbox "$destination"/CEF-LICENSE.txt "$destination"/CEF-CREDITS.html "$destination"/CEF-THIRD_PARTY_NOTICE.md \
    "$destination"/CefRuntimeManifest.txt
rm -rf "$destination"/locales

for file in "${runtime_files[@]}"; do
    cp -f --reflink=auto "$file" "$destination/"
done
mkdir -p "$destination/locales"
cp -f "$source_path/Resources/locales/$locale" "$destination/locales/"
cp -f "$source_path/LICENSE.txt" "$destination/CEF-LICENSE.txt"
cp -f "$source_path/CREDITS.html" "$destination/CEF-CREDITS.html"
cp -f "$root/Vendor/CEF/THIRD_PARTY_NOTICE.md" "$destination/CEF-THIRD_PARTY_NOTICE.md"
expected_manifest > "$destination/CefRuntimeManifest.txt"
