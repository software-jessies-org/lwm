#!/bin/bash
# strut_test.sh
#
# Regression test for start-up-order-dependent strut handling, run under a
# headless Xvfb display.
#
# A dock (gummiband, in the wild) sets _NET_WM_STRUT on its window to reserve
# an edge of the screen. In the field lwm honoured that strut only sometimes:
# start the dock before lwm and the reservation was ignored, start lwm first -
# or restart the dock - and it worked.
#
# The cause turned out to be on the dock's side: it interned _NET_WM_STRUT with
# XInternAtom(..., only_if_exists=True), which returns None on a display where
# nothing has interned that name yet. Started before the window manager on a
# fresh server, the dock therefore gave up and never set the property at all.
# Started after lwm - which interns the whole EWMH set on start-up - the name
# already existed and the strut was set. Nothing in lwm was misreading it.
#
# What lwm does own is the path that consumes such a strut when the dock is
# already on screen at start-up: the window-tree scan, manage() reading the
# property, ewmh_set_strut() folding every client's request into one screen
# reservation, and InitEWMH()'s recompute after the scan. Only the second half
# of that (a strut arriving as a PropertyNotify on an already-managed window)
# had test coverage, in smoke_test.sh. This test covers the start-up half, by
# mapping a strut-setting dock *before* lwm starts and checking all three
# things the strut should then affect:
#   1. _NET_WORKAREA published on the root,
#   2. lwm's own visible-areas-minus-struts (what placement actually reads),
#   3. where an auto-placed window actually lands.
#
# Usage: ./strut_test.sh [path-to-lwm-binary]
#
# Exits 0 if every check passes, 1 otherwise.

set -u

LWM_BIN="${1:-./lwm}"

DISPLAY_NUM=$((90 + RANDOM % 400))
DISPLAY_SPEC=":${DISPLAY_NUM}"
SCREEN_W=1280
SCREEN_H=1024
STRUT_TOP=200
WORKDIR="$(mktemp -d)"
XVFB_LOG="${WORKDIR}/xvfb.log"
LWM_LOG="${WORKDIR}/lwm.log"
CLI_FIFO="${WORKDIR}/cli"

export DISPLAY="${DISPLAY_SPEC}"

FAILURES=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

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

cli() { echo "$1" >"${CLI_FIFO}"; }

