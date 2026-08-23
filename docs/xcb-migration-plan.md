# XCB migration plan

A staged plan to move lwm off Xlib and onto XCB. Written in the same spirit as
`refactoring-plan.md`: every phase leaves the tree building and lwm usable,
and the risky parts are named rather than hidden.

## Why bother

Three concrete reasons, in order of how much they matter here:

1. **Round trips.** Xlib blocks on every request that has a reply. lwm does a
   lot of these in bursts: start-up interns ~70 atoms one at a time (62 EWMH in
   `ewmh.cc` + 8 ICCCM in `lwm.cc`), `ScanWindowTree()` does a `QueryTree`
   then two blocking queries *per adopted window*, `manage()` reads six or
   seven properties back to back, and `setScreenAreasFromXRandR()` walks CRTCs
   one blocking `GetCrtcInfo` at a time. XCB splits every request into a
   `_cookie` and a `_reply` call, so these become one flush and one wait
   instead of N of each.
2. **A latency-free error model.** Xlib delivers errors to a global handler at
   an unpredictable time, which is why `ScopedIgnoreBadWindow` is a global flag
   and why it only really works around requests that happen to round-trip. XCB
   gives errors a sequence number, so "ignore BadWindow *from these requests*"
   becomes precise instead of approximate. The "another window manager is
   already running" detection stops needing the `is_initialising` global.
3. **The shim becomes fakeable.** `refactoring-plan.md` phase E wants to invert
   `namespace xlib` into a `Server`/`FakeServer` pair. That is much easier
   against XCB, where the shim is already pure data-in/data-out over a socket,
   than against Xlib, where `Display*` hides a pile of caching and state. **Do
   the XCB port before phase E, not after.**

## Starting position: better than the plan assumed

`refactoring-plan.md` phase E estimated ~140 direct X calls to route through
the shim. That work is essentially done. Outside `xlib.{h,cc}` there are now
**25 direct Xlib calls in the whole binary**:

| File | Direct calls | What they are |
| --- | ---: | --- |
| `ewmh.cc` | 6 | `XFree` on property replies |
| `manage.cc` | 6 | `XFree`, `XSelectInput` |
| `error.cc` | 2 | `XGetErrorText`, `XGetErrorDatabaseText` |
| `shape.cc` | 2 | `XFree` |
| `resource.h` | 1 | `XRenderColor` in a signature |
| `lwm.cc` | 0 calls, but all the Xft/font code | `XftFontOpenName`, `XftDrawStringUtf8`, … |

Plus the `Display*` convenience macros, which are calls in disguise:
`DefaultVisual`, `DefaultColormap`, `DefaultScreen`, `RootWindow`,
`DisplayWidth`/`Height`, `BlackPixel`/`WhitePixel`, `ConnectionNumber` —
scattered across `screen.{h,cc}`, `client.cc`, `debug.cc`, `cursor.cc`,
`ewmh.cc`, `resource.cc`, `lwm.cc`.

`xdbg.cc`, `ruler.cc` and `setvisname.cc` are standalone tools that aren't
linked into lwm. **They are explicitly out of scope** and can stay on Xlib
forever.

So the migration is mostly a rewrite of `xlib.cc` (978 lines) behind a mostly
unchanged `xlib.h` (358 lines), plus a font extraction, plus the event loop and
error model.

## The one real blocker: Xft

Everything lwm needs has an XCB equivalent (all present on this machine):

| Xlib thing | XCB replacement | Installed |
| --- | --- | --- |
| core protocol | `xcb` | 1.15 |
| `XGetWMHints`/`NormalHints`/`Protocols`/`TransientFor` | `xcb-icccm` | 0.4.1 |
| `XRR*` | `xcb-randr` | 1.15 |
| `XShape*` | `xcb-shape` | 1.15 |
| `XCreateFontCursor`/`XRecolorCursor` | `xcb-cursor` | 0.1.4 |
| `XCreateImage`/`XGetImage`/`XPutImage` | `xcb-image` | 0.4.0 |
| `Xrm*` resource database | `xcb-xrm` | 1.0 |
| `XAllocNamedColor` | `xcb_alloc_named_color` | — |
| libSM / libICE session mgmt | **unchanged** — libSM doesn't link libX11 | 1.2.3 |
| `XftFontOpenName`, `XftDrawStringUtf8`, `XftTextExtentsUtf8` | **nothing** | — |

Xft is built on Xlib and there is no XCB port of it. That is the only thing
forcing `libX11` into the link. Three ways out:

