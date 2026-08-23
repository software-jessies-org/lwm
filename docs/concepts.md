# Concepts and invariants

## Frame rect vs content rect

A framed client has two windows: the LWM-created *frame* (`Client::parent`) and
the client's own window (`Client::window`), reparented inside it.

* `Client::ContentRect()` — the client window, in **root** coordinates. This is
  the stored state (`content_rect_`); everything else is derived.
* `Client::FrameRect()` — the frame, root coordinates. Equals the content rect
  when `!framed`, and **also when the client is full screen**: there's no
  furniture to make room for, so the frame covers exactly the same pixels as
  the client. `EnterFullScreen` also drops the frame's X border
  (`kFrameBorderWidth`) to zero, because X puts a window's border outside its
  coordinate space and a full-screen window has to line up with the monitor
  exactly.
* `Client::ContentRectRelative()` — the content rect translated into
  **frame-window** coordinates. Needed whenever you call an X move/resize on
  `window` while it is reparented.
* `ContentFromFrameRect` / `FrameFromContentRect` — static converters. The frame
  adds `borderWidth()` on left/right/bottom and `titleBarHeight()` on top
  (`titleBarHeight() == textHeight() + borderWidth()`).

Everything that places the two windows — `MoveTo`, `MoveResizeTo`,
`EnterFullScreen`, `EvConfigureRequest` — goes through `FrameRect()` for the
frame and `ContentRectRelative()` for the client, so the full-screen case falls
out of those two rather than being special-cased at each site. Passing root
coordinates for the client window is the mistake to watch for: it is
indistinguishable from the right answer until the frame's origin isn't (0, 0),
which on a multi-monitor layout means it only shows up when the screen being
filled isn't the leftmost one.

Mutators: `MoveTo` (size must be unchanged — it `LOGF`s, i.e. exits, otherwise)
and `MoveResizeTo`. Both update `content_rect_`, move the X windows and send a
synthetic `ConfigureNotify`. Neither does visibility bounds checking; call
`LimitResize()` first, and check against `VisibleAreas()` yourself.

## Edges

`Edge` (`edge.h`) names the eight resize directions plus three specials:
`ENone` = the title bar (i.e. move, not resize), `EClose` = the close cross,
`EContents` = the client window itself, not window furniture.

`Client::EdgeAt(w, x, y)` maps a point on the frame to an `Edge`; the ordering
matters (close box, then title bar, then the eight edges). `EdgeBounds()` uses
`-1`/`+1` fudges because the frame's own 1px X border sits outside its
coordinate space. `closeBounds(false)` (the clickable area) is deliberately
bigger than `closeBounds(true)` (the drawn cross) — clicking just below/right of
the cross should close, not resize.

There are two ways to arrive at an `Edge` from a click, and they cover
different parts of the window. `Client::EdgeAt` is the furniture one, above.
`NineGridEdgeAt` (`gesture.h`) is the Windows-key one: it divides the client
area into a 3x3 grid and names the cell, so the whole window can pick an edge
rather than just its border. Note the two disagree about `ENone` — for
`EdgeAt` it means the title bar, and so "move"; for `NineGridEdgeAt` it means
the centre cell, and so "no edge in particular", which the resize gesture
reads as "do nothing" and the expand gesture as "all four".

## Size limits: `DimensionLimiter`

One per axis, built in `LScr::AddClient` from `XGetWMNormalHints`. Holds
min/max/base/increment. `Limit(oldMin, oldMax, newMin&, newMax&)` snaps a
proposed range, adjusting whichever end actually moved — that's what makes
resizing an xterm from the left edge behave. `DisplayableSize()` converts pixels
to the units shown in the resize popup ("80 x 24" for an xterm).

## Focus (`Focuser`, in `focus.cc`)

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
comment on the `Focuser` class in `focus.h`.

`ReallyFocusClient` has three paths, which are ICCCM section 4.1.7's input
models:

| `WM_HINTS` input | `WM_TAKE_FOCUS` | What lwm does |
| --- | --- | --- |
| true | either | *Passive/locally active*: `XSetInputFocus` on the top-level, plus `WM_TAKE_FOCUS` if the client listed it |
| false | yes | *Globally active*: send `WM_TAKE_FOCUS` and let the client focus whichever of its windows it likes — **plus** ping every child that selected `FocusChangeMask`, for Java |
| false | no | *No input*: give up (`XSetInputFocus(None)`) |

