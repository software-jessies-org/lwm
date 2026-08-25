#ifndef NAVIGATE_H_included
#define NAVIGATE_H_included

#include <vector>

#include "geometry.h"

// The pure part of keyboard focus navigation: given the window which has
// focus and every window which could take it, which one does an arrow key
// pick? The grabs and the focus change itself live in disp.cc; lwm.man
// describes what the user sees.

// Which arrow was pressed.
enum class Direction { kLeft, kRight, kUp, kDown };

// Returns the index into candidates of the window focus should move to when
// travelling in dir from the window occupying from, or -1 if there is no
// window that way.
//
// Windows are compared by the centres of their rectangles, and a candidate
// only qualifies if its centre lies in the quarter-plane cone whose point is
// the centre of from and which opens towards dir: the delta along the axis
// dir names must have the right sign, and its magnitude must be at least that
// of the delta along the other axis. So Direction::kLeft wants a centre to the
// left (dx < 0) which is no further away vertically than it is horizontally
// (|dx| >= |dy|). Of those that qualify, the one whose centre is nearest wins.
//
// A candidate exactly on the cone's diagonal belongs to two directions at
// once, which is deliberate: it keeps the four cones between them covering the
// whole plane, so no window is unreachable. Ties in distance go to the
// earlier candidate, which makes the choice depend only on the order the
// caller supplies.
//
// A candidate whose centre coincides with from's is never picked - the strict
// sign test excludes it - so passing the focused window in the list costs
// nothing but is not required.
int PickWindowInDirection(const Rect& from,
                          const std::vector<Rect>& candidates,
                          Direction dir);

// Returns the rectangle a window occupying `frame` should be given when the
// user asks for it to be moved in dir (Super+Shift+arrow). `areas` is the
// monitor layout, with struts subtracted: a window moved to the inner edge of
// its monitor should stop at a panel, not slide under one.
//
// The window travels one step at a time. The first step takes it to the inner
// edge of the monitor it's on - flush against that monitor's edge on the side
// dir names, keeping its position on the other axis. Once it's there, the
// next step hands it to the next monitor that way, where it lands against
// that monitor's *near* edge: the one it has just crossed, so that it ends up
// beside where it was rather than jumping the width of the new monitor in one
// go. The step after that takes it across to the far side of its new monitor,
// and so on, so repeating the gesture walks a window across the desk edge by
// edge rather than shuffling it about within one screen.
//
// A window too big for the monitor it lands on is shrunk to fit it, on either
// axis or both. That only happens on arrival: nothing here grows a window
// which has been shrunk, so a trip to a small monitor and back leaves the
// window the size the small monitor allowed.
//
// Returns `frame` unchanged when there's nowhere to go: no monitors at all,
// or the window is already at the edge of the last monitor in that direction.
Rect MoveRectInDirection(const Rect& frame,
                         Direction dir,
                         const std::vector<Rect>& areas);

// Where a point which was at p within the rectangle `from` should be once
// that rectangle has become `to`. Used to carry the mouse pointer along with
// a window moved by MoveRectInDirection: it keeps its place on the window,
// proportionally, so that a window which shrank on its way to a smaller
// monitor still has the pointer over the same part of itself.
//
// Rounding means "the same rough position", not the same pixel: a window
// which is moved and moved back can leave the pointer a pixel from where it
// started. An empty `from` (a zero-width or zero-height rectangle, which no
// real window has) has no proportions to preserve, so the point comes back at
// the corresponding corner of `to`.
Point MapPointToMovedRect(Point p, const Rect& from, const Rect& to);

#endif
