#!/bin/bash
# gummiband_test.sh
#
# Tests for gummiband, the panel, run against a headless Xvfb display.
#
# gummiband has no unit tests and nothing to hook one onto: it is a single
# translation unit whose every interesting behaviour is a request to an X
# server. So this drives the real thing - config file in, pointer clicks in,
# pixels and properties out - which is also what makes it the regression test
# for the XCB port. Almost everything the port could have broken is invisible
# to the compiler:
#
#   * argument order. xcb_copy_area takes both positions before the size where
#     XCopyArea took the size in the middle; xcb_create_pixmap leads with the
#     depth; xcb_change_property leads with the mode. Each of those compiles
#     perfectly happily when scrambled and paints (or sets) nonsense, so the
#     checks here look at painted pixels and at property values.
#   * value-list ordering. XCB takes a bare uint32_t[] that must be in
#     mask-bit order and doesn't check, so a mis-ordered CreateWindow or
#     CreateGC silently sets the wrong attribute. What catches this is the
#     pointer checks rather than the colour ones: a scrambled list usually
#     lands a nonsense value in the event mask, and the panel stops reacting.
#     Note the limit - swapping two attributes that nothing here can see (the
#     window's background pixel is painted over by the copy from the buffer
#     pixmap on the very first Expose) stays invisible, as it does in use.
#   * 32-bit property data. Xlib widened format-32 properties to long; XCB
#     sends what you give it. _NET_WM_STRUT written as an array of long would
#     put four 64-bit values into a property declared as four 32-bit ones.
#   * flushing. XCB never pushes requests on its own, and the failure mode is
#     "the panel stops repainting until you jiggle the mouse" - so the checks
#     that follow a click or a timer tick are checking the flush as much as
#     the drawing.
#
# No window manager runs here: gummiband is override-redirect-free but maps
# itself at 0,0 regardless, and leaving lwm out keeps this test independent of
# it. The lwm-plus-dock interaction has its own test, strut_test.sh.
#
# Usage: ./gummiband_test.sh [path-to-gummiband-binary]
#
# Exits 0 if every check passes, 1 otherwise, printing a PASS/FAIL line per
# check. Picks a random display number, so it's safe to run alongside a real X
# session - but note that it moves the pointer, so don't point DISPLAY at your
# own session.

set -u

GUMMIBAND_BIN="${1:-./bin/gummiband}"

DISPLAY_NUM=$((90 + RANDOM % 400))
DISPLAY_SPEC=":${DISPLAY_NUM}"
SCREEN_W=1280
SCREEN_H=1024
WORKDIR="$(mktemp -d)"
XVFB_LOG="${WORKDIR}/xvfb.log"
GB_LOG="${WORKDIR}/gummiband.log"

export DISPLAY="${DISPLAY_SPEC}"
# XENVIRONMENT names a resource file that xcb-xrm merges in on top of
# everything else, which would let the developer's environment override the
# colours asserted on below.
unset XENVIRONMENT

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

cleanup() {
  [ -n "${GAME_PID:-}" ] && kill "${GAME_PID}" >/dev/null 2>&1
  [ -n "${GB_PID:-}" ] && kill "${GB_PID}" >/dev/null 2>&1
  [ -n "${XVFB_PID:-}" ] && kill "${XVFB_PID}" >/dev/null 2>&1
  wait >/dev/null 2>&1
  rm -rf "${WORKDIR}"
}
trap cleanup EXIT

if [ ! -x "${GUMMIBAND_BIN}" ]; then
  echo "gummiband binary not found or not executable: ${GUMMIBAND_BIN}" >&2
  exit 1
fi
# Absolute, because gummiband is started with its working directory set to the
# temp dir below (that's where it looks for .gummiband) and because SIGHUP
# makes it execvp(argv[0]) itself - a relative path would resolve against the
# wrong directory, or against nothing at all.
GUMMIBAND_BIN="$(cd "$(dirname "${GUMMIBAND_BIN}")" && pwd)/$(basename "${GUMMIBAND_BIN}")"

Xvfb "${DISPLAY_SPEC}" -noreset -screen 0 "${SCREEN_W}x${SCREEN_H}x24" \
  >"${XVFB_LOG}" 2>&1 &
XVFB_PID=$!
if ! wait_for 50 xdpyinfo; then
  echo "Xvfb did not start on ${DISPLAY_SPEC}" >&2
  cat "${XVFB_LOG}" >&2
  exit 1
