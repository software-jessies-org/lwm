#!/bin/bash
# ui_test.sh
#
# UI tests for lwm's mouse gestures, driven with xdotool against a headless
# Xvfb display. Where smoke_test.sh checks that lwm survives a session and
# handles the protocol, this drives the pointer the way a user would and
# checks where the windows end up.
#
# Everything here is about the 'Windows' key (Super/Mod4) gestures on a
# window's own background:
#
#   button 1 drag          move the window
#   button 2 drag          resize the nearest edge or corner, chosen from the
#                          3x3 grid over the window
#   button 1 click         raise the window
#   button 1 double click  expand that edge (or all of them, from the middle)
#                          up to the nearest window or the monitor, or, from
#                          the middle with no room left, un-expand instead
#   button 2 double click  the same, but ignoring other windows
#   button 3 click         hide the window
#
# ...plus the one Super+Control gesture:
#
#   button 1 click         turn lwm's furniture on or off for the window
#
# These need a real X server: the passive grabs in Client::GrabSuperButtons,
# the modifier state in the ButtonPress event and the pointer grab that keeps
# the drag alive are all things only the server does. drag_test.cc covers the
# same gestures against the fake server, and gesture_test.cc the arithmetic
# underneath them.
#
# Usage: ./ui_test.sh [path-to-lwm-binary]
#
# Exits 0 if every check passes, 1 otherwise, printing a PASS/FAIL line per
# check. Picks a random display number, so it's safe to run alongside a real
# X session - but note that it moves the pointer, so don't point DISPLAY at
# your own session.

set -u

LWM_BIN="${1:-./lwm}"

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

# check_eq <description> <got> <want>
check_eq() {
  if [ "$2" = "$3" ]; then
    pass "$1"
  else
    fail "$1 (got '$2', want '$3')"
  fi
}

# Send a line to lwm's -debugcli on stdin.
cli() { echo "$1" >"${CLI_FIFO}"; }

