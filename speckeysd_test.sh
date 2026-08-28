#!/bin/bash
# speckeysd_test.sh
#
# Tests for speckeysd, the hot key daemon, run against a headless Xvfb
# display.
#
# speckeysd has no unit tests and nothing to hook one onto: it is a single
# translation unit whose every interesting behaviour is a request to an X
# server, or a signal. So this drives the real thing - a keys file in, key
# presses in, the commands it runs out - which is also what makes it the
# regression test for the XCB port. Almost everything the port could have
# broken is invisible to the compiler:
#
#   * argument order. XGrabKey takes (keycode, modifiers, window,
#     owner_events, ...); xcb_grab_key takes (owner_events, window,
#     modifiers, keycode, ...) - the flag moves to the front and the keycode
#     and the modifiers swap places. Every one of those is an integer, so any
#     permutation compiles, and the result is a grab of the wrong key, or of
#     no key at all. What catches it is pressing keys and seeing what runs.
#   * the root window of each screen. Xlib had RootWindow(dpy, n); XCB makes
#     you walk xcb_setup_roots_iterator n times, and it is very easy to grab
#     on screen 0's root n times instead. So the display here has two screens
#     and one check presses a hot key on the second.
#   * the fields of a key event. XCB's key press event calls the keycode
#     'detail', has no xkey sub-struct to name it, and sets bit 0x80 of
#     response_type on events that arrived via SendEvent. Reading the wrong
#     field means no hot key ever matches.
#   * the keysym lookup. Both the level (shifted or not) and the refresh on
#     MappingNotify are load-bearing, and both fail silently: the wrong level
#     breaks only the shifted hot keys, and a missing refresh breaks nothing
#     at all until the keyboard map changes. There is a check for each.
#   * flushing. XCB never pushes requests on its own, so grabs made at
#     start-up can sit unsent in the output buffer while the daemon blocks in
#     select(). Every check that presses a key is a check that the grabs
#     reached the server at all - which they do here because each one is
#     waited on, but a port that sent them and then forgot to flush would
#     fail every one of them.
#   * the error model. Xlib reported a failed grab through a global handler;
#     XCB delivers it as an event, or as a reply to a _checked request. The
#     "second instance" check below is what keeps that path honest, since a
#     failed grab is otherwise completely silent.
#
# No window manager runs here, and none is needed: speckeysd creates no
# windows of its own and grabs on the root.
#
# Usage: ./speckeysd_test.sh [path-to-speckeysd-binary]
#
# Exits 0 if every check passes, 1 otherwise, printing a PASS/FAIL line per
# check. Picks a random display number, so it's safe to run alongside a real X
# session - but note that it types keys and moves the pointer, so don't point
# DISPLAY at your own session.
#
# One trap for anyone adding a check: don't use Control-Alt with a function
# key. The default keyboard map makes Ctrl+Alt+F1..F12 the VT switch keys,
# which the server swallows before any client, grab or no grab. Ctrl+Alt with
# a letter is fine, and so is Ctrl or Alt alone with a function key.

set -u

SPECKEYSD_BIN="${1:-./bin/speckeysd}"

DISPLAY_NUM=$((90 + RANDOM % 400))
DISPLAY_SPEC=":${DISPLAY_NUM}"
WORKDIR="$(mktemp -d)"
XVFB_LOG="${WORKDIR}/xvfb.log"
SK_LOG="${WORKDIR}/speckeysd.log"
KEYS="${WORKDIR}/keys"
# Where the hot key commands leave their evidence. One file per hot key.
FIRED="${WORKDIR}/fired"

export DISPLAY="${DISPLAY_SPEC}"
# speckeysd runs its commands with $SHELL, falling back to /bin/sh. Pin it, so
# a developer running fish or zsh doesn't change what "-c <command>" means.
export SHELL=/bin/sh

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
  [ -n "${SK_PID:-}" ] && kill "${SK_PID}" >/dev/null 2>&1
  [ -n "${SK2_PID:-}" ] && kill "${SK2_PID}" >/dev/null 2>&1
  [ -n "${SK3_PID:-}" ] && kill "${SK3_PID}" >/dev/null 2>&1
  [ -n "${XLOGO_PID:-}" ] && kill "${XLOGO_PID}" >/dev/null 2>&1
  [ -n "${XVFB_PID:-}" ] && kill "${XVFB_PID}" >/dev/null 2>&1
  wait >/dev/null 2>&1
  rm -rf "${WORKDIR}"
}
trap cleanup EXIT

