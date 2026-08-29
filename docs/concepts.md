# Concepts and invariants

## Frame rect vs content rect

A framed client has two windows: the LWM-created *frame* (`Client::parent`) and
the client's own window (`Client::window`), reparented inside it.

* `Client::ContentRect()` — the client window, in **root** coordinates. This is
  the stored state (`content_rect_`); everything else is derived.
* `Client::FrameRect()` — the frame, root coordinates. Equals the content rect
  whenever `Client::HasFurniture()` is false: there's nothing to make room
  for, so the frame covers exactly the same pixels as the client. That covers
  three cases — no frame at all, full screen, and a window the user has
  undecorated. In the last two the frame's X border (`kFrameBorderWidth`) is
  dropped to zero as well, because X puts a window's border *outside* its
  coordinate space, so leaving it on would draw a line round a window that is
  supposed to have nothing round it.
* `Client::ContentRectRelative()` — the content rect translated into
  **frame-window** coordinates. Needed whenever you call an X move/resize on
  `window` while it is reparented.
* `ContentFromFrameRect` / `FrameFromContentRect` — static converters. The frame
  adds `borderWidth()` on left/right/bottom and `titleBarHeight()` on top
  (`titleBarHeight() == textHeight() + borderWidth()`).

Everything that places the two windows — `MoveTo`, `MoveResizeTo`,
`EnterFullScreen`, `HideFurniture`, `EvConfigureRequest` — goes through
`FrameRect()` for the frame and `ContentRectRelative()` for the client, so the
furniture-free cases fall out of those two rather than being special-cased at
each site. Passing root
coordinates for the client window is the mistake to watch for: it is
indistinguishable from the right answer until the frame's origin isn't (0, 0),
which on a multi-monitor layout means it only shows up when the screen being
filled isn't the leftmost one.

`_NET_FRAME_EXTENTS` (`ewmh_set_frame_extents`) is the one place that *adds*
`kFrameBorderWidth` back on. The rest of lwm works in frame-window coordinates,
where that pixel doesn't exist; the property is defined in terms of what's on
the screen, and the frame's X border is a black pixel drawn all the way round.
So the published extents are `FrameRect() - ContentRect()` plus one on each
side — all four zero whenever `HasFurniture()` is false, and for a withdrawn
window. It's republished from `Client::SetState` (which covers being managed,
hidden and withdrawn), from `EnterFullScreen`/`ExitFullScreen`, and from
`Client::SetFurniture`. Those are the only moments it can change: `manage()`
makes the framing decision, and only the user's decoration toggle revisits it.

`SetFurniture` publishes it *before* it touches either window, which is the
opposite of every other caller and deliberate. A client that reads the
property in response to being resized asks the server after the event it is
reacting to, so anything changed before that event is certain to be what it
reads; changing it afterwards is a race between lwm's request and the
client's. Java's XAWT is the client that noticed.

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
| false | no | *No input*: `XSetInputFocus` on **lwm's own EWMH window** — there is nowhere else to put it, but see below for why "nowhere" is not an option |

Chrome breaks if you focus its children as well as the top level; Java breaks if
you don't. Don't "simplify" this.

The no-input row is xclock and xload: they neither want the focus nor
understand `WM_TAKE_FOCUS`. That row used to read `XSetInputFocus(None)`, which
looks like the honest answer and is a trap. With the input focus set to `None`
the server discards **every** key event, and a passive grab can never activate:
`XGrabKey` fires only when the grab window is the focus window, an ancestor of
it, or a descendant of it containing the pointer, and none of those can hold
when there is no focus window at all. lwm's arrow grabs are on the root, so all
of its keyboard gestures vanished for as long as the pointer sat on an xclock —
Super+arrow and Super+Shift+arrow alike. Parking the focus on `ewmh_compat_`
satisfies the "ancestor of the focus window" case, because the root is its
parent, and it costs the client nothing: it asked for no key events and it
still gets none. Which window lwm *considers* focused is a separate matter —
that's `focus_history_`, and it still names the xclock, so the title bar stays
highlighted and the arrow keys still navigate relative to it. `ui_test.sh`
covers this one, because `FakeServer` delivers a `KeyPress` whatever the focus
is: only a real server shows the grab going dead.

