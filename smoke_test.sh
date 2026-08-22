#!/bin/bash
# smoke_test.sh
#
# Functional smoke test for lwm, run under a headless Xvfb display. This is
# not a substitute for `./lwm -test` (which covers pure logic); it's a
# behavioural regression check for the parts that only a live X server can
# exercise: does lwm actually frame, move, resize, focus, hide, shape and
# close windows without crashing or logging errors.
#
# Usage: ./smoke_test.sh [--record-golden] [path-to-lwm-binary]
#
# Exits 0 if every check passes, 1 otherwise. Prints a PASS/FAIL line per
# check.
#
# --record-golden rewrites testdata/golden_shim_calls.txt from this run
# instead of comparing against it. See "the golden shim-call set" below.

set -u

RECORD_GOLDEN=0
if [ "${1:-}" = "--record-golden" ]; then
  RECORD_GOLDEN=1
  shift
fi

LWM_BIN="${1:-./lwm}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GOLDEN_FILE="${SCRIPT_DIR}/testdata/golden_shim_calls.txt"

DISPLAY_NUM=$((90 + RANDOM % 400))
DISPLAY_SPEC=":${DISPLAY_NUM}"
SCREEN_W=1280
SCREEN_H=1024
WORKDIR="$(mktemp -d)"
XVFB_LOG="${WORKDIR}/xvfb.log"
LWM_LOG="${WORKDIR}/lwm.log"
CLI_FIFO="${WORKDIR}/cli"

export DISPLAY="${DISPLAY_SPEC}"

FAILURES=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# check <description> <condition-exit-code-command...>
check() {
  local desc="$1"
  shift
  if "$@"; then pass "${desc}"; else fail "${desc}"; fi
}

# wait_for <timeout-tenths> <command...>: polls until the command succeeds.
wait_for() {
  local tries="$1"
  shift
  local i
  for ((i = 0; i < tries; i++)); do
    "$@" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  return 1
}

# Send a line to lwm's -debugcli on stdin.
cli() { echo "$1" >"${CLI_FIFO}"; }

cleanup() {
  [ -n "${XEYES_PID:-}" ] && kill "${XEYES_PID}" >/dev/null 2>&1
  [ -n "${XTERM_PID:-}" ] && kill "${XTERM_PID}" >/dev/null 2>&1
  [ -n "${CLI_HOLD_PID:-}" ] && kill "${CLI_HOLD_PID}" >/dev/null 2>&1
  [ -n "${LWM_PID:-}" ] && kill "${LWM_PID}" >/dev/null 2>&1
  [ -n "${XVFB_PID:-}" ] && kill "${XVFB_PID}" >/dev/null 2>&1
  wait >/dev/null 2>&1
  rm -rf "${WORKDIR}"
}
trap cleanup EXIT

if [ ! -x "${LWM_BIN}" ]; then
  echo "lwm binary not found or not executable: ${LWM_BIN}" >&2
  exit 1
fi

Xvfb "${DISPLAY_SPEC}" -screen 0 "${SCREEN_W}x${SCREEN_H}x24" >"${XVFB_LOG}" 2>&1 &
XVFB_PID=$!
if ! wait_for 50 xdpyinfo; then
  echo "Xvfb did not start on ${DISPLAY_SPEC}" >&2
  cat "${XVFB_LOG}" >&2
  exit 1
fi

# lwm is run with the debug CLI on a fifo so the test can drive it (the fake
# xrandr layouts) and so that `dbg auto` turns on the per-client LOGD trace
# the golden shim-call check reads at the end. The extra `sleep infinity`
# holds the write end open, so lwm doesn't see EOF between commands.
mkfifo "${CLI_FIFO}"
sleep infinity >"${CLI_FIFO}" &
CLI_HOLD_PID=$!
"${LWM_BIN}" -debugcli="dbg auto" <"${CLI_FIFO}" >"${LWM_LOG}" 2>&1 &
LWM_PID=$!
sleep 0.5
if ! kill -0 "${LWM_PID}" 2>/dev/null; then
  fail "lwm started and stayed running"
  cat "${LWM_LOG}" >&2
  exit 1