* **F1 — keep libX11 open, for Xft only.** Open with `XOpenDisplay`, get the
  connection with `XGetXCBConnection(dpy)` (`x11-xcb`, 1.8.7), and hand the
  event queue to XCB with `XSetEventQueueOwner(dpy, XCBOwnsEventQueue)`.
  Everything except text goes through XCB. Zero risk, zero font work.
* **F2 — Pango + `cairo-xcb`.** (pangocairo 1.52, cairo-xcb 1.18 both present.)
  Drops libX11 entirely, and gives proper text shaping, which Xft doesn't.
  ~150 lines in a new `xfont.cc`. **Behaviour change:** font naming goes from
  Xft's `"roboto-16"` to a Pango description `"roboto 16"`, and metrics differ
  by a pixel or two, so title bar height moves. `titleFont` in `lwm.man` and
  `testXresources` would need updating.
* **F3 — `xcb-render` + FreeType by hand.** More work than F2 and buys nothing
  over it. Don't.

**Recommendation: F1 as the bridge, F2 as an optional final phase.** F1 makes
every intermediate phase shippable; F2 is what actually removes `-lX11`, and it
is a font project rather than an XCB project, so it should be decided on its
own merits after the port is done and stable.

## The four hazards

These are the things that will silently break rather than fail to compile.
Read this section again before starting phase 3.

### 1. 32-bit vs 64-bit property data — the icon corruption trap

Xlib's `XGetWindowProperty` expands `format == 32` properties into an array of
`long`, which is **64 bits** on LP64. XCB's `xcb_get_property_value()` returns
the wire format, which is **actually 32 bits**.

`ewmh.cc:234` does exactly the thing that breaks:

```cpp
return xlib::ImageIcon::CreateFromPixels((unsigned long*)prop.data,
                                         prop.nitems);
```

Ported naively this compiles cleanly, runs, and produces garbage icons. The
same hazard applies everywhere a 32-bit property is read: `_NET_WM_STATE`,
`_NET_WM_WINDOW_TYPE`, `_NET_WM_STRUT[_PARTIAL]`, `WM_STATE`,
`_MOTIF_WM_HINTS` (`motifWouldDecorate` casts to `unsigned long*` too), and
`getProperty()`'s `unsigned char**` out-param in `manage.cc`.

Fix: change `xlib::WindowProperty` to carry `uint32_t*` explicitly rather than
`unsigned char*`, and change `ImageIcon::CreateFromPixels` and `getProperty`
to match. Do this **as a separate, Xlib-side commit in phase 0** — convert the
call sites to a 32-bit-explicit accessor while still on Xlib, where you can
verify against known-good behaviour, then the XCB swap underneath is a no-op.

Test with Chrome/Chromium, per the existing comment at `xlib.cc:882`.

### 2. Flushing

Xlib flushes implicitly all over the place — `XPending`, `XSync`, and every
blocking round trip. XCB flushes only when you call `xcb_flush()` or wait for a
reply. Get this wrong and requests sit in the output buffer while lwm blocks in
`select()`, which looks like "the window manager randomly stops repainting
until you jiggle the mouse".

lwm already has one instance of this bug's Xlib-flavoured cousin, and its
comment (`lwm.cc:227`, the `XSync` after `TimerFDTriggered`) explains it well.
Under XCB it stops being a special case and becomes the general rule:

> **Rule: `xcb_flush()` exactly once, immediately before `select()`, on every
> pass of the event loop.** Individual shim functions never flush.

Keep the existing comment; it documents the *why* and stays accurate.

Related: `xcb_poll_for_event()` may read from the socket, whereas
`xcb_poll_for_queued_event()` only drains what's already buffered. Drain with
`poll_for_event` in a loop until it returns null, then flush, then `select()`.
Also check `xcb_connection_has_error()` on each pass — it replaces Xlib's IO
error handler, which lwm currently doesn't have at all.

### 3. Value lists

Xlib takes structs (`XSetWindowAttributes`, `XWindowChanges`, `XGCValues`) plus
a mask, and picks out the fields the mask names. XCB takes a bare `uint32_t[]`
whose entries must appear **in increasing bit-position order of the mask**, and
does not check. Get the order wrong and you set the wrong attribute to the
wrong value, silently.

This affects every `XCreateWindow`, `XChangeWindowAttributes`,
`XConfigureWindow` and `XCreateGC` in the tree. Write **one** `ValueList`
builder in `xlib.cc` that takes (mask bit, value) pairs and sorts by bit, and
route all of them through it. Do not hand-write the arrays.

### 4. Events

* `response_type` has bit 0x80 set for events delivered by `SendEvent`. Mask
  with `& 0x7f` before switching, and use the bit where lwm currently reads
  `ev->xany.send_event`.