CLIENT_PIDS=()
cleanup() {
  for pid in ${CLIENT_PIDS[@]+"${CLIENT_PIDS[@]}"}; do
    kill "${pid}" >/dev/null 2>&1
  done
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
for _ in $(seq 1 50); do
  xdpyinfo >/dev/null 2>&1 && break
  sleep 0.1
done
if ! xdpyinfo >/dev/null 2>&1; then
  echo "Xvfb did not start on ${DISPLAY_SPEC}" >&2
  cat "${XVFB_LOG}" >&2
  exit 1
fi

# The gestures are keyed off Mod4, so if this server's keymap doesn't put
# Super there, every check below would fail for a reason which has nothing to
# do with lwm. Say so once, up front, rather than a dozen times over.
if ! xmodmap 2>/dev/null | grep -E '^mod4' | grep -q Super; then
  echo "Super is not on mod4 in this server's keymap; the gestures can't work" >&2
  xmodmap >&2
  exit 1
fi

# The debug CLI is on a fifo so the fake-xrandr checks at the end can drive
# it. The `sleep infinity` holds the write end open so lwm doesn't see EOF.
mkfifo "${CLI_FIFO}"
sleep infinity >"${CLI_FIFO}" &
CLI_HOLD_PID=$!
# HOME points at the temp dir: with no RESOURCE_MANAGER set, xcb-xrm falls
# back to ~/.Xresources, and these checks assume lwm's compiled-in defaults.
HOME="${WORKDIR}" "${LWM_BIN}" -debugcli="dbg auto" <"${CLI_FIFO}" \
  >"${LWM_LOG}" 2>&1 &
LWM_PID=$!
sleep 0.5
if ! kill -0 "${LWM_PID}" 2>/dev/null; then
  echo "lwm did not start" >&2
  cat "${LWM_LOG}" >&2
  exit 1
fi

# --- helpers ----------------------------------------------------------------

# geom <window-id> -> "x y width height", root-relative.
geom() {
  xwininfo -id "$1" 2>/dev/null | awk '
    /Absolute upper-left X:/ { x = $NF }
    /Absolute upper-left Y:/ { y = $NF }
    /^  Width:/ { w = $NF }
    /^  Height:/ { h = $NF }
    END { print x, y, w, h }'
}

# map_state <window-id> -> IsViewable, IsUnMapped, IsUnviewable, or "" if the
# window has gone away. Hiding unmaps the frame (the client inside it goes
# with it implicitly), so it's the frame this gets asked about.
map_state() {
  xwininfo -id "$1" 2>/dev/null | awk '/Map State:/ { print $NF }'
}

# in_front <window-id> <window-id>: true if the first is nearer the front of
# the stacking order than the second. xwininfo prints the root's children
# top-most first, so the one which appears earlier in the list is in front.
in_front() {
  local line
  line=$(xwininfo -root -children |
    grep -nE "^ *(0x[0-9a-f]+) " | grep -E "$(printf '0x%x|0x%x' "$1" "$2")" |
    head -1)
  [[ "${line}" == *"$(printf '0x%x' "$1")"* ]]
}

# frame_of <client-window-id> -> lwm's frame window for it, or "".
# LScr::Furnish names each frame after the client it belongs to, which is the
# only reliable way to pair the two up from outside.
frame_of() {
  local hex
  hex=$(printf '0x%x' "$1")
  xwininfo -root -tree 2>/dev/null |
    grep -o "0x[0-9a-f]\+ \"LWM frame for ${hex}\"" | head -1 | awk '{print $1}'
}

# start_client <wm-name> <geometry>
# Sets NEW_CLIENT to the client's window id (empty if it never appeared) and
# NEW_CLIENT_PID to the process, so that it can be shut down later. Globals
# rather than a return value, because a command substitution would run this
# in a subshell and lose the pid.
#
# xlogo because it's rectangular (so lwm frames it) and sets no size hints at
# all, so it ends up exactly the size it's asked for - unlike xterm, which
# rounds every resize down to a whole character cell, and would turn every
# expected geometry below into an inequality.
#
# An application can own several windows, so the one to take is whichever of
# them lwm decided to frame.
start_client() {
  local name="$1" geometry="$2" candidate
  NEW_CLIENT=""
  xlogo -name "${name}" -geometry "${geometry}" \
    >>"${WORKDIR}/clients.log" 2>&1 &
  NEW_CLIENT_PID=$!
  CLIENT_PIDS+=("${NEW_CLIENT_PID}")
  for _ in $(seq 1 50); do
    for candidate in $(xdotool search --name "^${name}\$" 2>/dev/null); do
      if [ -n "$(frame_of "${candidate}")" ]; then
        NEW_CLIENT="${candidate}"
        break
      fi
    done
    [ -n "${NEW_CLIENT}" ] && break
    sleep 0.1
  done
  # Let the map, the frame and the initial focus settle before anything reads
  # a geometry back.
  sleep 0.3
}

# place <client-id> <x> <y> <width> <height>
# Puts a window back somewhere known between groups of checks, through the
# ordinary ConfigureRequest path rather than by dragging it there: x and y
# become the *frame's* origin (see EvConfigureRequest), and the size is the
# client area's.
#
# Deliberately not xdotool's --sync: that waits for the window to move, and a
# request which asks for the position it's already in makes lwm quite
# correctly do nothing at all, so --sync waits for ever. Waiting for the
# frame to read back as expected has no such hole, and turns a placement that
# never happens into a failed check rather than a hung script.
place() {
  local client="$1" x="$2" y="$3" w="$4" h="$5"
  local frame want got=""
  frame=$(frame_of "${client}")
  want="${x} ${y} $((w + FURNITURE_W)) $((h + FURNITURE_H))"
  xdotool windowsize "${client}" "${w}" "${h}" >/dev/null 2>&1
  xdotool windowmove "${client}" "${x}" "${y}" >/dev/null 2>&1
  for _ in $(seq 1 40); do
    got=$(geom "${frame}")
    [ "${got}" = "${want}" ] && break
    sleep 0.1
  done
  sleep 0.2
  if [ "${got}" != "${want}" ]; then
    fail "could not place the window for the next check (got '${got}', want '${want}')"
  fi
}

# cell <client-id> <col> <row> -> "x y": the middle of one cell of the
# window's 3x3 grid, in root coordinates. lwm lays the grid over the client
# window, which is the only part of it these gestures can start from.
cell() {
  local g x y w h
  g=$(geom "$1")
  read -r x y w h <<<"${g}"
  echo "$((x + w * (2 * $2 + 1) / 6)) $((y + h * (2 * $3 + 1) / 6))"
}

# super_drag <button> <x1> <y1> <x2> <y2>
# Holds Super, presses at the first point, drags to the second in two steps
# (one warp would do, but two is closer to a real drag and means more than
# one MotionNotify has to be handled), and lets go.
super_drag() {
  local button="$1" x1="$2" y1="$3" x2="$4" y2="$5"
  xdotool keydown super
  xdotool mousemove "${x1}" "${y1}"
  xdotool mousedown "${button}"
  sleep 0.2
  xdotool mousemove $(((x1 + x2) / 2)) $(((y1 + y2) / 2))
  sleep 0.1
  xdotool mousemove "${x2}" "${y2}"
  sleep 0.2
  xdotool mouseup "${button}"
  xdotool keyup super
  sleep 0.3
}

# super_double_click <button> <x> <y>
super_double_click() {
  local button="$1" x="$2" y="$3"
  xdotool keydown super
  xdotool mousemove "${x}" "${y}"
  # Two press/release pairs well inside DoubleClickTracker::kIntervalMillis.
  xdotool click --repeat 2 --delay 60 "${button}"
  xdotool keyup super
  sleep 0.4
}

# super_click <button> <x> <y>
super_click() {
  local button="$1" x="$2" y="$3"
  xdotool keydown super
  xdotool mousemove "${x}" "${y}"
  xdotool click "${button}"
  xdotool keyup super
  sleep 0.4
}

# super_arrow <left|right|up|down> - a Super+arrow press, the focus-navigation
# gesture. Unlike the mouse gestures, this one doesn't touch the pointer: the
# whole point of it is that the focus moves without the pointer having to.
super_arrow() {
  xdotool key "super+$1"
  sleep 0.4
}

# super_shift_arrow <left|right|up|down> - a Super+Shift+arrow press, which
# moves the focused window rather than the focus. Like super_arrow it leaves
# the pointer alone: the window travels, the pointer doesn't.
super_shift_arrow() {
  xdotool key "super+shift+$1"
  sleep 0.4
}

# pointer_position -> "x y", the pointer's position in root coordinates.
pointer_position() {
  xdotool getmouselocation --shell 2>/dev/null |
    awk -F= '/^X=/ { x = $2 } /^Y=/ { y = $2 } END { print x, y }'
}

# focused_window -> the window id with the input focus, in hex, or "" if the
# server says there isn't one. lwm focuses the client window itself, not the
# frame, so this is comparable with a client id from start_client.
focused_window() {
  local id
  id=$(xdotool getwindowfocus 2>/dev/null) || return 0
  [ -n "${id}" ] && printf '0x%x' "${id}"
}

# focus_after_settling <hex-id> -> the focused window, having waited for it to
# become the given one. Giving a window the focus is several round trips (lwm
# sees the event, decides, and calls XSetInputFocus; the server then tells
# everyone), and lwm deliberately defers a focus change that arrives hard on
# the heels of another - see the timerfd in focus.h - so a fixed sleep is
# either too short or a waste of time. Returns whatever it last saw, so a
# focus which never arrives fails the check rather than hanging it. This can
# only rescue a slow right answer, never turn a wrong one into a pass.
focus_after_settling() {
  local want="$1" got=""
  # Three seconds. A focus lwm has decided on for itself - which is all of
  # them here - appears as fast as the server can be asked; this is slack for
  # a loaded machine, not for the focus-follows-mouse deferral, which the
  # checks below stay away from.
  for _ in $(seq 1 30); do
    got=$(focused_window)
    [ "${got}" = "${want}" ] && break
    sleep 0.1
  done
  printf '%s' "${got}"
}

# super_ctrl_click <button> <x> <y> - the Super+Control gesture set.
super_ctrl_click() {
  local button="$1" x="$2" y="$3"
  xdotool keydown super
  xdotool keydown ctrl
  xdotool mousemove "${x}" "${y}"
  xdotool click "${button}"
  xdotool keyup ctrl
  xdotool keyup super
  sleep 0.4
}

# --- move -------------------------------------------------------------------

start_client lwmtest1 '200x200+300+300'
CLIENT="${NEW_CLIENT}"
if [ -z "${CLIENT}" ]; then
  echo "xlogo did not appear, or lwm did not frame it" >&2
  cat "${LWM_LOG}" >&2
  exit 1
fi
FRAME=$(frame_of "${CLIENT}")
pass "xlogo mapped and framed"

# How much bigger the frame is than the window inside it - two borders wide,
# and a border plus a title bar high - and where inside the frame the client
# window sits. Measured rather than assumed: the sizes come from Xresources,
# and the offsets also carry the frame's own 1px X border, which lives outside
# the frame's coordinate space and so isn't part of either size. `place` needs
# the sizes to know what it's waiting for, and the decoration checks need the
# offsets.
read -r CX0 CY0 CW CH <<<"$(geom "${CLIENT}")"
read -r FX0 FY0 FW FH <<<"$(geom "${FRAME}")"
FURNITURE_W=$((FW - CW))
FURNITURE_H=$((FH - CH))
FURNITURE_X=$((CX0 - FX0))
FURNITURE_Y=$((CY0 - FY0))

place "${CLIENT}" 300 300 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_drag 1 "${CX}" "${CY}" $((CX + 120)) $((CY + 80))
check_eq "Super+button 1 drag moves the window" \
  "$(geom "${FRAME}")" "$((FX + 120)) $((FY + 80)) ${FW} ${FH}"

# The gesture works anywhere on the window, not just in the middle: unlike a
# resize, a move doesn't care which cell of the grid it started in.
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 0 0)"
super_drag 1 "${CX}" "${CY}" $((CX - 50)) $((CY + 30))
check_eq "Super+button 1 drag works from a corner of the window too" \
  "$(geom "${FRAME}")" "$((FX - 50)) $((FY + 30)) ${FW} ${FH}"