The parking spot used to be the root itself, which works but is shared: the
root is common property, so any other program on the display can drop the
focus there too, and then lwm's parked focus and someone else's are the same
thing, with whoever went last winning. `ewmh_compat_` is lwm's alone — which
is also why it is mapped, off at (-200, -200): `XSetInputFocus` on an unmapped
window is a `BadMatch`.

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
`hidden_` is a list of the window ids `hiddenIDFor` returns: the frame for a
framed client, and the client's own window for one with no frame — which is
also what gets unmapped, since the server ignores an `UnmapWindow` on the root
and an unframed client's `parent` *is* the root. Note that "no frame" here
means `!framed`, not "no furniture": a window the user has undecorated still
has a frame, and hides by it like any other. Unmapping the client window
directly means telling the `Client` to expect the resulting `UnmapNotify`; see
"Turning the furniture on and off" below.

The unhide menu is rebuilt on open: hidden windows first, then normal ones,
separated by a dotted line; entries whose `Client` has vanished are pruned at
that point. The normal half filters on `ewmh_hasframe()`, not `framed`, so an
ordinary window the user has undecorated is still listed while the
furniture-free window types are not.

The red outline box is four 1px-wide override windows (`highlightL/R/T/B`),
hidden and re-shown around each menu repaint to avoid corruption — the menu GC
uses `GXxor` so highlights are drawn by EORing.

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
`Client::MakeContentRectVisible` (`makeVisible` in frame coordinates), because
the monitor layout can change while a window is maximised and the rect to go
back to may name a monitor that has since been unplugged. A restore with no
saved rect at all does nothing rather than moving the window to a zero-sized
rect at the origin — every route in records one, so this shouldn't happen, but
"shouldn't" is not a reason to place a window at 0x0+0+0.

lwm has no maximise gesture of its own — this exists for clients that ask, and
`_NET_WM_ALLOWED_ACTIONS` now says they may. When the user takes the geometry
into their own hands with a resize, a keyboard move or an expand,
`DropMaximization` clears the flags without moving the window: whatever it is
at that point, it isn't maximised.

A *drag* is the exception, and only half of one (`WindowMover` in `drag.cc`).
What it goes by is not the flags but the window's own geometry, per axis:
whether the window is currently as big as its monitor lets it be, which
`WindowMover::axesToKeep` works out by asking `Client::LimitResize` what
maximising on that axis would actually give this client (an xterm maximises to
a whole number of character cells, a few pixels short of the screen, and it
would be a strange rule that called that not maximised). The EWMH flags are
folded in, but they are not the question: lwm's own expand gesture fills a
monitor without setting them, and so does a user dragging an edge to the top of
the screen, and to the user those are all "maximised".

* Filling the monitor on **one** axis: it goes on filling whichever monitor it
  is dragged onto, on that axis. That's what makes a vertically maximised
  window dragged onto a taller monitor grow to the new monitor's height instead
  of arriving short, and one dragged onto a shorter monitor and back again come
  home its old size. The drag still means something, because the other axis is
  free. The offset the content rect moves by therefore comes from the frame
  rather than from the pointer's dx/dy: on a filled axis the window doesn't
  follow the pointer at all. At the end of the drag
  `Client::TranslatePreMaximizeRect` slides `pre_maximize_content_rect_` along
  with the window, so un-maximising later gives it back on the monitor it's on
  now rather than the one it left.
* Filling it on **both**: whatever maximisation there was is dropped. There's
  no free axis, so keeping it would leave the user unable to drag the window
  anywhere but from monitor to monitor; a drag there can only mean "let this
  go".

**Which monitor** a drag is over is `findDragScreen`, and it is the monitor the
*pointer* is in — not `findBestScreenFor`'s "the one the window overlaps most",
which is what everything outside a drag uses. The window can be far bigger than
the pointer's travel and is dragged from wherever the user grabbed it, so a
wide window picked up near one edge stays majority-over the monitor it came
from long after the pointer has crossed. Going by overlap, such a window had to
be dragged half a monitor further than the user thought before it noticed the
new monitor, and dragging it back left it stuck at the height of the monitor it
had been on. The same monitor answers for the shrink-to-fit
(`ShrinkToFitGivenMonitor`), so the two can't disagree part way through a drag.
A pointer over no monitor at all - the dead space beside a monitor shorter than
its neighbour - falls back to the window's own monitor, so crossing a gap
changes nothing.

### Un-expanding

