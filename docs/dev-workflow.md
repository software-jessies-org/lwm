# Dev workflow

## Build

```sh
make -j20          # incremental; the Makefile is hand-written and checked in
```

Flags live in the `Makefile`: `-std=c++17 -g3 -O0 -DSHAPE -Wall -Werror -Wextra
-Wpedantic -Wno-sign-compare`. **`-Werror` is on**, so warnings break the build.

Source lives under `src/`, one directory per program; objects mirror that
layout under `build/`, and the binaries land in `bin/`. `make` builds all
three: `bin/lwm`, `bin/gummiband` (a panel) and `bin/speckeysd` (a hot key
daemon). `make lwm` (or `gummiband`, or `speckeysd`) builds just the one; the
test targets are named separately (`make gummi`, `make speckeys`) so that
building and testing stay different verbs.

Adding a source file means adding it to that program's source list in the
`Makefile` (`LWM_SRCS` and friends); adding a *program* means a new
`src/<name>/` directory with its own source list, its own pkg-config package
list and a link rule, alongside the three that are there.

Each program compiles against its own packages: lwm and gummiband are both on
XCB and keep libX11 in the link for Xft alone, and speckeysd is on XCB with no
libX11 at all — it draws nothing, so Xft never came into it. Which set a file
gets is decided by the directory it's in, via the pattern-specific `PKG_CFLAGS` assignments in the
`Makefile`. gummiband borrows lwm's logging by including `"lwm/log.h"` — the
`-Isrc` on every compile is what makes that work — and links `build/lwm/log.o`
directly. Don't add `-Isrc/lwm` to make that shorter: it would put lwm's
`strings.h` ahead of the system one, which libc's `string.h` includes.

## Tests

```sh
./bin/lwm -test    # runs testing::RunAll() and exits; 0 = pass
```

Tests use the in-tree framework in `test.h`/`test.cc` (see
`../docs/refactoring-plan.md`, phase A): `TEST(Suite, Name) { ... }`
self-registers at static-init time, no central list to update. Assertions are
`EXPECT_EQ/NE/TRUE/FALSE/NEAR` (keep running on failure) and
`ASSERT_EQ/NE/TRUE/FALSE/NEAR` (return from the test on failure); both accept
extra context via `<<`, evaluated only on failure. Wrap table-driven case
bodies in `testing::Context ctx(tc.name)` so failures are labelled with the
case name. Tests live next to what they test. Still compiled straight into the
main binary; a separate X11-free tier-0 test target is planned but not built
yet.

There are two kinds of test:

* **Tier-0 tests** (`geometry_test.cc`, `sizelimits_test.cc`,
  `strings_test.cc`, `screenlayout_test.cc`, `placement_test.cc`,
  `framegeometry_test.cc`, `menulayout_test.cc`, `gesture_test.cc`) call pure
  functions. Nothing to set up.
* **Tests that need a server** (`disp_test.cc`, `focus_test.cc`,
  `client_test.cc`, `ewmh_test.cc`, `drag_test.cc`, `screen_test.cc`,
  `error_test.cc`) start
  with a `wmtest::World` on the stack.
  That installs an `xlib::FakeServer` and stands up `Resources`, the atoms, a
  fixed-metric font and `LScr` in the same order `main()` does, then puts it
  all back when it goes out of scope. Drive lwm by pushing events —
  `world.server().PushEvent(ev); ProcessPendingEvents();` — and assert either
  on the resulting state (`world.server().Get(w)->rect`, `LScr::I->Clients()`)
  or on what lwm asked the server to do
  (`world.server().CallsMatching("ConfigureWindow(")`).

```cpp
TEST(Something, DoesTheThing) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  world.server().ClearCalls();
  ...
}
```

`FakeServer` records mutating calls only, one readable line each
(`ConfigureWindow(0x102) x=10 y=20 width=100 height=50`); queries are answers
rather than effects, so they'd only be noise. It models the parts of the
protocol lwm relies on and no more — if a test starts depending on X error
semantics, it has stopped testing lwm.