# Without the Windows key the click belongs to the application, and lwm must
# keep its hands off the window entirely.
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
xdotool mousemove "${CX}" "${CY}" mousedown 1
xdotool mousemove $((CX + 100)) $((CY + 100))
sleep 0.2
xdotool mouseup 1
sleep 0.3
check_eq "a plain drag on the window background does not move it" \
  "$(geom "${FRAME}")" "${FX} ${FY} ${FW} ${FH}"

# --- resize -----------------------------------------------------------------
#
# Which edge or corner moves comes from the cell of the window's 3x3 grid the
# press lands in. It's the frame that gets measured: a resize which moved the
# frame and the client by different amounts would be a bug.

# resize_check <description> <col> <row> <dx> <dy> <want-delta "dx dy dw dh">
resize_check() {
  local desc="$1" col="$2" row="$3" dx="$4" dy="$5"
  local fx fy fw fh nfx nfy nfw nfh cx cy
  place "${CLIENT}" 400 400 200 200
  read -r fx fy fw fh <<<"$(geom "${FRAME}")"
  read -r cx cy <<<"$(cell "${CLIENT}" "${col}" "${row}")"
  super_drag 2 "${cx}" "${cy}" $((cx + dx)) $((cy + dy))
  read -r nfx nfy nfw nfh <<<"$(geom "${FRAME}")"
  check_eq "${desc}" \
    "$((nfx - fx)) $((nfy - fy)) $((nfw - fw)) $((nfh - fh))" "$6"
}

