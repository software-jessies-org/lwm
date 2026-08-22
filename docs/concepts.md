# Concepts and invariants

## Frame rect vs content rect

A framed client has two windows: the LWM-created *frame* (`Client::parent`) and
the client's own window (`Client::window`), reparented inside it.

* `Client::ContentRect()` — the client window, in **root** coordinates. This is
  the stored state (`content_rect_`); everything else is derived.
* `Client::FrameRect()` — the frame, root coordinates. Equals the content rect
  when `!framed`.
* `Client::ContentRectRelative()` — the content rect translated into
  **frame-window** coordinates. Needed whenever you call an X move/resize on
  `window` while it is reparented.
* `ContentFromFrameRect` / `FrameFromContentRect` — static converters. The frame
  adds `borderWidth()` on left/right/bottom and `titleBarHeight()` on top
  (`titleBarHeight() == textHeight() + borderWidth()`).

Mutators: `MoveTo` (size must be unchanged — it `LOGF`s, i.e. exits, otherwise)
and `MoveResizeTo`. Both update `content_rect_`, move the X windows and send a
synthetic `ConfigureNotify`. Neither does visibility bounds checking; call
`LimitResize()` first, and check against `VisibleAreas()` yourself.

## Edges

`Edge` (`geometry.h:13`) names the eight resize directions plus three specials:
`ENone` = the title bar (i.e. move, not resize), `EClose` = the close cross,
`EContents` = the client window itself, not window furniture.

`Client::EdgeAt(w, x, y)` maps a point on the frame to an `Edge`; the ordering
matters (close box, then title bar, then the eight edges). `EdgeBounds()` uses
`-1`/`+1` fudges because the frame's own 1px X border sits outside its
coordinate space. `closeBounds(false)` (the clickable area) is deliberately
bigger than `closeBounds(true)` (the drawn cross) — clicking just below/right of
the cross should close, not resize.

## Size limits: `DimensionLimiter`

One per axis, built in `LScr::AddClient` from `XGetWMNormalHints`. Holds
min/max/base/increment. `Limit(oldMin, oldMax, newMin&, newMax&)` snaps a
proposed range, adjusting whichever end actually moved — that's what makes
resizing an xterm from the left edge behave. `DisplayableSize()` converts pixels
to the units shown in the resize popup ("80 x 24" for an xterm).

## Focus (`Focuser`, in `client.cc:620`)

Sloppy focus by default; `focus: click` in Xresources switches to click-to-focus
(in which mode `FocusLost` grabs buttons on the client window so LWM sees the
click, and `FocusGained` ungrabs).

`focus_history_` is a most-recent-first list of `Client*`; the front element *is*
the focused client (`GetFocusedClient`). When a client is hidden or destroyed,
`UnfocusClient` drops it and hands focus to the next in the list. The list must
never contain a dangling pointer — `LScr::Remove` calls `UnfocusClient` first.

The timerfd exists to defuse an X race: mouse crossing A→B→C fast can let B grab
focus after C. So the first enter is honoured immediately and any enter within
`focusDelayMillis` (default 50) is deferred through the timer. See the long
comment at `lwm.h:356`.

`ReallyFocusClient` has three paths: normal (`XSetInputFocus` on the top-level,
plus `WM_TAKE_FOCUS` if supported), the Java case (`accepts_focus == false` but
`Ptakefocus` — ping every child with `FocusChangeMask`), and the give-up case.
Chrome breaks if you focus its children as well as the top level; Java breaks if
you don't. Don't "simplify" this.

## Hiding (`Hider`, in `mouse.cc`)

Hiding = unmap the frame + `IconicState`. No icons are placed on the desktop.
`hidden_` is a list of **frame** window ids (see `hiddenIDFor`). The unhide menu
is rebuilt on open: hidden windows first, then normal ones, separated by a dotted
line; entries whose `Client` has vanished are pruned at that point. The red
outline box is four 1px-wide override windows (`highlightL/R/T/B`), hidden and
re-shown around each menu repaint to avoid corruption — the menu GC uses `GXxor`
so highlights are drawn by EORing.

## Visible areas and struts

`LScr::VisibleAreas(withStruts)` returns one `Rect` per active xrandr CRTC.
They may overlap, and the union may be non-rectangular — the concave gaps are
"inaccessible", and code must not drop windows into them (see
`Client::FurnishAt`, which re-centres such windows on the primary screen).
`GetPrimaryVisibleArea()` picks the largest, tie-broken by lowest y then x.

A *strut* is an EWMH reservation along a screen edge (panels, launchers).
`LScr::strut_` is the max over all clients (`ewmh_set_strut`); `withStruts=true`
clips every visible area by it. Clients that set struts are skipped during
xrandr re-layout — they're expected to reposition themselves.

On an xrandr change, `LScr::SetVisibleAreas` computes every client's new rect
with `MapToNewAreas` *before* swapping in the new geometry, then applies the
moves. `MapToNewAreas` handles, in order: height-maximised windows, windows
against the left/right edge (`mapEdges` → `mapLeftEdge`, with the right edge
handled by mirroring and top/bottom by flipping x/y), then free-floating windows
scaled proportionally into the tallest screen at their new mid-x. This is the
one part of the codebase with unit tests — extend `tests.cc` when touching it.

## Icons (`xlib::ImageIcon`)

Sourced from `WM_HINTS` pixmaps or `_NET_WM_ICON` pixel data. Three pre-rendered
pixmaps per icon (active title / inactive title / menu background), because
compositing against the final background at creation time avoids per-paint alpha
work. Scaled down only, with simple box-filter anti-aliasing
(`copyWithScaling`); channels are averaged separately to avoid colour bleed.
Cached by content hash with refcounts — `ImageIcon::Create*` returns a `clone()`,
and the last clone destroyed frees the pixmaps. 24bpp only.

## Naming

`Client::Name()` prefers `visible_name_` (`_NET_WM_VISIBLE_NAME`, which the user
sets via `setvisname`/`force_title.sh`) over `name_` (`_NET_WM_NAME`, falling
back to `XA_WM_NAME` for non-UTF8 servers). `MenuName()` truncates to 100 UTF-8
characters. This is the "user is in control" feature: it lets you override a web
app that animates its title.