The Super double click in the *middle* of a window grows every edge, which on
an otherwise empty screen means "maximise". The same gesture on a window with
nowhere left to grow does the opposite, so one gesture toggles:

* `NotePreExpandRect` records the geometry just before an expansion, in
  `pre_expand_content_rect_`. It's called before `DropMaximization`, so that a
  window which is big *because* it was maximised (or full screen) records the
  smaller rect that state was standing in front of, rather than the
  screen-sized one.
* `Unexpand` puts the window back there, and then forgets it: the record is
  spent, so a window that's been restored has nothing to restore to until it's
  expanded again. A maximised window is handed to `SetMaximized(false, false)`
  instead, which owns both the flags and its own saved rect; a full-screen one
  is left alone, since full screen is the client's to end.
* With nothing recorded, `Unexpand` does nothing. There is no state flag for
  "expanded" — such a window is indistinguishable from one the user sized by
  hand — so the recorded rect is the whole of lwm's memory that there's
  anything to undo, and inventing a geometry when it's missing is how windows
  end up in silly places.

Only the centre cell restores. From an edge, the user asked for that one edge
to grow; if it can't, nothing happens, exactly as before.

### Borderless full screen, and `SnapToMonitor`

There are two ways a client goes full screen, and only one of them is lwm's
decision. `_NET_WM_STATE_FULLSCREEN` asks lwm to place the window, and
`Client::EnterFullScreen` does it. *Borderless* full screen - what a game's
display settings usually call it - asks for nothing: the client turns its
decorations off (`_MOTIF_WM_HINTS`, see `motifWouldDecorate` in `manage.cc`)
and sizes itself to the monitor, so lwm never gets a say and its own arithmetic
has to be right.

It sometimes isn't, and always for the same reason: what such a game has to
work from is the Windows work area, which Wine derives from the panel struts
lwm publishes in `_NET_WORKAREA`. That goes wrong in two ways, and
`SnapToMonitor` (`screenlayout.cc`) fixes both. It takes an **unframed** window
which already covers most of one monitor and whose size is *exactly* either

* that monitor's, in which case it has only been mis-placed - it lands the
  height of a top panel's strut above the monitor - and is translated onto the
  monitor's origin; or
* that monitor's **work area**, the monitor minus the strut, in which case it
  is grown over the panel as well as moved. That's the shape Shadow of the
  Tomb Raider comes back in after being hidden and restored: it stops being a
  full-screen window and becomes a maximised one, and a maximised window
  respects the panel, which for a game covering the monitor is exactly wrong.
  Growing it is safe because the client asked to fill the screen; it gets a
  `ConfigureNotify` for a size it didn't name and adjusts, in the game's case
  by dropping its own fixed-size `WM_NORMAL_HINTS`.

Only an exact match of one of those two sizes counts: a window a pixel off
either isn't trying to cover a monitor, and guessing on its behalf would be
worse than leaving it alone. Nor does it ever move a window to a monitor it
wasn't mostly on already, or apply to framed windows, where a
monitor-sized window is just a big window. A client with **struts of its own**
is exempt too: a panel the size of the work area is doing exactly what it means
to, and blowing it up to the size of the monitor would cover the screen with
somebody's furniture.

Two call sites, because a client need not use either alone: `manage()`, for a
client that creates its window in the wrong place and simply maps it, and the
pass-through branch of `handleConfigureRequest`, for one that moves itself
afterwards. The latter only acts on a request naming all of x, y, width and
height - lwm's `content_rect_` for an unframed client is only ever the geometry
it had when we adopted it, since these requests are passed to the server rather
than applied through `Client`, so there is nothing trustworthy to combine a
partial request with. The `manage()` one uses `MoveResizeTo`, not `MoveTo`,
since the work-area case changes the size.

`strut_test.sh` covers the work-area case end to end, since it already has a
dock with a strut on screen; `csdclient.cc` plays the borderless game.

### Which monitor a new window opens on

`LScr::PlacementAreaFor(c)` answers it, and `LScr::NextAutoPosition` cascades
within whatever it returns. Normally that's the primary monitor's work area,
as it always was. The exception is a monitor another program has a full-screen
window on: opening a window there would put it either behind the game or on
top of it, so the free monitors are collected and `PrimaryArea` picks the best
of those instead.