resize_check "Super+button 2 drag on the right edge widens the window" \
  2 1 60 40 "0 0 60 0"
resize_check "Super+button 2 drag on the left edge moves and widens it" \
  0 1 -60 40 "-60 0 60 0"
resize_check "Super+button 2 drag on the bottom edge makes it taller" \
  1 2 60 40 "0 0 0 40"
resize_check "Super+button 2 drag on the top edge moves and grows it upwards" \
  1 0 60 -40 "0 -40 0 40"
resize_check "Super+button 2 drag on the bottom right corner resizes both" \
  2 2 60 40 "0 0 60 40"
resize_check "Super+button 2 drag on the top left corner resizes both" \
  0 0 -60 -40 "-60 -40 60 40"
resize_check "Super+button 2 drag on the top right corner resizes both" \
  2 0 60 -40 "0 -40 60 40"
resize_check "Super+button 2 drag in the middle of the window does nothing" \
  1 1 60 40 "0 0 0 0"

# --- expand, with nothing else on screen ------------------------------------

place "${CLIENT}" 400 400 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 2 1)"
super_double_click 1 "${CX}" "${CY}"
check_eq "Super+button 1 double click expands the right edge to the screen" \
  "$(geom "${FRAME}")" "${FX} ${FY} $((SCREEN_W - FX)) ${FH}"

place "${CLIENT}" 400 400 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 0)"
super_double_click 1 "${CX}" "${CY}"
check_eq "Super+button 1 double click expands the top edge to the screen" \
  "$(geom "${FRAME}")" "${FX} 0 ${FW} $((FH + FY))"

place "${CLIENT}" 400 400 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 0 2)"
super_double_click 1 "${CX}" "${CY}"
check_eq "Super+button 1 double click expands a corner's two edges" \
  "$(geom "${FRAME}")" "0 ${FY} $((FW + FX)) $((SCREEN_H - FY))"

place "${CLIENT}" 400 400 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_double_click 1 "${CX}" "${CY}"
check_eq "Super+button 1 double click in the middle fills the screen" \
  "$(geom "${FRAME}")" "0 0 ${SCREEN_W} ${SCREEN_H}"

# The middle square is a toggle: with nothing left to grow into, the same
# gesture puts the window back to the size it had before it was expanded.
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_double_click 1 "${CX}" "${CY}"
check_eq "a second double click in the middle restores the previous size" \
  "$(geom "${FRAME}")" "${FX} ${FY} ${FW} ${FH}"

# And there's nothing to go back to once it's been back: the next one expands.
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_double_click 1 "${CX}" "${CY}"
check_eq "a third double click in the middle expands again" \
  "$(geom "${FRAME}")" "0 0 ${SCREEN_W} ${SCREEN_H}"

# Two clicks further apart than the double-click interval are two single
# clicks, and a single Super+click does nothing at all.
place "${CLIENT}" 400 400 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
xdotool keydown super
xdotool mousemove "${CX}" "${CY}" click 1
sleep 0.6
xdotool click 1
xdotool keyup super
sleep 0.4
check_eq "two slow Super+clicks are not a double click" \
  "$(geom "${FRAME}")" "${FX} ${FY} ${FW} ${FH}"

# --- expand, with another window in the way ---------------------------------

place "${CLIENT}" 200 300 200 200
start_client lwmtest2 '200x200+800+350'
OTHER="${NEW_CLIENT}"
OTHER_PID="${NEW_CLIENT_PID}"
if [ -z "${OTHER}" ]; then
  fail "second xlogo mapped and framed"
