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

// If `r` is exactly the size of one of the monitors in `areas`, and already
// covers most of that monitor, returns `r` translated onto the monitor's
// origin. Otherwise returns `r` unchanged. Never resizes, and never moves a
// window to a monitor it wasn't mostly on already.
//
// This exists for undecorated windows that go full screen by sizing themselves
// to a monitor rather than by asking for _NET_WM_STATE_FULLSCREEN - which is
// what "borderless fullscreen" means in every game's display settings. Their
// arithmetic is their own, and it can be wrong: a game under Proton positions
// such a window using the Windows work area, and lands it exactly the height
// of a top panel's strut above the monitor. Since the request can only have
// been meant to cover the monitor, put it on the monitor.
//
// `areas` should be the visible areas *without* struts subtracted: a window
// covering a monitor covers the panels too.
Rect SnapToMonitor(const Rect& r, const std::vector<Rect>& areas);

// Remaps rect, which lived within the monitor layout oldVis, to its
// equivalent position in the new layout newVis: preserving edge-attachment
// (flush against a screen edge stays flush), height-maximisation, and
// otherwise scaling position/size proportionally. Used when xrandr reports a
// layout change.
Rect MapToNewAreas(Rect rect,
                   const std::vector<Rect>& oldVis,
                   const std::vector<Rect>& newVis);

#endif
