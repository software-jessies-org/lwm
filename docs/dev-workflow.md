# Dev workflow

## Build

```sh
xmkmf && make      # Imakefile -> Makefile; the Makefile is gitignored
make -j20          # incremental
```

FreeBSD: `ln -s Makefile.freebsd Makefile; make` (note `INSTALL` misspells it
as `Makefile.FreeBSD`, and it still says `-std=c++14`, which no longer builds —
`manage.cc` uses `std::optional`). `no_xmkmf_makefile` is a fallback sample for
systems without imake, and is likewise stale. Flags live in `Imakefile`:
`-std=c++17 -g3 -O0 -DSHAPE -Wall -Werror -Wextra -Wpedantic -Wno-sign-compare`.
**`-Werror` is on**, so warnings break the build.

Adding a source file means adding it to `SRCS` in `Imakefile` (and re-running
`xmkmf`), plus `no_xmkmf_makefile` and `Makefile.freebsd` if you care.

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
case name. Tests live next to what they test: `geometry_test.cc`
(`Rect::Parse`, `DimensionLimiter`), `strings_test.cc` (`Split`), `tests.cc`
(the `MapToNewAreas` table — still here because `MapToNewAreas` itself hasn't
moved out of `screen.cc` yet). Still compiled straight into the main binary;
a separate X11-free tier-0 test target is planned but not built yet.

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
* All Xlib calls should go through `namespace xlib` where a wrapper exists; the
  long-term plan in `../BUGS` is to make that shim complete enough to fake for
  tests.
* `../BUGS` is the TODO list (crashes → breakages → cosmetics → features →
  cleanups). `../ChangeLog` is history. `../PROMPTLOG` logs prompts used when
  working on lwm with an LLM.