else
  pass "second xlogo mapped and framed"
  OTHER_FRAME=$(frame_of "${OTHER}")
  place "${OTHER}" 800 350 200 200
  read -r OFX OFY OFW OFH <<<"$(geom "${OTHER_FRAME}")"

  # The first window is to the left of the second and overlaps it vertically,
  # so expanding its right edge has to stop at the second one's left edge.
  read -r FX FY FW FH <<<"$(geom "${FRAME}")"
  read -r CX CY <<<"$(cell "${CLIENT}" 2 1)"
  super_double_click 1 "${CX}" "${CY}"
  check_eq "Super+button 1 double click stops at the next window" \
    "$(geom "${FRAME}")" "${FX} ${FY} $((OFX - FX)) ${FH}"

  # Button 2's double click ignores other windows, so from the same place it
  # goes all the way to the screen edge - straight underneath the other one.
  place "${CLIENT}" 200 300 200 200
  read -r FX FY FW FH <<<"$(geom "${FRAME}")"
  read -r CX CY <<<"$(cell "${CLIENT}" 2 1)"
  super_double_click 2 "${CX}" "${CY}"
  check_eq "Super+button 2 double click ignores the next window" \
    "$(geom "${FRAME}")" "${FX} ${FY} $((SCREEN_W - FX)) ${FH}"

  # ...and the other window has sat there untouched throughout.
  check_eq "expanding one window doesn't disturb the other" \
    "$(geom "${OTHER_FRAME}")" "${OFX} ${OFY} ${OFW} ${OFH}"

  # Stacking: a Super+button 1 press that goes nowhere is a click, and raises
  # the window; one that drags is a move, and leaves the stacking order alone
  # (as the middle button on the frame does).
  place "${CLIENT}" 200 300 200 200

  # Put the second window in front, so the first has something to be raised
  # past. Which is the same gesture under test, so it gets checked as well.
  read -r OCX OCY <<<"$(cell "${OTHER}" 1 1)"
  super_click 1 "${OCX}" "${OCY}"
  check "a Super+button 1 click raises the window" \
    in_front "${OTHER_FRAME}" "${FRAME}"

  read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
  super_drag 1 "${CX}" "${CY}" $((CX + 30)) $((CY + 20))
  check "a Super+button 1 drag moves the window without raising it" \
    in_front "${OTHER_FRAME}" "${FRAME}"

  read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
  super_click 1 "${CX}" "${CY}"
  check "a Super+button 1 click raises the window past another one" \
    in_front "${FRAME}" "${OTHER_FRAME}"

  kill "${OTHER_PID}" >/dev/null 2>&1
  sleep 0.5
fi

# --- expand: don't cross a monitor boundary ---------------------------------
#
# The real xrandr path can't be driven from a test, but the debug CLI's
# `xrandr` command feeds the same LScr::SetVisibleAreas an RRScreenChangeNotify
# ends up calling, so the expansion sees a genuine two-monitor layout.

HALF=$((SCREEN_W / 2))
cli "xrandr ${HALF}x${SCREEN_H}+0+0 ${HALF}x${SCREEN_H}+${HALF}+0"
sleep 0.5

place "${CLIENT}" 150 300 200 200
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_double_click 2 "${CX}" "${CY}"
check_eq "expanding stops at the edge of the monitor the window is on" \
  "$(geom "${FRAME}")" "0 0 ${HALF} ${SCREEN_H}"

# The same on the right-hand monitor: the left edge must stop at the join,
# not run on to zero.
place "${CLIENT}" $((HALF + 150)) 300 200 200
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_double_click 2 "${CX}" "${CY}"
check_eq "a window on the second monitor expands within that monitor" \
  "$(geom "${FRAME}")" "${HALF} 0 ${HALF} ${SCREEN_H}"

cli "xrandr"
sleep 0.5

# --- decorations ------------------------------------------------------------
#
# Super+Control+button 1 turns lwm's furniture on and off for the window under
# the pointer, keeping its outer extent where it is. The frame window itself
# stays either way: with the furniture off it shrinks onto the client window,
# which then covers it exactly, so what's on screen is only the client. That
# is a thing only a real server can be asked about, since it comes down to
# where two windows ended up rather than to what lwm asked for.

place "${CLIENT}" 400 300 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_ctrl_click 1 "${CX}" "${CY}"

check_eq "Super+Control+button 1 keeps the frame window" \
  "$(frame_of "${CLIENT}")" "${FRAME}"
check_eq "and the client grows to keep the outer extent" \
  "$(geom "${CLIENT}")" "${FX} ${FY} ${FW} ${FH}"
check_eq "and the frame shrinks onto it, so no furniture shows" \
  "$(geom "${FRAME}")" "${FX} ${FY} ${FW} ${FH}"
check_eq "and the window is still on screen" \
  "$(map_state "${CLIENT}")" "IsViewable"

# It's still a window lwm manages, with all the gestures it had before.
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_drag 1 "${CX}" "${CY}" $((CX + 60)) $((CY + 40))
check_eq "an undecorated window still moves with Super+button 1" \
  "$(geom "${CLIENT}")" "$((FX + 60)) $((FY + 40)) ${FW} ${FH}"

read -r FX FY FW FH <<<"$(geom "${CLIENT}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
super_ctrl_click 1 "${CX}" "${CY}"
check_eq "the furniture comes back on the same frame" \
  "$(frame_of "${CLIENT}")" "${FRAME}"
check_eq "and the outer extent is still where it was" \
  "$(geom "${FRAME}")" "${FX} ${FY} ${FW} ${FH}"
WANT="$((FX + FURNITURE_X)) $((FY + FURNITURE_Y))"
WANT="${WANT} $((FW - FURNITURE_W)) $((FH - FURNITURE_H))"
check_eq "so the client shrinks back inside it by exactly the furniture" \
  "$(geom "${CLIENT}")" "${WANT}"

