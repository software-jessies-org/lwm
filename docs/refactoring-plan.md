# Refactoring and testing plan

A staged plan to break the remaining spaghetti into testable units, and to add
a minimal in-tree unit test framework. This continues the CLEANUPS list in
`../BUGS` rather than replacing it.

## The core problem

`lwm.h` is a god header. 864 lines holding `Client`, `LScr`, `Focuser`,
`Hider`, `CursorMap`, `Resources`, `DebugCLI`, `DragHandler`, the EWMH value
types, and a block of `extern`s for every cross-file function *grouped by which
`.cc` owns it*. Every `.cc` includes it, and it includes `xlib.h`, which drags
in all of X11.

Two consequences:

1. Everything depends on everything. There is no compilation firewall and no
   meaningful module boundary — the "interfaces between components aren't
   well-defined" note in `../BUGS` is really a symptom of this one file.
2. Nothing can be unit-tested without linking the whole program and, for most
   of it, having a live X display. That's why `tests.cc` covers exactly one
   function: `MapToNewAreas` happens to be the only interesting thing that
   doesn't touch X.

Everything below follows from fixing that.

## Organising principle: split by dependency level, not by topic

Three tiers. A file may only depend on its own tier and the ones below.

| Tier | Contents | Includes X11? | Testable |
| --- | --- | --- | --- |
| 0 — pure | geometry, size limits, layout maths, placement policy, string utils | **no** | now, with no fakes |
| 1 — X shim | `namespace xlib`, extended to cover everything, then made an interface | yes | n/a (it *is* the boundary) |
| 2 — WM logic | `Client`, `LScr`, `Focuser`, `Hider`, event dispatch, `manage` | yes, via tier 1 | once tier 1 has a fake |

Tier 0 is where the payoff is, and it's bigger than it looks: a lot of the
decision-making currently sits inside X-coupled functions purely because it
reads a global (`LScr::I`, `Resources::I`, `borderWidth()`, `textHeight()`)
rather than because it needs the X server.

## What's currently in the wrong place

Independent of the tiering, several things live where they do for historical
reasons only:

