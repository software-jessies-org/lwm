# Code map

## Sources built into `lwm`

Build list lives in `Imakefile` (`SRCS`), which generates `Makefile`.

| File | Lines | Contents |
| --- | ---: | --- |
| `lwm.h` | 864 | The header. `Client`, `LScr`, `Focuser`, `Hider`, `CursorMap`, `Resources`, `DebugCLI`, `DragHandler`, plus all the cross-file `extern`s grouped by owning `.cc`. |
| `lwm.cc` | 386 | `main()`: arg parsing, atoms, fonts, signal handlers, the `select()` event loop. Also XRandR notification handling and `RunCommand`/`shell`, plus the Xft text helpers (`textWidth`, `drawString`). |
| `disp.cc` | 1067 | X event dispatch (`DispatchXEvent`) and one `Ev<EventName>` handler per event type. Also all the `DragHandler` subclasses: `WindowMover`, `WindowResizer`, `WindowCloser`/`Hider`/`Lowerer`, `MenuDragger`, `ShellRunner`. |
| `client.cc` | 804 | `Client` methods (geometry, border drawing, raise/lower/close/state, full-screen) **and** all of `Focuser`. Also the resize-feedback popup (`Client_SizeFeedback`, `size_expose`). |
| `screen.cc` | 650 | `LScr`: window/client registry, GCs and colours, EWMH root properties, window-tree scan at start-up. Bottom half is the XRandR re-layout maths — `MapToNewAreas` and helpers, exercised by `tests.cc`. |
| `xlib.cc` | 668 | `namespace xlib`: logging wrappers around Xlib calls, `CreateNamedWindow`, `WindowTree`, and `ImageIcon` (icon scaling, background compositing, pixmap cache with refcounts). |
| `ewmh.cc` | 584 | EWMH atom table and every `ewmh_*` getter/setter; `fix_stack()` stacking policy; `ewmh_set_client_list()`. |
| `manage.cc` | 368 | `manage()` — the big "adopt this window" routine (hints, protocols, icons, framing decision, reparent, initial placement via `NextAutoPosition`). Also `withdraw()`, `Terminate()`, `getProperty`, name/transient/state getters. |
| `mouse.cc` | 347 | All of `Hider`: hide/unhide, the unhide menu (layout, paint, hit-testing, the red highlight box). Also `getMousePosition()`. |
| `debug.cc` | 328 | `DebugCLI` — stdin command interpreter (`ls`, `dbg`, `xrandr`, `help`) and the fake-xrandr "dead zone" overlay windows. |
| `session.cc` | 212 | X session management (ICE/SM) — save-yourself, restart properties. Self-contained. |
| `geometry.cc` | 198 | `Point`/`Area`/`Rect` methods, `Rect::Parse`, edge predicates, `DimensionLimiter` (min/max/base/increment size rules). |
| `resource.cc` | 198 | `Resources`: every Xresource name, class and default value in one constructor. `borderWidth()`/`topBorderWidth()`. |
| `tests.cc` | 131 | The whole unit test suite (`./lwm -test`). Currently only table-driven cases for `MapToNewAreas`. |
| `shape.cc` | 93 | X Shape extension support, all behind `#ifdef SHAPE`. |
| `error.cc` | 88 | X error handler + backtrace, `panic`, `ScopedIgnoreBadWindow`/`ScopedIgnoreBadMatch`. |
| `cursor.cc` | 60 | `CursorMap`: one cursor per `Edge`, recoloured. |
| `log.cc` | 44 | `Log` implementation for the `LOGI/W/E/F/D` macros in `log.h`. |

Headers: `geometry.h` (pure geometry, no Xlib), `xlib.h` (the shim + all X11
includes), `ewmh.h` (the `EWMHAtom` enum — order matters, it builds
`_NET_SUPPORTED`), `log.h`.

## Standalone tools (not linked into lwm)

Each carries its own compile command in a comment at the top of the file.

* `xdbg.cc` — window showing live mouse coords and last configure request;
  shift-drag measures a distance. The tool for checking window positioning.
* `ruler.cc` — paints a grid on the root window, for eyeballing placement.
* `setvisname.cc` — sets `_NET_WM_VISIBLE_NAME` on a window id.
* `force_title.sh` — zenity prompt + `setvisname`; the default action for
  alt-button1 on a title bar.

## Where to look for...

* **A new X event needs handling** → `disp.cc`, add `Ev<Name>` and an `EV()`
  line in `DispatchXEvent`.
* **A new mouse gesture** → `getDragHandlerForEvent` in `disp.cc:327`, plus a
  `DragHandler` subclass.
* **A new config option** → `Resources::SR`/`IR` enum in `lwm.h:693`, default in
  `resource.cc:32`, and document it in `lwm.man` + `testXresources`.
* **Window furniture appearance** → `Client::DrawBorder` (`client.cc:224`) and
  the `closeBounds`/`titleBarBounds`/`titleBarHeight` helpers above it.
* **Which windows get frames** → `ewmh_hasframe` (`ewmh.cc:229`) and the motif
  hint override in `manage()` (`manage.cc:105`).
* **Multi-monitor behaviour** → `LScr::SetVisibleAreas` / `MapToNewAreas` in
  `screen.cc`.