Two things make that fall out simply. `PrimaryArea` applies the same rule to
any set of areas it's given (largest, then topmost, then leftmost), so running
it over the free subset returns the primary monitor whenever the primary is
free — there is no separate "is the primary blocked?" test, and none is
needed. And an empty free set means every monitor is taken, where the primary
is as good an answer as any.

`LScr::FullScreenOccupantOf(area)` decides what counts as taken, and covers
both senses of full screen described under "Undecorated windows": a client
which asked for `_NET_WM_STATE_FULLSCREEN` need only cover most of the
monitor, while one which merely sized itself to it has to cover it completely.
The second test is against `ContentRect()`, not `FrameRect()`, and that is
what keeps a *maximised* window out of it: a maximised window's content sits
inside lwm's furniture and inside any panel's strut, so it never covers the
whole monitor, while a borderless-fullscreen one (which `SnapToMonitor` has
put exactly on the monitor) does. Desktop and dock windows, and anything with
a strut, are skipped: filling the screen is their job.

`Client::SameProgramAs` is the exemption, so a game's own dialogs and second
windows still open where the game is. It's a "probably yes" built from
whatever the windows say about themselves — a transient-for link either way, a
shared `WM_CLIENT_LEADER`, a shared `WM_HINTS` window group, or the same
`_NET_WM_PID` on the same `WM_CLIENT_MACHINE` — and nothing obliges a client
to say any of it. That's tolerable only because of where the question is
asked: a wrong "yes" gives the behaviour lwm had before, and a wrong "no" only
opens a window on the next monitor along. Don't reuse it for anything that
can't shrug off both answers.

On an xrandr change, `LScr::SetVisibleAreas` computes every client's new rect
with `MapToNewAreas` *before* swapping in the new geometry, then applies the
moves. `MapToNewAreas` handles, in order: height-maximised windows, windows
against the left/right edge (`mapEdges` → `mapLeftEdge`, with the right edge
handled by mirroring and top/bottom by flipping x/y), then free-floating windows
scaled proportionally into the tallest screen at their new mid-x. Extend
`screenlayout_test.cc` when touching it.

## Undecorated windows

A window can be undecorated for three quite different reasons, and the
difference matters every time you touch code that keys off it. The first two
leave `Client::framed` false; the third only clears `HasFurniture()`.

* **Furniture-free by nature** — `_NET_WM_WINDOW_TYPE_DESKTOP`, `DOCK`, `MENU`
  and `SPLASH`, screened out by `ewmh_hasframe()`. Dragging one is
  meaningless.
* **Ordinary windows lwm chose not to decorate** — shaped windows, and
  anything that draws its own title bar and says so through
  `_MOTIF_WM_HINTS` (the Steam launcher, GTK client-side decorations, Java's
  `setUndecorated(true)`). These are perfectly normal, movable windows.
* **Windows the user undecorated** — the Super+Control click, described below.
  These keep their frame; only the furniture goes.

The second kind is the trap. They have no furniture to drag, so the only two
ways left to move or resize them are the Windows-key gestures and the
`_NET_WM_MOVERESIZE` message the client sends when the user grabs its own
title bar or resize grip. Withhold both and the window is pinned to the
screen for the rest of its life, with no way out — which is exactly what used
to happen. So:

* `Client::GrabSuperButtons()` keys off `ewmh_hasframe()`, **not** `framed`.
* An unframed client — the first two kinds — is never reparented, so
  `c->parent` is still the root,
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

### Turning the furniture on and off

Super+Control+button 1 (`WindowDecorationToggler` in `drag.cc`) calls
`Client::SetFurniture`, which moves a live client between the two states above
— though only the *second* kind of undecorated window, since it refuses when
`ewmh_hasframe()` says no. It also refuses while the window is full screen
(the furniture is already off, and the geometry that means anything belongs to
`ExitFullScreen`) and while it is hidden (the `Hider` is holding it by
whichever window would be mapped or unmapped). What is preserved is
`FrameRect()`, lwm's idea of the outer extent everywhere else (maximisation
and expansion both work in those coordinates): the client window grows into
the space the furniture was using, and shrinks back out of it. The frame's own
1px X border is not accounted for, because it isn't part of `FrameRect()`
either.

**The frame window stays.** `HideFurniture` shrinks it onto the client window
and drops its X border, so the client covers it exactly and nothing of it
shows; `ShowFurniture` grows it back. That is the same shape a frame takes
while its client is full screen, and it's why `FrameRect()` has always had a
case for returning the content rect unchanged. Hence the two predicates:

