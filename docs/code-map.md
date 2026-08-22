# Code map

Build list lives in `Imakefile` (`SRCS`), which generates `Makefile`. This
page tracks progress against `refactoring-plan.md`: phases A through D are
done — tier-0 layer extracted and tested with no X11 dependency, `lwm.h` is
split into one header per `.cc`, and the misplaced classes
(`Focuser`/`Hider`/`DragHandler`) now live in their own files. `mouse.cc`/
`mouse.h` no longer exist: `Hider` moved to `hider.{h,cc}`, and
`getMousePosition()`/`MousePos` moved to `xlib.{h,cc}` (it's an X query, per
the plan's "wrong place" table) — between the two, nothing was left in
`mouse.cc`. Phase E (completing and inverting the `xlib` shim into a
`Server`/`FakeServer` interface, then the group-2 tests that need it) is not
started.

## Tier 0 — pure, no X11 headers, unit-testable with no fakes

Each has a matching `*_test.cc`.

| File | Contents |
| --- | --- |
| `geometry.{h,cc}` | `Point`/`Area`/`Rect`, `Rect::Parse`. |
| `edge.{h,cc}` | The `Edge` enum and `isLeftEdge`/`isRightEdge`/`isTopEdge`/`isBottomEdge`. |
| `sizelimits.{h,cc}` | `DimensionLimiter` (min/max/base/increment size rules). |
| `strings.{h,cc}` | `Split`, `TruncateUtf8` (the UTF-8-aware menu-name truncation). |
| `strut.h` | `EWMHStrut` and `MaxStrut` (header-only, no `.cc`). |
| `screenlayout.{h,cc}` | `MapToNewAreas`, `PrimaryArea`, `findBestScreenFor`, `makeVisible`, `areasMinusStruts`. The xrandr re-layout maths, formerly in `screen.cc`/`disp.cc`. |
| `placement.{h,cc}` | `AutoPlacer` — auto-placement cascade for new windows (formerly `NextAutoPosition` in `manage.cc`, function-local statics turned into fields owned by `LScr::auto_placer_`). |
| `framegeometry.{h,cc}` | `FrameStyle` value type (border width / top border width / text height) plus `CloseBounds`, `TitleBarBounds`, `EdgeBoundsFor`, `ContentFromFrameRect`, `FrameFromContentRect`. `Client`'s same-named methods in `client.cc` are now thin wrappers around these, built from `CurrentFrameStyle()`. |
| `menulayout.{h,cc}` | `MenuStyle` value type (text height) plus the unhide-menu item/icon/margin arithmetic. `mouse.cc`'s `menu*()` free functions wrap these via `CurrentMenuStyle()`. |

## Tier 1/2 — still needs X11, one header per `.cc` (phases C and D)

Each `.cc` below has a matching `.h` of the same name declaring what it
provides. Most `.cc` files include only the specific headers they need
rather than one umbrella — `dpy` moved from `lwm.h` to `xlib.h` (used almost
everywhere and conceptually belongs with the shim); `ButtonMask` moved to
`xlib.h` too. A handful of files (`client.cc`, `hider.cc`, `focus.cc`,
`drag.cc`, `disp.cc`, `manage.cc`, `ewmh.cc`, `debug.cc`, `error.cc`,
`session.cc`, `shape.cc`) still include `lwm.h` for `main()`'s own globals
(`g_font*`, `textHeight`/`textWidth`/`drawString`, the ICCCM atoms,
`argv0`/`forceRestart`, `RunCommand`/`shell`) — those don't have a better
home yet. `resource.cc` and `screen.cc` don't need `lwm.h` at all.

| File | Lines (approx) | Contents |
| --- | ---: | --- |
| `lwm.h` | ~80 | What's left of the god header: `main()`'s own globals/atoms/fn decls, plus `#include`s of every other header as a convenience umbrella (mainly for `lwm.cc` and the files listed above). |
| `lwm.cc` | ~370 | `main()`: arg parsing, atoms, fonts, signal handlers, the `select()` event loop. XRandR notification handling, `RunCommand`/`shell`, the Xft text helpers (`textHeight`/`textWidth`/`drawString`). |
| `disp.cc` / `disp.h` | ~595 | X event dispatch (`DispatchXEvent`) and one `Ev<EventName>` handler per event type — including `EvConfigureRequest`, the Nautilus-offset code; don't touch without re-testing against Nautilus. `disp.h` declares the `DragHandler` interface and `EWMHDirection`; `current_dragger`/`startDragging`/`stopDragging` (drives whichever `DragHandler` is active) stay here too. |
| `drag.cc` / `drag.h` | ~335 | The 8 `DragHandler` subclasses (`MenuDragger`, `WindowMover`/`Resizer`/`Closer`/`Hider`/`Lowerer`, `ShellRunner`, plus their `WindowDragger`/`WindowClicker` base classes) and `getDragHandlerForEvent`, the factory that picks one from a `ButtonPress`. Only the factory is exported; the subclasses are an anonymous-namespace implementation detail. Moved out of `disp.cc`. |
| `xdebugprint.cc` / `xdebugprint.h` | ~90 | `operator<<` for raw X11 event structs (`XConfigureRequestEvent`, `XConfigureEvent`, `XFocusChangeEvent`) plus `diff` (an `EWMHWindowState` before/after formatter), used only by `LOGD`/`LOGI` calls in `disp.cc`. Moved out of `disp.cc`; `EWMHWindowState`'s own `operator<<` moved to `ewmh.cc` instead, next to the type. |
| `client.cc` / `client.h` | ~552 | `Client` methods (geometry, border drawing, raise/lower/close/state, full-screen). Also the resize-feedback popup. `Focuser` moved out to `focus.{h,cc}`. |
| `focus.cc` / `focus.h` | ~214 | `Focuser`: focus history, the focus-follows-mouse race-avoidance timerfd (see the long comment in `focus.h`), `ReallyFocusClient`'s three paths (normal/Java/give-up — don't "simplify" this, see `concepts.md`). Moved out of `client.cc`. |
| `hider.cc` / `hider.h` | ~320 | `Hider`: hide/unhide, the unhide menu (layout, paint, hit-testing, the red highlight box), built on `menulayout.h`. `menuItemHeight()` is the one function here also used outside `Hider` (by `xlib.cc`'s icon sizing), so it's the one non-anonymous-namespace free function. Moved out of `mouse.cc`, which no longer exists. |
| `screen.cc` / `screen.h` | ~345 | `LScr`: window/client registry, GCs and colours, EWMH root properties, window-tree scan at start-up, `SetVisibleAreas` (calls into `screenlayout.cc`). Owns `Hider`/`Focuser` by value, so `screen.h` includes `hider.h`/`focus.h`. |
| `xlib.cc` / `xlib.h` | ~682 | `namespace xlib`: logging wrappers around Xlib calls, `CreateNamedWindow`, `WindowTree`, `ImageIcon` (icon scaling, compositing, refcounted pixmap cache). Also now home to `dpy`, `ButtonMask`, and `MousePos`/`getMousePosition()` (it's an X query — moved out of `mouse.cc`). |
| `ewmh.cc` / `ewmh.h` | ~596 | EWMH atom table and every `ewmh_*` getter/setter; `fix_stack()` stacking policy; `ewmh_set_client_list()`. `ewmh.h` also carries `EWMHWindowType`/`EWMHWindowState` (used by `Client`); `ewmh.cc` has the `XSizeHints`/`EWMHWindowState` debug `operator<<`s, next to the types they print. |
| `manage.cc` / `manage.h` | ~325 | `manage()` — the big "adopt this window" routine (hints, protocols, icons, framing decision, reparent, initial placement via `LScr::NextAutoPosition`). Also `withdraw()`, `Terminate()`, `getProperty`, name/transient/state getters. |
| `debug.cc` / `debug.h` | 328 | `DebugCLI` — stdin command interpreter (`ls`, `dbg`, `xrandr`, `help`) and the fake-xrandr "dead zone" overlay windows. |
| `session.cc` / `session.h` | 212 | X session management (ICE/SM) — save-yourself, restart properties. Self-contained. |
| `resource.cc` / `resource.h` | 198 | `Resources`: every Xresource name, class and default value in one constructor. `borderWidth()`/`topBorderWidth()`. |
| `shape.cc` / `shape.h` | 93 | X Shape extension support, all behind `#ifdef SHAPE`. |
| `error.cc` / `error.h` | 88 | X error handler + backtrace, `panic`, `ScopedIgnoreBadWindow`/`ScopedIgnoreBadMatch`. |
| `cursor.cc` / `cursor.h` | 60 | `CursorMap`: one cursor per `Edge`, recoloured. Fully self-contained. |
| `log.cc` / `log.h` | 44 | `Log` implementation for the `LOGI/W/E/F/D` macros. X11-free; `LOGD` needs `debug.h` wherever it's used, for `DebugCLI::DebugEnabled`/`NameFor`. |

## Test framework and tests

| File | Contents |
| --- | --- |
| `test.h`/`test.cc` | The self-registering framework: `TEST`, `EXPECT_*`/`ASSERT_*`, `testing::RunAll`. See `../docs/dev-workflow.md`. |
| `tests.cc` | Just the `RunAllTests()` glue now; all suites live in `*_test.cc`. |
| `geometry_test.cc`, `sizelimits_test.cc`, `strings_test.cc`, `screenlayout_test.cc`, `placement_test.cc`, `framegeometry_test.cc`, `menulayout_test.cc` | One per tier-0 file above. |

Still compiled straight into the main binary and run via `./lwm -test`; the
separate X11-free tier-0 test target mentioned in the plan doesn't exist yet.

## Standalone tools (not linked into lwm)

Each carries its own compile command in a comment at the top of the file.

* `xdbg.cc` — window showing live mouse coords and last configure request;
  shift-drag measures a distance. The tool for checking window positioning.
* `ruler.cc` — paints a grid on the root window, for eyeballing placement.
* `setvisname.cc` — sets `_NET_WM_VISIBLE_NAME` on a window id.
* `force_title.sh` — zenity prompt + `setvisname`; the default action for
  alt-button1 on a title bar.
* `smoke_test.sh` — Xvfb + xdotool functional smoke test; see
  `../docs/dev-workflow.md`.

## Where to look for...

* **A new X event needs handling** → `disp.cc`, add `Ev<Name>` and an `EV()`
  line in `DispatchXEvent`.
* **A new mouse gesture** → `getDragHandlerForEvent` in `drag.cc`, plus a
  `DragHandler` subclass.
* **A new config option** → `Resources::SR`/`IR` enum in `lwm.h`, default in
  `resource.cc`, and document it in `lwm.man` + `testXresources`.
* **Window furniture appearance/geometry** → `framegeometry.{h,cc}`
  (`FrameStyle`, bounds calculations) for the pure maths; `Client::DrawBorder`
  in `client.cc` for the actual painting.
* **Unhide menu geometry** → `menulayout.{h,cc}` for the pure maths;
  `Hider::Paint`/`OpenMenu` in `mouse.cc` for painting and event handling.
* **Which windows get frames** → `ewmh_hasframe` (`ewmh.cc`) and the motif
  hint override in `manage()` (`manage.cc`).
* **Multi-monitor behaviour** → `LScr::SetVisibleAreas` in `screen.cc`, which
  delegates the actual maths to `screenlayout.{h,cc}`.
* **Auto-placement of new windows** → `placement.{h,cc}` (`AutoPlacer`),
  invoked via `LScr::NextAutoPosition` from `manage()`.