if [ ! -x "${SPECKEYSD_BIN}" ]; then
  echo "speckeysd binary not found or not executable: ${SPECKEYSD_BIN}" >&2
  exit 1
fi
# Absolute, because SIGHUP makes speckeysd execvp(argv[0]) itself, and a
# relative path only survives that by luck.
SPECKEYSD_BIN="$(cd "$(dirname "${SPECKEYSD_BIN}")" && pwd)/$(basename "${SPECKEYSD_BIN}")"

# Two screens, so that the per-screen root grab has something to get wrong.
# They're separate screens, not a Xinerama pair: :N.0 and :N.1 each have their
# own root window, which is exactly the arrangement speckeysd's loop over
# ScreenCount is for.
Xvfb "${DISPLAY_SPEC}" -noreset -screen 0 800x600x24 -screen 1 640x480x24 \
  >"${XVFB_LOG}" 2>&1 &
XVFB_PID=$!
if ! wait_for 50 xdpyinfo; then
  echo "Xvfb did not start on ${DISPLAY_SPEC}" >&2
  cat "${XVFB_LOG}" >&2
  exit 1
fi

# --- helpers ----------------------------------------------------------------

mkdir -p "${FIRED}"

# fired_cmd names the command a hot key is given in the keys file: touch a
# file named after it. Checking a hot key then means checking that file turned
# up, which is also a check that the command ran through a shell at all.
fired_cmd() { # fired_cmd <name> -> command line
  echo "touch ${FIRED}/$1"
}

# Presses a key combination, in xdotool's syntax (eg "ctrl+F2").
press() { xdotool key "$1"; }

# The two halves of every hot key check. arm removes the evidence file, and
# fired waits a moment for it to reappear - the command runs in a forked
# shell, so it is never instant.
arm() { rm -f "${FIRED}/$1"; }
fired() { wait_for 30 test -e "${FIRED}/$1"; }

# For the checks that a key does *not* run anything. There's no event to wait
# for, so this waits out the time the command would have taken.
not_fired() {
  sleep 0.6
  [ ! -e "${FIRED}/$1" ]
}

check_fires() { # check_fires <name> <key combination> <description>
  arm "$1"
  press "$2"
  if fired "$1"; then
    pass "$3"
  else
    fail "$3"
  fi
}

check_no_fire() { # check_no_fire <name> <key combination> <description>
  arm "$1"
  press "$2"
  if not_fired "$1"; then
    pass "$3"
  else
    fail "$3"
  fi
}

write_keys_file() {
  {
    # A comment, with a tab in it so that only the leading '#' can be what
    # makes it ignored.
    printf '# Control-F8\t%s\n' "$(fired_cmd comment)"
    # No tab, so no command: skipped rather than treated as a key with an
    # empty command.
    printf 'Control-F9 no tab here\n'
    printf 'Control-F2\t%s\n' "$(fired_cmd ctrl-f2)"
    printf 'Control-Alt-x\t%s\n' "$(fired_cmd ctrl-alt-x)"
    printf 'F5\t%s\n' "$(fired_cmd f5)"
    # Not "Shift-a": the name in the file is the keysym the shifted key
    # produces, which is what the daemon looks up when it sees the press.
    printf 'Shift-A\t%s\n' "$(fired_cmd shift-a)"
    printf 'Super-F7\t%s\n' "$(fired_cmd super-f7)"
    # An unknown modifier is complained about and dropped; the rest of the
    # specification still stands, so this is Control-F6.
    printf 'Bogus-Control-F6\t%s\n' "$(fired_cmd bogus-mod)"
    # A key name that isn't a keysym at all.
    printf 'Control-Alt-NoSuchKeyName\t%s\n' "$(fired_cmd no-such-key)"
    # Long-running, for the zombie and file descriptor checks below.
    printf 'Control-F3\tsleep 30\n'
  } >"${KEYS}"
}