* `Client::framed` — lwm maintains a frame window for this client, i.e.
  `parent` is that frame rather than the root. Decided in `manage()`, and only
  ever turned *on* afterwards.
* `Client::HasFurniture()` — `framed && furniture_ && !wstate.fullscreen`.
  This is the one the geometry asks: `FrameRect`, `MaximizedRect`,
  `MakeContentRectVisible`, `DrawBorder`, `setShape` and
  `ewmh_set_frame_extents`.

The obvious alternative — reparent the client back out to the root and destroy
the frame — is what this did at first, and it doesn't survive contact with
real clients. **Reparenting a window to the root is the ICCCM's way of saying
"I have stopped managing this window"**, which is what a window manager does
on its way out, and toolkits know it. Java's XAWT logs "WM exited", stops
trusting the geometry it's given, writes off the following `ConfigureNotify`
events as reparenting debris — it decides which by comparing X sequence
numbers, which lwm can neither predict nor control — and then puts the window
back to the last size it was sure of. The visible result was a Java window
that kept its old size and slid up and to the left into the space the title
bar had been in, *sometimes*. GTK and Chromium windows were fine. Not
reparenting removes the question: an unreparented client sees a move and a
resize, the most ordinary thing a window manager can do to it.

One reparent is left, in `ShowFurniture`: a client lwm chose not to decorate
has no frame to grow, so the first time the user decorates it, it gets one.
That path still pays the three costs a reparent has always had.

* **It unmaps the window.** X unmaps a mapped window on its way out of its old
  parent and maps it again afterwards, and the `UnmapNotify` is
  indistinguishable from the client withdrawing its own window — which lwm
  answers by dropping the window. So `Client::ExpectUnmap()` counts the unmaps
  lwm causes and `EvUnmapNotify` ticks them off. `Hider::Hide` uses the same
  counter, because a client with no frame has none to unmap in its place.
* **It restacks the window.** A reparented window goes to the top of its new
  siblings, so the frame is slotted in immediately above the client window
  before the reparent: gaining furniture must not raise the window.
* **It drops the input focus.** The server hands it back to `PointerRoot` when
  the window holding it stops being viewable, and nothing in lwm's focus
  history changed, so `FocusClient()` sees a client which is already focused
  and does nothing. `Focuser::ReassertFocus` is for exactly that case.

`LimitResize` gets the last word on the size, so an xterm rounds the space it
gains or loses to whole character cells — which means the arithmetic isn't
reversible, and doing it in both directions cost such a client a row and a
column per round trip. `pre_undecorated_content_rect_` is the fix: the
geometry the window had when the furniture came off, handed straight back if
`undecorated_content_rect_` says nothing has moved the window since.

Covered by the `Decorations` tests in `drag_test.cc` and the decorations
section of `ui_test.sh`.

## Icons (`xlib::ImageIcon`)

Sourced from `WM_HINTS` pixmaps or `_NET_WM_ICON` pixel data. Three pre-rendered
pixmaps per icon (active title / inactive title / menu background), because
compositing against the final background at creation time avoids per-paint alpha
work. Scaled down only, with simple box-filter anti-aliasing
(`copyWithScaling`); channels are averaged separately to avoid colour bleed.
Cached by content hash with refcounts — `ImageIcon::Create*` returns a `clone()`,
and the last clone destroyed frees the pixmaps. The hash is over the pixels (for
`WM_HINTS` icons, that means reading the pixmap back off the server before the
cache can be consulted) and never over the pixmap ID: X11 recycles resource IDs
once their client has gone, so an ID-keyed entry eventually hands the next
application to be given that ID the previous owner's icon. `Client::SetIcon`
releases the icon it replaces, which matters because `manage()` sets both the
`WM_HINTS` and the `_NET_WM_ICON` one for a window carrying both. 24bpp only.
Tested in `icon_test.cc`, against a `FakeServer` that stores pixmap contents.

## Naming

`Client::Name()` prefers `visible_name_` (`_NET_WM_VISIBLE_NAME`, which the user
sets via `setvisname`/`force_title.sh`) over `name_` (`_NET_WM_NAME`, falling
back to `XA_WM_NAME` for non-UTF8 servers). `MenuName()` truncates to 100 UTF-8
characters. This is the "user is in control" feature: it lets you override a web
app that animates its title.
