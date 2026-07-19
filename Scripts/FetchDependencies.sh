#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

sync_git_dependency() {
    local name="$1"
    local url="$2"
    local commit="$3"
    local path="$4"
    local destination="$ROOT/$path"

    if [[ ! -d "$destination" ]]; then
        echo "Cloning $name..."
        git clone "$url" "$destination"
    elif [[ ! -d "$destination/.git" ]]; then
        echo "$name already exists as vendored source; skipping git sync."
        return
    fi

    git -C "$destination" fetch --all --tags
    git -C "$destination" checkout "$commit"
}

include_nvrhi=false
include_nvrhi_platform_headers=false
include_ktx_software=false

for arg in "$@"; do
    case "$arg" in
        --include-nvrhi)
            include_nvrhi=true
            ;;
        --include-nvrhi-platform-headers)
            include_nvrhi_platform_headers=true
            ;;
        --include-ktx-software)
            include_ktx_software=true
            ;;
    esac
done

if [[ "$include_nvrhi" == true ]]; then
    sync_git_dependency \
        "NVRHI" \
        "https://github.com/NVIDIA-RTX/NVRHI.git" \
        "8e8c36e37558acec333204619b95d9d2fcdc4a79" \
        "Vendor/NVRHI"
fi

if [[ "$include_nvrhi_platform_headers" == true ]]; then
    sync_git_dependency \
        "Vulkan-Headers" \
        "https://github.com/KhronosGroup/Vulkan-Headers.git" \
        "v1.4.352" \
        "Vendor/Vulkan-Headers"

    sync_git_dependency \
        "DirectX-Headers" \
        "https://github.com/microsoft/DirectX-Headers.git" \
        "v1.717.0-preview" \
        "Vendor/DirectX-Headers"
fi

if [[ "$include_ktx_software" == true ]]; then
    sync_git_dependency \
        "KTX-Software" \
        "https://github.com/KhronosGroup/KTX-Software.git" \
        "4d6fc70eaf62ad0558e63e8d97eb9766118327a6" \
        "Vendor/KTX-Software"

    ktx_path="$ROOT/Vendor/KTX-Software"
    [[ "$(git -C "$ktx_path" rev-parse HEAD)" == "4d6fc70eaf62ad0558e63e8d97eb9766118327a6" ]] || {
        echo "KTX-Software did not resolve to the admitted source commit." >&2
        exit 1
    }
    for notice in LICENSE.md LICENSES NOTICE.md; do
        [[ -e "$ktx_path/$notice" ]] || {
            echo "KTX-Software source is missing required notice path: $notice" >&2
            exit 1
        }
    done
fi

echo "Dependency fetch complete."