fi

# --- helpers ----------------------------------------------------------------

# gummiband's windows carry no WM_NAME - nothing needs one, since no window
# manager is meant to decorate them - so xdotool's usual --name search is no
# use. What they do carry is _NET_WM_WINDOW_TYPE, which is both a reliable
# handle and one of the properties under test.
window_of_type() { # window_of_type <_NET_WM_WINDOW_TYPE_FOO> -> window id
  local id
  for id in $(xwininfo -root -children 2>/dev/null |
    sed -n 's/^ *\(0x[0-9a-f]*\) .*/\1/p'); do
    if xprop -id "${id}" _NET_WM_WINDOW_TYPE 2>/dev/null | grep -q "$1"; then
      echo "${id}"
      return 0
    fi
  done
  return 1
}

geom() { # geom <window> -> "x y w h"
  xwininfo -id "$1" 2>/dev/null | sed -n \
    's/.*Absolute upper-left X: *\(.*\)/\1/p;
     s/.*Absolute upper-left Y: *\(.*\)/\1/p;
     s/.*Width: *\(.*\)/\1/p;
     s/.*Height: *\(.*\)/\1/p' | tr '\n' ' '
}

mapped() { # mapped <window> -> true if viewable
  xwininfo -id "$1" 2>/dev/null | grep -q 'Map State: IsViewable'
}

# The property, with the spaces xprop pads it with taken out.
prop() { # prop <window> <name> -> value
  xprop -id "$1" "$2" 2>/dev/null | sed 's/.*= *//' | tr -d ' '
}

snap() { # snap <window> <name> -> path to an xwd file
  xwd -id "$1" -out "${WORKDIR}/$2.xwd" 2>/dev/null
  echo "${WORKDIR}/$2.xwd"
}

pixel() { # pixel <xwd-file> <x> <y> -> "#RRGGBB"
  convert "$1" -depth 8 txt:- 2>/dev/null |
    sed -n "s/^$2,$3: ([^)]*)  *\(#[0-9A-Fa-f]\{6\}\).*/\1/p" | head -1
}

count_colour() { # count_colour <xwd-file> <#RRGGBB> -> number of such pixels
  convert "$1" -depth 8 txt:- 2>/dev/null | grep -c -- "$2"
}

# Anything that isn't the background. Text is antialiased, so counting pure
# black would make the "is there any text here" checks depend on how the font
# happens to render; counting what isn't background does not.
count_not_colour() { # count_not_colour <xwd-file> <#RRGGBB> -> pixel count
  convert "$1" -depth 8 txt:- 2>/dev/null |
    grep -v '^#' | grep -c -v -- "$2"
}

# Every check that looks at what was actually painted needs these. The
# property and geometry checks don't, so run what we can without them rather
# than refusing to run at all.
HAVE_PIXELS=no
if command -v xwd >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
  HAVE_PIXELS=yes
fi

# --- the configuration under test -------------------------------------------
#
# Three items, positioned so that each one can be aimed at without knowing how
# wide the font renders them:
#
#   * left items start at x=5 and are 20 pixels wider than their text, so the
#     first one always covers x=15.
#   * right items are laid out from display_width-5 leftwards and are at least
#     20 wide, so the first one always covers display_width-15.
#
# The middle item is the dynamic one, whose value comes from a file this
# script rewrites; it needs no aiming, only a repaint.

cat >"${WORKDIR}/.gummiband" <<EOF
name=Launch
click=exec touch ${WORKDIR}/clicked

name=exec cat ${WORKDIR}/dyn.txt
position=left
updatesecs=1

name=Menu
position=right
menuitems=alpha,beta,gamma
menuclick=exec echo '<item>' >${WORKDIR}/menuclicked
EOF
echo "AAAA" >"${WORKDIR}/dyn.txt"

# --- start gummiband --------------------------------------------------------
#
# HOME points at the temp dir: with no RESOURCE_MANAGER on the root, xcb-xrm
# falls back to ~/.Xresources, and the developer's own font and colour
# settings would otherwise change both the colours asserted on below and the
# height of the panel, which is derived from the font.
#
# The working directory is the temp dir too, because .gummiband is read from
# the current directory and nowhere else.
start_gummiband() {
  (cd "${WORKDIR}" && HOME="${WORKDIR}" exec "${GUMMIBAND_BIN}") \
    >>"${GB_LOG}" 2>&1 &
  GB_PID=$!
}