start_speckeysd() {
  "${SPECKEYSD_BIN}" "${KEYS}" >>"${SK_LOG}" 2>&1 &
  SK_PID=$!
}

# --- start it up ------------------------------------------------------------

write_keys_file
start_speckeysd
sleep 1
if ! kill -0 "${SK_PID}" 2>/dev/null; then
  fail "speckeysd started and stayed running"
  cat "${SK_LOG}" >&2
  exit 1
fi
pass "speckeysd started and stayed running"

# --- the hot keys themselves ------------------------------------------------

check_fires ctrl-f2 ctrl+F2 "a hot key with one modifier runs its command"
check_fires ctrl-alt-x ctrl+alt+x "a hot key with two modifiers runs its command"
check_fires f5 F5 "a hot key with no modifiers runs its command"
check_fires super-f7 super+F7 "a Super (Mod4) hot key runs its command"

# The shifted-keysym path: the daemon has to look the key event's keycode up
# at level 1 to get 'A' rather than 'a', or this never matches.
check_fires shift-a shift+a "a Shift hot key matches the shifted keysym"

check_fires bogus-mod ctrl+F6 \
  "an unknown modifier is dropped and the rest of the key still works"

# --- and what shouldn't happen ----------------------------------------------

check_no_fire comment ctrl+F8 "a commented-out line is not a hot key"
check_no_fire no-such-key ctrl+alt+n "an unbound key runs nothing"

# Grabs are for an exact modifier set: Control-F2 is not Control-Shift-F2.
check_no_fire ctrl-f2 ctrl+shift+F2 \
  "extra modifiers don't match a hot key (Control-Shift-F2 vs Control-F2)"

# --- the lock keys ----------------------------------------------------------
#
# Caps Lock, Num Lock and Scroll Lock are all modifier bits as far as a grab
# is concerned, so a hot key has to be grabbed once for every combination of
# them or it stops working the moment the user leaves Num Lock on. That's
# eight grabs per hot key; these check three of the eight.

press Caps_Lock
sleep 0.2
check_fires ctrl-f2 ctrl+F2 "a hot key still works with Caps Lock on"

press Num_Lock
sleep 0.2
check_fires ctrl-f2 ctrl+F2 "a hot key still works with Caps and Num Lock on"

press Caps_Lock
sleep 0.2
check_fires ctrl-f2 ctrl+F2 "a hot key still works with Num Lock on"

press Num_Lock
sleep 0.2

# --- the grab is on the root window -----------------------------------------
#
# Which is the whole point of a hot key daemon: it has no window of its own
# and no focus, so if the grab went anywhere else nothing would reach it once
# an application had the keyboard.

xlogo -geometry 200x200+10+10 >/dev/null 2>&1 &
XLOGO_PID=$!
if wait_for 50 xdotool search --class xlogo; then
  XLOGO_WIN=$(xdotool search --class xlogo | head -1)
  xdotool windowfocus "${XLOGO_WIN}"
  sleep 0.3
  check_fires ctrl-f2 ctrl+F2 \
    "a hot key works while another window has the input focus"
else
  fail "a hot key works while another window has the input focus (no xlogo)"
fi
kill "${XLOGO_PID}" >/dev/null 2>&1
XLOGO_PID=

# --- the second screen ------------------------------------------------------
#
# Key events go to the root of the screen the pointer is on, so a grab that
# was only ever made on screen 0's root produces nothing here.
#
# That is true only while the input focus is PointerRoot, which is what makes
# the pointer the thing that decides. The check above left the focus on a
# window of screen 0, and killing that window reverts the focus to its parent
# - screen 0's root - rather than to PointerRoot, which would send these key
# presses to screen 0 whatever the pointer is doing and leave this check
# passing when it shouldn't. Window 1 is PointerRoot; put it back.
xdotool windowfocus 1 >/dev/null 2>&1
sleep 0.3