Two clocks are injectable for testing: `focus::NowMillis` (so the A→B→C focus
race can be provoked without waiting) and `xfont::InitForTest` (so title bar
geometry doesn't need a font on a display).

## Functional smoke test

```sh
./smoke_test.sh [path-to-lwm-binary]   # defaults to ./bin/lwm
```

Runs lwm under a headless `Xvfb` and drives an `xterm` with `xdotool` to
check the things `./bin/lwm -test` can't: framing, `EvConfigureRequest`
move/resize, focus-on-activate, `WM_DELETE_WINDOW` close, and that lwm logs
no `E `-level lines and is still alive at the end. Picks a random display
number so it's safe to run alongside a real X session. Not exhaustive —
it's a regression tripwire for the refactor in `../docs/refactoring-plan.md`,
not a replacement for manual Xephyr testing of new behaviour.

## UI tests

```sh
./ui_test.sh [path-to-lwm-binary]   # defaults to ./bin/lwm; also `make ui`
```

The mouse gestures, driven with `xdotool` under the same headless `Xvfb`
arrangement as the smoke test: Windows-key drags to move and resize, double
clicks to expand a window up to its neighbours or its monitor, clicks to raise
or hide it, the Super+Control click that turns its furniture on and off,
Super+arrow to move the input focus between windows, Super+Shift+arrow to
move a window between monitor edges, and the arrows still working when the
current window is one which doesn't take the input focus.
These need a real server - the passive grabs, the modifier bits in the
`ButtonPress` and the pointer grab that keeps a drag alive are all things
`FakeServer` doesn't model - so `drag_test.cc` covers the same gestures at
the event level and this covers them at the pointer level.

The focus-navigation section starts with the pointer parked on the root
window, off both test windows. Focus follows the mouse by default, and a
pointer over a window keeps producing enter events whose focus changes lwm
defers on the timer in `focus.h` — under Xvfb that deferral has been seen to
take seconds, which lands in the middle of whatever check is running.
Keyboard-driven focus has no such delay, so with the pointer out of the way
the section is deterministic. Don't reintroduce a pointer-based way of
deciding which window starts with the focus: use the fact that lwm focuses a
window as it maps it.

Each Super+arrow then raises the window it focused and lands the pointer on
it, which doesn't disturb that: the enter event names the window which already
has the focus, and it is in front by then, so nothing else can claim it.

The window-moving section is the exception to the pointer-parking rule above:
one check deliberately puts the pointer on the window before moving it, since
carrying the pointer along is half of what that gesture does. It fakes a
two-monitor desktop through the debug CLI's `xrandr` command — Xvfb has one screen and no RandR outputs to reconfigure —
and puts it back to one screen afterwards. That's also what makes it the only
place the shrink-onto-a-smaller-monitor behaviour can be seen end to end, both
from the keyboard and from a drag.

The decorations section is the one that most needs a real server: the client
window is reparented out of its frame and back in again, and what that costs
(an unmap, a restack and the input focus) is exactly what `FakeServer` models
only as far as lwm asked for it.

One section covers a window which doesn't take the input focus (an `xclock`,
started by `start_no_input_client`). It's here rather than in `focus_test.cc`
because the thing that used to break is invisible to `FakeServer`: it hands a
`KeyPress` to `HandleKeyPress` whatever the input focus is, whereas a real
server discards key events, and refuses to activate any passive grab, when the
focus is `None`. The unit test pins where the focus goes; this pins what the
keyboard then does. `xdotool getwindowfocus -f` prints 0 for a focus of
`None`, which is what `x_focus_is_set` looks at.