start_gummiband
sleep 1
if ! kill -0 "${GB_PID}" 2>/dev/null; then
  fail "gummiband started and stayed running"
  cat "${GB_LOG}" >&2
  exit 1
fi
pass "gummiband started and stayed running"

PANEL=$(window_of_type _NET_WM_WINDOW_TYPE_DOCK)
if [ -z "${PANEL}" ]; then
  fail "gummiband created a dock window"
  cat "${GB_LOG}" >&2
  exit 1
fi
pass "gummiband created a dock window"

# --- geometry ---------------------------------------------------------------
#
# The height is whatever the font makes it, so it is read rather than
# asserted; everything after this is expressed in terms of it.

read -r PX PY PW PH <<<"$(geom "${PANEL}")"
if [ "${PX}" = "0" ] && [ "${PY}" = "0" ] && [ "${PW}" = "${SCREEN_W}" ] &&
  [ "${PH}" -gt 0 ]; then
  pass "panel spans the top of the screen (${PW}x${PH}+${PX}+${PY})"
else
  fail "panel spans the top of the screen (got ${PW}x${PH}+${PX}+${PY}, want ${SCREEN_W}xN+0+0)"
fi
MID_Y=$((PH / 2))

# --- the EWMH properties ----------------------------------------------------

GOT=$(prop "${PANEL}" _NET_WM_STATE)
if [ "${GOT}" = "_NET_WM_STATE_BELOW" ]; then
  pass "panel asks to be stacked below other windows"
else
  fail "panel asks to be stacked below other windows (got '${GOT}')"
fi

# The 32-bit property check. Written as an array of long instead of uint32_t
# this comes back as "0, 0, 0, 0" or worse, because the server reads four
# 32-bit values out of what is really two 64-bit ones.
GOT=$(prop "${PANEL}" _NET_WM_STRUT)
WANT="0,0,${PH},0"
if [ "${GOT}" = "${WANT}" ]; then
  pass "_NET_WM_STRUT reserves the panel's height at the top"
else
  fail "_NET_WM_STRUT reserves the panel's height at the top (got '${GOT}', want '${WANT}')"
fi

# --- what actually got painted ----------------------------------------------

if [ "${HAVE_PIXELS}" = "no" ]; then
  echo "SKIP: painting, hover and drop-down pixel checks (need xwd and ImageMagick)"
else
  # Park the pointer off the panel: anything it hovers over is highlighted,
  # and that would change the colours sampled here.
  xdotool mousemove $((SCREEN_W / 2)) $((SCREEN_H / 2))
  sleep 0.5
  SNAP=$(snap "${PANEL}" panel)

  # Mid-screen is clear of both the left-hand items and the right-hand one, so
  # it is background and nothing else. White is gummiband's compiled-in
  # default, and there is no ~/.Xresources yet to say otherwise.
  GOT=$(pixel "${SNAP}" $((SCREEN_W / 2)) "${MID_Y}")
  if [ "${GOT}" = "#FFFFFF" ]; then
    pass "panel is painted in the default background colour"
  else
    fail "panel is painted in the default background colour (got '${GOT}', want '#FFFFFF')"
  fi

  # Text is drawn by Xft onto an XCB-created pixmap, which is then copied to
  # the window: a panel of nothing but background means one of those three
  # steps went wrong.
  INK=$(count_not_colour "${SNAP}" '#FFFFFF')
  if [ "${INK}" -gt 100 ]; then
    pass "item text is drawn on the panel (${INK} non-background pixels)"
  else
    fail "item text is drawn on the panel (only ${INK} non-background pixels)"
  fi
fi

# --- hovering highlights the item under the pointer -------------------------

if [ "${HAVE_PIXELS}" = "yes" ]; then
  xdotool mousemove 15 "${MID_Y}"
  sleep 0.5
  SNAP=$(snap "${PANEL}" hover)
  # #B3E0FF is the default selBackground.
  N=$(count_colour "${SNAP}" '#B3E0FF')
  if [ "${N}" -gt 20 ]; then
    pass "hovering an item highlights it"
  else
    fail "hovering an item highlights it (${N} highlight pixels)"
  fi

  xdotool mousemove $((SCREEN_W / 2)) $((SCREEN_H / 2))
  sleep 0.5
  SNAP=$(snap "${PANEL}" unhover)
  N=$(count_colour "${SNAP}" '#B3E0FF')
  if [ "${N}" -eq 0 ]; then
    pass "leaving the panel clears the highlight"
  else
    fail "leaving the panel clears the highlight (${N} highlight pixels remain)"
  fi
