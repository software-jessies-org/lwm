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

// Remaps rect, which lived within the monitor layout oldVis, to its
// equivalent position in the new layout newVis: preserving edge-attachment
// (flush against a screen edge stays flush), height-maximisation, and
// otherwise scaling position/size proportionally. Used when xrandr reports a
// layout change.
Rect MapToNewAreas(Rect rect,
                   const std::vector<Rect>& oldVis,
                   const std::vector<Rect>& newVis);

#endif