xdotool mousemove --screen 1 100 100
sleep 0.3
GOT_SCREEN=$(xdotool getmouselocation --shell 2>/dev/null | sed -n 's/^SCREEN=//p')
if [ "${GOT_SCREEN}" != "1" ]; then
  fail "a hot key works on the second screen (pointer didn't move there)"
else
  arm ctrl-f2
  DISPLAY="${DISPLAY_SPEC}.1" xdotool key ctrl+F2
  if fired ctrl-f2; then
    pass "a hot key works on the second screen"
  else
    fail "a hot key works on the second screen"
  fi
fi
xdotool mousemove --screen 0 100 100
sleep 0.3

# --- the commands it runs ---------------------------------------------------

# The X connection is closed in the child before the exec, so that a
# long-running command doesn't hold a copy of the socket open.
arm sleep-marker
press ctrl+F3
if wait_for 30 pgrep -P "${SK_PID}"; then
  CHILD=$(pgrep -P "${SK_PID}" | head -1)
  if [ -d "/proc/${CHILD}/fd" ]; then
    SOCKETS=$(find "/proc/${CHILD}/fd" -type l 2>/dev/null |
      xargs -r -I{} readlink {} 2>/dev/null | grep -c '^socket:')
    if [ "${SOCKETS}" = "0" ]; then
      pass "a command doesn't inherit the X connection"
    else
      fail "a command doesn't inherit the X connection (${SOCKETS} socket fds)"
    fi
  else
    echo "SKIP: the X connection check (no /proc)"
  fi
else
  fail "a command doesn't inherit the X connection (command never started)"
fi

# SIGCHLD is handled, and the handler reinstalls itself, so nothing a hot key
# starts is left behind as a zombie. Run a few short commands first to give it
# something to reap.
press ctrl+F2
press F5
press ctrl+F2
sleep 1
ZOMBIES=$(ps -o stat= --ppid "${SK_PID}" 2>/dev/null | grep -c '^Z')
if [ "${ZOMBIES}" = "0" ]; then
  pass "finished commands are reaped, not left as zombies"
else
  fail "finished commands are reaped, not left as zombies (${ZOMBIES} zombies)"
fi

# --- what it said about the keys file ---------------------------------------

if grep -q 'ignoring unknown modifier "Bogus" in "Bogus-Control-F6"' "${SK_LOG}"; then
  pass "an unknown modifier is reported"
else
  fail "an unknown modifier is reported"
fi

if grep -q 'NoSuchKeyName' "${SK_LOG}"; then
  pass "an unknown key name is reported"
else
  fail "an unknown key name is reported"
fi

if grep -q "couldn't grab" "${SK_LOG}"; then
  fail "no grab failures from the only instance running"
  grep "couldn't grab" "${SK_LOG}" | sed 's/^/    /'
else
  pass "no grab failures from the only instance running"
fi

# --- a second instance, competing for the same keys -------------------------
#
# A grab that fails is silent unless someone looks, so the daemon asks the
# server about each one. This is the only check of that path, and it is the
# one that goes red if the port stops looking at errors at all.

SK2_LOG="${WORKDIR}/speckeysd2.log"
"${SPECKEYSD_BIN}" "${KEYS}" >"${SK2_LOG}" 2>&1 &
SK2_PID=$!
sleep 1.5
if grep -q "couldn't grab key \"Control-F2\": the key/button combination is already in use" "${SK2_LOG}"; then
  pass "a second instance reports the keys it couldn't grab"
else
  fail "a second instance reports the keys it couldn't grab"
  sed 's/^/    /' "${SK2_LOG}"
fi

# One message per hot key, not one per grab. A hot key is eight grabs (the
# lock combinations) on each of the two screens, and all sixteen are refused
# together, so the complaint is worth de-duplicating.
SK2_COMPLAINTS=$(grep -c "couldn't grab key \"Control-F2\"" "${SK2_LOG}")
if [ "${SK2_COMPLAINTS}" -eq 1 ]; then
  pass "a key it can't grab is complained about once, not once per grab"
else
  fail "a key it can't grab is complained about once, not once per grab (${SK2_COMPLAINTS} times)"