* There is no `xany`. `window` sits at a different offset in each event type,
  so `DispatchXEvent`'s macro switch needs a per-type accessor. This is fine —
  the macro is the right shape for it already.
* Events are heap-allocated and must be `free()`d.
* Extension events are `first_event + N` where `first_event` comes from
  `xcb_get_extension_data()`, same idea as today's `rr_event_base` and
  `shape_event`.
* **RandR duplicate suppression is behavioural and will need re-deriving.**
  `rrScreenChangeNotify` currently drops duplicates by comparing
  `XRRScreenChangeNotifyEvent.serial` against a static. XCB's event doesn't
  carry Xlib's notion of serial; use `config_timestamp` instead. The comment
  says "we get lots of these", so this dedup is load-bearing — verify with
  `-debugcli` and real monitor hotplug, not just the fake xrandr.

## Phases

### Phase 0 — Prerequisites, all still on Xlib

Nothing here touches the wire protocol. Every step is independently verifiable
against current behaviour, which is the point.

1. **Extract Xft into `xfont.{h,cc}`** with an X11-free interface —
   `TextHeight()`, `TextWidth(const std::string&)`, `DrawString(Window, int x,
   int y, const std::string&, FontColour)`. Move `g_font`, the three
   `XftColor`s and `textHeight`/`textWidth`/`drawString` out of `lwm.{h,cc}`.
   This move is already on `refactoring-plan.md`'s "wrong place" list; it is
   also the **load-bearing prerequisite for the whole migration**, because it's
   what lets the rest of the tree stop including `X11/Xlib.h`. (It must, in
   phase 2: Xlib and XCB both define `Window`, and you can't include both sets
   of headers in one translation unit.)
2. **Drop `XRenderColor` from `resource.h`.** `Resources::GetXRenderColor`
   becomes `GetRGB()` returning a plain `{r,g,b}` struct; `xfont.cc` converts.
3. **Make property reads 32-bit-explicit** (hazard 1). `WindowProperty::data`
   becomes `uint32_t*`; fix `ewmh_get_window_icon`, `ImageIcon::CreateFromPixels`,
   `motifWouldDecorate`, `getProperty`, `getWindowState`. Verify icons in
   Chrome, struts in a panel app, `_NET_WM_STATE` full-screen.
4. **Route the last 25 direct calls through the shim**, and add shim accessors
   for the `Display*` macros: `xlib::Root()`, `xlib::ScreenWidth()`,
   `xlib::Black()`/`White()`, `xlib::Colormap()`, `xlib::Visual()`,
   `xlib::Depth()`, `xlib::ConnectionFD()`. `XFree` call sites become the RAII
   holder from step 5.
5. **Replace `XFreer` with a typed `xlib::Reply<T>`** (a `unique_ptr` with a
   `free`-ish deleter). Under Xlib the deleter is `XFree`; under XCB it's
   `free`. One-line change later, and it kills the manual `XFree` calls.
6. **Add a build gate**: a `make` rule that greps the tree for `X11/Xlib.h` and
   fails if it appears anywhere except `xlib.h` and `xfont.cc`. Cheap, and it
   stops the boundary rotting during phases 2–3.

**Exit criterion:** `xlib.h` and `xfont.cc` are the only files that include X11
headers, and `./lwm -test` plus `./smoke_test.sh` pass.

### Phase 1 — Safety net

You cannot eyeball a wire-protocol change. Build the harness before you need
it.

1. **Extend `smoke_test.sh`.** It currently covers framing, configure-request
   move/resize, focus-on-activate and `WM_DELETE_WINDOW`. Add: window icons
   (`_NET_WM_ICON` via a Chrome-like test client), the unhide menu, a shaped
   window, `_NET_WM_STRUT` handling, and a fake multi-monitor layout driven
   through `-debugcli`'s `xrandr` command.
2. **Record golden shim logs.** The `xlib::` wrappers already `LOGD` every
   call. Run a scripted Xvfb session with `dbg auto` and capture the log. After
   each phase-3 step, re-run and diff: the sequence of shim calls should be
   *identical*, because the shim's API isn't changing — only its guts. This is
   the highest-value thing in the plan and it's nearly free.
3. Record the golden logs **now**, on Xlib, before anything changes.

### Phase 2 — Connect with XCB, keep Xlib for text (the bridge)

The scary phase. Do it in one commit; there's no useful half-way state.

1. `XOpenDisplay` as now, then `conn = XGetXCBConnection(dpy)` and
   `XSetEventQueueOwner(dpy, XCBOwnsEventQueue)`. `dpy` stays a global,
   used only by `xfont.cc`; `conn` joins it in `xlib.h`.