fi

# --- clicking an item runs its command --------------------------------------

rm -f "${WORKDIR}/clicked"
xdotool mousemove 15 "${MID_Y}"
sleep 0.3
xdotool click 1
if wait_for 30 test -e "${WORKDIR}/clicked"; then
  pass "clicking an item runs its command"
else
  fail "clicking an item runs its command"
fi

# --- the drop-down menu -----------------------------------------------------

xdotool mousemove $((SCREEN_W - 15)) "${MID_Y}"
sleep 0.3
xdotool click 1
sleep 0.5

DROPDOWN=$(window_of_type _NET_WM_WINDOW_TYPE_MENU)
if [ -z "${DROPDOWN}" ]; then
  fail "clicking a menu item opens a drop-down window"
else
  pass "clicking a menu item opens a drop-down window"

  if mapped "${DROPDOWN}"; then
    pass "the drop-down is mapped"
  else
    fail "the drop-down is mapped"
  fi

  # Three items, each one line of text tall - the same line height the panel
  # is sized from - hung directly below the panel and kept on screen.
  read -r DX DY DW DH <<<"$(geom "${DROPDOWN}")"
  if [ "${DY}" = "${PH}" ] && [ "${DH}" = "$((3 * PH))" ] &&
    [ "${DX}" -ge 0 ] && [ "$((DX + DW))" -le "${SCREEN_W}" ]; then
    pass "drop-down is sized to its items and sits below the panel (${DW}x${DH}+${DX}+${DY})"
  else
    fail "drop-down is sized to its items and sits below the panel (got ${DW}x${DH}+${DX}+${DY}, want Nx$((3 * PH))+N+${PH}, on screen)"
  fi

  # Aim at the middle item, "beta": the drop-down's items are PH tall in the
  # order they were configured.
  ITEM_X=$((DX + DW / 2))
  ITEM_Y=$((DY + PH + PH / 2))

  if [ "${HAVE_PIXELS}" = "yes" ]; then
    xdotool mousemove "${ITEM_X}" "${ITEM_Y}"
    sleep 0.5
    SNAP=$(snap "${DROPDOWN}" dropdown)
    N=$(count_colour "${SNAP}" '#B3E0FF')
    if [ "${N}" -gt 20 ]; then
      pass "hovering a drop-down item highlights it"
    else
      fail "hovering a drop-down item highlights it (${N} highlight pixels)"
    fi
    INK=$(count_not_colour "${SNAP}" '#FFFFFF')
    if [ "${INK}" -gt 100 ]; then
      pass "drop-down items and border are drawn (${INK} non-background pixels)"
    else
      fail "drop-down items and border are drawn (only ${INK} non-background pixels)"
    fi
  fi

  # Clicking an item substitutes it into the configured menuclick command.
  rm -f "${WORKDIR}/menuclicked"
  xdotool mousemove "${ITEM_X}" "${ITEM_Y}"
  sleep 0.3
  xdotool click 1
  if wait_for 30 test -e "${WORKDIR}/menuclicked"; then
    GOT=$(tr -d ' \n' <"${WORKDIR}/menuclicked")
    if [ "${GOT}" = "beta" ]; then
      pass "clicking a drop-down item runs menuclick with <item> substituted"
    else
      fail "clicking a drop-down item runs menuclick with <item> substituted (got '${GOT}', want 'beta')"
    fi
  else
    fail "clicking a drop-down item runs menuclick with <item> substituted (command never ran)"
  fi

  sleep 0.5
  if mapped "${DROPDOWN}"; then
    fail "choosing a drop-down item closes the menu"
  else
    pass "choosing a drop-down item closes the menu"
  fi
fi

# --- an updating item repaints itself ---------------------------------------
#
# The 'updatesecs' path, and with it the once-a-second wake-up in the event
# loop that drives it. This is the check that a missing flush shows up in:
# nothing arrives from the server to prompt the repaint, so the requests it
# makes are the ones most likely to sit unsent in the output buffer.

