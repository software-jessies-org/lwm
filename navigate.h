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

#endif