| Code | Currently in | Belongs in |
| --- | --- | --- |
| All of `Focuser` (~180 lines) | `client.cc` | `focus.cc` |
| All of `Hider` (~250 lines) | `mouse.cc` | `hider.cc` |
| `getMousePosition()` | `mouse.cc` | `xlib.cc` (it's an X query) |
| `MapToNewAreas` + `mirrorX`/`flipXY`/`tallestScreenAtX`/… (~280 lines) | `screen.cc` | `screenlayout.cc` (tier 0) |
| `findBestScreenFor`, `makeVisible`, `absDist` | `disp.cc` | `screenlayout.cc` (tier 0) |
| All 8 `DragHandler` subclasses (~250 lines) | `disp.cc` | `drag.cc` |
| `NextAutoPosition` | `manage.cc` | `placement.cc` (tier 0) |
| `menuItemHeight`/`menuIconYPad`/`menu*` arithmetic | `mouse.cc` | `menulayout.cc` (tier 0) |
| `closeBounds`, `titleBarBounds`, `titleBarHeight`, `EdgeBounds` | `client.cc` | `framegeometry.cc` (tier 0) |
| `textHeight`/`textWidth`/`drawString` | `lwm.cc` (with `main`) | `xfont.cc` |
| `Split()` | `lwm.cc` | `strings.cc` (tier 0) |
| ICCCM atom globals (`wm_state`, `wm_delete`, …) | `lwm.cc` + `lwm.h` externs | `atoms.cc`, alongside the EWMH ones |
| `operator<<` for X event structs | scattered in `disp.cc` | `xdebugprint.cc`, or next to the type |

`disp.cc` drops from 1067 to roughly 550 lines (dispatch + `Ev*` only);
`client.cc` from 804 to roughly 450 (`Client` only); `screen.cc` from 650 to
roughly 300 (`LScr` only).

## Target file layout

One header per `.cc`, each declaring only what that `.cc` provides. `lwm.h`
ends up deleted, or reduced to an umbrella that includes the handful of headers
`main()` needs.

**Tier 0 (no X11 headers at all):**

```
geometry.{h,cc}      Point / Area / Rect            (exists; trim to this)
edge.{h,cc}          Edge enum + isLeftEdge etc     (out of geometry.h)
sizelimits.{h,cc}    DimensionLimiter               (out of geometry)
strings.{h,cc}       Split, UTF-8 truncation
strut.h              EWMHStrut + its operations
screenlayout.{h,cc}  MapToNewAreas, makeVisible, findBestScreenFor,
                     areasMinusStruts, primary-area selection
placement.{h,cc}     auto-placement policy
framegeometry.{h,cc} frame<->content conversion, edge/close/titlebar bounds
menulayout.{h,cc}    unhide menu item geometry
```

**Tier 1:**

```
xlib.{h,cc}          extended shim; later split into Server interface,
                     RealServer, FakeServer
xfont.{h,cc}         text metrics and drawing
atoms.{h,cc}         ICCCM + EWMH atom interning
```

**Tier 2:** `client`, `focus`, `hider`, `screen`, `dispatch`, `drag`, `manage`,
`ewmh`, `resource`, `session`, `shape`, `cursor`, `debugcli`, `error`, `log`,
each with its own header; `lwm.cc` reduced to `main()` plus the `select()`
loop.

## Making tier-0 code actually pure

The extractions above are blocked by global reads. Three fixes cover nearly all
of it:

**`FrameStyle`** — a small value type holding `border_width`,
`top_border_width`, `text_height`, built once from `Resources::I` and the Xft
font, then passed by const reference into the geometry functions. This is what
lets `framegeometry` and `menulayout` leave tier 2. Today `Client::EdgeBounds`,
`closeBounds`, `titleBarBounds` and `ContentFromFrameRect` all call the free
functions `borderWidth()`/`textHeight()`, which reach for globals.

**Pass the visible areas in.** `findBestScreenFor` and `makeVisible` call
`LScr::I->VisibleAreas(withStruts)` internally. Taking
`const std::vector<Rect>&` instead makes them trivially testable, and
`MapToNewAreas` already demonstrates the pattern works.

**Make `NextAutoPosition`'s state explicit.** It's a free function with two
function-local `static unsigned int`s. Turn it into a tiny `AutoPlacer` class
holding `next_x_`/`next_y_`, owned by `LScr`. Same behaviour, and the
"reset when the monitor layout changed" branch becomes testable.

While there: `Rect::Parse` signals failure by returning the empty rect, which
is indistinguishable from successfully parsing a zero-size rect (it explicitly
refuses `w == 0 || h == 0` for this reason). `std::optional<Rect>` is the
honest signature and removes the `if (rect.empty()) return {};` dance in
`tests.cc` and `debug.cc`.

## The test framework

In-tree, no external dependency, target under ~250 lines across `test.h` and
`test.cc`. Design:

**Self-registration.** `TEST(Suite, Name) { ... }` expands to a function plus a
file-scope registrar object that pushes it onto a global vector at static-init
time. No central list to update, so `tests.cc` stops being a bottleneck and
tests live next to what they test (`geometry_test.cc`, `screenlayout_test.cc`,
…).

**Assertions that stream.** `EXPECT_EQ(a, b)`, `EXPECT_NE`, `EXPECT_TRUE`,
`EXPECT_FALSE`, `EXPECT_NEAR`, plus `ASSERT_*` variants that abandon the
current test. Each returns a temporary whose destructor reports on failure, so
extra context streams in naturally:

```cpp
EXPECT_EQ(got, want) << "case " << tc.name;
```

This is worth doing rather than the current bare `LOGE()`, because the codebase
already has `operator<<` for `Rect`, `Point`, `Area`, `Client`, `WinID`,
`AtomName` and `EWMHWindowState` — so readable failure output is free.

Sketch:

```cpp
namespace testing {

// Returned by the EXPECT_ macros. Reports on destruction if failed.
class Result {
 public:
  Result(bool ok, const char* file, int line, const std::string& what);
  ~Result();  // records the failure and prints buf_, if !ok_.
  template <typename T>
  Result& operator<<(const T& t) {
    if (!ok_) buf_ << t;
    return *this;
  }
 private:
  bool ok_;
  ...
};

template <typename A, typename B>
Result CheckEq(const A& a, const B& b, const char* file, int line,
               const char* expr) {
  std::ostringstream os;
  if (!(a == b)) os << expr << ": got " << a << ", want " << b;
  return Result(a == b, file, line, os.str());
}

}  // namespace testing

#define EXPECT_EQ(a, b) \
  testing::CheckEq(a, b, __FILE__, __LINE__, #a " == " #b)
```

Wrapping the macro body in a returned object rather than an `if` also retires
the warning at the top of `tests.cc` about the macros being unsafe inside
braceless `if` statements.

**Runner.** `testing::RunAll(const std::string& filter)` returns bool, prints
one line per test and a `N passed, M failed` summary. A substring filter makes
iterating on a single test cheap.

**Table-driven cases** keep working as they do now, but the `for` loop body
gets a `testing::Context ctx(tc.name)` whose destructor pops a label stack that
`Result` prefixes onto failures — cheaper than threading `tc.name` into every
assertion.

**Build.** Keep `./lwm -test` working during the transition. Once tier 0
exists, add a second target linking only tier-0 sources, the framework, and the
`*_test.cc` files — no X11, no display, sub-second. Add to `Imakefile`
alongside `ComplexProgramTarget(lwm)`; `Makefile.freebsd` and
`no_xmkmf_makefile` are already stale (`-std=c++14`, missing `geometry.cc` in
one) and should either be fixed in the same change or deleted.

## What to test first

Ordered by value-per-effort. Everything in group 1 needs only tier 0.

**1. Pure logic, currently untested and genuinely subtle:**

* `DimensionLimiter::Limit` — the "adjust whichever end moved" rule is the
  single most behaviour-critical untested function in the tree; it's what makes
  resizing an xterm from the left edge work. Also note the dead
  `old_max = old_max;` self-assignment at the end.
* `DimensionLimiter::DisplayableSize` — base/increment arithmetic.
* `MenuName` UTF-8 truncation — hand-rolled multi-byte scanning with a
  non-obvious `if (uniLeft && --uniLeft)` continue; test 4-byte sequences
  straddling the 100-character boundary.
* `Rect::Parse` — negative offsets (`50x50-5+45`), malformed input, the
  `w == 0` refusal.
* `makeVisible` / `findBestScreenFor` — the "no overlapping screen, pick the
  nearest" path is exercised in the wild by ImageMagick `display` (see the
  commit that added it) and by nothing else.