fi
kill "${SK2_PID}" >/dev/null 2>&1
wait "${SK2_PID}" >/dev/null 2>&1
SK2_PID=
sleep 0.3

check_fires ctrl-f2 ctrl+F2 \
  "the original instance still owns its keys afterwards"

# --- the keyboard map changing underneath it --------------------------------
#
# The grab is on a keycode, but the match is on a keysym, so the daemon has to
# keep its idea of the keyboard map up to date from MappingNotify. Move F5's
# keysym to a keycode nothing has grabbed and the old keycode - which is still
# grabbed - stops matching: it produces F13 now, and F13 isn't a hot key. A
# daemon working from a stale map would still see F5 there and run the command.

F5_KEYCODE=$(xmodmap -pke | sed -n 's/^keycode *\([0-9]*\) = F5 .*/\1/p' | head -1)
# Somewhere to move F5 to: a keycode the default map leaves empty.
SPARE_KEYCODE=$(xmodmap -pke | sed -n 's/^keycode *\([0-9]*\) =$/\1/p' |
  awk '$1 > 100 { print; exit }')
if [ -z "${F5_KEYCODE}" ] || [ -z "${SPARE_KEYCODE}" ]; then
  fail "the keyboard map is re-read after a MappingNotify (no keycode to use)"
else
  xmodmap -e "keycode ${F5_KEYCODE} = F13"
  sleep 0.7
  # xdotool finds F13 on that same keycode, so this presses the key that was
  # grabbed. Nothing should fire, but for either of two reasons - a stale map
  # would match the wrong keysym, and a moved grab isn't listening to that
  # keycode at all - which is why the re-grab gets its own checks below.
  check_no_fire f5 F13 \
    "the keyboard map is re-read after a MappingNotify"

  # F5 is nowhere on the keyboard now, and the grab that was on it has been
  # given up. Worth saying out loud: the hot key has silently stopped working
  # and it isn't the daemon's doing.
  if grep -q "key \"F5\" is no longer on this keyboard" "${SK_LOG}"; then
    pass "a hot key vanishing from the keyboard map is reported"
  else
    fail "a hot key vanishing from the keyboard map is reported"
  fi

  xmodmap -e "keycode ${F5_KEYCODE} = F5"
  sleep 0.7
  check_fires f5 F5 "the hot key works again once the map is put back"

  # --- the grabs move with the keysym ---------------------------------
  #
  # The check the one above can't be: F5 doesn't vanish here, it *moves*.
  # xdotool presses whichever keycode carries F5, so it presses the new one.
  # A daemon that only re-read the map still holds the grab on the old
  # keycode, never sees the press, and the hot key is dead until it restarts.
  #
  # Order matters: F5 has to leave the old keycode before it arrives at the
  # new one, or it's on both at once and xdotool presses the lower.
  xmodmap -e "keycode ${F5_KEYCODE} = F13"
  xmodmap -e "keycode ${SPARE_KEYCODE} = F5"
  sleep 0.7
  check_fires f5 F5 \
    "a hot key whose keysym moves to another keycode is grabbed there"

  xmodmap -e "keycode ${SPARE_KEYCODE} ="
  xmodmap -e "keycode ${F5_KEYCODE} = F5"
  sleep 0.7
  check_fires f5 F5 "the hot key follows the keysym back again"

  # A remap with nothing to do with any hot key, which is this pass's chance
  # to break a key that was working: it runs over every hot key, moved or not.
  # It does not pin down the re-grabbing of keys that didn't move - nothing
  # here does, and regrab_after_mapping_change says why that's done anyway.
  xmodmap -e "keycode ${SPARE_KEYCODE} = F14"
  sleep 0.7
  check_fires f5 F5 "an unrelated remap leaves the hot keys working"
  xmodmap -e "keycode ${SPARE_KEYCODE} ="
  sleep 0.7
fi

# --- SIGHUP reloads the configuration ---------------------------------------
#
# By exec'ing itself: same process, same pid, fresh everything else. The old
# key going quiet is as much a part of that as the new one working.

cat >"${KEYS}" <<EOF
Control-F4	$(fired_cmd ctrl-f4)
EOF

