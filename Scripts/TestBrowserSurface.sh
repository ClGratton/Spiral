#!/usr/bin/env bash
# Behavioural gate for the CEF browser adapter (libSpiralBrowserHost.so), driven
# through the CEF-free SpiralBrowserSmoke executable exactly as the Editor will
# use it: dlopen, one C factory, IBrowserSurface. Software rendering only; no
# GPU, window, or display connection is used. Linux only.
#
# Usage: Scripts/TestBrowserSurface.sh [Debug|Release|Dist] [gmake|gmake2] [build|--skip-build]
# Overrides: BROWSER_SURFACE_SMOKE and BROWSER_SURFACE_LIBRARY select explicit
# binaries (the library must sit beside libcef.so and the staged runtime);
# BROWSER_SURFACE_TIMEOUT_SECONDS bounds every child (default 90);
# BROWSER_SURFACE_KEEP_WORK=1 keeps the work directory and logs.
#
# Independent oracles: the smoke driver mirrors frames from the declared dirty
# rectangles only and asserts pixels of fixture pages; the HTTPS fixture server
# logs the Cookie header of every request; the downloaded bytes are hashed here;
# the process tree is read from /proc.
set -euo pipefail

CONFIGURATION="${1:-Debug}"
ACTION="${2:-gmake}"
BUILD_MODE="${3:-build}"
SMOKE_TIMEOUT_SECONDS="${BROWSER_SURFACE_TIMEOUT_SECONDS:-90}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUPPORT="$ROOT/Scripts/TestSupport"
HOST_NAME="fixture.spiral.test"

case "$CONFIGURATION" in
    Debug|Release|Dist) ;;
    *) echo "Unsupported configuration: $CONFIGURATION" >&2; exit 1 ;;
esac
case "$ACTION" in
    gmake|gmake2) ;;
    *) echo "Unsupported action: $ACTION" >&2; exit 1 ;;
esac
if [[ "$BUILD_MODE" != "build" && "$BUILD_MODE" != "--skip-build" ]]; then
    echo "Unsupported build mode: $BUILD_MODE" >&2
    exit 1