2. **Rewrite the event loop** on `xcb_poll_for_event` / `xcb_flush` /
   `xcb_get_file_descriptor`, per hazard 2.
3. **Rewrite `disp.cc` onto XCB event structs directly.** Resist the temptation
   to write an `xcb_generic_event_t` → `XEvent` translation shim: it's throwaway
   work and it's exactly where the `0x80` send-event masking gets fumbled.
   `xdebugprint.cc`'s `operator<<`s port alongside.
4. **Replace the error model.** `XSetErrorHandler` → handling
   `response_type == 0` in the event loop. `errorHandler`'s logic survives
   mostly intact, minus the two Xlib string lookups (phase 4 replaces those;
   until then print the numeric code).
5. **`ScopedIgnoreBadWindow` becomes sequence-scoped.** Record
   `xcb_get_request_sequence`-style bounds on construction and destruction, and
   suppress matching errors by sequence number instead of by global flag. All
   four call sites (`manage.cc:248`, `manage.cc:262`, `disp.cc:291`,
   `disp.cc:434`) keep working and become correct rather than approximate.
6. **WM detection stops being a hack.** `xcb_change_window_attributes_checked`
   + `xcb_request_check` on the root's `SubstructureRedirect` grab gives an
   immediate answer. Delete `is_initialising`'s role in `errorHandler`.

**Exit criterion:** lwm runs under Xephyr with everything except text drawing
going through XCB. Golden log diff is clean.

### Phase 3 — Port the shim internals, group by group

The `xlib::` API doesn't change, so the golden-log diff should stay empty
throughout. Ordered lowest-risk first; each step is independently shippable.

| # | Group | Files | Notes |
| --- | --- | --- | --- |
| 1 | Cursors | `cursor.cc` (60 lines) | Done, and then done again: the first pass used `xcb_create_glyph_cursor`, which takes fg/bg at creation so `XRecolorCursor` disappeared entirely; the second switched to `xcb-cursor`'s themed cursors, which took the colours away too. Fully self-contained — a good confidence-builder. |
| 2 | Resources + colours | `resource.cc` | `xcb-xrm`; `xcb_alloc_named_color`. `Resources::Set(name, class, default)` maps directly onto `xcb_xrm_resource_get_string`. |
| 3 | Atoms | `lwm.cc`, `ewmh.cc` | Batch the ~70 interns: fire all cookies, then collect. First real latency win, and trivially safe. |
| 4 | Window ops | most of `xlib.cc` | Biggest by call count, purely mechanical. Introduce the `ValueList` builder (hazard 3) here and use it everywhere. |
| 5 | Properties + ICCCM | `xlib.cc`, `ewmh.cc`, `manage.cc` | `xcb-icccm` for `WM_NORMAL_HINTS`/`WM_HINTS`/`WM_PROTOCOLS`/`WM_TRANSIENT_FOR`. `XSizeHints` in signatures becomes a local plain struct. Hazard 1 already defused in phase 0. |
| 6 | Drawing | `xlib.cc`, `client.cc`, `hider.cc` | GC creation, fill/line/clear/copy. Watch the `GXxor` menu GC — the `Hider` highlight box depends on it. |
| 7 | `ImageIcon` | `xlib.cc` (~380 lines) | **Second-riskiest after properties.** `xcb-image`, and mind byte order and scanline padding (`xcb_image_native()`). The refcounted pixmap cache and the hash-keyed clone logic stay as-is. Test with Chrome, Firefox and a Java app. |
| 8 | Shape | `shape.cc` (93 lines) | `xcb-shape`. Straightforward. |
| 9 | RandR | `lwm.cc`, `screen.cc` | `xcb-randr`. Must call `xcb_randr_query_version` first or requests fail. Batch the per-CRTC queries. Re-derive the duplicate-event dedup (hazard 4). |
| 10 | Input | `xlib.cc`, `focus.cc`, `drag.cc` | Grabs, `SetInputFocus`, `QueryPointer`, `SendEvent`. `Focuser::ReallyFocusClient`'s three paths must not change — re-test Chrome and Java/Terminator. |

### Phase 4 — Remove libX11 (optional)

Only worth doing if F2 (Pango) is judged worth it on font-rendering grounds.

1. Rewrite `xfont.cc` on `pangocairo` + `cairo-xcb`. Update `titleFont`'s
   documented syntax in `lwm.man` and the example in `testXresources`. Expect
   title-bar height to shift by a pixel or two — `FrameStyle` and
   `framegeometry_test.cc` will show it.