kill -HUP "${SK_PID}"
sleep 1.5
if kill -0 "${SK_PID}" 2>/dev/null; then
  pass "SIGHUP restarts speckeysd in place"
else
  fail "SIGHUP restarts speckeysd in place (process gone)"
fi

check_fires ctrl-f4 ctrl+F4 "the reloaded configuration's hot key works"
check_no_fire ctrl-f2 ctrl+F2 "a hot key dropped from the file stops working"

# --- the command line and a keys file that isn't there ----------------------

if "${SPECKEYSD_BIN}" >"${WORKDIR}/noargs.log" 2>&1; then
  fail "no keys file argument is a usage error"
else
  if grep -q "syntax: speckeysd" "${WORKDIR}/noargs.log"; then
    pass "no keys file argument is a usage error"
  else
    fail "no keys file argument is a usage error (wrong message)"
    sed 's/^/    /' "${WORKDIR}/noargs.log"
  fi
fi

"${SPECKEYSD_BIN}" "${WORKDIR}/nonexistent" >"${WORKDIR}/nofile.log" 2>&1 &
NOFILE_PID=$!
sleep 1
if kill -0 "${NOFILE_PID}" 2>/dev/null &&
  grep -q "couldn't read" "${WORKDIR}/nofile.log"; then
  pass "an unreadable keys file is reported, and doesn't stop it running"
else
  fail "an unreadable keys file is reported, and doesn't stop it running"
  sed 's/^/    /' "${WORKDIR}/nofile.log"
fi
kill "${NOFILE_PID}" >/dev/null 2>&1
wait "${NOFILE_PID}" >/dev/null 2>&1

# --- still standing ---------------------------------------------------------

if kill -0 "${SK_PID}" 2>/dev/null; then
  pass "speckeysd survived the whole session"
else
  fail "speckeysd survived the whole session"
  tail -20 "${SK_LOG}" | sed 's/^/    /'
fi

# --- picking up a key that was taken when it started ------------------------
#
# This one goes last, because it works by taking the first instance away.
#
# A grab on the root is exclusive - that's the whole reason for grabbing there
# - so starting second means starting without your keys, and nothing ever
# tells you when the client holding them lets go. So the daemon asks again
# every few seconds. Without the retry the first check here still passes (the
# message is printed once at startup) and the last two go red: the key stays
# dead for the life of the process.
#
# The keys file is the one the SIGHUP section left behind, so Control-F4 is
# the only hot key, and the first instance is holding it.

SK3_LOG="${WORKDIR}/speckeysd3.log"
"${SPECKEYSD_BIN}" "${KEYS}" >"${SK3_LOG}" 2>&1 &
SK3_PID=$!
sleep 1.5

if grep -q "couldn't grab key \"Control-F4\".*will keep trying" "${SK3_LOG}"; then
  pass "a key another client holds is reported as one it will keep trying for"
else
  fail "a key another client holds is reported as one it will keep trying for"
  sed 's/^/    /' "${SK3_LOG}"
fi

# The server drops a client's grabs when its connection goes, so this is what
# frees Control-F4. Waited for, not just signalled: the grab is held until the
# process is actually gone.
kill "${SK_PID}" >/dev/null 2>&1
wait "${SK_PID}" >/dev/null 2>&1
SK_PID=

# The retry interval is 5s, so allow a couple of them before giving up.
if wait_for 150 grep -q "grabbed key \"Control-F4\" at last" "${SK3_LOG}"; then
  pass "the key is grabbed on a retry once the other client exits"
else
  fail "the key is grabbed on a retry once the other client exits"
  sed 's/^/    /' "${SK3_LOG}"
fi

# And the grab is real, not just a message: the point of the exercise.
check_fires ctrl-f4 ctrl+F4 "the retried hot key runs its command"

kill "${SK3_PID}" >/dev/null 2>&1
wait "${SK3_PID}" >/dev/null 2>&1
SK3_PID=

echo
if [ "${FAILURES}" -eq 0 ]; then
  echo "All speckeysd checks passed."
  exit 0
fi
echo "${FAILURES} speckeysd check(s) failed."
exit 1