The last section covers undecorated windows: the gestures and
`_NET_WM_MOVERESIZE` on a client that draws its own decorations, which are
the only two ways such a window can be moved at all (see "Undecorated
windows" in `concepts.md`). It compiles `csdclient.cc` into its temp dir to
play the part of that client, and skips the section with a failed check if
there's no compiler to hand.

## Strut test

```sh
./strut_test.sh [path-to-lwm-binary]   # defaults to ./bin/lwm; also `make strut`
```

Struts (`_NET_WM_STRUT`), from the direction that isn't covered elsewhere: a
dock window that is *already mapped when lwm starts*, so the strut arrives
through the start-up window-tree scan rather than as a `PropertyNotify` on a
window lwm already manages. `smoke_test.sh` covers the latter. The test
checks the three things a strut should reach — `_NET_WORKAREA` on the root,
`LScr::VisibleAreas(true)` (read back through the debug CLI's `xrandr ?`),
and the position `AutoPlacer` gives the next window.

A fourth check goes the other way: with the dock's strut in place, an
undecorated window mapped at exactly the size of the work area (a borderless
full-screen game, played by `csdclient.cc`) has to be grown to cover the whole
screen, dock and all. See "Borderless full screen" in `concepts.md`.

It uses a deep (200 pixel) strut on purpose: the auto-placement cascade
starts 100 pixels in, so a shallower strut would leave the placement check
unable to tell an honoured strut from an ignored one.

This whole path was suspected during a bug where lwm appeared to respect
gummiband's strut only when lwm started first. The real fault was in
gummiband, which interned `_NET_WM_STRUT` with `only_if_exists=True` and so
got `None` — and silently set no strut at all — whenever it beat the window
manager to a fresh X server. lwm's side was sound, and this test is here to
keep it that way.

Two things worth knowing before adding a check:

* Use `xlogo`, not `xterm`. `xlogo` sets no size hints, so it ends up exactly
  the size it was asked for; `xterm` rounds every resize down to a whole
  character cell and turns every expected geometry into an inequality.
* Don't use `xdotool windowmove --sync`. It waits for the window to move, and
  asking for the position it is already in makes lwm quite correctly do
  nothing, so `--sync` waits for ever. `place()` polls the frame geometry
  instead.

## gummiband tests

```sh
./gummiband_test.sh [path-to-gummiband-binary]   # defaults to ./bin/gummiband; also `make gummi`
```

gummiband's only tests. It has no unit tests and nothing to hang one on — a
single translation unit whose every interesting behaviour is a request to an
X server — so this runs the real binary under `Xvfb` with a generated
`.gummiband` in a temp directory, and checks the panel's geometry, its EWMH
properties, the pixels it paints, what its items do when clicked, the
drop-down menu's position, highlighting and `<item>` substitution, the
`updatesecs` repaint, the colours and tooltip an item's own output asks for,
the SIGHUP restart that reloads the config, and the step aside onto another
monitor when something goes full screen.

No window manager runs: gummiband maps itself and nothing here needs framing,
so leaving lwm out keeps the test independent of it. `strut_test.sh` covers
the lwm-and-dock direction.

It is also the regression test for the XCB port, which is why the checks
insist on looking at painted pixels and property *values* rather than at
whether a call succeeded. Nothing XCB gets wrong here fails to compile:
argument order (`xcb_copy_area` takes both positions before the size),
value-list ordering, 32-bit property data (`_NET_WM_STRUT` written as an
array of `long` comes back as `0,0,0,0`), and the missing flush that leaves
the panel one repaint behind. Each of those was reintroduced deliberately to
confirm a check goes red for it.

Four things worth knowing before adding a check:

* gummiband reads `.gummiband` from the *current directory*, and `~/.Xresources`
  when no `RESOURCE_MANAGER` is set, so the test runs it with both its working
  directory and `HOME` pointed at the temp dir. Without that, the developer's
  own font resource changes the panel's height and half the assertions with it.
* Pass it an *absolute* path. SIGHUP makes gummiband `execvp(argv[0])` itself,
  which a relative path can't survive a changed working directory.
* Its windows have no `WM_NAME`, so `xdotool search --name` can't find them.
  Find them by `_NET_WM_WINDOW_TYPE` instead — `DOCK` for the panel, `MENU`
  for the drop-down.
* Nothing asserts the panel's height, which depends on the font that happens
  to be installed. It's read once and everything else — the strut depth, the
  drop-down's height, where its items are — is expressed in terms of it.
* `Xvfb` has one screen and no way to fake a second monitor, so the
  two-monitor checks use `xrandr --setmonitor` to carve that one screen up by
  hand. That's a RandR 1.5 feature, and it's why gummiband reads the RandR
  monitor list rather than the CRTCs — asking the question the way `xrandr
  --listmonitors` does is what makes the multi-monitor behaviour testable at
  all. Setting a monitor list fires no RandR event at all, so those checks are
  also the test of gummiband's idle backstop: nothing sends a SIGHUP, and the
  panel is expected to notice on its own within a few seconds.
* The *event* path is tested separately, by resizing the screen — the checks
  that the panel follows the display shrinking and growing. Getting an
  interesting event out of `Xvfb` takes two steps, because the case that broke
  (see `concepts.md`) is a notification whose `config_timestamp` repeats one
  already seen: `xrandr --newmode`/`--addmode` changes the output's mode list
  and advances the stamp, and switching the output to that mode then changes
  the layout without advancing it. Those checks have a deliberately short
  deadline — about two seconds — because the idle backstop would otherwise
  cover for a broken event path five seconds later and the check would pass
  anyway.
* What stands in for a game is `xclock -geometry`, which honours the geometry
  to the pixel. Mind the *border*: a window with a border width of 1 covers a
  monitor when its own size is two pixels short of it, which is enough to turn
  a check meant to be negative green.

## speckeysd tests

```sh
./speckeysd_test.sh [path-to-speckeysd-binary]   # defaults to ./bin/speckeysd; also `make speckeys`
```

speckeysd's only tests, and its regression test for the XCB port, in the same
shape as gummiband's: the real binary under `Xvfb`, a generated keys file in a
temp directory, key presses driven with `xdotool`, and each hot key's command
touching a file so that "did it fire?" is a file test. They cover the hot key
matching (modifiers, the shifted keysym, Super, the lock-key combinations),
that the grab is on the root and reaches the second screen, that commands are
reaped and don't inherit the X connection, the errors it reports for a bad
keys file and for a key another client already holds, the `MappingNotify`
refresh, and the SIGHUP reload.

The `MappingNotify` checks come in two halves, because the daemon has two
separate things to do when the keyboard map changes: re-read the map, or it
matches a press against a stale keysym, and move its grabs, or they're left on
the old keycodes. The first half's check passes for either reason, so the
re-grab has checks of its own - F5's keysym is moved to a spare keycode and
the hot key has to follow it there and back.

The last section is the retry, and it runs last because it works by killing
the first instance to free the key the second one wanted. A grab on the root
is exclusive, so starting second means starting without your keys, and the
server never says when the holder lets go; speckeysd re-issues the failed
grabs every five seconds. Both halves are checked - the "at last" message and
the key actually firing - because the message alone would pass on a daemon
that logged its way through a retry that grabbed nothing.

Every check was confirmed to go red for the mistake it's there to catch, by
making that mistake on purpose: swapping `xcb_grab_key`'s keycode and
modifiers, grabbing on the first root only, looking the keysym up at the wrong
column, dropping `xcb_refresh_keyboard_mapping`, sending the grabs without
flushing, making `retry_busy_grabs` a no-op, reporting a refused grab once per
request instead of once per hot key (which prints it sixteen times: eight lock
combinations on each of the two screens), and skipping
`regrab_after_mapping_change`.

One check is deliberately weaker than that standard: "an unrelated remap
leaves the hot keys working" guards the re-grab pass against breaking keys it
touches unnecessarily, but nothing in the suite goes red if the pass skips
keys whose keycode didn't change. That skip is avoided on the evidence in
`regrab_after_mapping_change`'s comment rather than on a check here.

Four things worth knowing before adding a check:

* Don't use Control-Alt with a function key. The default keyboard map makes
  Ctrl+Alt+F1..F12 the VT switch keys, and the server swallows them before any
  client sees them, grab or no grab. Ctrl+Alt with a letter is fine.
* The display has two screens on purpose (`:N.0` and `:N.1`, separate roots,
  not Xinerama), because grabbing on screen 0's root *n* times instead of on
  each screen's root is the easy XCB mistake to make here.
* Which screen a key press goes to is decided by the pointer only while the
  input focus is `PointerRoot`. Anything that focuses a window has to put the
  focus back — `xdotool windowfocus 1` — or the second-screen check passes
  whatever the daemon does.
* Pass it an *absolute* path, for the same reason gummiband's test does:
  SIGHUP makes speckeysd `execvp(argv[0])` itself.

## Running it safely

**Never test lwm in the X session you're editing in.** Use Xephyr:

```sh
Xephyr :2 -screen 2000x1400 &
DISPLAY=:2 xsetroot -solid Grey
DISPLAY=:2 ./bin/lwm -debugcli="xrandr 800x1200+0+0 800x500+800+0;dbg auto" 2>$HOME/stderr
# separate terminal:
tail -f $HOME/stderr
```

Keeping stderr in another terminal matters: the debug CLI reads stdin and writes
stdout, and log spam over the top of it is unusable.

Xresources: `testXresources` is an example config; `xrdb -merge` it into the
Xephyr display to exercise the options. `lwm.man` documents them all. lwm reads
the database once, at start-up, so restart it after an `xrdb`. With no
`RESOURCE_MANAGER` set on the display, `xcb-xrm` falls back to `~/.Xresources`,
which is worth remembering when a test display doesn't behave like an empty
one.

## Debug CLI (`-debugcli`)

Line-based commands on stdin. No editing/history — compose long lines elsewhere
and paste. `-debugcli=cmd1;cmd2` runs commands at start-up.

| Command | Effect |
| --- | --- |
| `help` | list commands |
| `ls` | list active clients (uses `operator<<(ostream&, const Client&)`) |
| `dbg help` | sub-help |
| `dbg 0x123 foo` | enable `LOGD` logging for that window, labelled `foo` |
| `dbg auto` / `dbg noauto` | auto-enable debugging for each new client |
| `dbg off [0x123\|foo]` | disable, by id, by label, or all |
| `xrandr ?` | print visible areas, with and without struts |
| `xrandr 800x1200+0+0 800x500+800+0` | fake a monitor layout without touching cables; grey overlay windows mark the inaccessible areas |
| `xrandr` (no args) | back to full screen |

`LOGD(x)` compiles to a no-op-ish `Log` that only emits when `x` (a `Client*` or
a `Window`) is debug-enabled, and prefixes the label. Use it freely in new code;
the `xlib::` wrappers already log every X call this way.

## Other tools

```sh
xwininfo -root -tree     # the window tree, including our frames
xprop -id <id>           # properties on one window
```

* `xdbg.cc` — build with the command in its header comment. Shows live mouse
  coordinates and the last configure request; hold shift to measure a drag and
  get start/end/distance on stdout. The right tool for "is this window one pixel
  off?".
* `ruler.cc` — grid on the root window for eyeballing placement.
* `setvisname.cc` + `force_title.sh` — override a window's displayed title.

## Conventions

* Formatting is clang-format Chromium-ish: 2-space indent, 80 columns,
  `lowerCamelCase` free functions, `UpperCamelCase` methods/classes,
  `trailing_underscore_` privates.
* Comments here explain *why*, often citing the specific misbehaving
  application (Nautilus, Chrome, Java/Terminator, Rhythmbox, ImageMagick
  `display`). Those are regression notes — don't delete them, and re-test with
  the named app if you change that code.
* All X calls go through `namespace xlib` (`xlib.h`), which logs and composes,
  and then through `xlib::Server` (`server.h`), which is one method per
  request. A new request needs a `Server` method, an implementation in
  `realserver.cc` and `fakeserver.cc`, and a wrapper in `xlib.{h,cc}`. Only
  those files call `xcb_*`.
* `../BUGS` is the TODO list (crashes → breakages → cosmetics → features →
  cleanups). `../ChangeLog` is history. `../PROMPTLOG` logs prompts used when
  working on lwm with an LLM.
