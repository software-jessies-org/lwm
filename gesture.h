#ifndef GESTURE_H_included
#define GESTURE_H_included

#include <cstdint>
#include <vector>

#include "edge.h"
#include "geometry.h"

// The pure parts of the 'Windows' key mouse gestures: which edge a click
// picks, where an edge can expand to, and what counts as a double click.
// The handlers that use these live in drag.cc, and lwm.man describes what the
// user sees.

// Returns the Edge naming the cell of r's 3x3 grid which contains p: the
// corner cells give corner Edges, the side cells give side Edges, and the
// centre cell gives ENone (which the gestures read as "no particular edge").
// A point outside r counts as being in the nearest cell, and an axis with no
// extent at all is treated as centred on that axis.
Edge NineGridEdgeAt(const Rect& r, Point p);

// Expands the edges of r named by edge - or all four of them, if edge is
// ENone - outwards, each one until it meets either the facing edge of the
// nearest rectangle in obstacles which stands in its way, or the edge of the
// monitor r is on, whichever it reaches first.
//
// An obstacle only blocks an edge if it lies wholly beyond that edge and
// overlaps r on the other axis, so a window level with r but off to one side
// doesn't stop it growing. r itself must not be in obstacles; pass an empty
// list to expand to the monitor regardless of what else is on screen.
//
// Edges only ever move outwards: if r already extends past the monitor it's
// on, expanding it will not pull it back in. monitors may be empty, in which
// case only the obstacles constrain the result.
Rect ExpandRect(const Rect& r,
                Edge edge,
                const std::vector<Rect>& obstacles,
                const std::vector<Rect>& monitors);

// DoubleClickTracker turns a stream of button presses into "was that a double
// click?" answers. X has no notion of a double click at all: it's two presses
// of the same button on the same window, close together in both time and
// space, and every client that wants one has to decide that for itself.
class DoubleClickTracker {
 public:
  // X timestamps are in milliseconds.
  static constexpr uint32_t kIntervalMillis = 400;
  // How far the pointer may drift between the two presses. Without some
  // slack, a double click on a high-resolution mouse is easy to miss.
  static constexpr int kSlopPixels = 4;

  // Records a press and returns true if it completed a double click. Doing so
  // consumes both presses, so a third click starts counting afresh rather
  // than reporting a second double click.
  bool IsDoubleClick(unsigned int button,
                     uint32_t window,
                     Point p,
                     uint32_t time_millis);

 private:
  bool have_press_ = false;
  unsigned int button_ = 0;
  uint32_t window_ = 0;
  Point pos_ = {};
  uint32_t time_ = 0;
};

#endif