fi
if [[ ! "$SMOKE_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
    echo "BROWSER_SURFACE_TIMEOUT_SECONDS must be a positive integer: $SMOKE_TIMEOUT_SECONDS" >&2
    exit 1
fi
[[ "$(uname -s)" == "Linux" ]] || { echo "TestBrowserSurface.sh supports Linux only." >&2; exit 1; }
for tool in python3 openssl timeout sha256sum base64 stat; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool is missing: $tool" >&2; exit 1; }
done

if [[ "$BUILD_MODE" != "--skip-build" && -z "${BROWSER_SURFACE_SMOKE:-}" ]]; then
    bash "$ROOT/Scripts/Build.sh" "$CONFIGURATION" "$ACTION"
fi

BIN_DIR="$ROOT/bin/${CONFIGURATION}-linux-x86_64-${ACTION}/Editor"
EDITOR="$BIN_DIR/Editor"
SMOKE="${BROWSER_SURFACE_SMOKE:-$BIN_DIR/SpiralBrowserSmoke}"
LIBRARY="${BROWSER_SURFACE_LIBRARY:-$BIN_DIR/cef/libSpiralBrowserHost.so}"
[[ -x "$SMOKE" ]] || { echo "Smoke executable was not found (is CEF installed and the project generated?): $SMOKE" >&2; exit 1; }
[[ -f "$LIBRARY" ]] || { echo "Browser host library was not found: $LIBRARY" >&2; exit 1; }

# A running Editor may be replaced by the build and shares the user's profile
# directory conventions; never run beside one.
for process in /proc/[0-9]*; do
    if [[ "$(readlink "$process/exe" 2>/dev/null || true)" == "$EDITOR" ]]; then
        echo "The Editor is running (pid ${process#/proc/}); close it before running this test." >&2
        exit 1
    fi
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/spiral-browser-surface.XXXXXX")"
chmod 700 "$WORK"
WORK="$(cd "$WORK" && pwd -P)"
LOG_DIR="$WORK/logs"
mkdir -p "$LOG_DIR" "$WORK/staging"
SERVER_PID=""
BACKGROUND_PIDS=()
CHECKS=0
FAILED=1

cleanup() {
    local pid
    for pid in "${BACKGROUND_PIDS[@]}"; do
        kill -KILL "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
    done
    [[ -z "$SERVER_PID" ]] || kill -KILL "$SERVER_PID" 2>/dev/null || true
    # Anything still carrying one of this run's profile paths is a leak; remove it
    # so a failed run cannot strand browser processes.
    for pid in $(python3 "$SUPPORT/BrowserSurfaceProcessTree.py" profile-processes "$WORK" 2>/dev/null || true); do
        kill -KILL "$pid" 2>/dev/null || true
    done
    if [[ "$FAILED" != "0" || -n "${BROWSER_SURFACE_KEEP_WORK:-}" ]]; then
        echo "BROWSER_TEST work directory kept: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

die() {
    echo "BROWSER_TEST FAIL $1" >&2
    shift
    [[ $# -eq 0 ]] || echo "    $*" >&2
    exit 1
}

pass() {
    CHECKS=$((CHECKS + 1))
    echo "BROWSER_TEST PASS $1"
}

expect_log() { # log pattern description
    grep -Eq -- "$2" "$1" || die "$3" "pattern not found in $1: $2"
}

reject_log() { # log pattern description
    if grep -Eq -- "$2" "$1"; then die "$3" "unexpected pattern in $1: $2"; fi
}

# ---------------------------------------------------------------------------------
# Fixture: HTTPS server on an ephemeral loopback port. The host name resolves only
# through a Chromium host-resolver rule (everything else is NOTFOUND, so no test
# can reach the network) and the self-signed certificate is accepted by SPKI pin.
# ---------------------------------------------------------------------------------
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
    -keyout "$WORK/key.pem" -out "$WORK/cert.pem" -subj "/CN=$HOST_NAME" \
    -addext "subjectAltName=DNS:$HOST_NAME" >/dev/null 2>&1
SPKI="$(openssl x509 -in "$WORK/cert.pem" -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64)"

SERVER_LOG="$WORK/server.log"
: > "$SERVER_LOG"
python3 "$SUPPORT/BrowserSurfaceFixtureServer.py" --port-file "$WORK/port" --log "$SERVER_LOG" \
    --cert "$WORK/cert.pem" --key "$WORK/key.pem" --expected-hash "$WORK/expected.sha256" \
    >"$LOG_DIR/server.out" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 100); do
    [[ -s "$WORK/port" ]] && break
    sleep 0.1
done
[[ -s "$WORK/port" ]] || die "fixture server did not start" "$(cat "$LOG_DIR/server.out")"
PORT="$(cat "$WORK/port")"

COMMON_ARGS=(
    "--lib=$LIBRARY"
    "--provider-host=$HOST_NAME"
    "--test-switch=host-resolver-rules=MAP $HOST_NAME 127.0.0.1:$PORT, MAP consent.spiral.test 127.0.0.1:$PORT, MAP third.party.test 127.0.0.1:$PORT, MAP * ~NOTFOUND"
    "--test-switch=ignore-certificate-errors-spki-list=$SPKI"
)
URL="https://$HOST_NAME"
SMOKE_STAGING="$WORK/staging"
SMOKE_EXIT=0

# run_smoke NAME PROFILE STEPS [extra smoke arguments]: foreground, bounded by
# timeout (SIGTERM, then SIGKILL after 5 s), output streamed live and kept.
run_smoke() {
    local name="$1" profile="$2" steps="$3"
    shift 3
    local log="$LOG_DIR/$name.log"
    echo "BROWSER_TEST run $name"
    set +e
    timeout --kill-after=5 "$SMOKE_TIMEOUT_SECONDS" "$SMOKE" "${COMMON_ARGS[@]}" "--profile=$profile" \
        "--staging=$SMOKE_STAGING" "--steps=$steps" "$@" 2>&1 | tee "$log"
    SMOKE_EXIT=${PIPESTATUS[0]}
    set -e
    LAST_LOG="$log"
}

# start_smoke NAME PROFILE STEPS: background, own session; sets BG_PID (the
# session leader) and BG_LOG. The step script ends in "hold", so it runs until signalled.
start_smoke() {
    local name="$1" profile="$2" steps="$3"
    BG_LOG="$LOG_DIR/$name.log"
    echo "BROWSER_TEST start $name"
    setsid timeout --kill-after=5 "$SMOKE_TIMEOUT_SECONDS" "$SMOKE" "${COMMON_ARGS[@]}" "--profile=$profile" \
        "--staging=$SMOKE_STAGING" "--steps=$steps" >"$BG_LOG" 2>&1 &
    BG_PID=$!
    BACKGROUND_PIDS+=("$BG_PID")
}

wait_for_marker() { # log pattern seconds description
    local deadline=$((SECONDS + $3))
    while (( SECONDS < deadline )); do
        grep -Eq -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.2
    done
    die "$4" "timed out after $3 s waiting for: $2"
}

smoke_pid_from_log() { sed -n 's/.*marker name=[a-z]* pid=\([0-9]*\).*/\1/p' "$1" | head -1; }

wait_for_exit() { # pid seconds description
    local deadline=$((SECONDS + $2))
    while kill -0 "$1" 2>/dev/null; do
        (( SECONDS < deadline )) || die "$3" "pid $1 still running after $2 s"
        sleep 0.2
    done
}

# Cookie header the fixture saw on the last /whoami?run=<tag>, or "null"/"missing".
server_cookie() {
    python3 - "$SERVER_LOG" "$1" <<'PY'
import json, sys
last = "missing"
for line in open(sys.argv[1]):
    entry = json.loads(line)
    if entry["path"] == "/whoami" and entry["query"] == "run=" + sys.argv[2]:
        last = entry["cookie"] if entry["cookie"] is not None else "null"
print(last)
PY
}

# Any request header the fixture logged for /whoami?run=<tag> (key: lang, ua, cookie).
server_field() {
    python3 - "$SERVER_LOG" "$1" "$2" <<'PY'
import json, sys
last = "missing"
for line in open(sys.argv[1]):
    entry = json.loads(line)
    if entry["path"] == "/whoami" and entry["query"] == "run=" + sys.argv[2]:
        last = entry[sys.argv[3]] if entry[sys.argv[3]] is not None else "null"
print(last)
PY
}

assert_clean_run() { # description: markers every successful run must show
    expect_log "$LAST_LOG" "BROWSER_SMOKE initialized backend=cef" "$1: surface initialized"
    expect_log "$LAST_LOG" "BROWSER_SMOKE closed" "$1: close event delivered"
    expect_log "$LAST_LOG" "BROWSER_SMOKE shutdown_done" "$1: shutdown completed"
    reject_log "$LAST_LOG" "BROWSER_SMOKE step_failed" "$1: no failed step"
    [[ "$SMOKE_EXIT" == "0" ]] || die "$1: exit status" "smoke exited $SMOKE_EXIT"
}

assert_no_profile_processes() { # profile description
    local leaked
    leaked="$(python3 "$SUPPORT/BrowserSurfaceProcessTree.py" profile-processes "$1")"
    [[ -z "$leaked" ]] || die "$2" "processes still carry the profile: $leaked"
}

assert_all_dead() { # snapshot file description seconds
    local deadline=$((SECONDS + $3)) survivors
    while :; do
        survivors="$(python3 "$SUPPORT/BrowserSurfaceProcessTree.py" alive "$1")"
        [[ -z "$survivors" ]] && return 0
        (( SECONDS < deadline )) || die "$2" "orphan processes (pid starttime): $survivors"
        sleep 0.2
    done
}

# ---------------------------------------------------------------------------------
echo "BROWSER_TEST config smoke=$SMOKE library=$LIBRARY work=$WORK"

if command -v ldd >/dev/null 2>&1; then
    ldd "$SMOKE" > "$LOG_DIR/smoke.ldd" 2>&1 || true
    reject_log "$LOG_DIR/smoke.ldd" "libcef" "the smoke executable must not link libcef"
    pass "smoke_does_not_link_libcef"
fi
if [[ -f "$EDITOR" ]] && command -v readelf >/dev/null 2>&1; then
    readelf -d "$EDITOR" > "$LOG_DIR/editor.dynamic" 2>&1 || true
    reject_log "$LOG_DIR/editor.dynamic" "libcef|SpiralBrowserHost" "the Editor must not link the browser engine"
    pass "editor_does_not_link_cef"
fi

# --- A missing library never maps libcef and fails cleanly.
run_smoke missing-library "$WORK/profile-missing" "wait-ms:10" "--lib=$WORK/does-not-exist.so"
[[ "$SMOKE_EXIT" == "3" ]] || die "missing library exit status" "exit $SMOKE_EXIT"
expect_log "$LAST_LOG" "BROWSER_SMOKE start .*libcef_mapped=0" "missing library: libcef not mapped at start"
expect_log "$LAST_LOG" "BROWSER_SMOKE load_failed" "missing library: reported"
reject_log "$LAST_LOG" "libcef_mapped=1" "missing library: libcef never mapped"
pass "missing_library_never_maps_libcef"

# --- First paint, frame mirroring through dirty rectangles, and DPI scale.
run_smoke paint "$WORK/profile-paint" \
    "navigate:$URL/paint;wait-load;expect-pixel:100,100,c8640a;expect-pixel:600,300,c8640a;expect-size:640x360;view:320x200@1;expect-size:320x200;expect-pixel:300,180,c8640a;view:480x270@2;expect-size:960x540;expect-pixel:900,500,c8640a;wait-ms:200" \
    "--dump-frame=$LOG_DIR/paint.ppm"
assert_clean_run "paint"
expect_log "$LAST_LOG" "BROWSER_SMOKE start .*libcef_mapped=0" "libcef is not mapped before the library loads"
expect_log "$LAST_LOG" "BROWSER_SMOKE library_loaded libcef_mapped=1" "libcef is mapped by loading the library"
expect_log "$LAST_LOG" "BROWSER_SMOKE abi_mismatch_refused value=1" "a mismatched config size is refused by the factory"
expect_log "$LAST_LOG" "BROWSER_SMOKE first_frame .*width=640 height=360" "first paint frame arrived"
expect_log "$LAST_LOG" "BROWSER_SMOKE pixel_ok x=100 y=100 rgb=c8640a" "page pixels reached the listener"
expect_log "$LAST_LOG" "BROWSER_SMOKE size_ok width=960 height=540" "device scale 2 doubles the frame size"
expect_log "$LAST_LOG" "BROWSER_SMOKE size_ok width=320 height=200" "a resize changes the frame size"
pass "first_paint_dirty_rect_frames_resize_and_dpi_scale"
[[ "$(stat -c %a "$WORK/profile-paint")" == "700" ]] || die "the profile directory must be owner-only (0700)"
pass "profile_directory_is_owner_only"
assert_no_profile_processes "$WORK/profile-paint" "paint run left browser processes"
pass "clean_shutdown_leaves_no_processes"

# --- Mouse, wheel, key, char, Enter, Backspace.
run_smoke input "$WORK/profile-input" \
    "navigate:$URL/input;wait-load;focus:1;expect-pixel:320,300,ff0000;click:50,40;expect-pixel:320,30,00ff00;type:a;expect-pixel:370,30,00ff00;type:z;expect-pixel:420,30,00ff00;press:enter;expect-pixel:470,30,00ff00;expect-pixel:520,30,00ff00;press:backspace;expect-pixel:570,30,00ff00;wheel:320,200,0,-600;expect-pixel:320,300,0000ff"
assert_clean_run "input"
pass "mouse_wheel_key_char_input_reach_the_page"

# --- A <select> dropdown is a separate popup widget; it must be composited into
# the frame while open and leave no stale pixels when it closes.
run_smoke popup-widget "$WORK/profile-popup" \
    "navigate:$URL/select;wait-load;expect-pixel:300,200,123456;focus:1;move:100,30;click:100,30;expect-not-pixel:100,90,123456;press:escape;expect-pixel:100,90,123456"
assert_clean_run "popup-widget"
pass "select_popup_is_composited_and_removed"

# --- Editing shortcuts run as frame commands (copy, paste, cut, select all).
run_smoke clipboard "$WORK/profile-clipboard" \
    "navigate:$URL/clip;wait-load;focus:1;click:100,30;type:ab;expect-pixel:450,60,00ff00;chord:ctrl+a;chord:ctrl+c;press:right;chord:ctrl+v;expect-pixel:450,60,0000ff;chord:ctrl+a;chord:ctrl+x;expect-pixel:450,60,808080;type:ab;expect-pixel:450,60,00ff00;press:backspace;expect-pixel:450,60,ffff00;chord:ctrl+a;chord:ctrl+x;char:233;expect-pixel:450,60,ff00ff"
assert_clean_run "clipboard"
pass "clipboard_shortcuts_copy_paste_cut_select_all"
expect_log "$LAST_LOG" "BROWSER_SMOKE cursor value=1" "the text field reports an I-beam cursor"
pass "non_ascii_text_commits_through_ime_and_cursor_changes_reach_the_listener"

# --- The frame-rate cap is honoured, a hidden surface stops painting, and a
# shown one resumes (full-canvas animation, so the page itself never idles).
run_smoke frame-cap-30 "$WORK/profile-fps30" \
    "navigate:$URL/anim;wait-load;wait-ms:1000;frames:a;wait-ms:3000;frames:b;visible:0;wait-ms:500;frames:c;wait-ms:2000;frames:d;visible:1;wait-ms:1500;frames:e" \
    "--max-fps=30"
assert_clean_run "frame-cap-30"
frame_marker() { sed -n "s/^BROWSER_SMOKE frames name=$2 count=\\([0-9]*\\) ms=\\([0-9]*\\)\$/\\1 \\2/p" "$1"; }
read -r FRAMES_A MS_A < <(frame_marker "$LAST_LOG" a)
read -r FRAMES_B MS_B < <(frame_marker "$LAST_LOG" b)
read -r FRAMES_C _ < <(frame_marker "$LAST_LOG" c)
read -r FRAMES_D _ < <(frame_marker "$LAST_LOG" d)
read -r FRAMES_E _ < <(frame_marker "$LAST_LOG" e)
FPS=$(( (FRAMES_B - FRAMES_A) * 1000 / (MS_B - MS_A) ))
echo "BROWSER_TEST frame_rate visible_fps=$FPS hidden_frames=$((FRAMES_D - FRAMES_C)) resumed_frames=$((FRAMES_E - FRAMES_D))"
(( FPS >= 20 && FPS <= 36 )) || die "a 30 fps cap must deliver 20..36 frames per second" "measured $FPS"
pass "frame_rate_cap_is_honoured ($FPS fps at a 30 fps cap)"
(( FRAMES_D - FRAMES_C <= 1 )) || die "a hidden surface must stop painting" "frames while hidden: $((FRAMES_D - FRAMES_C))"
(( FRAMES_E - FRAMES_D >= 15 )) || die "a shown surface must resume painting" "frames after showing: $((FRAMES_E - FRAMES_D))"
pass "hidden_surface_stops_painting_and_resumes"

# --- Cookies: a Max-Age cookie and (persist_session_cookies) a session cookie
# survive a process restart; a different profile sees neither.
COOKIE_PROFILE="$WORK/profile-cookies"
run_smoke cookies-set "$COOKIE_PROFILE" "navigate:$URL/;wait-load;expect-pixel:100,100,204060;wait-ms:300"
assert_clean_run "cookies-set"
run_smoke cookies-restart "$COOKIE_PROFILE" "navigate:$URL/whoami?run=restart;wait-load;expect-pixel:100,100,602040"
assert_clean_run "cookies-restart"
RESTART_COOKIE="$(server_cookie restart)"
[[ "$RESTART_COOKIE" == *"spiral_persist=abc123"* ]] || die "Max-Age cookie did not survive a restart" "server saw: $RESTART_COOKIE"
pass "max_age_cookie_survives_restart"
RESTART_AGENT="$(python3 - "$SERVER_LOG" <<'PY'
import json, sys
agent = ""
for line in open(sys.argv[1]):
    entry = json.loads(line)
    if entry["path"] == "/whoami" and entry["query"] == "run=restart":
        agent = entry["ua"] or ""
print(agent)
PY
)"
[[ "$RESTART_AGENT" == Mozilla/5.0* && "$RESTART_AGENT" == *" Chrome/"* && ! "$RESTART_AGENT" =~ (CEF|Spiral|Headless) ]] \
    || die "the default user agent must be unmodified" "server saw: $RESTART_AGENT"
pass "default_user_agent_is_unmodified"
[[ "$RESTART_COOKIE" == *"spiral_session=sess456"* ]] || die "session cookie did not survive a restart" "server saw: $RESTART_COOKIE"
pass "session_cookie_survives_restart_with_persist_session_cookies"
run_smoke cookies-control "$WORK/profile-cookies-control" "navigate:$URL/whoami?run=control;wait-load;expect-pixel:100,100,602040"
assert_clean_run "cookies-control"
[[ "$(server_cookie control)" == "null" ]] || die "a fresh profile must send no cookie" "server saw: $(server_cookie control)"
pass "fresh_profile_sends_no_cookie"

# --- Third-party cookies keep the engine default (nothing is configured): a
# cross-site frame (the shape of a captcha frame inside a sign-in page) can set
# and read a SameSite=None cookie and gets it back on the next visit. The fixture
# server's log of the Cookie header is the independent oracle.
run_smoke third-party-cookies "$WORK/profile-third-party" \
    "navigate:$URL/tp;wait-load;expect-pixel:300,200,00ff00;wait-ms:300;navigate:$URL/tp?again=1;wait-load;expect-pixel:300,200,00ff00;wait-ms:300"
assert_clean_run "third-party-cookies"
THIRD_PARTY_COOKIE="$(python3 - "$SERVER_LOG" <<'PY'
import json, sys
last = "missing"
for line in open(sys.argv[1]):
    entry = json.loads(line)
    if entry["path"] == "/tpframe":
        last = entry["cookie"] if entry["cookie"] is not None else "null"
print(last)
PY
)"
[[ "$THIRD_PARTY_COOKIE" == *"tp_header=1"* && "$THIRD_PARTY_COOKIE" == *"tp_js=1"* ]] \
    || die "a cross-site frame must send back the cookies it set" "server saw: $THIRD_PARTY_COOKIE"
pass "third_party_cookies_work_in_a_cross_site_frame_with_engine_defaults"

# --- Downloads: staged owner-only with matching SHA-256, type policy enforced,
# and no URL, token, or query reaches any output.
DOWNLOAD_STAGING="$WORK/staging-download"
SMOKE_STAGING="$DOWNLOAD_STAGING"
run_smoke download "$WORK/profile-download" \
    "navigate:$URL/dlpage;wait-load;expect-pixel:400,300,304050;click:100,40;wait-download:completed;click:100,150;wait-download:blocked;wait-ms:500"
SMOKE_STAGING="$WORK/staging"
assert_clean_run "download"
expect_log "$LAST_LOG" "BROWSER_SMOKE download state=completed .*name=spiral-test.zip bytes=3145728" "download completed event"
expect_log "$LAST_LOG" "BROWSER_SMOKE download state=blocked .*name=evil.exe" "disallowed type reported as blocked"
STAGED_FILES=("$DOWNLOAD_STAGING"/*/*)
[[ "${#STAGED_FILES[@]}" == "1" && -f "${STAGED_FILES[0]}" ]] || die "exactly one file must be staged" "found: ${STAGED_FILES[*]}"
ACTUAL_HASH="$(sha256sum "${STAGED_FILES[0]}" | cut -d' ' -f1)"
[[ "$ACTUAL_HASH" == "$(tr -d '\n' < "$WORK/expected.sha256")" ]] || die "staged download hash mismatch" "got $ACTUAL_HASH"
pass "download_staged_with_matching_sha256"
[[ "$(stat -c %a "$DOWNLOAD_STAGING")" == "700" && "$(stat -c %a "$(dirname "${STAGED_FILES[0]}")")" == "700" \
    && "$(stat -c %a "${STAGED_FILES[0]}")" == "600" ]] || die "staging permissions must be 0700 directories and a 0600 file"
pass "staging_is_owner_only"
[[ -z "$(find "$DOWNLOAD_STAGING" -iname 'evil*')" ]] || die "a blocked download left a file behind"
pass "blocked_download_leaves_nothing"
cat "$LOG_DIR"/*.log > "$WORK/all-output.txt"
reject_log "$WORK/all-output.txt" "SECRETTOKEN123|/dl\\.zip|/evil\\.exe|token=" "no download URL or token in any output"
pass "no_url_or_token_in_output"

# --- Navigation and popups: only allow-listed top-level documents load.
run_smoke navigation "$WORK/profile-navigation" \
    "navigate:$URL/nav;wait-load;expect-pixel:400,300,405030;click:100,40;click:100,130;click:100,230;wait-denials:3;navigate:https://example.net/;navigate:http://$HOST_NAME/idle;wait-denials:5;wait-ms:500;expect-pixel:400,300,405030;navigate:https://unreachable.spiral.test/;wait-failures:1" \
    "--provider-host=unreachable.spiral.test"
assert_clean_run "navigation"
DENIED_HOSTS="$(sed -n 's/^BROWSER_SMOKE navigation_denied host=//p' "$LAST_LOG" | sort | tr '\n' ' ')"
[[ "$DENIED_HOSTS" == "example.com example.net example.org $HOST_NAME popup.spiral.test " ]] || die "denied navigation hosts" "got: $DENIED_HOSTS"
expect_log "$LAST_LOG" "BROWSER_SMOKE summary .*denials=5 failures=1" "an allowed host that fails to load is a failure, not a denial"
expect_log "$LAST_LOG" "BROWSER_SMOKE failed reason=the page failed to load" "load failure reported"
pass "off_list_navigation_scheme_and_popups_denied"

# --- A popup whose target host is allowed opens in the panel itself (no window is
# created); the denial counter stays at zero.
run_smoke popup-redirect "$WORK/profile-popup-redirect" \
    "navigate:$URL/popup-ok;wait-load;expect-pixel:400,300,405030;focus:1;click:100,40;wait-popups:1;expect-pixel:100,100,c8640a;wait-ms:300"
assert_clean_run "popup-redirect"
expect_log "$LAST_LOG" "BROWSER_SMOKE popup_redirected host=$HOST_NAME" "the allowed popup target was reported"
expect_log "$LAST_LOG" "BROWSER_SMOKE summary .*denials=0 failures=0" "an allowed popup is not a denial"
reject_log "$LAST_LOG" "navigation_denied" "an allowed popup is not denied"
pass "allowed_popup_opens_in_the_panel"

# --- Denied-navigation consent: the denial offers its host, the target is kept
# inside the surface, a retry without a grant stays denied, and a grant followed
# by a retry loads the kept target. The target is consumed by every retry.
run_smoke consent "$WORK/profile-consent" \
    "navigate:$URL/consent;wait-load;expect-pixel:400,300,304050;focus:1;click:100,40;wait-consents:1;retry-denied;wait-ms:300;expect-pixel:400,300,304050;navigate:$URL/consent;wait-load;click:100,40;wait-consents:2;grant:consent.spiral.test;retry-denied;expect-pixel:100,100,c8640a;retry-denied;ungrant-all;navigate:https://consent.spiral.test/paint;wait-denials:3;wait-consents:3;wait-ms:300"
assert_clean_run "consent"
[[ "$(grep -c 'BROWSER_SMOKE consent_offered host=consent.spiral.test$' "$LAST_LOG")" == "3" ]] || die "consent was offered three times for the denied host"
[[ "$(sed -n 's/^BROWSER_SMOKE retry_denied result=//p' "$LAST_LOG" | tr '\n' ' ')" == "0 1 0 " ]] \
    || die "retry results (without a grant, with a grant, with nothing kept)" "got: $(sed -n 's/^BROWSER_SMOKE retry_denied result=//p' "$LAST_LOG" | tr '\n' ' ')"
expect_log "$LAST_LOG" "BROWSER_SMOKE summary .*denials=3 failures=0" "three denials, no load failure"
pass "denied_navigation_consent_grants_and_retries_the_kept_target"
reject_log "$LAST_LOG" "/paint|step=1" "no denied address or query reaches any output"
pass "consent_markers_carry_the_host_only"

# --- Truthful screen geometry: the page sees the monitor and work area it was
# told about, unchanged when the view is resized, in DIPs at a device scale.
SCREEN_CHECKS="expect-pixel:30,30,00ff00;expect-pixel:80,30,00ff00;expect-pixel:130,30,00ff00;expect-pixel:180,30,00ff00"
run_smoke screen-info "$WORK/profile-screen" \
    "screen:0,0,2560,1440:0,0,2560,1400;navigate:$URL/screen?e=2560,1440,2560,1400;wait-load;$SCREEN_CHECKS;view:480x270@1;navigate:$URL/screen?e=2560,1440,2560,1400;wait-load;$SCREEN_CHECKS;view:320x200@1;navigate:$URL/screen?e=2560,1440,2560,1400;wait-load;$SCREEN_CHECKS;view:480x270@2;navigate:$URL/screen?e=1280,720,1280,700;wait-load;expect-pixel:60,60,00ff00;expect-pixel:160,60,00ff00;expect-pixel:260,60,00ff00;expect-pixel:360,60,00ff00"
assert_clean_run "screen-info"
pass "page_screen_geometry_is_the_reported_monitor_and_ignores_view_size"

# --- Accept-Language and navigator.languages follow the user's locale
# variables; the user agent stays untouched (checked above).
LANGUAGE="it_IT:en_US" LC_ALL="" LC_MESSAGES="" LANG="en_US.UTF-8" run_smoke language-italian "$WORK/profile-lang-it" \
    "navigate:$URL/lang?e=it-IT%2Cit%2Cen-US%2Cen;wait-load;expect-pixel:300,300,00ff00;navigate:$URL/whoami?run=lang-it;wait-load;expect-pixel:100,100,602040"
assert_clean_run "language-italian"
ITALIAN_LANGUAGE="$(server_field lang-it lang)"
[[ "$ITALIAN_LANGUAGE" == it-IT,it\;q=* ]] || die "Accept-Language must follow LANGUAGE" "server saw: $ITALIAN_LANGUAGE"
LANGUAGE="" LC_ALL="" LC_MESSAGES="" LANG="en_US.UTF-8" run_smoke language-english "$WORK/profile-lang-en" \
    "navigate:$URL/lang?e=en-US%2Cen;wait-load;expect-pixel:300,300,00ff00"
assert_clean_run "language-english"
pass "accept_language_follows_the_locale_variables"

# --- Single instance: a second surface on a live profile reports it as in use.
IN_USE_PROFILE="$WORK/profile-in-use"
start_smoke in-use-holder "$IN_USE_PROFILE" "navigate:$URL/paint;wait-load;expect-pixel:100,100,c8640a;marker:ready;hold"
wait_for_marker "$BG_LOG" "marker name=ready" 60 "in-use holder never became ready"
HOLDER_LOG="$BG_LOG"
HOLDER_PID="$(smoke_pid_from_log "$HOLDER_LOG")"
run_smoke in-use-second "$IN_USE_PROFILE" "wait-ms:10"
[[ "$SMOKE_EXIT" == "3" ]] || die "second instance exit status" "exit $SMOKE_EXIT"
expect_log "$LAST_LOG" "BROWSER_SMOKE init_failed error=the browser profile is in use" "second instance reports the profile in use"
kill -0 "$HOLDER_PID" 2>/dev/null || die "the first instance must survive a second attempt"
reject_log "$HOLDER_LOG" "navigation_denied|step_failed|failed reason" "the first instance saw no side effect"
pass "second_instance_reports_profile_in_use"

# --- SIGTERM is delivered to the host's own handler and shuts down gracefully
# with every browser process gone.
python3 "$SUPPORT/BrowserSurfaceProcessTree.py" descendants "$HOLDER_PID" > "$WORK/holder.tree"
TREE_SIZE="$(wc -l < "$WORK/holder.tree")"
(( TREE_SIZE >= 4 )) || die "the browser should run several processes" "saw $TREE_SIZE"
kill -TERM "$HOLDER_PID"
wait_for_exit "$HOLDER_PID" 30 "holder did not exit after SIGTERM"
expect_log "$HOLDER_LOG" "BROWSER_SMOKE signal_received" "the host's SIGTERM handler ran (CEF must not own it)"
expect_log "$HOLDER_LOG" "BROWSER_SMOKE shutdown_done" "graceful shutdown after SIGTERM"
assert_all_dead "$WORK/holder.tree" "no orphans after graceful shutdown" 5
assert_no_profile_processes "$IN_USE_PROFILE" "graceful shutdown left browser processes"
pass "sigterm_graceful_shutdown_no_orphans ($TREE_SIZE processes)"

# --- Sign-out removes the whole profile and everything it held.
SIGNOUT_PROFILE="$WORK/profile-signout"
run_smoke signout "$SIGNOUT_PROFILE" "navigate:$URL/;wait-load;expect-pixel:100,100,204060;wait-ms:300;signout" "--reinit-probe"
assert_clean_run "signout"
[[ ! -e "$SIGNOUT_PROFILE" ]] || die "sign-out must delete the profile directory"
expect_log "$LAST_LOG" "BROWSER_SMOKE second_initialize accepted=0 error=the browser engine can only be started once per process" \
    "a second engine start in one process is refused (CEF crashes on it)"
run_smoke signout-after "$SIGNOUT_PROFILE" "navigate:$URL/whoami?run=after-signout;wait-load;expect-pixel:100,100,602040"
assert_clean_run "signout-after"
[[ "$(server_cookie after-signout)" == "null" ]] || die "cookies survived sign-out" "server saw: $(server_cookie after-signout)"
pass "sign_out_removes_the_profile"

# --- SIGKILL of the host: every browser process dies, and the profile reopens.
KILL_PROFILE="$WORK/profile-kill"
start_smoke kill9 "$KILL_PROFILE" "navigate:$URL/paint;wait-load;marker:ready;hold"
wait_for_marker "$BG_LOG" "marker name=ready" 60 "kill -9 victim never became ready"
KILL_PID="$(smoke_pid_from_log "$BG_LOG")"
python3 "$SUPPORT/BrowserSurfaceProcessTree.py" descendants "$KILL_PID" > "$WORK/kill.tree"
KILL_TREE_SIZE="$(wc -l < "$WORK/kill.tree")"
(( KILL_TREE_SIZE >= 4 )) || die "the browser should run several processes" "saw $KILL_TREE_SIZE"
KILL_STARTED=$SECONDS
kill -KILL "$KILL_PID"
assert_all_dead "$WORK/kill.tree" "orphan browser processes survive kill -9 of the host" 10
assert_no_profile_processes "$KILL_PROFILE" "kill -9 left browser processes"
pass "kill9_leaves_no_orphans ($KILL_TREE_SIZE processes, $((SECONDS - KILL_STARTED)) s)"
run_smoke kill9-reopen "$KILL_PROFILE" "navigate:$URL/paint;wait-load;expect-pixel:100,100,c8640a"
assert_clean_run "kill9-reopen"
pass "profile_reopens_after_kill9"

# --- A hidden idle surface is nearly free: under 1 percent of a core over 10 s
# for the whole process tree.
IDLE_PROFILE="$WORK/profile-idle"
start_smoke idle "$IDLE_PROFILE" "navigate:$URL/idle;wait-load;expect-pixel:100,100,223344;visible:0;marker:idle;hold"
wait_for_marker "$BG_LOG" "marker name=idle" 60 "idle surface never became ready"
IDLE_LOG="$BG_LOG"
IDLE_PID="$(smoke_pid_from_log "$IDLE_LOG")"
sleep 5
IDLE_RESULT="$(python3 "$SUPPORT/BrowserSurfaceProcessTree.py" cpu "$IDLE_PID" 10)"
echo "BROWSER_TEST idle_cpu $IDLE_RESULT"
IDLE_PERCENT="${IDLE_RESULT#percent=}"
IDLE_PERCENT="${IDLE_PERCENT%% *}"
python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) < 1.0 else 1)" "$IDLE_PERCENT" || die "hidden idle CPU is not below 1 percent" "$IDLE_RESULT"
[[ "$IDLE_RESULT" == *"stable=1"* ]] || die "the process tree changed during the idle sample" "$IDLE_RESULT"
kill -TERM "$IDLE_PID"
wait_for_exit "$IDLE_PID" 30 "idle surface did not exit after SIGTERM"
assert_no_profile_processes "$IDLE_PROFILE" "idle surface left browser processes"
pass "hidden_idle_cpu_below_one_percent ($IDLE_PERCENT)"

# --- The Chromium sandbox is mandatory: a system that denies user namespaces is
# a clean error, never an in-process abort and never a --no-sandbox fallback.
if command -v systemd-run >/dev/null 2>&1 && systemd-run --user --pipe --wait --quiet true >/dev/null 2>&1; then
    SANDBOX_LOG="$LOG_DIR/sandbox-denied.log"
    echo "BROWSER_TEST run sandbox-denied"
    set +e
    timeout --kill-after=5 "$SMOKE_TIMEOUT_SECONDS" systemd-run --user --pipe --wait --quiet \
        -p RestrictNamespaces=~user -p NoNewPrivileges=yes -p "WorkingDirectory=$WORK" \
        "$SMOKE" "--lib=$LIBRARY" "--profile=$WORK/profile-sandbox" "--staging=$WORK/staging-sandbox" \
        "--steps=wait-ms:10" 2>&1 | tee "$SANDBOX_LOG"
    SANDBOX_EXIT=${PIPESTATUS[0]}
    set -e
    [[ "$SANDBOX_EXIT" == "3" ]] || die "sandbox-denied exit status" "exit $SANDBOX_EXIT"
    expect_log "$SANDBOX_LOG" "BROWSER_SMOKE init_failed error=the Chromium sandbox is unavailable" "denied sandbox is a clean initialization error"
    [[ ! -e "$WORK/profile-sandbox" ]] || die "a denied sandbox must not create the profile"
    pass "denied_user_namespace_sandbox_is_a_clean_error"
else
    die "sandbox-denied scenario cannot run" "systemd-run --user is unavailable; this check is required"
fi

# --- Profile placement: never inside a project, never over an unrelated directory.
mkdir -p "$WORK/project"
: > "$WORK/project/game.spiralproject"
run_smoke profile-in-project "$WORK/project/browser-profile" "wait-ms:10"
[[ "$SMOKE_EXIT" == "3" ]] || die "profile-in-project exit status" "exit $SMOKE_EXIT"
expect_log "$LAST_LOG" "BROWSER_SMOKE init_failed error=the browser profile directory must not be inside a project" "profile inside a project is refused"
[[ ! -e "$WORK/project/browser-profile" ]] || die "a refused profile must not be created inside the project"
mkdir -p "$WORK/not-a-profile"
: > "$WORK/not-a-profile/notes.txt"
run_smoke profile-not-a-profile "$WORK/not-a-profile" "wait-ms:10"
[[ "$SMOKE_EXIT" == "3" ]] || die "not-a-profile exit status" "exit $SMOKE_EXIT"
expect_log "$LAST_LOG" "BROWSER_SMOKE init_failed error=the browser profile directory is not empty and is not a browser profile" "unrelated directory is refused"
[[ -f "$WORK/not-a-profile/notes.txt" ]] || die "an unrelated directory must be left untouched"
# A staging directory that reaches into the profile through a symbolic link is
# nested after canonicalisation, which the lexical config check cannot see.
mkdir -p "$WORK/symlink-profile"
ln -s "$WORK/symlink-profile" "$WORK/symlink-alias"
SMOKE_STAGING="$WORK/symlink-alias/inner"
run_smoke staging-through-symlink "$WORK/symlink-profile" "wait-ms:10"
SMOKE_STAGING="$WORK/staging"
[[ "$SMOKE_EXIT" == "3" ]] || die "nested staging exit status" "exit $SMOKE_EXIT"
expect_log "$LAST_LOG" "BROWSER_SMOKE init_failed error=the profile and download staging directories must be distinct and not nested" \
    "a staging directory nested in the profile through a symlink is refused"
pass "profile_placement_is_checked_before_anything_is_created"

assert_no_profile_processes "$WORK" "browser processes remain after the whole run"
pass "no_browser_processes_remain"

FAILED=0
echo "BROWSER_SURFACE_TEST PASSED checks=$CHECKS"
