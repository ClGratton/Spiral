#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

sync_git_dependency() {
    local name="$1"
    local url="$2"
    local commit="$3"
    local path="$4"
    local skip_lfs_payloads="${5:-false}"
    local destination="$ROOT/$path"

    if [[ ! -d "$destination" ]]; then
        echo "Cloning $name..."
        if [[ "$skip_lfs_payloads" == true ]] && ! command -v git-lfs >/dev/null 2>&1; then
            git -c filter.lfs.process= -c filter.lfs.smudge= -c filter.lfs.required=false \
                clone "$url" "$destination"
        else
            git clone "$url" "$destination"
        fi
    elif [[ ! -d "$destination/.git" ]]; then
        echo "$name already exists as vendored source; skipping git sync."
        return
    fi

    git -C "$destination" fetch --all --tags
    if [[ "$skip_lfs_payloads" == true ]] && ! command -v git-lfs >/dev/null 2>&1; then
        git -c filter.lfs.process= -c filter.lfs.smudge= -c filter.lfs.required=false \
            -C "$destination" checkout "$commit"
    else
        git -C "$destination" checkout "$commit"
    fi
}

git_dependency_checkout_is_verified() {
    local destination="$1"
    local quiet="${2:-false}"

    if [[ -z "$(git -C "$destination" status --porcelain)" ]]; then
        return 0
    fi

    # A shared checkout may contain correctly materialized upstream LFS payloads
    # even when this host has no git-lfs clean filter with which to compare them.
    # Accept only exact committed payload bytes; every other dirty state remains a
    # hard failure.
    command -v git-lfs >/dev/null 2>&1 && return 1
    git -C "$destination" diff --cached --quiet || return 1
    [[ -z "$(git -C "$destination" ls-files --others --exclude-standard)" ]] || return 1
    [[ -z "$(git -C "$destination" diff --summary)" ]] || return 1

    local path=""
    local pointer=""
    local expected_hash=""
    local expected_size=""
    local actual_hash=""
    local actual_size=""
    local attribute=""
    local verified_count=0
    while IFS= read -r -d '' path; do
        [[ -f "$destination/$path" ]] || return 1
        attribute="$(git -c core.quotePath=false -C "$destination" check-attr filter -- "$path")"
        [[ "$attribute" == *": filter: lfs" ]] || return 1

        pointer="$(git -C "$destination" show "HEAD:$path")" || return 1
        [[ "$(printf '%s\n' "$pointer" | sed -n '1p')" == "version https://git-lfs.github.com/spec/v1" ]] || return 1
        expected_hash="$(printf '%s\n' "$pointer" | sed -n '2s/^oid sha256://p')"
        expected_size="$(printf '%s\n' "$pointer" | sed -n '3s/^size //p')"
        [[ "$expected_hash" =~ ^[0-9a-f]{64}$ && "$expected_size" =~ ^[0-9]+$ ]] || return 1
        [[ -z "$(printf '%s\n' "$pointer" | sed -n '4p')" ]] || return 1

        if command -v sha256sum >/dev/null 2>&1; then
            actual_hash="$(sha256sum -- "$destination/$path")"
            actual_hash="${actual_hash%% *}"
        elif command -v shasum >/dev/null 2>&1; then
            actual_hash="$(shasum -a 256 -- "$destination/$path")"
            actual_hash="${actual_hash%% *}"
        else
            echo "sha256sum or shasum is required to verify materialized Git LFS payloads." >&2
            return 1
        fi
        actual_size="$(wc -c < "$destination/$path")"
        actual_size="${actual_size//[[:space:]]/}"
        [[ "$actual_hash" == "$expected_hash" && "$actual_size" == "$expected_size" ]] || return 1
        verified_count=$((verified_count + 1))
    done < <(git -C "$destination" diff --name-only -z)

    [[ "$verified_count" -gt 0 ]] || return 1
    if [[ "$quiet" != true ]]; then
        echo "Verified $verified_count materialized Git LFS payloads without git-lfs."
    fi
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
    ktx_path="$ROOT/Vendor/KTX-Software"
    ktx_ready=false
    if [[ -d "$ktx_path/.git" ]] \
        && [[ "$(git -C "$ktx_path" rev-parse HEAD)" == "4d6fc70eaf62ad0558e63e8d97eb9766118327a6" ]] \
        && git_dependency_checkout_is_verified "$ktx_path" true \
        && [[ -e "$ktx_path/LICENSE.md" && -e "$ktx_path/LICENSES" && -e "$ktx_path/NOTICE.md" ]]; then
        ktx_ready=true
    fi
    if [[ "$ktx_ready" == false ]]; then
        sync_git_dependency \
            "KTX-Software" \
            "https://github.com/KhronosGroup/KTX-Software.git" \
            "4d6fc70eaf62ad0558e63e8d97eb9766118327a6" \
            "Vendor/KTX-Software" \
            true
    fi

    [[ "$(git -C "$ktx_path" rev-parse HEAD)" == "4d6fc70eaf62ad0558e63e8d97eb9766118327a6" ]] || {
        echo "KTX-Software did not resolve to the admitted source commit." >&2
        exit 1
    }
    git_dependency_checkout_is_verified "$ktx_path" || {
        echo "KTX-Software checkout contains local modifications." >&2
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