fi
pass "lwm started and stayed running"

# --- framing ---------------------------------------------------------------

xterm -geometry 80x24+50+50 >"${WORKDIR}/xterm.log" 2>&1 &
XTERM_PID=$!

frame_id() {
  xwininfo -root -tree 2>/dev/null |
    grep -o '0x[0-9a-f]\+ "LWM frame for[^"]*"' | head -1 | awk '{print $1}'
}
FRAME_ID=""
for _ in $(seq 1 50); do
  FRAME_ID=$(frame_id)
  [ -n "${FRAME_ID}" ] && break
  sleep 0.1
done
if [ -n "${FRAME_ID}" ]; then
  pass "xterm got a frame window"
else
  fail "xterm got a frame window"
fi

CLIENT_ID=""
if [ -n "${FRAME_ID}" ]; then
  CLIENT_ID=$(xdotool search --class xterm 2>/dev/null | head -1)
  check "xdotool can find the client window" test -n "${CLIENT_ID}"
fi

# --- move / resize / focus, via the ConfigureRequest path -------------------

if [ -n "${CLIENT_ID}" ]; then
  xdotool windowmove --sync "${CLIENT_ID}" 200 200 >/dev/null 2>&1
  xdotool windowsize --sync "${CLIENT_ID}" 400 300 >/dev/null 2>&1
  sleep 0.3
  GEOM=$(xdotool getwindowgeometry --shell "${CLIENT_ID}" 2>/dev/null)
  GOT_X=$(echo "${GEOM}" | sed -n 's/^X=//p')
  GOT_Y=$(echo "${GEOM}" | sed -n 's/^Y=//p')
  # xdotool's target coordinates go through WM gravity/frame handling, so
  # allow slack rather than requiring an exact pixel match; the point of
  # this check is that EvConfigureRequest moved the window roughly where
  # asked, not pixel-perfect gravity semantics.
  if [ -n "${GOT_X}" ] && [ -n "${GOT_Y}" ] &&
    [ "${GOT_X}" -ge 150 ] && [ "${GOT_X}" -le 250 ] &&
    [ "${GOT_Y}" -ge 150 ] && [ "${GOT_Y}" -le 350 ]; then
    pass "EvConfigureRequest moved the client window"
  else
    fail "EvConfigureRequest moved the client window (got: ${GEOM})"
  fi
  if echo "${GEOM}" | grep -q "WIDTH=400" && echo "${GEOM}" | grep -q "HEIGHT=300"; then
    pass "EvConfigureRequest resized the client window"
  else
    fail "EvConfigureRequest resized the client window (got: ${GEOM})"
  fi

  xdotool windowactivate --sync "${CLIENT_ID}" >/dev/null 2>&1
  sleep 0.2
  FOCUSED=$(xdotool getactivewindow 2>/dev/null)
  if [ "${FOCUSED}" = "${CLIENT_ID}" ]; then
    pass "window activation gives it input focus"
  else
    fail "window activation gives it input focus (active=${FOCUSED}, want=${CLIENT_ID})"
  fi
fi

# --- _NET_WM_ICON: the 32-vs-64-bit property trap ---------------------------
#
# This is the specific detector for hazard 1 in docs/xcb-migration-plan.md.
# _NET_WM_ICON is CARDINAL[]/32: width, height, then one ARGB word per pixel.
# Xlib widens format-32 properties to 64-bit longs; XCB returns the real wire
# data. Read it at the wrong width and data[0] becomes (height << 32) | width,
# which fails ImageIcon::CreateFromPixels' sanity check and prints "Invalid
# width". So this asserts on the absence of that message: it is a genuine
# tripwire, not just a "didn't crash" check.