if [ "${HAVE_PIXELS}" = "yes" ]; then
  xdotool mousemove $((SCREEN_W / 2)) $((SCREEN_H / 2))
  sleep 0.5
  BEFORE=$(snap "${PANEL}" before)
  BEFORE_SUM=$(md5sum <"${BEFORE}")
  echo "WWWW" >"${WORKDIR}/dyn.txt"
  sleep 3
  AFTER=$(snap "${PANEL}" after)
  AFTER_SUM=$(md5sum <"${AFTER}")
  if [ "${BEFORE_SUM}" != "${AFTER_SUM}" ]; then
    pass "an item with updatesecs repaints when its value changes"
  else
    fail "an item with updatesecs repaints when its value changes (panel unchanged)"
  fi
fi

# --- X resources, and the SIGHUP restart that reloads them ------------------
#
# SIGHUP makes gummiband execvp() itself, which is how a configuration change
# is picked up: same process, same pid, fresh everything else. Writing
# ~/.Xresources first means the restart has something new to read, so one
# sequence covers both.

cat >"${WORKDIR}/.Xresources" <<'EOF'
gummiband.background: #204060
gummiband.selBackground: #FF0000
EOF

kill -HUP "${GB_PID}"
sleep 1.5
if kill -0 "${GB_PID}" 2>/dev/null; then
  pass "SIGHUP restarts gummiband in place"
else
  fail "SIGHUP restarts gummiband in place (process gone)"
fi

PANEL=$(window_of_type _NET_WM_WINDOW_TYPE_DOCK)
if [ -z "${PANEL}" ]; then
  fail "the restarted gummiband recreated its dock window"
elif [ "${HAVE_PIXELS}" = "yes" ]; then
  pass "the restarted gummiband recreated its dock window"
  xdotool mousemove $((SCREEN_W / 2)) $((SCREEN_H / 2))
  sleep 0.5
  read -r PX PY PW PH <<<"$(geom "${PANEL}")"
  MID_Y=$((PH / 2))
  SNAP=$(snap "${PANEL}" resources)
  GOT=$(pixel "${SNAP}" $((SCREEN_W / 2)) "${MID_Y}")
  if [ "${GOT}" = "#204060" ]; then
    pass "gummiband.background from ~/.Xresources is honoured"
  else
    fail "gummiband.background from ~/.Xresources is honoured (got '${GOT}', want '#204060')"
  fi

  xdotool mousemove 15 "${MID_Y}"
  sleep 0.5
  SNAP=$(snap "${PANEL}" selresources)
  N=$(count_colour "${SNAP}" '#FF0000')
  if [ "${N}" -gt 20 ]; then
    pass "gummiband.selBackground from ~/.Xresources is honoured"
  else
    fail "gummiband.selBackground from ~/.Xresources is honoured (${N} highlight pixels)"
  fi
else
  pass "the restarted gummiband recreated its dock window"
fi

# --- stepping aside for a full-screen window ---------------------------------
#
# The panel gets out of the way of a game running full screen on its own
# monitor, and comes back when the game goes. That needs two monitors, which
# Xvfb has no way of providing - but RandR 1.5 lets a client carve the screen
# up into monitors by hand, with 'xrandr --setmonitor', and gummiband reads
# that list rather than the CRTCs precisely so that this is testable. So carve
# the 1280x1024 screen into an 800x600 monitor at the top left (the "main"
# one, which gets the real output) and a 400x300 one below and to the right of
# it.
#
# Note that a monitor list set this way is a client-side override, and setting
# it fires no RandR event, so gummiband has to be restarted to see it. SIGHUP
# does that, and keeps the same pid, so the checks after this one still apply
# to the same process.
#
# What stands in for the game is xclock, which honours -geometry exactly and
# so can be told to cover a monitor to the pixel. That's the case worth
# testing: it is what "borderless fullscreen" produces, and unlike
# _NET_WM_STATE_FULLSCREEN it can only be spotted by measuring the window.

OUTPUT=$(xrandr --query 2>/dev/null | awk '/ connected/ {print $1; exit}')
if [ -z "${OUTPUT}" ] || ! command -v xclock >/dev/null 2>&1; then
  echo "SKIP: two-monitor checks (need xrandr with an output, and xclock)"