2. `XOpenDisplay` → `xcb_connect`. Drop `-lX11 -lXft -lXrandr -lXext -lXrender`.
3. Replace `XGetErrorText`/`XGetErrorDatabaseText` with a static table of the
   17 core error codes and the core request opcode names (~40 lines in
   `error.cc`). `libxcb-errors` would do this for you but isn't packaged here.
4. Tighten the phase-0 build gate to reject `X11/` includes anywhere in the
   lwm sources (still excluding `xdbg.cc`/`ruler.cc`/`setvisname.cc`).

### Phase 5 — Actually exploit XCB

Everything up to here is behaviour-preserving. This is where the payoff is
collected, and it *must* come after, so that any regression is attributable.

1. **Batch the round trips.** `LScr::ScanWindowTree()` (fan out
   `GetWindowAttributes` + `GetGeometry` cookies for every child, then collect)
   and `manage()` (fan out all the property and hint cookies, then collect).
   Measure with the shim logging.
2. **Add split cookie/reply forms to the shim only where measured.** The
   current blocking convenience signatures — `xlib::XGetWindowAttributes(w)`
   returning a struct by value — are right for the ~40 cold call sites. Don't
   churn them all for nothing.
3. **Then do `refactoring-plan.md` phase E.** With the shim sitting on XCB it
   is pure request-in / reply-out with no hidden `Display*` state, so
   `FakeServer` becomes a genuinely small class, and the group-2 tests
   (`EvConfigureRequest`'s Nautilus arithmetic, `Focuser`'s three-window race,
   client lifecycle, `fix_stack` recursion) become writable.

## Build changes

`Imakefile` currently hard-codes `$(XLIB) $(XFREETYPELIB) $(SMLIB)
$(XRANDRLIB) -lICE $(XFTLIB)`. imake has no notion of the xcb libraries and no
pkg-config integration. Rather than fight it, **this is the moment to drop
imake** and write a plain `Makefile` driven by
`pkg-config --cflags --libs xcb xcb-icccm xcb-randr xcb-shape xcb-cursor
xcb-image xcb-xrm sm ice` (plus `x11-xcb xft` during phases 2–3, or
`pangocairo cairo-xcb` after phase 4).

`no_xmkmf_makefile` and `Makefile.freebsd` are already stale (the latter still
says `-std=c++14`, which no longer builds). Fold all three into one generated
`Makefile` and delete the others; update `docs/dev-workflow.md`'s build section
in the same change.

## Things not to break

Everything in `refactoring-plan.md`'s list still applies, and re-reading
`concepts.md` first still applies. XCB adds these:

* **Icon pixel data is 32-bit under XCB, 64-bit under Xlib** (hazard 1). The
  failure mode is garbage pixels, not a crash. Test with Chrome.
* **Flush once per event-loop pass, never inside a shim function** (hazard 2).
  The failure mode is "lwm intermittently stops responding until you move the
  mouse" — which is exactly what the existing `XSync`-after-timerfd comment
  describes, so that comment stays.
* **Value-list ordering** (hazard 3). Silent misconfiguration. One builder,
  used everywhere.
* **`response_type & 0x7f`**, and bit 0x80 as `send_event` (hazard 4).
* **RandR duplicate suppression** needs re-deriving from `config_timestamp`
  (hazard 4). Test against real hotplug, not just `-debugcli xrandr`.
* **Don't adopt `xcb-ewmh`.** It's tempting, but `ewmh.cc`'s hand-rolled table
  encodes specific behaviour (`fix_stack`'s stacking policy, `ewmh_hasframe`'s
  window-type rules, the recursion guard) that a library swap would quietly
  rewrite. Port the property mechanics underneath and leave the policy alone.
* **`Focuser::ReallyFocusClient`'s three paths** are input-focus and
  `WM_TAKE_FOCUS` message ordering, both of which the port touches. Chrome and
  Java/Terminator are the regression tests.

## Effort, roughly

| Phase | Size | Risk |
| --- | --- | --- |
| 0 — prerequisites (still Xlib) | ~600 lines moved/changed | Low; each step verifiable against current behaviour |
| 1 — safety net | ~200 lines of shell | None |
| 2 — bridge + event loop + errors | `disp.cc` (595) + loop + `error.cc` | **High** — one commit, no half-way state |
| 3 — shim internals, 10 steps | most of `xlib.cc` (978) | Low per step except 5 and 7 |
| 4 — drop libX11 | new `xfont.cc` (~150) + error table | Medium; visible font behaviour change |
| 5 — exploit XCB | `manage.cc`, `screen.cc` hot paths | Low, and measurable |

Phases 0, 1 and 3 are the bulk of the wall-clock time and almost none of the
risk. Phase 2 is the one to do when you have an afternoon and a Xephyr.
