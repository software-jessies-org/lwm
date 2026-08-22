# Architecture

## Start-up (`main`, `lwm.cc:83`)

1. Parse argv: `-debugcli[=cmds;...]` creates a `DebugCLI`; `-test` runs
   `RunAllTests()` and exits; `-s <id>` is consumed by `session_init`.
2. `XOpenDisplay` → global `dpy`. Only one X screen is supported (use xrandr).
3. `Resources::Init()` reads the Xresource database.
4. `XSetErrorHandler(errorHandler)`; SIGTERM/INT/HUP → `Terminate`.
   `is_initialising` is true here, so an error is fatal — that's how "another
   window manager is already running" is detected.
5. Intern ICCCM atoms; `ewmh_init()` interns the EWMH ones.
6. Open the Xft font and allocate the three title/popup colours.
7. `LScr::I = new LScr(dpy); LScr::I->Init()` — creates GCs, popup and menu
   windows, grabs `SubstructureRedirectMask` on the root (this is the moment we
   become *the* WM), then `ScanWindowTree()` adopts pre-existing windows and
   `InitEWMH()` publishes the root properties.
8. `session_init()`, then `is_initialising = false`.
9. XRandR: select `RRScreenChangeNotifyMask` and take the initial layout via
   `setScreenAreasFromXRandR()`.

## Event loop (`lwm.cc:205`)

A single `select()` over up to four fds:

| fd | Handler |
| --- | --- |
| X connection | drain `XPending` → `DispatchXEvent`, except XRandR events which go to `rrScreenChangeNotify` |
| `Focuser::GetTimerFD()` (timerfd) | `Focuser::TimerFDTriggered()`, then an explicit `XSync` (nothing else flushes here) |
| ICE fd (session manager) | `session_process()` |
| stdin, if `-debugcli` | `DebugCLI::Read()` |

The loop exits only when `forceRestart` is set (SIGHUP), whereupon lwm
`execvp`s itself to reload config.

## Event dispatch (`disp.cc`)

`DispatchXEvent` is a macro-driven switch mapping event type → `Ev<Name>`.
Deliberately ignored: `LeaveNotify`, `CreateNotify`, `GravityNotify`,
`MapNotify`, `MappingNotify`, the Selection events, `NoExpose`. Anything else
unknown is offered to `shapeEvent()` before being logged.

The interesting handlers:

* `EvMapRequest` — client wants to appear. `LScr::GetOrAddClient`, then
  `manage()` on first sight, then map + raise + `NormalState`.
* `EvConfigureRequest` — client wants to move/resize itself. Heavily commented;
  the offset arithmetic exists because a reparented client reports coordinates
  relative to the *frame*. **Test any change here against Nautilus.**
* `EvButtonPress` → `getDragHandlerForEvent` (in `drag.cc`) → `startDragging`.
  All mouse gestures are modelled as a `DragHandler` (`Start`/`Move`/`End`) held
  in the `current_dragger` static in `disp.cc`; `EvMotionNotify` and
  `EvButtonRelease` drive it. The concrete `DragHandler` subclasses live in
  `drag.cc`.
* `EvEnterNotify` → `Focuser::EnterWindow` (sloppy focus) plus cursor reset.
* `EvPropertyNotify` — name, visible name, transient-for, strut, `_NET_WM_STATE`
  (this is where full-screen enter/exit is triggered).
* `EvClientMessage` — EWMH requests: state change, activate, close, moveresize.

## Ownership

```
LScr::I  (screen.cc)
 ├── clients_   map<Window(client), Client*>   OWNS the Clients
 ├── parents_   map<Window(frame),  Client*>   non-owning alias
 ├── hider_     Hider     (hider.cc) — hidden list + unhide menu
 ├── focuser_   Focuser   (focus.cc) — focus history + timerfd
 ├── cursor_map_, GCs, colours, popup_/menu_/ewmh_compat_ windows
 └── visible_areas_ + strut_
```

`LScr::GetClient(w, scan_parents=true)` resolves *any* window (frame,
client, or a sub-window of the client) to its `Client` by walking up the tree.
Pass `false` during `DestroyNotify` — the windows are gone and walking the tree
generates errors.

Client lifecycle: `GetOrAddClient` → `AddClient` (reads `XSizeHints` into two
`DimensionLimiter`s) → `manage()` (`manage.cc:105`) does the real work →
`Client::FurnishAt` → `LScr::Furnish` creates the frame window. Removal is
`Client::Remove()` → `LScr::Remove()` which unfocuses, erases from both maps and
deletes. On exit, `Client_FreeAll()` → `Client::Release()` reparents every
client back to the root and restores its original border width.

## Things to know before changing behaviour

* `ewmh_set_client_list()` calls `fix_stack()`, which re-raises/lowers windows
  by EWMH state. It is called after nearly every stacking change and guards
  itself against recursion with a static flag.
* `LOGD(x)` only prints when the debug CLI has debugging enabled for that client
  or window — it's free to sprinkle liberally.
* Errors on already-destroyed windows are routine; wrap such code in
  `ScopedIgnoreBadWindow` rather than trying to avoid the race.
