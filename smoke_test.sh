#!/bin/bash
# smoke_test.sh
#
# Functional smoke test for lwm, run under a headless Xvfb display. This is
# not a substitute for `./lwm -test` (which covers pure logic); it's a
# behavioural regression check for the parts that only a live X server can
# exercise: does lwm actually frame, move, resize, focus and close a window
# without crashing or logging errors.
#
# Usage: ./smoke_test.sh [path-to-lwm-binary]
#
# Exits 0 if every check passes, 1 otherwise. Prints a PASS/FAIL line per
# check.

set -u

LWM_BIN="${1:-./lwm}"
DISPLAY_NUM=$((90 + RANDOM % 400))
DISPLAY_SPEC=":${DISPLAY_NUM}"
WORKDIR="$(mktemp -d)"
XVFB_LOG="${WORKDIR}/xvfb.log"
LWM_LOG="${WORKDIR}/lwm.log"

FAILURES=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

cleanup() {
  [ -n "${XTERM_PID:-}" ] && kill "${XTERM_PID}" >/dev/null 2>&1
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

Xvfb "${DISPLAY_SPEC}" -screen 0 1280x1024x24 >"${XVFB_LOG}" 2>&1 &
XVFB_PID=$!
for _ in $(seq 1 50); do
  DISPLAY="${DISPLAY_SPEC}" xdpyinfo >/dev/null 2>&1 && break
  sleep 0.1
done
if ! DISPLAY="${DISPLAY_SPEC}" xdpyinfo >/dev/null 2>&1; then
  echo "Xvfb did not start on ${DISPLAY_SPEC}" >&2
  cat "${XVFB_LOG}" >&2
  exit 1
fi

DISPLAY="${DISPLAY_SPEC}" "${LWM_BIN}" >"${LWM_LOG}" 2>&1 &
LWM_PID=$!
sleep 0.5
if ! kill -0 "${LWM_PID}" 2>/dev/null; then
  fail "lwm started and stayed running"
  cat "${LWM_LOG}" >&2
  exit 1
fi
pass "lwm started and stayed running"

DISPLAY="${DISPLAY_SPEC}" xterm -geometry 80x24+50+50 >"${WORKDIR}/xterm.log" 2>&1 &
XTERM_PID=$!

FRAME_ID=""
for _ in $(seq 1 50); do
  FRAME_ID=$(DISPLAY="${DISPLAY_SPEC}" xwininfo -root -tree 2>/dev/null |
    grep -o '0x[0-9a-f]\+ "LWM frame for[^"]*"' | head -1 | awk '{print $1}')
  [ -n "${FRAME_ID}" ] && break
  sleep 0.1
done
if [ -n "${FRAME_ID}" ]; then
  pass "xterm got a frame window"
else
  fail "xterm got a frame window"
fi

if [ -n "${FRAME_ID}" ]; then
  CLIENT_ID=$(DISPLAY="${DISPLAY_SPEC}" xdotool search --class xterm 2>/dev/null | head -1)
  if [ -n "${CLIENT_ID}" ]; then
    pass "xdotool can find the client window"
  else
    fail "xdotool can find the client window"
  fi

  if [ -n "${CLIENT_ID}" ]; then
    # Move+resize via the WM (ConfigureRequest path), not a direct Xlib call.
    DISPLAY="${DISPLAY_SPEC}" xdotool windowmove --sync "${CLIENT_ID}" 200 200 \
      >/dev/null 2>&1
    DISPLAY="${DISPLAY_SPEC}" xdotool windowsize --sync "${CLIENT_ID}" 400 300 \
      >/dev/null 2>&1
    sleep 0.3
    GEOM=$(DISPLAY="${DISPLAY_SPEC}" xdotool getwindowgeometry --shell "${CLIENT_ID}" 2>/dev/null)
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

    DISPLAY="${DISPLAY_SPEC}" xdotool windowactivate --sync "${CLIENT_ID}" \
      >/dev/null 2>&1
    sleep 0.2
    FOCUSED=$(DISPLAY="${DISPLAY_SPEC}" xdotool getactivewindow 2>/dev/null)
    if [ "${FOCUSED}" = "${CLIENT_ID}" ]; then
      pass "window activation gives it input focus"
    else
      fail "window activation gives it input focus (active=${FOCUSED}, want=${CLIENT_ID})"
    fi
  fi
fi

# Close via WM_DELETE_WINDOW (what a title-bar close click sends).
if [ -n "${CLIENT_ID:-}" ]; then
  DISPLAY="${DISPLAY_SPEC}" xdotool windowclose "${CLIENT_ID}" >/dev/null 2>&1
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
fi

sleep 0.2
if kill -0 "${LWM_PID}" 2>/dev/null; then
  pass "lwm still running after the session"
else
  fail "lwm still running after the session"
fi

if grep -qE '^E ' "${LWM_LOG}"; then
  fail "no error-level log lines from lwm"
  grep -E '^E ' "${LWM_LOG}" | sed 's/^/    /'
else
  pass "no error-level log lines from lwm"
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
