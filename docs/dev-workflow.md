# Dev workflow

## Build

```sh
make -j20          # incremental; the Makefile is hand-written and checked in
```

Flags live in the `Makefile`: `-std=c++17 -g3 -O0 -DSHAPE -Wall -Werror -Wextra
-Wpedantic -Wno-sign-compare`. **`-Werror` is on**, so warnings break the build.

Adding a source file means adding it to `SRCS` in the `Makefile`.

## Tests

```sh
./lwm -test        # runs testing::RunAll() and exits; 0 = pass
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
  `client_test.cc`, `ewmh_test.cc`, `drag_test.cc`) start with a
  `wmtest::World` on the stack.
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
./smoke_test.sh [path-to-lwm-binary]   # defaults to ./lwm
```

Runs lwm under a headless `Xvfb` and drives an `xterm` with `xdotool` to
check the things `./lwm -test` can't: framing, `EvConfigureRequest`
move/resize, focus-on-activate, `WM_DELETE_WINDOW` close, and that lwm logs
no `E `-level lines and is still alive at the end. Picks a random display
number so it's safe to run alongside a real X session. Not exhaustive —
it's a regression tripwire for the refactor in `../docs/refactoring-plan.md`,
not a replacement for manual Xephyr testing of new behaviour.

## UI tests

```sh
./ui_test.sh [path-to-lwm-binary]   # defaults to ./lwm; also `make ui`
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
./strut_test.sh [path-to-lwm-binary]   # defaults to ./lwm; also `make strut`
```

Struts (`_NET_WM_STRUT`), from the direction that isn't covered elsewhere: a
dock window that is *already mapped when lwm starts*, so the strut arrives
through the start-up window-tree scan rather than as a `PropertyNotify` on a
window lwm already manages. `smoke_test.sh` covers the latter. The test
checks the three things a strut should reach — `_NET_WORKAREA` on the root,
`LScr::VisibleAreas(true)` (read back through the debug CLI's `xrandr ?`),
and the position `AutoPlacer` gives the next window.

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

## Running it safely

**Never test lwm in the X session you're editing in.** Use Xephyr:

```sh
Xephyr :2 -screen 2000x1400 &
DISPLAY=:2 xsetroot -solid Grey
DISPLAY=:2 ./lwm -debugcli="xrandr 800x1200+0+0 800x500+800+0;dbg auto" 2>$HOME/stderr
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