# --- focus navigation -------------------------------------------------------
#
# Super+arrow moves the input focus to the next window that way. This needs a
# real server for the same reason the gestures do, and one more besides: the
# grab is on the root window and is matched on the modifier state, so a keymap
# where Super isn't Mod4, or a stray lock modifier, would stop it dead.
# keyboard_test.cc covers the same path against the fake server, and
# navigate_test.cc the geometry underneath it.
#
# The pointer is parked on the root throughout, clear of both windows. That's
# not just tidiness: focus follows the mouse by default, and a pointer sitting
# over a window keeps generating enter events whose focus changes lwm delays
# on the timer in focus.h - which on a server as slow as Xvfb can be seconds
# later, landing in the middle of a check. Off the windows, nothing but the
# arrow keys moves the focus, which is the thing being tested anyway.

place "${CLIENT}" 700 300 200 200
start_client lwmtest3 '200x200+100+300'
NAV="${NEW_CLIENT}"
NAV_PID="${NEW_CLIENT_PID}"
if [ -z "${NAV}" ]; then
  fail "third xlogo mapped and framed"
else
  pass "third xlogo mapped and framed"
  place "${NAV}" 100 300 200 200
  xdotool mousemove 5 5
  sleep 0.4

  # lwm focuses a window as it maps it, so the left-hand one starts with the
  # focus by virtue of being the one just created. Nothing here relies on the
  # pointer to establish that.
  WANT=$(printf '0x%x' "${NAV}")
  check_eq "the newly mapped left-hand window starts with the focus" \
    "$(focus_after_settling "${WANT}")" "${WANT}"

  WANT=$(printf '0x%x' "${CLIENT}")
  super_arrow Right
  check_eq "Super+Right moves the focus to the window on the right" \
    "$(focus_after_settling "${WANT}")" "${WANT}"

  WANT=$(printf '0x%x' "${NAV}")
  super_arrow Left
  check_eq "Super+Left moves it back again" \
    "$(focus_after_settling "${WANT}")" "${WANT}"

  # The two windows are side by side, so neither is in the other's vertical
  # cone: there is nothing above, and the focus must stay where it is rather
  # than settling for the window to the right.
  super_arrow Up
  check_eq "Super+Up does nothing when there is no window above" \
    "$(focused_window)" "$(printf '0x%x' "${NAV}")"

  # Nothing further left than the left-hand window either.
  super_arrow Left
  check_eq "Super+Left does nothing at the left-hand end" \
    "$(focused_window)" "$(printf '0x%x' "${NAV}")"

  # Navigation moves the focus and nothing else: in particular it does not
  # raise the window it moves to. The left-hand window goes up and to the
  # left, further along x than y so that it stays in the left-hand cone, and
  # clear of the point clicked below.
  place "${NAV}" 400 150 200 200
  NAV_FRAME=$(frame_of "${NAV}")
  WANT=$(printf '0x%x' "${CLIENT}")
  super_arrow Right
  check_eq "the focus is on the right-hand window before the raise check" \
    "$(focus_after_settling "${WANT}")" "${WANT}"

  # A Super+click raises the window under the pointer - which is the one that
  # already has the focus, so the click can't change the focus either way.
  read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"
  super_click 1 "${CX}" "${CY}"
  check "the right-hand window is in front before navigating" \
    in_front "${FRAME}" "${NAV_FRAME}"

  WANT=$(printf '0x%x' "${NAV}")
  super_arrow Left
  check_eq "Super+Left focuses the window it navigates to" \
    "$(focus_after_settling "${WANT}")" "${WANT}"
  check "but does not raise it" in_front "${FRAME}" "${NAV_FRAME}"

  xdotool mousemove 5 5
  kill "${NAV_PID}" >/dev/null 2>&1
  sleep 0.5
fi

place "${CLIENT}" 400 400 200 200

# --- moving windows from the keyboard ---------------------------------------
#
# Super+Shift+arrow moves the focused window itself: to the inner edge of its
# monitor, and then on to the next monitor that way. The multi-monitor half
# needs a multi-monitor desktop, which the debug CLI's fake xrandr provides -
# Xvfb has one screen and no RandR outputs to reconfigure.
#
# The pointer stays parked on the root for the same reason as the section
# above: nothing here should be able to move the focus but the keyboard.
#
# navigate_test.cc covers the geometry and keyboard_test.cc the wiring; what
# only a real server can show is that the grab matches Super+Shift at all.

start_client lwmtest4 '200x200+400+400'
MOVE="${NEW_CLIENT}"
MOVE_PID="${NEW_CLIENT_PID}"
if [ -z "${MOVE}" ]; then
  fail "fourth xlogo mapped and framed"