Chrome breaks if you focus its children as well as the top level; Java breaks if
you don't. Don't "simplify" this.

The globally active row is the one every Wine/Proton window lands in —
`winex11.drv`'s `UseTakeFocus` defaults on, so it sets input=false and lists
`WM_TAKE_FOCUS`. It has no child windows either, so before lwm sent the message
the focus simply never moved: `XGetInputFocus` stayed at `PointerRoot`, Wine
never believed it had been activated, and Steam games got no key events at all.
`focus_test.cc` covers both halves of the row.

lwm does **not** delete `_NET_ACTIVE_WINDOW` before setting it.
`XChangeProperty` notifies whether or not the value changed, so the delete
bought nothing and momentarily announced that no window was active, which Wine
acts on by deactivating its foreground window.

## Hiding (`Hider`, in `hider.cc`)

Hiding = unmap the frame + `IconicState`. No icons are placed on the desktop.
`hidden_` is a list of **frame** window ids (see `hiddenIDFor`). The unhide menu
is rebuilt on open: hidden windows first, then normal ones, separated by a dotted
line; entries whose `Client` has vanished are pruned at that point. The red
outline box is four 1px-wide override windows (`highlightL/R/T/B`), hidden and
re-shown around each menu repaint to avoid corruption — the menu GC uses `GXxor`
so highlights are drawn by EORing.

## The client lifecycle and the save-set

`manage()` adopts a window; `withdraw()` gives it back. Both are in `manage.cc`,
and the pair has to balance, because `manage()` puts the client window in lwm's
**save-set** and only `withdraw()` takes it out again.

The save-set is the X server's insurance against a window manager dying while it
holds other applications' windows inside its frames. It has a sting in the tail:
when a client disconnects, the server maps *every* unmapped window in that
client's save-set, whether or not it was ever reparented. So a window lwm still
has in its save-set at exit, but which the application had deliberately unmapped,
comes back on screen — and the next lwm's start-up scan then adopts it, which
looks for all the world like the *new* lwm having made it visible.

That's why `EvUnmapNotify` has to fire for unframed clients as well as framed
ones. A framed client's unmaps arrive against its frame; the root-reported unmap
is the spurious one the reparent into the frame produced, and is ignored. An
unframed client (`ewmh_hasframe`: desktops, docks, menus, splash screens) is
never reparented, so the root is the *only* place its unmap can come from, and
the test has to be on whether this particular client was reparented rather than
on where the event came from.

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

### Maximisation

`_NET_WM_STATE_MAXIMIZED_VERT` and `_HORZ` are two independent states, and
clients do use them separately, so `Client::SetMaximized(vert, horz)` takes
both and `MaximizedRect` applies whichever are set. Three things about it:

* It works in **frame** coordinates. What fills the screen is the window plus
  its furniture; maximising the content rect would push the title bar off the
  top.
* It uses `VisibleAreas(**true**)` — *with* struts. This is the difference
  between maximised and full screen: a full-screen window covers the panels, a
  maximised one stops at them.
* The un-maximised geometry is saved in `pre_maximize_content_rect_` on the
  transition *into* maximisation, which is why `SetMaximized` takes the new
  flags rather than reading `wstate` — setting the second axis while the first
  is already set must not overwrite the saved rect with a half-screen-sized
  one. While the window is full screen the saved rect comes from
  `pre_full_screen_content_rect_`, since `content_rect_` is then the screen.

Full screen wins while it lasts: `SetMaximized` records the flags and returns,
and `ExitFullScreen` applies whatever they say by then. Restoring goes through
`makeVisible`, because the monitor layout can change while a window is
maximised and the rect to go back to may name a monitor that has since been
unplugged.

lwm has no maximise gesture of its own — this exists for clients that ask, and
`_NET_WM_ALLOWED_ACTIONS` now says they may. When the user takes the geometry
into their own hands with a Super-drag or an expand, `DropMaximization` clears
the flags without moving the window: whatever it is at that point, it isn't
maximised.

