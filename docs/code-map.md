# Code map

Build list lives in `SRCS` in the hand-written `Makefile` (imake is gone; see
`xcb-migration-plan.md`). This page tracks progress against
`refactoring-plan.md`: phases A through E are done. Tier 0 is extracted and
tested with no X11 dependency, `lwm.h` is split into one header per `.cc`, the
misplaced classes (`Focuser`/`Hider`/`DragHandler`) live in their own files,
and the `xlib` shim has been inverted into an interface — `xlib::Server`
(`server.h`) with `RealServer` (`realserver.cc`) and `FakeServer`
(`fakeserver.cc`) behind it — so the event handlers, `Focuser`, the client
lifecycle and the EWMH stacking rules are now unit-tested with no display.
`mouse.cc`/`mouse.h` no longer exist: `Hider` moved to `hider.{h,cc}`, and
`getMousePosition()`/`MousePos` moved to `xlib.{h,cc}` (it's an X query, per
the plan's "wrong place" table) — between the two, nothing was left in
`mouse.cc`. Only phase F (putting `Client`'s public data members behind
accessors) remains.

lwm now speaks **XCB**, not Xlib. The migration in `xcb-migration-plan.md` is
done except its optional phase 4 (replacing Xft with Pango + cairo-xcb, which
is a font-rendering decision rather than an XCB one). libX11 remains linked
solely to serve Xft, quarantined in `xbridge.cc` and `xfont.cc` — the only two
files that may include Xlib headers, enforced by `make check-x11-boundary`.

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
| `gesture.{h,cc}` | The pure half of the Windows-key mouse gestures: `NineGridEdgeAt` (which cell of a window's 3x3 grid a point is in), `ExpandRect` (how far an edge may grow before it meets another window or the monitor edge) and `DoubleClickTracker` (X has no double clicks; every client decides for itself). |

## Tier 1/2 — still needs X11, one header per `.cc` (phases C and D)

Each `.cc` below has a matching `.h` of the same name declaring what it
provides. Most `.cc` files include only the specific headers they need rather
than one umbrella — `conn` (the XCB connection) lives in `xlib.h`, as do the
`Window`/`Atom`/`Cursor`/... typedefs, the ICCCM `NormalState` constants and
`ButtonMask`. A handful of files (`client.cc`, `hider.cc`, `focus.cc`,
`drag.cc`, `disp.cc`, `manage.cc`, `ewmh.cc`, `debug.cc`, `error.cc`,
`session.cc`, `shape.cc`) still include `lwm.h` for `main()`'s own globals
(the ICCCM atoms, `argv0`/`forceRestart`, `RunCommand`/`shell`) — those don't
have a better home yet. `resource.cc` and `screen.cc` don't need `lwm.h` at
all, and neither do `xbridge.cc` or `xfont.cc` (they *can't*: see below).

| File | Lines (approx) | Contents |
| --- | ---: | --- |
| `lwm.h` | ~80 | What's left of the god header: `main()`'s own globals/atoms/fn decls, plus `#include`s of every other header as a convenience umbrella (mainly for `lwm.cc` and the files listed above). |
| `lwm.cc` | ~330 | `main()`: arg parsing, atoms (interned in one batch), signal handlers, the `select()` event loop — which drains with `xcb_poll_for_event` and flushes exactly once per pass, immediately before `select()`. XRandR notification handling (`randrEvent`, deduplicated on `config_timestamp`), `RunCommand`/`shell`. |
| `disp.cc` / `disp.h` | ~600 | X event dispatch (`DispatchXEvent`, `ProcessPendingEvents`) and one `Ev<EventName>` handler per event type. Handlers take a raw `xcb_generic_event_t*` and cast: there is no `XAnyEvent` equivalent, and the window an event concerns is called `window` in some structs and `event` in others. `response_type` must be masked with `0x7f`; bit `0x80` means the event came from `SendEvent`. Response type 0 is an *error*, routed to `HandleXError`. Includes `EvConfigureRequest`, the Nautilus-offset code; don't touch without re-testing against Nautilus. `disp.h` declares the `DragHandler` interface and `EWMHDirection`; `current_dragger`/`startDragging`/`stopDragging` (drives whichever `DragHandler` is active) stay here too. |
| `drag.cc` / `drag.h` | ~470 | The 9 `DragHandler` subclasses (`MenuDragger`, `WindowMover`/`Resizer`/`Expander`/`Closer`/`Hider`/`Lowerer`, `ShellRunner`, plus their `WindowDragger`/`WindowClicker` base classes) and `getDragHandlerForEvent`, the factory that picks one from a `ButtonPress`. Only the factory is exported; the subclasses are an anonymous-namespace implementation detail. Moved out of `disp.cc`. `getSuperDragHandler` is the Windows-key branch of the factory, reached only for a press on the client's own window; the arithmetic it runs on is in `gesture.{h,cc}`. |
| `xdebugprint.cc` / `xdebugprint.h` | ~90 | `operator<<` for raw XCB event structs (`xcb_configure_request_event_t`, `xcb_configure_notify_event_t`, `xcb_focus_in_event_t`) plus `diff` (an `EWMHWindowState` before/after formatter), used only by `LOGD`/`LOGI` calls in `disp.cc`. Moved out of `disp.cc`; `EWMHWindowState`'s own `operator<<` moved to `ewmh.cc` instead, next to the type. |
| `client.cc` / `client.h` | ~552 | `Client` methods (geometry, border drawing, raise/lower/close/state, full-screen). Also the resize-feedback popup. `Focuser` moved out to `focus.{h,cc}`. |
| `focus.cc` / `focus.h` | ~225 | `Focuser`: focus history, the focus-follows-mouse race-avoidance timerfd (see the long comment in `focus.h`), `ReallyFocusClient`'s three paths (normal/Java/give-up — don't "simplify" this, see `concepts.md`). Moved out of `client.cc`. `focus::NowMillis` is the clock it reads, replaceable so the A→B→C race can be tested rather than waited for. |
| `hider.cc` / `hider.h` | ~320 | `Hider`: hide/unhide, the unhide menu (layout, paint, hit-testing, the red highlight box), built on `menulayout.h`. `menuItemHeight()` is the one function here also used outside `Hider` (by `xlib.cc`'s icon sizing), so it's the one non-anonymous-namespace free function. Moved out of `mouse.cc`, which no longer exists. |
| `screen.cc` / `screen.h` | ~345 | `LScr`: window/client registry, GCs and colours, EWMH root properties, window-tree scan at start-up, `SetVisibleAreas` (calls into `screenlayout.cc`). Owns `Hider`/`Focuser` by value, so `screen.h` includes `hider.h`/`focus.h`. |
| `xlib.cc` / `xlib.h` | ~1210 | `namespace xlib`: the public face of the shim, and the only thing above it that anything calls. Every function here logs, composes (`XMoveWindow` is a `ConfigureWindow` with two fields set) and delegates to the installed `Server`; none of them issues an X request itself. Also `CreateNamedWindow`, `WindowTree`, the client-side parser for `#rrggbb` colour specifications, and `ImageIcon` (icon scaling, compositing, refcounted pixmap cache — on plain 32-bit buffers, no `XImage`). Home to `conn`, the resource-ID typedefs, `ButtonMask`, `MousePos`/`getMousePosition()`, `Reply<T>` (RAII for reply memory), and the `ValueList`/`WindowAttrs`/`WindowChanges`/`GCValues` builders. **Use the builders.** XCB takes a bare `uint32_t[]` that must be ordered by increasing mask bit and checks nothing, so a hand-written array is a silent-corruption bug waiting to happen. |
| `server.h` | ~250 | `xlib::Server`: one pure-virtual method per X request, plus `xlib::server` (the installed one) and `SetServer`. The seam sits *below* `xlib.cc`'s composition on purpose, so what a fake records is what would have gone on the wire. |
| `realserver.cc` | ~700 | `RealServer`: the XCB implementation. Nothing here does anything but issue one request (or the smallest group that must travel together, such as the attributes-plus-geometry pair XCB needs where Xlib pretended one request would do). Installed by `OpenDisplay()` unless something got in first. |
| `fakeserver.cc` / `fakeserver.h` | ~700 | `FakeServer`: an in-memory window tree that answers queries and records every mutating call as a readable one-line string. Models the parts of the protocol lwm depends on — reparenting, stacking order, viewability, properties, hints — and nothing else. |
| `xbridge.cc` / `xbridge.h` | ~60 | The last of libX11, quarantined. Opens the connection with `XOpenDisplay`, hands the event queue to XCB (`XSetEventQueueOwner`), exposes the XCB connection, and installs Xlib's error handler so an Xft error can't exit the process. Also `Flush()`: Xlib has *its own* output buffer that `xcb_flush` knows nothing about, so both must be flushed. Deliberately does not include `xlib.h` — see the boundary note below. Disappears entirely if phase 4 ever happens. |
| `xfont.cc` / `xfont.h` | ~110 | All text rendering, via Xft. The other file allowed to include Xlib, and the reason libX11 is still linked: Xft has no XCB port. Its interface (`TextHeight`, `TextAscent`, `TextWidth`, `DrawString`) names no X type more specific than `xcb_window_t`, so replacing the implementation with Pango + cairo-xcb would touch nothing above it. `InitForTest()` substitutes fixed metrics for a real font, which is what lets everything derived from the title bar height be tested without a display. |
| `ewmh.cc` / `ewmh.h` | ~580 | EWMH atom table (interned in one batch) and every `ewmh_*` getter/setter; `fix_stack()` stacking policy; `ewmh_set_client_list()`. `ewmh.h` also carries `EWMHWindowType`/`EWMHWindowState` (used by `Client`); `ewmh.cc` has the `EWMHWindowState` debug `operator<<`, next to the type it prints. Note lwm deliberately does **not** use `xcb-ewmh`: this file's hand-rolled table encodes specific policy (`fix_stack`'s stacking rules, `ewmh_hasframe`'s window-type rules, the recursion guard) that a library swap would quietly rewrite. |
| `manage.cc` / `manage.h` | ~315 | `manage()` — the big "adopt this window" routine (hints, protocols, icons, framing decision, reparent, initial placement via `LScr::NextAutoPosition`). Also `withdraw()`, `Terminate()`, name/transient/state getters. |
| `debug.cc` / `debug.h` | 328 | `DebugCLI` — stdin command interpreter (`ls`, `dbg`, `xrandr`, `help`) and the fake-xrandr "dead zone" overlay windows. |
| `session.cc` / `session.h` | 212 | X session management (ICE/SM) — save-yourself, restart properties. Self-contained. |
| `resource.cc` / `resource.h` | ~195 | `Resources`: every Xresource name and default value in one constructor. Lookup is by name only — `xcb_xrm_resource_get_string` demands a class string with exactly as many components as the name, and fails the whole lookup otherwise, so the class argument is null. `borderWidth()`/`topBorderWidth()`. |
| `shape.cc` / `shape.h` | ~95 | X Shape extension support, all behind `#ifdef SHAPE`. Its event number is allocated by the server at run time, so `shapeEvent()` can't be part of `DispatchXEvent`'s switch. |
| `error.cc` / `error.h` | ~200 | Error reporting + backtrace, `panic`, `ScopedIgnoreBadWindow`/`ScopedIgnoreBadMatch`. Errors arrive on the event queue carrying the sequence number of the request that caused them, so the `ScopedIgnore*` classes suppress a *range of sequence numbers* rather than setting a global flag and hoping. Also the static tables of core error-code and request-opcode names (`libxcb-errors` would supply these but isn't packaged everywhere). |
| `cursor.cc` / `cursor.h` | ~72 | `CursorMap`: one cursor per `Edge`. XCB takes the colours when the cursor is created, so there's no recolouring step; the cursor-font glyph indices live here because XCB ships no equivalent of `<X11/cursorfont.h>`. Fully self-contained. |
| `log.cc` / `log.h` | 44 | `Log` implementation for the `LOGI/W/E/F/D` macros. X11-free; `LOGD` needs `debug.h` wherever it's used, for `DebugCLI::DebugEnabled`/`NameFor`. |

## Test framework and tests

| File | Contents |
| --- | --- |
| `test.h`/`test.cc` | The self-registering framework: `TEST`, `EXPECT_*`/`ASSERT_*`, `testing::RunAll`. See `../docs/dev-workflow.md`. |
| `tests.cc` | Just the `RunAllTests()` glue now; all suites live in `*_test.cc`. |
| `geometry_test.cc`, `sizelimits_test.cc`, `strings_test.cc`, `screenlayout_test.cc`, `placement_test.cc`, `framegeometry_test.cc`, `menulayout_test.cc`, `gesture_test.cc` | One per tier-0 file above. No fakes needed. |
| `wmtest.cc` / `wmtest.h` | `wmtest::World`: stands up a `FakeServer`, `Resources`, the atoms, the fake font and `LScr` in main()'s own order, and puts everything back afterwards. One per test, on the stack. |
| `disp_test.cc`, `focus_test.cc`, `client_test.cc`, `ewmh_test.cc`, `drag_test.cc` | The tests that need a server: `EvConfigureRequest`'s Nautilus offset arithmetic, `Focuser`'s history and its timerfd deferral, the client lifecycle across `LScr`'s registries, `fix_stack` plus the recursion guard, and the Windows-key gestures from the `ButtonPress` down to the window moving. |

Still compiled straight into the main binary and run via `./lwm -test`; the
separate X11-free tier-0 test target mentioned in the plan doesn't exist yet.

## The Xlib boundary

Xlib and lwm's own headers both define `Window`, to different widths, so a
translation unit may include one set or the other but **never both**. Exactly
two files pick Xlib — `xbridge.cc` and `xfont.cc` — and neither includes
`xlib.h`. That is also why `error.h` and `resource.h` include `<xcb/xcb.h>`
and `<xcb/xcb_xrm.h>` directly rather than `xlib.h`: `xfont.cc` needs both of
them. `make check-x11-boundary` fails the build if anything else reaches for
an Xlib header. (`X11/SM` and `X11/ICE` are exempt: session management shares
the include prefix but is a separate library that doesn't link libX11.)

## Standalone tools (not linked into lwm)

These are still on Xlib and always will be; they aren't part of lwm and
aren't in `SRCS`.

Each carries its own compile command in a comment at the top of the file.

* `xdbg.cc` — window showing live mouse coords and last configure request;
  shift-drag measures a distance. The tool for checking window positioning.
* `ruler.cc` — paints a grid on the root window, for eyeballing placement.
* `setvisname.cc` — sets `_NET_WM_VISIBLE_NAME` on a window id.
* `force_title.sh` — zenity prompt + `setvisname`; the default action for
  alt-button1 on a title bar.
* `smoke_test.sh` — Xvfb + xdotool functional smoke test; see
  `../docs/dev-workflow.md`.
* `ui_test.sh` — Xvfb + xdotool UI test for the Windows-key mouse gestures;
  see `../docs/dev-workflow.md`.
* `strut_test.sh` — Xvfb test for a strut set by a dock that was already on
  screen before lwm started; see `../docs/dev-workflow.md`.

## Where to look for...

* **A new X event needs handling** → `disp.cc`, add `Ev<Name>` and an `EV()`
  line in `DispatchXEvent`.
* **A new X request** → add a method to `xlib::Server` (`server.h`), implement
  it in `realserver.cc` and `fakeserver.cc`, and give it a logging wrapper in
  `xlib.{h,cc}` for everything else to call. Nothing outside those three files
  calls `xcb_*` directly.
* **A new mouse gesture** → `getDragHandlerForEvent` in `drag.cc`, plus a
  `DragHandler` subclass. If it acts on the client's own window rather than
  on the frame, it also needs a passive grab: see `Client::GrabSuperButtons`.
* **A new config option** → `Resources::SR`/`IR` enum in `resource.h`, default
  in `resource.cc`, and document it in `lwm.man` + `testXresources`.
* **Window furniture appearance/geometry** → `framegeometry.{h,cc}`
  (`FrameStyle`, bounds calculations) for the pure maths; `Client::DrawBorder`
  in `client.cc` for the actual painting.
* **Unhide menu geometry** → `menulayout.{h,cc}` for the pure maths;
  `Hider::Paint`/`OpenMenu` in `hider.cc` for painting and event handling.
* **Which windows get frames** → `ewmh_hasframe` (`ewmh.cc`) and the motif
  hint override in `manage()` (`manage.cc`).
* **Multi-monitor behaviour** → `LScr::SetVisibleAreas` in `screen.cc`, which
  delegates the actual maths to `screenlayout.{h,cc}`.
* **Auto-placement of new windows** → `placement.{h,cc}` (`AutoPlacer`),
  invoked via `LScr::NextAutoPosition` from `manage()`.