else
  pass "fourth xlogo mapped and framed"
  MOVE_FRAME=$(frame_of "${MOVE}")
  xdotool mousemove 5 5
  place "${MOVE}" 400 400 200 200
  read -r _ MY MW MH <<<"$(geom "${MOVE_FRAME}")"

  # As in the section above, the window just mapped is the one lwm focused.
  WANT=$(printf '0x%x' "${MOVE}")
  check_eq "the newly mapped window has the focus, so it's the one that moves" \
    "$(focus_after_settling "${WANT}")" "${WANT}"

  super_shift_arrow Left
  check_eq "Super+Shift+Left puts the window against the left edge" \
    "$(geom "${MOVE_FRAME}")" "0 ${MY} ${MW} ${MH}"
  check_eq "and moving the window doesn't move the focus" \
    "$(focused_window)" "${WANT}"

  super_shift_arrow Left
  check_eq "a second press leaves it there: no monitor further left" \
    "$(geom "${MOVE_FRAME}")" "0 ${MY} ${MW} ${MH}"

  super_shift_arrow Down
  check_eq "Super+Shift+Down puts it against the bottom edge" \
    "$(geom "${MOVE_FRAME}")" "0 $((SCREEN_H - MH)) ${MW} ${MH}"

  # With the pointer on the window, it goes along for the ride - otherwise the
  # window slides out from under it, and the enter event on whatever was
  # behind takes the focus with it. The other window is parked exactly where
  # this one is going, so this also shows the moved window arriving in front
  # of it rather than under it.
  place "${CLIENT}" 0 400 200 200
  place "${MOVE}" 400 400 200 200
  xdotool mousemove 500 500
  sleep 0.4
  super_shift_arrow Left
  check_eq "the pointer goes with the window it was on" \
    "$(pointer_position)" "100 500"
  check "and the moved window arrives in front of the one already there" \
    in_front "$(frame_of "${MOVE}")" "$(frame_of "${CLIENT}")"
  check_eq "so the focus stays on the window that moved" \
    "$(focus_after_settling "${WANT}")" "${WANT}"
  xdotool mousemove 5 5
  sleep 0.4

  # Two monitors: a tall one on the left, a small one to the right of it.
  cli "xrandr 800x1024+0+0 480x400+800+0"
  sleep 0.5
  place "${MOVE}" 100 100 200 200
  read -r _ _ MW MH <<<"$(geom "${MOVE_FRAME}")"

  super_shift_arrow Right
  check_eq "Super+Shift+Right stops at the inner edge of the first monitor" \
    "$(geom "${MOVE_FRAME}")" "$((800 - MW)) 100 ${MW} ${MH}"

  super_shift_arrow Right
  check_eq "and the next press hands it to the monitor beside it" \
    "$(geom "${MOVE_FRAME}")" "800 100 ${MW} ${MH}"

  super_shift_arrow Right
  check_eq "and the one after that crosses to that monitor's far edge" \
    "$(geom "${MOVE_FRAME}")" "$((SCREEN_W - MW)) 100 ${MW} ${MH}"

  # A window too big for the monitor it arrives on is cut down to it, and a
  # window exactly the size of a monitor has nowhere to sit but on it.
  place "${MOVE}" 100 100 600 600
  super_shift_arrow Right
  super_shift_arrow Right
  check_eq "a window too big for the monitor it lands on is shrunk to fit" \
    "$(geom "${MOVE_FRAME}")" "800 0 480 400"

  # The same thing happens to a window dragged there with the mouse, which is
  # the other half of the feature and the half a user is more likely to meet.
  place "${MOVE}" 100 100 600 600
  read -r DX DY <<<"$(cell "${MOVE}" 1 1)"
  super_drag 1 "${DX}" "${DY}" 1050 200
  check_eq "and to one dragged onto a monitor too small to take it" \
    "$(geom "${MOVE_FRAME}")" "800 0 480 400"

  cli "xrandr"
  sleep 0.5
  xdotool mousemove 5 5
  kill "${MOVE_PID}" >/dev/null 2>&1
  sleep 0.5
fi

place "${CLIENT}" 400 400 200 200

# --- hide -------------------------------------------------------------------
#
# Button 3 hides, exactly as it does on the frame. This goes last: there's no
# way to unhide from here (the unhide menu is a button 3 drag on the root
# window, and driving it would be a test of the menu, not of the gestures),
# so every check that needs to see this window has already run.

place "${CLIENT}" 400 400 200 200
read -r FX FY FW FH <<<"$(geom "${FRAME}")"
read -r CX CY <<<"$(cell "${CLIENT}" 1 1)"

# A press the user thought better of and dragged away from does nothing: the
# window is neither hidden nor, unlike button 2, resized on the way.
super_drag 3 "${CX}" "${CY}" $((CX + 50)) $((CY + 30))
check_eq "a Super+button 3 drag neither hides nor resizes the window" \
  "$(map_state "${FRAME}") $(geom "${FRAME}")" \
  "IsViewable ${FX} ${FY} ${FW} ${FH}"

super_click 3 "${CX}" "${CY}"
check_eq "Super+button 3 click hides the window" \
  "$(map_state "${FRAME}")" "IsUnMapped"

# --- undecorated windows ----------------------------------------------------
#
# A client that draws its own title bar asks lwm, through _MOTIF_WM_HINTS, for
# no decorations, and lwm gives it no frame. That leaves it exactly two ways
# of being moved: these gestures, and the _NET_WM_MOVERESIZE message the
# client sends when the user grabs its own title bar. Both used to do nothing
# at all, which pinned such a window where it opened - the Steam launcher
# being the case that found it.