if [ -n "${CLIENT_ID}" ]; then
  ICON_LOG_MARK=$(wc -l <"${LWM_LOG}")
  xprop -id "${CLIENT_ID}" -f _NET_WM_ICON 32c \
    -set _NET_WM_ICON "2,2,4278190335,4278255360,4294901760,4294967295" \
    >/dev/null 2>&1
  sleep 0.4
  if tail -n "+${ICON_LOG_MARK}" "${LWM_LOG}" | grep -q "Invalid width"; then
    fail "_NET_WM_ICON read at the right width (icon data was misread)"
    tail -n "+${ICON_LOG_MARK}" "${LWM_LOG}" | grep "Invalid width" | sed 's/^/    /'
  else
    pass "_NET_WM_ICON read at the right width"
  fi
fi

# --- _NET_WM_STRUT reserves screen area -------------------------------------
#
# Also a 32-bit property read, and one with a visible consequence: lwm
# republishes _NET_WORKAREA on the root with the strut subtracted.

if [ -n "${CLIENT_ID}" ]; then
  xprop -id "${CLIENT_ID}" -f _NET_WM_STRUT 32c -set _NET_WM_STRUT "0,0,40,0" \
    >/dev/null 2>&1
  sleep 0.4
  WORKAREA=$(xprop -root _NET_WORKAREA 2>/dev/null |
    sed -n 's/.*= *//p' | tr -d ' ')
  WANT_WORKAREA="0,40,${SCREEN_W},$((SCREEN_H - 40))"
  if [ "${WORKAREA}" = "${WANT_WORKAREA}" ]; then
    pass "_NET_WM_STRUT updates _NET_WORKAREA"
  else
    fail "_NET_WM_STRUT updates _NET_WORKAREA (got '${WORKAREA}', want '${WANT_WORKAREA}')"
  fi
  # Put it back, so the later checks see a full-size work area.
  xprop -id "${CLIENT_ID}" -f _NET_WM_STRUT 32c -set _NET_WM_STRUT "0,0,0,0" \
    >/dev/null 2>&1
  sleep 0.3
fi

# --- _NET_WM_STATE full-screen round trip -----------------------------------
#
# An atom-array property read (also format 32), with the client window
# resizing to the whole screen as the observable effect.

if [ -n "${CLIENT_ID}" ]; then
  xprop -id "${CLIENT_ID}" -f _NET_WM_STATE 32a \
    -set _NET_WM_STATE _NET_WM_STATE_FULLSCREEN >/dev/null 2>&1
  sleep 0.5
  GEOM=$(xdotool getwindowgeometry --shell "${CLIENT_ID}" 2>/dev/null)
  if echo "${GEOM}" | grep -q "WIDTH=${SCREEN_W}" &&
    echo "${GEOM}" | grep -q "HEIGHT=${SCREEN_H}"; then
    pass "_NET_WM_STATE_FULLSCREEN fills the screen"
  else
    fail "_NET_WM_STATE_FULLSCREEN fills the screen (got: ${GEOM})"
  fi

  # Replace the state with a different one rather than removing the property:
  # ewmh_get_state only rewrites the flags when it can read a list, which is
  # what a real client does too (it never deletes _NET_WM_STATE outright).
  xprop -id "${CLIENT_ID}" -f _NET_WM_STATE 32a \
    -set _NET_WM_STATE _NET_WM_STATE_SKIP_PAGER >/dev/null 2>&1
  sleep 0.5
  GEOM=$(xdotool getwindowgeometry --shell "${CLIENT_ID}" 2>/dev/null)
  if echo "${GEOM}" | grep -q "WIDTH=400" && echo "${GEOM}" | grep -q "HEIGHT=300"; then
    pass "leaving full screen restores the old size"
  else
    fail "leaving full screen restores the old size (got: ${GEOM})"
  fi
fi