* Auto-placement — the centre-instead-of-cascade branches and the wrap-around.
* Frame/content conversions and `EdgeAt` ordering — especially that
  `closeBounds(false)` is deliberately larger than `closeBounds(true)`, and the
  `-1`/`+1` fudges in `EdgeBounds`.
* Edge resistance (`getResistanceOffset` plus the "only apply the first spotted
  correction" loop in `WindowMover::moveImpl`) — the multi-monitor
  double-correction bug that comment describes is a regression test waiting to
  be written.
* `new_state()` in `ewmh.cc` — `_NET_WM_STATE` add/remove/toggle.
* `areasMinusStruts` and `GetPrimaryVisibleArea`'s size-then-y-then-x
  tie-break.
* `Split`.
* `MapToNewAreas` — port the existing table across unchanged.

**2. Needs the fake X server (after phase E):**

* `EvConfigureRequest`'s offset arithmetic — the Nautilus hack. Assert on the
  sequence of `XConfigureWindow` calls the fake records. This is the highest-
  risk code in the tree and currently can only be tested by dragging a Nautilus
  window around by hand.
* `Focuser` — history ordering, `UnfocusClient` handing over to the next in
  the list, and the timerfd deferral (inject the clock; `GetTimeMilliseconds`
  is already a free function in `client.cc`, so it just needs to become
  injectable). The three-window A→B→C race is otherwise untestable.
* Client lifecycle — `manage` → hide → unhide → destroy, asserting `clients_`,
  `parents_` and `focus_history_` stay consistent and free of dangling
  pointers.
* `fix_stack()` ordering, and its recursion guard.

## Sequencing

Each phase is independently shippable and leaves the tree building.

**A. Framework first, no production changes.** Add `test.{h,cc}`, port
`tests.cc` to it, then write tests for what is *already* pure:
`DimensionLimiter`, `Rect::Parse`, `Split`, `MenuName`. Immediate value, zero
risk, and it proves the framework before anything depends on it.

**B. Extract tier 0.** Introduce `FrameStyle`, de-globalise the functions
listed above, and move them into the new pure files. Behaviour-preserving
moves; add the group-1 tests as each piece lands. Do this before the header
split — it's much easier to move code while `lwm.h` still declares everything.

**C. Split `lwm.h`.** One header per `.cc`; delete the extern-blocks-by-owning-
file. Mechanical and noisy, but the pure code is already out by now so the
remaining graph is smaller. Expect `-Werror` to find a few files that were
relying on transitive includes.

**D. Relocate the misplaced classes.** `Focuser` → `focus.cc`, `Hider` →
`hider.cc`, the `DragHandler`s → `drag.cc`, event `operator<<`s out of
`disp.cc`. Cheap once C is done.

**E. Complete and invert the xlib shim.** This is the BUGS list's steps 1–3,
and the big one. First route the remaining ~140 direct X calls through
`namespace xlib` (client.cc 42, manage.cc 23, ewmh.cc 22, screen.cc 21,
lwm.cc 17, disp.cc 16, mouse.cc 15) — this needs new wrappers for properties,
atoms, hints, GCs and event sending, not just window ops. Then turn the
namespace into a `Server` interface with `RealServer` and a `FakeServer` that
records calls and answers queries from a scripted window tree. Then write the
group-2 tests.

**F. Tighten `Client`.** Its public/private sections currently seesaw (there's
an empty `private:` immediately followed by `public:` at `lwm.h:150`), and
`window`, `parent`, `trans`, `framed`, `hidden`, `proto`, `accepts_focus`,
`cursor`, `wtype`, `wstate` and `strut` are all public. Put them behind
accessors, one at a time, with the tests from E as the safety net. Deliberately
last: it touches every file, and it's the change most likely to be abandoned
half-done if attempted first.

## Things not to break

Re-read `concepts.md` before touching any of these; the comments in the code
naming specific applications are regression notes.

* `Focuser::ReallyFocusClient`'s three paths. Chrome breaks if you focus its
  children as well as the top level; Java breaks if you don't.
* `EvConfigureRequest`'s frame-relative offset arithmetic. Test with Nautilus.
* `fix_stack()`'s recursion guard.
* The `Hider` highlight box being four override-redirect windows around a
  `GXxor` menu GC.
* `ImageIcon`'s refcounted pixmap cache.