CSD_BIN="${WORKDIR}/csdclient"
if ! g++ -o "${CSD_BIN}" -std=c++17 csdclient.cc -lxcb \
    >>"${WORKDIR}/clients.log" 2>&1; then
  fail "could not build csdclient.cc, skipping the undecorated-window checks"
else
  "${CSD_BIN}" window 200 200 300 250 >"${WORKDIR}/csdwin" 2>&1 &
  CSD_PID=$!
  CLIENT_PIDS+=("${CSD_PID}")
  CSD=""
  for _ in $(seq 1 50); do
    CSD=$(xdotool search --name '^csdwin$' 2>/dev/null | head -1)
    [ -n "${CSD}" ] && break
    sleep 0.1
  done
  sleep 0.3

  if [ -z "${CSD}" ]; then
    fail "the undecorated test client never appeared"
  else
    # lwm gives it no frame at all, which is what the rest of this section is
    # about. If this fails the others are meaningless.
    check_eq "an undecorated client is left unframed" \
      "$(frame_of "${CSD}")" ""

    # 1. The Windows-key gestures. With no furniture, these are the only
    #    gesture the user has.
    read -r BX BY BW BH <<<"$(geom "${CSD}")"
    read -r CX CY <<<"$(cell "${CSD}" 1 1)"
    super_drag 1 "${CX}" "${CY}" $((CX + 90)) $((CY + 60))
    check_eq "Super+button 1 drag moves an undecorated window" \
      "$(geom "${CSD}")" "$((BX + 90)) $((BY + 60)) ${BW} ${BH}"

    read -r BX BY BW BH <<<"$(geom "${CSD}")"
    read -r CX CY <<<"$(cell "${CSD}" 2 2)"
    super_drag 2 "${CX}" "${CY}" $((CX + 40)) $((CY + 30))
    check_eq "Super+button 2 drag resizes an undecorated window" \
      "$(geom "${CSD}")" "${BX} ${BY} $((BW + 40)) $((BH + 30))"

    # 2. _NET_WM_MOVERESIZE: the client's own title bar and resize grip. The
    #    press goes to the client, not to lwm, so lwm has to take a pointer
    #    grab of its own to see the drag at all.
    read -r BX BY BW BH <<<"$(geom "${CSD}")"
    xdotool mousemove $((BX + 40)) $((BY + 10))
    xdotool mousedown 1
    "${CSD_BIN}" moveresize "${CSD}" 8 1   # 8 = _NET_WM_MOVERESIZE_MOVE
    sleep 0.3
    xdotool mousemove $((BX + 110)) $((BY + 60))
    sleep 0.3
    xdotool mouseup 1
    sleep 0.3
    check_eq "_NET_WM_MOVERESIZE move drags an undecorated window" \
      "$(geom "${CSD}")" "$((BX + 70)) $((BY + 50)) ${BW} ${BH}"

    # The press has to land inside the client, where a real resize grip is: a
    # press on the root is lwm's own gesture, and the request would then
    # quite rightly be refused as arriving mid-drag.
    read -r BX BY BW BH <<<"$(geom "${CSD}")"
    GX=$((BX + BW - 3))
    GY=$((BY + BH - 3))
    xdotool mousemove "${GX}" "${GY}"
    xdotool mousedown 1
    "${CSD_BIN}" moveresize "${CSD}" 4 1   # 4 = SIZE_BOTTOMRIGHT
    sleep 0.3
    xdotool mousemove $((GX + 50)) $((GY + 40))
    sleep 0.3
    xdotool mouseup 1
    sleep 0.3
    check_eq "_NET_WM_MOVERESIZE resizes from the bottom right corner" \
      "$(geom "${CSD}")" "${BX} ${BY} $((BW + 50)) $((BH + 40))"

    # 3. The grab has to be handed back, or nothing else on the display ever
    #    sees the mouse again - including the gestures, which is what this
    #    checks by using one.
    read -r BX BY BW BH <<<"$(geom "${CSD}")"
    read -r CX CY <<<"$(cell "${CSD}" 1 1)"
    super_drag 1 "${CX}" "${CY}" $((CX + 30)) $((CY + 20))
    check_eq "the pointer grab is released when the drag ends" \
      "$(geom "${CSD}")" "$((BX + 30)) $((BY + 20)) ${BW} ${BH}"

    # 4. A direction lwm can't track must not leave a drag running: it would
    #    block every later gesture, since lwm starts only one at a time.
    read -r BX BY BW BH <<<"$(geom "${CSD}")"
    "${CSD_BIN}" moveresize "${CSD}" 9 0   # 9 = SIZE_KEYBOARD, no button
    sleep 0.3
    read -r CX CY <<<"$(cell "${CSD}" 1 1)"
    super_drag 1 "${CX}" "${CY}" $((CX + 25)) $((CY + 15))
    check_eq "an untrackable move/resize request leaves no drag stuck" \
      "$(geom "${CSD}")" "$((BX + 25)) $((BY + 15)) ${BW} ${BH}"
  fi
fi

# --- the session survived it ------------------------------------------------

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
  echo "All UI checks passed."
  exit 0
else
  echo "${FAILURES} UI check(s) failed. lwm log follows:"
  cat "${LWM_LOG}"
  exit 1
fi
