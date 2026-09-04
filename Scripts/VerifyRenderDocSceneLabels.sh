#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 --qrenderdoc PATH --editor PATH --repo PATH --artifacts DIR [--renderdoc-library PATH] [--vulkan-layer-manifest PATH] [--runtime-lib-dir DIR] [--timeout SECONDS]" >&2
}

QRENDERDOC=""
EDITOR=""
REPOSITORY=""
ARTIFACTS=""
RENDERDOC_LIBRARY=""
VULKAN_LAYER_MANIFEST=""
RUNTIME_LIB_DIRS=()
TIMEOUT_SECONDS=120

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qrenderdoc) QRENDERDOC="${2:-}"; shift 2 ;;
        --editor) EDITOR="${2:-}"; shift 2 ;;
        --repo) REPOSITORY="${2:-}"; shift 2 ;;
        --artifacts) ARTIFACTS="${2:-}"; shift 2 ;;
        --renderdoc-library) RENDERDOC_LIBRARY="${2:-}"; shift 2 ;;
        --vulkan-layer-manifest) VULKAN_LAYER_MANIFEST="${2:-}"; shift 2 ;;
        --runtime-lib-dir) RUNTIME_LIB_DIRS+=("${2:-}"); shift 2 ;;
        --timeout) TIMEOUT_SECONDS="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ -z "$QRENDERDOC" || -z "$EDITOR" || -z "$REPOSITORY" || -z "$ARTIFACTS" ]]; then
    usage
    exit 2
