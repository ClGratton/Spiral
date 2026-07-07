#!/usr/bin/env bash
set -euo pipefail

PREMAKE_VERSION="${PREMAKE_VERSION:-5.0.0-beta8}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREMAKE_DIR="$ROOT/Vendor/premake/bin"

mkdir -p "$PREMAKE_DIR"

case "$(uname -s)" in
    Linux*)
        ARCHIVE_NAME="premake-${PREMAKE_VERSION}-linux.tar.gz"
        PREMAKE_EXE="$PREMAKE_DIR/premake5"
        ;;
    Darwin*)
        ARCHIVE_NAME="premake-${PREMAKE_VERSION}-macosx.tar.gz"
        PREMAKE_EXE="$PREMAKE_DIR/premake5"
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

echo "Setup complete."
echo "Premake: $PREMAKE_EXE"