# --- hide, and the unhide menu ----------------------------------------------
#
# HIDE_BUTTON is button 3 on window furniture; button 3 on the root opens the
# unhide menu. Both go through the DragHandler machinery, which nothing else
# in this script touches.

menu_is_mapped() {
  xwininfo -root -tree 2>/dev/null |
    grep '"LWM unhide menu"' | grep -q '+[0-9]'
}

if [ -n "${CLIENT_ID}" ] && [ -n "${FRAME_ID}" ]; then
  FGEOM=$(xwininfo -id "${FRAME_ID}" 2>/dev/null)
  FX=$(echo "${FGEOM}" | sed -n 's/.*Absolute upper-left X: *//p')
  FY=$(echo "${FGEOM}" | sed -n 's/.*Absolute upper-left Y: *//p')
  FW=$(echo "${FGEOM}" | sed -n 's/.*Width: *//p')
  # Click in the middle of the title bar, well clear of the close cross.
  xdotool mousemove $((FX + FW / 2)) $((FY + 4)) click 3 >/dev/null 2>&1
  sleep 0.5
  STATE=$(xprop -id "${CLIENT_ID}" WM_STATE 2>/dev/null)
  if echo "${STATE}" | grep -q "Iconic"; then
    pass "button 3 on the title bar hides the window"
  else
    fail "button 3 on the title bar hides the window (WM_STATE: ${STATE})"
  fi

  # Open the unhide menu on the root window and keep the button down, which
  # is how the menu stays up.
  xdotool mousemove 600 400 mousedown 3 >/dev/null 2>&1
  sleep 0.4
  check "button 3 on the root opens the unhide menu" menu_is_mapped
  # Releasing over the first (and only) entry unhides it again.
  xdotool mouseup 3 >/dev/null 2>&1
  sleep 0.5
  STATE=$(xprop -id "${CLIENT_ID}" WM_STATE 2>/dev/null)
  if echo "${STATE}" | grep -q "Normal"; then
    pass "selecting from the unhide menu unhides the window"
  else
    fail "selecting from the unhide menu unhides the window (WM_STATE: ${STATE})"
  fi
fi

# --- shaped windows get no frame --------------------------------------------
#
# xeyes is non-rectangular, so isShaped() should be true and manage() should
# leave it unframed. This is the only thing in the suite that exercises the
# Shape extension at all.

xeyes >"${WORKDIR}/xeyes.log" 2>&1 &
XEYES_PID=$!
EYES_ID=""
for _ in $(seq 1 50); do
  EYES_ID=$(xdotool search --class '[Xx][Ee]yes' 2>/dev/null | head -1)
  [ -n "${EYES_ID}" ] && break
  sleep 0.1
done
if [ -n "${EYES_ID}" ]; then
  pass "xeyes mapped"
  sleep 0.4
  # A framed client is a child of an "LWM frame for ..." window; an unframed
  # one stays a direct child of the root.
  EYES_PARENT=$(xwininfo -id "${EYES_ID}" -tree 2>/dev/null |
    sed -n 's/.*Parent window id: *\(0x[0-9a-f]*\).*/\1/p')
  ROOT_ID=$(xwininfo -root 2>/dev/null |
    sed -n 's/.*Window id: *\(0x[0-9a-f]*\).*/\1/p')
  if [ "${EYES_PARENT}" = "${ROOT_ID}" ]; then
    pass "shaped window (xeyes) is left unframed"
  else
    fail "shaped window (xeyes) is left unframed (parent=${EYES_PARENT}, root=${ROOT_ID})"
  fi
else
  fail "xeyes mapped"
fi
kill "${XEYES_PID}" >/dev/null 2>&1
XEYES_PID=""
sleep 0.3

# --- fake multi-monitor layouts, via the debug CLI --------------------------
#
# The real xrandr path can't be driven from a test, but `xrandr <rects>` in
# the debug CLI feeds the same LScr::SetVisibleAreas that RRScreenChangeNotify
# ends up calling, so the client-repositioning logic underneath does get
# exercised.