fi
if [[ ! "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
    echo "--timeout must be a positive integer: $TIMEOUT_SECONDS" >&2
    exit 2
fi
for command_name in awk cp dirname grep ldd mkdir perl readlink realpath tee uname; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "$command_name is required for bounded RenderDoc verification." >&2
        exit 1
    fi
done
if [[ "$(uname -s)" != "Linux" ]]; then
    echo "RenderDoc scene-label verification currently supports Linux only." >&2
    exit 1
fi

QRENDERDOC="$(realpath "$QRENDERDOC")"
EDITOR="$(realpath "$EDITOR")"
REPOSITORY="$(realpath "$REPOSITORY")"
ARTIFACTS="$(realpath -m "$ARTIFACTS")"
if [[ ! -x "$QRENDERDOC" ]]; then
    echo "qrenderdoc is not executable: $QRENDERDOC" >&2
    exit 1
fi
if [[ ! -x "$EDITOR" ]]; then
    echo "Editor is not executable: $EDITOR" >&2
    exit 1
fi
if [[ ! -d "$REPOSITORY/.git" && ! -f "$REPOSITORY/.git" ]]; then
    echo "Repository path is not a Git worktree: $REPOSITORY" >&2
    exit 1
fi

QRENDERDOC_PACKAGE_ROOT="$(realpath "$(dirname "$QRENDERDOC")/../..")"
if [[ -z "$RENDERDOC_LIBRARY" ]]; then
    RENDERDOC_LIBRARY="$QRENDERDOC_PACKAGE_ROOT/usr/lib/librenderdoc.so"
fi
if [[ -z "$VULKAN_LAYER_MANIFEST" ]]; then
    VULKAN_LAYER_MANIFEST="$QRENDERDOC_PACKAGE_ROOT/etc/vulkan/implicit_layer.d/renderdoc_capture.json"
fi
RENDERDOC_LIBRARY="$(realpath "$RENDERDOC_LIBRARY")"
VULKAN_LAYER_MANIFEST="$(realpath "$VULKAN_LAYER_MANIFEST")"
if [[ ! -f "$RENDERDOC_LIBRARY" ]]; then
    echo "RenderDoc capture library was not found: $RENDERDOC_LIBRARY" >&2
    exit 1
fi
if [[ ! -f "$VULKAN_LAYER_MANIFEST" ]]; then
    echo "RenderDoc Vulkan implicit-layer manifest was not found: $VULKAN_LAYER_MANIFEST" >&2
    exit 1
fi

for runtime_index in "${!RUNTIME_LIB_DIRS[@]}"; do
    if [[ -z "${RUNTIME_LIB_DIRS[$runtime_index]}" ]]; then
        echo "--runtime-lib-dir requires a directory path." >&2
        exit 2
    fi
    RUNTIME_LIB_DIRS[$runtime_index]="$(realpath "${RUNTIME_LIB_DIRS[$runtime_index]}")"
    if [[ ! -d "${RUNTIME_LIB_DIRS[$runtime_index]}" ]]; then
        echo "RenderDoc runtime library directory was not found: ${RUNTIME_LIB_DIRS[$runtime_index]}" >&2
        exit 1
    fi
done

SCRIPT="$REPOSITORY/Scripts/VerifyRenderDocSceneLabels.py"
CONFIG_TEMPLATE="$REPOSITORY/Scripts/TestSupport/RenderDocUI.config"
if [[ ! -f "$SCRIPT" ]]; then
    echo "qrenderdoc verification script was not found: $SCRIPT" >&2
    exit 1
fi
if [[ ! -f "$CONFIG_TEMPLATE" ]]; then
    echo "isolated qrenderdoc configuration was not found: $CONFIG_TEMPLATE" >&2
    exit 1
fi

EDITOR_REAL="$(realpath "$EDITOR")"
for process_exe in /proc/[0-9]*/exe; do
    running_exe="$(readlink "$process_exe" 2>/dev/null || true)"
    if [[ -n "$running_exe" && "$running_exe" == "$EDITOR_REAL" ]]; then
        echo "Refusing to capture while this Editor executable is already running: $EDITOR_REAL" >&2
        exit 1
    fi
done

RENDERDOC_LIBRARY_DIR="$(dirname "$RENDERDOC_LIBRARY")"
LIBRARY_SEARCH_PATH="$RENDERDOC_LIBRARY_DIR"
for runtime_directory in "${RUNTIME_LIB_DIRS[@]}"; do
    LIBRARY_SEARCH_PATH+=":$runtime_directory"
done
LIBRARY_SEARCH_PATH+="${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
LDD_OUTPUT="$(LD_LIBRARY_PATH="$LIBRARY_SEARCH_PATH" ldd "$QRENDERDOC" 2>&1)"
LDD_OUTPUT+=$'\n'
LDD_OUTPUT+="$(LD_LIBRARY_PATH="$LIBRARY_SEARCH_PATH" ldd "$RENDERDOC_LIBRARY" 2>&1)"
MISSING_LIBRARIES="$(awk '/not found/ { print $1 }' <<<"$LDD_OUTPUT")"
if [[ -n "$MISSING_LIBRARIES" ]]; then
    echo "The explicit qrenderdoc/RenderDoc closure has unresolved runtime libraries:" >&2
    echo "$MISSING_LIBRARIES" >&2
    exit 1
fi
RESOLVED_RENDERDOC_LIBRARY="$(awk '$1 == "librenderdoc.so" && $2 == "=>" { print $3; exit }' <<<"$LDD_OUTPUT")"
if [[ -z "$RESOLVED_RENDERDOC_LIBRARY"
    || "$(realpath "$RESOLVED_RENDERDOC_LIBRARY")" != "$RENDERDOC_LIBRARY" ]]; then
    echo "qrenderdoc did not resolve the explicitly selected RenderDoc library: $RENDERDOC_LIBRARY" >&2
    exit 1
fi

mkdir -p "$ARTIFACTS"
CAPTURE="$ARTIFACTS/spiral-renderdoc-scene-labels.rdc"
REPORT="$ARTIFACTS/spiral-renderdoc-scene-labels.json"
LOG="$ARTIFACTS/qrenderdoc-scene-labels.log"
CAPTURE_TEMPLATE="$ARTIFACTS/spiral-renderdoc-scene-source"
QRENDERDOC_STATE="$ARTIFACTS/qrenderdoc-state"
VULKAN_LAYER_ROOT="$ARTIFACTS/renderdoc-vulkan-layer"
VULKAN_LAYER_DIR="$VULKAN_LAYER_ROOT/implicit_layer.d"
EFFECTIVE_VULKAN_LAYER_MANIFEST="$VULKAN_LAYER_DIR/renderdoc_capture.json"
for output_path in "$CAPTURE" "$REPORT" "$LOG" "$QRENDERDOC_STATE" "$VULKAN_LAYER_ROOT"; do
    if [[ -e "$output_path" ]]; then
        echo "Refusing to overwrite an existing verification artifact: $output_path" >&2
        exit 1
    fi
done

mkdir -p "$VULKAN_LAYER_DIR"
perl -MJSON::PP -0777 -e '
    my ($source, $destination, $library) = @ARGV;
    open my $input, "<", $source or die "Could not open $source: $!\n";
    my $document = decode_json(<$input>);
    close $input;
    my $layer = $document->{layer};
    die "Manifest is not RenderDoc capture layer metadata\n"
        unless ref($layer) eq "HASH"
            && ($layer->{name} // "") eq "VK_LAYER_RENDERDOC_Capture"
            && ($layer->{type} // "") eq "GLOBAL"
            && ($layer->{implementation_version} // "") eq "45"
            && ref($layer->{enable_environment}) eq "HASH"
            && ($layer->{enable_environment}->{ENABLE_VULKAN_RENDERDOC_CAPTURE} // "") eq "1"
            && ref($layer->{disable_environment}) eq "HASH"
            && ($layer->{disable_environment}->{DISABLE_VULKAN_RENDERDOC_CAPTURE_1_45} // "") eq "1";
    $layer->{library_path} = $library;
    open my $output, ">", $destination or die "Could not create $destination: $!\n";
    print {$output} JSON::PP->new->canonical->pretty->encode($document);
    close $output or die "Could not close $destination: $!\n";
' "$VULKAN_LAYER_MANIFEST" "$EFFECTIVE_VULKAN_LAYER_MANIFEST" "$RENDERDOC_LIBRARY"

export XDG_DATA_HOME="$QRENDERDOC_STATE/data"
export XDG_CONFIG_HOME="$QRENDERDOC_STATE/config"
export XDG_CACHE_HOME="$QRENDERDOC_STATE/cache"
mkdir -p "$XDG_DATA_HOME/qrenderdoc" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
cp "$CONFIG_TEMPLATE" "$XDG_DATA_HOME/qrenderdoc/UI.config"

export SPIRAL_RENDERDOC_EDITOR="$EDITOR"
export SPIRAL_RENDERDOC_QRENDERDOC="$QRENDERDOC"
export SPIRAL_RENDERDOC_LIBRARY="$RENDERDOC_LIBRARY"
export SPIRAL_RENDERDOC_SOURCE_VULKAN_LAYER_MANIFEST="$VULKAN_LAYER_MANIFEST"
export SPIRAL_RENDERDOC_EFFECTIVE_VULKAN_LAYER_MANIFEST="$EFFECTIVE_VULKAN_LAYER_MANIFEST"
export SPIRAL_RENDERDOC_VULKAN_LAYER_DIR="$VULKAN_LAYER_DIR"
export SPIRAL_RENDERDOC_RUNTIME_LIBRARY_PATH="$LIBRARY_SEARCH_PATH"
export SPIRAL_RENDERDOC_REPOSITORY="$REPOSITORY"
export SPIRAL_RENDERDOC_CAPTURE="$CAPTURE"
export SPIRAL_RENDERDOC_CAPTURE_TEMPLATE="$CAPTURE_TEMPLATE"
export SPIRAL_RENDERDOC_REPORT="$REPORT"
export SPIRAL_RENDERDOC_TIMEOUT_SECONDS="$TIMEOUT_SECONDS"
export LD_LIBRARY_PATH="$LIBRARY_SEARCH_PATH"
unset ENABLE_VULKAN_RENDERDOC_CAPTURE
unset DISABLE_VULKAN_RENDERDOC_CAPTURE_1_45
unset VK_IMPLICIT_LAYER_PATH
unset VK_LOADER_LAYERS_DISABLE
QT_PLUGIN_SEARCH_PATH=""
for runtime_directory in "${RUNTIME_LIB_DIRS[@]}"; do
    if [[ -d "$runtime_directory/qt/plugins" ]]; then
        QT_PLUGIN_SEARCH_PATH+="${QT_PLUGIN_SEARCH_PATH:+:}$runtime_directory/qt/plugins"
    fi
done
if [[ -n "$QT_PLUGIN_SEARCH_PATH" ]]; then
    export QT_PLUGIN_PATH="$QT_PLUGIN_SEARCH_PATH${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
fi
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

SUPERVISOR_TIMEOUT_SECONDS=$((TIMEOUT_SECONDS + 30))
set +e
perl -e '
    my $timeout = shift;
    my $child = fork();
    die "fork failed: $!\n" unless defined $child;
    if ($child == 0) {
        setpgrp(0, 0) or die "setpgrp failed: $!\n";
        exec @ARGV or die "exec failed: $!\n";
    }
    $SIG{ALRM} = sub {
        warn "RenderDoc scene-label verification timed out after ${timeout}s; terminating process group\n";
        kill "TERM", -$child;
        sleep 1;
        kill "KILL", -$child;
        waitpid($child, 0);
        exit 124;
    };
    alarm $timeout;
    waitpid($child, 0);
    alarm 0;
    my $status = $?;
    kill "TERM", -$child;
    select undef, undef, undef, 0.25;
    kill "KILL", -$child;
    exit(128 + ($status & 127)) if $status & 127;
    exit($status >> 8);
' "$SUPERVISOR_TIMEOUT_SECONDS" "$QRENDERDOC" --python "$SCRIPT" 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e

if [[ $STATUS -ne 0 ]]; then
    echo "RenderDoc scene-label verification failed with exit code $STATUS; artifacts remain in $ARTIFACTS" >&2
    exit "$STATUS"
fi
if [[ ! -s "$CAPTURE" || ! -s "$REPORT" ]]; then
    echo "RenderDoc verification exited successfully without both required artifacts." >&2
    exit 1
fi
if ! grep -Fq '"result": "pass"' "$REPORT" \
    || ! grep -Fq '"schema": "SpiralRenderDocSceneLabelsV1"' "$REPORT"; then
    echo "RenderDoc verification report did not record a schema-valid pass: $REPORT" >&2
    exit 1
fi

echo "RenderDoc scene-label verification passed: $REPORT"
