#!/usr/bin/env bash
set -euo pipefail

PREMAKE_VERSION="${PREMAKE_VERSION:-5.0.0-beta8}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREMAKE_DIR="$ROOT/Vendor/premake/bin"

[[ "$(uname -m)" == "x86_64" ]] || {
    echo "This workspace currently generates x86_64 projects only; host architecture '$(uname -m)' is unsupported. FetchSlang.sh may be used separately to audit a pinned ARM64 package, but Setup will not fetch an unusable build toolchain." >&2
    exit 1
}

mkdir -p "$PREMAKE_DIR"

case "$(uname -s)" in
    Linux*)
        ARCHIVE_NAME="premake-${PREMAKE_VERSION}-linux.tar.gz"
        PREMAKE_EXE="$PREMAKE_DIR/premake5"
        ;;
    Darwin*)
        if [[ "$(uname -m)" == "x86_64" ]]; then
            ARCHIVE_NAME="premake-${PREMAKE_VERSION}-macosx-x64.tar.gz"
        else
            ARCHIVE_NAME="premake-${PREMAKE_VERSION}-macosx.tar.gz"
        fi
        PREMAKE_EXE="$PREMAKE_DIR/premake5"
        if ! command -v brew >/dev/null 2>&1; then
            echo "Homebrew is required to install the macOS MoltenVK runtime." >&2
            exit 1
        fi
        MISSING_VULKAN_FORMULAE=()
        for FORMULA in vulkan-loader molten-vk; do
            if ! brew list --versions "$FORMULA" >/dev/null 2>&1; then
                MISSING_VULKAN_FORMULAE+=("$FORMULA")
            fi
        done
        if [[ ${#MISSING_VULKAN_FORMULAE[@]} -ne 0 ]]; then
            echo "Installing Vulkan loader and MoltenVK for the macOS NVRHI Vulkan backend..."
            brew install "${MISSING_VULKAN_FORMULAE[@]}"
        fi
        ;;
    MINGW*|MSYS*|CYGWIN*)
        ARCHIVE_NAME="premake-${PREMAKE_VERSION}-windows.zip"
        PREMAKE_EXE="$PREMAKE_DIR/premake5.exe"
        ;;
    *)
        echo "Unsupported OS: $(uname -s)" >&2
        exit 1
        ;;
esac

if [[ ! -x "$PREMAKE_EXE" ]]; then
    URL="https://github.com/premake/premake-core/releases/download/v${PREMAKE_VERSION}/${ARCHIVE_NAME}"
    ARCHIVE_PATH="$PREMAKE_DIR/$ARCHIVE_NAME"

    echo "Downloading Premake $PREMAKE_VERSION..."
    if command -v curl >/dev/null 2>&1; then
        curl -L "$URL" -o "$ARCHIVE_PATH"
    elif command -v wget >/dev/null 2>&1; then
        wget "$URL" -O "$ARCHIVE_PATH"
    else
        echo "curl or wget is required to download Premake." >&2
        exit 1
    fi

    echo "Extracting Premake..."
    case "$ARCHIVE_NAME" in
        *.zip)
            if command -v unzip >/dev/null 2>&1; then
                unzip -o "$ARCHIVE_PATH" -d "$PREMAKE_DIR"
            else
                echo "unzip is required for Windows Premake archives." >&2
                exit 1
            fi
            ;;
        *.tar.gz)
            tar -xzf "$ARCHIVE_PATH" -C "$PREMAKE_DIR"
            ;;
    esac

    chmod +x "$PREMAKE_EXE" || true
fi

bash "$ROOT/Scripts/FetchSlang.sh"
bash "$ROOT/Scripts/FetchDependencies.sh" --include-ktx-software

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) bash "$ROOT/Scripts/FetchDXC.sh" ;;
esac

echo "Setup complete."
echo "Premake: $PREMAKE_EXE"