else
  xrandr --setmonitor main 800/212x600/159+0+0 "${OUTPUT}" >/dev/null 2>&1
  xrandr --setmonitor side 400/106x300/79+800+600 none >/dev/null 2>&1

  kill -HUP "${GB_PID}"
  sleep 1.5
  PANEL=$(window_of_type _NET_WM_WINDOW_TYPE_DOCK)
  read -r PX PY PW PH <<<"$(geom "${PANEL}")"
  if [ "${PX}" = "0" ] && [ "${PY}" = "0" ] && [ "${PW}" = "800" ]; then
    pass "panel spans only the top monitor (${PW}x${PH}+${PX}+${PY})"
  else
    fail "panel spans only the top monitor (got ${PW}x${PH}+${PX}+${PY}, want 800xN+0+0)"
  fi

  # A window that covers the main monitor exactly: the panel should move to
  # the other one.
  xclock -geometry 800x600+0+0 >/dev/null 2>&1 &
  GAME_PID=$!
  sleep 1.5
  read -r PX PY PW PH <<<"$(geom "${PANEL}")"
  if [ "${PX}" = "800" ] && [ "${PY}" = "600" ] && [ "${PW}" = "400" ]; then
    pass "panel steps aside onto the other monitor (${PW}x${PH}+${PX}+${PY})"
  else
    fail "panel steps aside onto the other monitor (got ${PW}x${PH}+${PX}+${PY}, want 400xN+800+600)"
  fi

  # _NET_WM_STRUT can only name an edge of the whole display, so while the
  # panel is off on a lower monitor it must claim nothing at all - a top strut
  # would reserve a strip across the very window it stepped aside for.
  GOT=$(prop "${PANEL}" _NET_WM_STRUT)
  if [ "${GOT}" = "0,0,0,0" ]; then
    pass "panel claims no strut while it has stepped aside"
  else
    fail "panel claims no strut while it has stepped aside (got '${GOT}')"
  fi

  kill "${GAME_PID}" 2>/dev/null
  wait "${GAME_PID}" 2>/dev/null
  unset GAME_PID
  sleep 1.5
  read -r PX PY PW PH <<<"$(geom "${PANEL}")"
  if [ "${PX}" = "0" ] && [ "${PY}" = "0" ] && [ "${PW}" = "800" ]; then
    pass "panel comes home when the full-screen window goes"
  else
    fail "panel comes home when the full-screen window goes (got ${PW}x${PH}+${PX}+${PY}, want 800xN+0+0)"
  fi

  GOT=$(prop "${PANEL}" _NET_WM_STRUT)
  if [ "${GOT}" = "0,0,${PH},0" ]; then
    pass "panel claims its strut again once home"
  else
    fail "panel claims its strut again once home (got '${GOT}', want '0,0,${PH},0')"
  fi

  # A big window that doesn't quite cover the monitor is just a big window -
  # a maximised one, most likely - and is no reason to go anywhere.
  xclock -geometry 700x500+0+0 >/dev/null 2>&1 &
  GAME_PID=$!
  sleep 1.5
  read -r PX PY PW PH <<<"$(geom "${PANEL}")"
  if [ "${PX}" = "0" ] && [ "${PY}" = "0" ] && [ "${PW}" = "800" ]; then
    pass "panel stays put for a window that doesn't cover the monitor"
  else
    fail "panel stays put for a window that doesn't cover the monitor (got ${PW}x${PH}+${PX}+${PY})"
  fi
  kill "${GAME_PID}" 2>/dev/null
  wait "${GAME_PID}" 2>/dev/null
  unset GAME_PID
  sleep 1
fi

# --- no errors along the way ------------------------------------------------
#
# X errors reach gummiband as events with a response_type of zero and are
# logged at E, so a bad request anywhere above lands here even when whatever
# it was meant to do happened to look right.

if grep -q '^E ' "${GB_LOG}"; then
  fail "no error-level log lines from gummiband"
  grep '^E ' "${GB_LOG}" | sed 's/^/    /'
else
  pass "no error-level log lines from gummiband"
fi

if kill -0 "${GB_PID}" 2>/dev/null; then
  pass "gummiband survived the whole session"
else
  fail "gummiband survived the whole session"
  tail -20 "${GB_LOG}" | sed 's/^/    /'
fi

echo
if [ "${FAILURES}" -eq 0 ]; then
  echo "All gummiband checks passed."
  exit 0
fi
echo "${FAILURES} gummiband check(s) failed."
exit 1
