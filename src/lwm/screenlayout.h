#ifndef SCREENLAYOUT_H_included
#define SCREENLAYOUT_H_included

#include <vector>

#include "geometry.h"
#include "strut.h"

// Clips each area in `in` to the bounding box of `in` shrunk by `strut` on
// each edge. Used to turn "all monitor areas" into "all monitor areas minus
// the space reserved by panels/launchers".
std::vector<Rect> areasMinusStruts(std::vector<Rect> in, EWMHStrut strut);

// Picks the "primary" area out of a set of visible areas: the largest by
// pixel count, ties broken by topmost then leftmost.
Rect PrimaryArea(const std::vector<Rect>& areas);

// Finds the area in `areas` that `r` overlaps the most; if it overlaps none
// of them, the one closest to it.
Rect findBestScreenFor(const Rect& r, const std::vector<Rect>& areas);

// Translates and/or clips r so that it's entirely within some area in
// `areas` (specifically, the one findBestScreenFor picks).
Rect makeVisible(Rect r, const std::vector<Rect>& areas);

// If `r` is exactly the size of one of the monitors in `areas`, or of that
// monitor's work area (the monitor minus `strut`), and `r` already covers most
// of that monitor, returns the monitor's rectangle. Otherwise returns `r`
// unchanged. It never moves a window to a monitor it wasn't mostly on already.
//
// This exists for undecorated windows that go full screen by sizing themselves
// to a monitor rather than by asking for _NET_WM_STATE_FULLSCREEN - which is
// what "borderless fullscreen" means in every game's display settings. Their
// arithmetic is their own, and it can be wrong, because what a game under
// Proton has to hand is the Windows work area, which Wine derives from the
// panel struts lwm publishes in _NET_WORKAREA. So such a game lands its window
// either the height of a top panel's strut above the monitor (the right size,
// the wrong place) or filling the work area exactly (the wrong size as well,
// which is what you get if the game is hidden and restores itself as a
// maximised window rather than a full-screen one). Since neither request can
// have been meant as anything but "cover that monitor", cover that monitor:
// the work-area case is grown over the panel as well as moved.
//
// `areas` should be the visible areas *without* struts subtracted: a window
// covering a monitor covers the panels too.
Rect SnapToMonitor(const Rect& r,
                   const std::vector<Rect>& areas,
                   const EWMHStrut& strut);

// The monitor a drag is over. That's the one the *pointer* is in, not the one
// the window overlaps most: the window can be far bigger than the pointer's
// travel and is dragged from wherever the user grabbed it, so a wide window
// grabbed near one edge stays majority-over the monitor it came from long
// after the user has dragged it onto the next one. Asking where the pointer
// is makes "drag it onto that monitor" mean what it says, whatever the size of
// the window or where it was picked up.
//
// If the pointer is over no monitor at all - the dead space beside a monitor
// shorter than its neighbour, which the pointer can cross - the window's own
// monitor (findBestScreenFor) is the answer, so that passing through a gap
// changes nothing.
Rect findDragScreen(Point pointer,
                    const Rect& r,
                    const std::vector<Rect>& areas);

// As ShrinkToFitMonitor below, but against a monitor the caller has already
// picked (a drag picks it with findDragScreen).
Rect ShrinkToFitGivenMonitor(Rect r, const Rect& mon);

// Shrinks r, if it's too big for the monitor it's on (the one
// findBestScreenFor picks), until it fits, and slides it onto that monitor on
// whichever axis it had to shrink on. An axis which already fitted is left
// exactly where it was, so a window may still be dragged half off the side of
// a screen; only one which cannot fit gets touched.
//
// An axis that does shrink ends up exactly the monitor's size on that axis,
// so there is nowhere left for it to sit but flush against that edge: a
// window dragged onto a monitor too small for it snaps to the corner it can
// fit in. Nothing here can keep it under the mouse pointer, because nothing
// here has any freedom left to place it with.
//
// This is what happens when a window is dragged onto a monitor smaller than
// it is. Note that it only ever shrinks: dragging the window back to the big
// monitor doesn't grow it again, so callers which want that (the mover does)
// must work from the size the window started the drag at rather than from
// where it has got to.
Rect ShrinkToFitMonitor(Rect r, const std::vector<Rect>& areas);

// Remaps rect, which lived within the monitor layout oldVis, to its
// equivalent position in the new layout newVis: preserving edge-attachment
// (flush against a screen edge stays flush), height-maximisation, and
// otherwise scaling position/size proportionally. Used when xrandr reports a
// layout change.
Rect MapToNewAreas(Rect rect,
                   const std::vector<Rect>& oldVis,
                   const std::vector<Rect>& newVis);

#endif