### Borderless full screen, and `SnapToMonitor`

There are two ways a client goes full screen, and only one of them is lwm's
decision. `_NET_WM_STATE_FULLSCREEN` asks lwm to place the window, and
`Client::EnterFullScreen` does it. *Borderless* full screen - what a game's
display settings usually call it - asks for nothing: the client turns its
decorations off (`_MOTIF_WM_HINTS`, see `motifWouldDecorate` in `manage.cc`)
and sizes itself to the monitor, so lwm never gets a say and its own arithmetic
has to be right.

It sometimes isn't. A game under Proton positions such a window using the
Windows work area, which lwm derives from panel struts, and lands the window
exactly the height of a top panel's strut above the monitor. So
`SnapToMonitor` (`screenlayout.cc`) puts it back: an **unframed** window whose
requested size is *exactly* one monitor's, and which already covers most of
that monitor, is translated onto the monitor's origin. It never resizes, never
moves a window to a monitor it wasn't mostly on already, and doesn't apply to
framed windows, where a monitor-sized window is just a big window.

Two call sites, because a client need not use either alone: `manage()`, for a
client that creates its window in the wrong place and simply maps it, and the
pass-through branch of `handleConfigureRequest`, for one that moves itself
afterwards. The latter only acts on a request naming all of x, y, width and
height - lwm's `content_rect_` for an unframed client is only ever the geometry
it had when we adopted it, since these requests are passed to the server rather
than applied through `Client`, so there is nothing trustworthy to combine a
partial request with.

On an xrandr change, `LScr::SetVisibleAreas` computes every client's new rect
with `MapToNewAreas` *before* swapping in the new geometry, then applies the
moves. `MapToNewAreas` handles, in order: height-maximised windows, windows
against the left/right edge (`mapEdges` → `mapLeftEdge`, with the right edge
handled by mirroring and top/bottom by flipping x/y), then free-floating windows
scaled proportionally into the tallest screen at their new mid-x. Extend
`screenlayout_test.cc` when touching it.

## Undecorated windows

`Client::framed` is false for two quite different kinds of window, and the
difference matters every time you touch code that keys off it.

* **Furniture-free by nature** — `_NET_WM_WINDOW_TYPE_DESKTOP`, `DOCK`, `MENU`
  and `SPLASH`, screened out by `ewmh_hasframe()`. Dragging one is
  meaningless.
* **Ordinary windows lwm chose not to decorate** — shaped windows, and
  anything that draws its own title bar and says so through
  `_MOTIF_WM_HINTS` (the Steam launcher, GTK client-side decorations, Java's
  `setUndecorated(true)`). These are perfectly normal, movable windows.

The second kind is the trap. They have no furniture to drag, so the only two
ways left to move or resize them are the Windows-key gestures and the
`_NET_WM_MOVERESIZE` message the client sends when the user grabs its own
title bar or resize grip. Withhold both and the window is pinned to the
screen for the rest of its life, with no way out — which is exactly what used
to happen. So:

* `Client::GrabSuperButtons()` keys off `ewmh_hasframe()`, **not** `framed`.
* An unframed client is never reparented, so `c->parent` is still the root,
  and `LScr::GetClient()` answers `nullptr` for the root on purpose. Anything
  that remembers a client across a drag must therefore hold `c->window`, not
  `c->parent`. Every `DragHandler` does.

`_NET_WM_MOVERESIZE` (`EvClientMessage` in `disp.cc`) has one wrinkle the
other gestures don't: the button press that started the drag went to the
*client*, so lwm has no implicit grab bringing it the motion and release
events. It takes an explicit `XGrabPointer` on the root — the root, because a
resize drag routinely leaves the window being resized — and gives it back in
`startDragging()`, which is the single point every drag ends at. That grab is
the one place in the shim that waits for its reply: a grab that silently
failed would leave a `DragHandler` running which can never receive the events
that would retire it, and lwm starts only one drag at a time, so every later
mouse gesture would be refused with "already doing something".

Covered by the `Undecorated` and `MoveResize` tests in `drag_test.cc`, and
end-to-end against a real server by the undecorated-window section of
`ui_test.sh`.

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