cleanup() {
  [ -n "${CLIENT_PID:-}" ] && kill "${CLIENT_PID}" >/dev/null 2>&1
  [ -n "${DOCK_PID:-}" ] && kill "${DOCK_PID}" >/dev/null 2>&1
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

Xvfb "${DISPLAY_SPEC}" -noreset -screen 0 "${SCREEN_W}x${SCREEN_H}x24" \
  >"${XVFB_LOG}" 2>&1 &
XVFB_PID=$!
if ! wait_for 50 xdpyinfo; then
  echo "Xvfb did not start on ${DISPLAY_SPEC}" >&2
  cat "${XVFB_LOG}" >&2
  exit 1
fi

# --- the dock, mapped before lwm exists -------------------------------------
#
# xterm stands in for gummiband: with no window manager running it maps itself
# at the position it asks for, and xprop can then dress it up as a dock. What
# matters is only that a viewable window carrying _NET_WM_STRUT is already on
# the screen when lwm performs its start-up window-tree scan.

# -e sleep, not a shell: an interactive shell rewrites the window title with
# its own escape sequence, and -title would never survive to be searched for.
xterm -geometry 100x2+0+0 -title lwm-strut-dock -e sleep 600 \
  >"${WORKDIR}/dock.log" 2>&1 &
DOCK_PID=$!

DOCK_ID=""
for _ in $(seq 1 50); do
  DOCK_ID=$(xdotool search --name lwm-strut-dock 2>/dev/null | head -1)
  [ -n "${DOCK_ID}" ] && break
  sleep 0.1
done
if [ -z "${DOCK_ID}" ]; then
  echo "dock window never appeared" >&2
  exit 1
fi

# _NET_WM_WINDOW_TYPE_DOCK keeps lwm from framing it (as it doesn't frame
# gummiband), which also keeps it out of the auto-placement cascade.
xprop -id "${DOCK_ID}" -f _NET_WM_WINDOW_TYPE 32a \
  -set _NET_WM_WINDOW_TYPE _NET_WM_WINDOW_TYPE_DOCK >/dev/null 2>&1
# _NET_WM_STRUT is left, right, top, bottom.
xprop -id "${DOCK_ID}" -f _NET_WM_STRUT 32c \
  -set _NET_WM_STRUT "0,0,${STRUT_TOP},0" >/dev/null 2>&1

STRUT_SET=$(xprop -id "${DOCK_ID}" _NET_WM_STRUT 2>/dev/null)
if echo "${STRUT_SET}" | grep -q "0, 0, ${STRUT_TOP}, 0"; then
  pass "dock set _NET_WM_STRUT before lwm started"
else
  fail "dock set _NET_WM_STRUT before lwm started (got '${STRUT_SET}')"
  exit 1
fi

# --- now start lwm ----------------------------------------------------------

mkfifo "${CLI_FIFO}"
sleep infinity >"${CLI_FIFO}" &
CLI_HOLD_PID=$!
# HOME is redirected at the temp dir so the developer's own ~/.Xresources
# can't change the frame metrics this test asserts on.
HOME="${WORKDIR}" "${LWM_BIN}" -debugcli="" <"${CLI_FIFO}" \
  >"${LWM_LOG}" 2>&1 &
LWM_PID=$!
sleep 1
if ! kill -0 "${LWM_PID}" 2>/dev/null; then
  fail "lwm started and stayed running"
  cat "${LWM_LOG}" >&2
  exit 1
fi
pass "lwm started and stayed running"

# --- 1: the strut reaches _NET_WORKAREA -------------------------------------

WORKAREA=$(xprop -root _NET_WORKAREA 2>/dev/null |
  sed 's/.*= //' | tr -d ' ')
WANT_WORKAREA="0,${STRUT_TOP},${SCREEN_W},$((SCREEN_H - STRUT_TOP))"
if [ "${WORKAREA}" = "${WANT_WORKAREA}" ]; then
  pass "strut set before lwm started reaches _NET_WORKAREA"
else
  fail "strut set before lwm started reaches _NET_WORKAREA (got '${WORKAREA}', want '${WANT_WORKAREA}')"
fi

# --- 2: and lwm's own visible areas -----------------------------------------
#
# _NET_WORKAREA is what other clients read; VisibleAreas(true) is what lwm's
# own placement reads. The bug left both at their unstrutted values, so check
# the internal one too rather than trusting the published property to imply it.

CLI_MARK=$(($(wc -l <"${LWM_LOG}") + 1))
cli "xrandr ?"
sleep 0.5
WANT_AREA="${SCREEN_W}x$((SCREEN_H - STRUT_TOP))+0+${STRUT_TOP}"
if tail -n "+${CLI_MARK}" "${LWM_LOG}" |
  grep -q "With struts:.*${WANT_AREA}"; then
  pass "strut is subtracted from lwm's visible areas"
else
  fail "strut is subtracted from lwm's visible areas (want '${WANT_AREA}')"
  tail -n "+${CLI_MARK}" "${LWM_LOG}" | grep -i 'struts' | sed 's/^/    /'
fi

# --- 3: and an auto-placed window keeps clear of it -------------------------
#
# A framed window asking for +0+0 gets no position of its own, so lwm invents
# one with AutoPlacer, whose cascade is confined to the primary visible area
# *with struts removed*. The cascade starts at (100, 100), so the strut has to
# be deep enough to push that starting point out of the reserved area before
# this check can tell an honoured strut from an ignored one - hence the 200
# pixel STRUT_TOP above. Honoured, AutoPlacer resets the cascade to 100 inside
# the strutted area (y=300); ignored, the window lands at y=100, straddling
# the dock.
#
# Read the position with xwininfo, not xdotool: xdotool's getwindowgeometry
# adds the frame's border and title-bar offsets to a reparented window's
# already-absolute coordinates, so its Y is 36 pixels adrift here.

xterm -geometry 40x10+0+0 -title lwm-strut-client -e sleep 600 \
  >"${WORKDIR}/client.log" 2>&1 &
CLIENT_PID=$!
CLIENT_ID=""
for _ in $(seq 1 50); do
  CLIENT_ID=$(xdotool search --name lwm-strut-client 2>/dev/null | head -1)
  [ -n "${CLIENT_ID}" ] && break
  sleep 0.1
done
if [ -z "${CLIENT_ID}" ]; then
  fail "auto-placed window appeared"
else
  sleep 0.5
  GOT_Y=$(xwininfo -id "${CLIENT_ID}" 2>/dev/null |
    sed -n 's/.*Absolute upper-left Y: *//p')
  WANT_Y=$((STRUT_TOP + 100))
  if [ -n "${GOT_Y}" ] && [ "${GOT_Y}" -ge "${WANT_Y}" ]; then
    pass "auto-placed window is positioned below the strut"
  else
    fail "auto-placed window is positioned below the strut (y=${GOT_Y}, want >= ${WANT_Y})"
  fi
fi

# --- no errors along the way ------------------------------------------------

if grep -q '^E ' "${LWM_LOG}"; then
  fail "no error-level log lines from lwm"
  grep '^E ' "${LWM_LOG}" | sed 's/^/    /'
else
  pass "no error-level log lines from lwm"
fi

echo
if [ "${FAILURES}" -eq 0 ]; then
  echo "All strut checks passed."
  exit 0
fi
echo "${FAILURES} strut check(s) failed."
exit 1