CLI_MARK=$(wc -l <"${LWM_LOG}")
cli "xrandr 800x1200+0+0 800x500+800+0"
sleep 0.5
cli "xrandr ?"
sleep 0.5
if tail -n "+${CLI_MARK}" "${LWM_LOG}" |
  grep -q 'Without struts:.*800x1200+0+0.*800x500+800+0'; then
  pass "debug CLI xrandr sets a two-monitor layout"
else
  fail "debug CLI xrandr sets a two-monitor layout"
  tail -n "+${CLI_MARK}" "${LWM_LOG}" | grep -i 'struts' | sed 's/^/    /'
fi
cli "xrandr"
sleep 0.5

# --- close ------------------------------------------------------------------

if [ -n "${CLIENT_ID}" ]; then
  xdotool windowclose "${CLIENT_ID}" >/dev/null 2>&1
  for _ in $(seq 1 50); do
    kill -0 "${XTERM_PID}" 2>/dev/null || break
    sleep 0.1
  done
  if ! kill -0 "${XTERM_PID}" 2>/dev/null; then
    pass "WM_DELETE_WINDOW closed the client"
  else
    fail "WM_DELETE_WINDOW closed the client"
    kill "${XTERM_PID}" 2>/dev/null
  fi
  XTERM_PID=""
fi

sleep 0.3
check "lwm still running after the session" kill -0 "${LWM_PID}"

if grep -qE '^E ' "${LWM_LOG}"; then
  fail "no error-level log lines from lwm"
  grep -E '^E ' "${LWM_LOG}" | sed 's/^/    /'
else
  pass "no error-level log lines from lwm"
fi

# --- the golden shim-call set -----------------------------------------------
#
# Every xlib:: wrapper LOGDs itself, so with `dbg auto` the log above contains
# a trace of which X requests lwm made on behalf of each client. Comparing the
# exact *sequence* would be hopelessly flaky (a live X session reorders expose
# and motion events run to run), so what's compared is the set of distinct
# wrapper functions called. That is stable, and it is what actually matters
# during the XCB port: the shim's API isn't changing, only its guts, so if a
# step drops an X request entirely or starts making a new kind, this notices.
#
# Regenerate deliberately with --record-golden after an intentional change.

ACTUAL_CALLS="${WORKDIR}/shim_calls.txt"
grep -oE '^D [^ ]+ [^ ]+ xlib\.cc:[0-9]+: [^:]*: [A-Za-z_]+\(' "${LWM_LOG}" |
  sed 's/.*: \([A-Za-z_]*\)($/\1/' | sort -u >"${ACTUAL_CALLS}"

if [ ! -s "${ACTUAL_CALLS}" ]; then
  fail "captured some shim calls (is LOGD/dbg auto still working?)"
elif [ "${RECORD_GOLDEN}" = "1" ]; then
  mkdir -p "$(dirname "${GOLDEN_FILE}")"
  cp "${ACTUAL_CALLS}" "${GOLDEN_FILE}"
  echo "Recorded $(wc -l <"${GOLDEN_FILE}") shim calls to ${GOLDEN_FILE}"
elif [ ! -f "${GOLDEN_FILE}" ]; then
  fail "golden shim-call set exists (run with --record-golden to create it)"
elif diff -u "${GOLDEN_FILE}" "${ACTUAL_CALLS}" >"${WORKDIR}/shim.diff"; then
  pass "shim calls match the golden set"
else
  fail "shim calls match the golden set"
  sed 's/^/    /' "${WORKDIR}/shim.diff"
fi

echo
if [ "${FAILURES}" -eq 0 ]; then
  echo "All smoke checks passed."
  exit 0
else
  echo "${FAILURES} smoke check(s) failed. lwm log follows:"
  cat "${LWM_LOG}"
  exit 1
fi
