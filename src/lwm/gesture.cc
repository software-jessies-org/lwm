#include "gesture.h"

#include <algorithm>
#include <cstdlib>

#include "screenlayout.h"

namespace {

// Which third of a span of the given length the offset falls in: 0, 1 or 2.
// Done by multiplication rather than division so that the boundaries land
// exactly on thirds however awkward the length is, and so that a zero length
// (which would otherwise divide by zero) can answer "the middle".
int thirdOf(int offset, int length) {
  if (length <= 0) {
    return 1;
  }
  if (offset < 0) {
    return 0;
  }
  if (3 * offset < length) {
    return 0;
  }
  if (3 * offset < 2 * length) {
    return 1;
  }
  return 2;
}

// A barrier an edge can expand to. Nothing in the way at all is a real
// possibility (no monitors known, no other windows), and is not the same
// thing as a barrier at coordinate zero.
struct Barrier {
  bool found = false;
  int at = 0;

  // Keeps the barrier nearest the edge being expanded. 'outwards' is +1 for
  // an edge growing towards larger coordinates and -1 for one growing
  // towards smaller, which is the only difference between the two cases.
  void Consider(int candidate, int outwards) {
    if (!found || candidate * outwards < at * outwards) {
      found = true;
      at = candidate;
    }
  }
};

bool overlapsVertically(const Rect& a, const Rect& b) {
  return a.yMin < b.yMax && b.yMin < a.yMax;
}

bool overlapsHorizontally(const Rect& a, const Rect& b) {
  return a.xMin < b.xMax && b.xMin < a.xMax;
}

}  // namespace

Edge NineGridEdgeAt(const Rect& r, Point p) {
  static const Edge kGrid[3][3] = {
      {ETopLeft, ETop, ETopRight},
      {ELeft, ENone, ERight},
      {EBottomLeft, EBottom, EBottomRight},
  };
  const int col = thirdOf(p.x - r.xMin, r.width());
  const int row = thirdOf(p.y - r.yMin, r.height());
  return kGrid[row][col];
}

Rect ExpandRect(const Rect& r,
                Edge edge,
                const std::vector<Rect>& obstacles,
                const std::vector<Rect>& monitors) {
  // ENone means the click was in the middle of the 3x3 grid, which asks for
  // every edge to be expanded rather than none of them.
  const bool all = edge == ENone;
  const bool left = all || isLeftEdge(edge);
  const bool right = all || isRightEdge(edge);
  const bool top = all || isTopEdge(edge);
  const bool bottom = all || isBottomEdge(edge);

  Barrier lb, rb, tb, bb;
  if (!monitors.empty()) {
    // Only the monitor the window is already on: expanding across a gap onto
    // the next monitor is never what the user meant.
    const Rect mon = findBestScreenFor(r, monitors);
    lb.Consider(mon.xMin, -1);
    rb.Consider(mon.xMax, 1);
    tb.Consider(mon.yMin, -1);
    bb.Consider(mon.yMax, 1);
  }
  for (const Rect& o : obstacles) {
    if (overlapsVertically(r, o)) {
      if (o.xMax <= r.xMin) {
        lb.Consider(o.xMax, -1);
      }
      if (o.xMin >= r.xMax) {
        rb.Consider(o.xMin, 1);
      }
    }
    if (overlapsHorizontally(r, o)) {
      if (o.yMax <= r.yMin) {
        tb.Consider(o.yMax, -1);
      }
      if (o.yMin >= r.yMax) {
        bb.Consider(o.yMin, 1);
      }
    }
  }

  // std::min/max are what stop an edge from moving inwards: a window which is
  // already hanging off the side of its monitor stays that size.
  Rect res = r;
  if (left && lb.found) {
    res.xMin = std::min(r.xMin, lb.at);
  }
  if (right && rb.found) {
    res.xMax = std::max(r.xMax, rb.at);
  }
  if (top && tb.found) {
    res.yMin = std::min(r.yMin, tb.at);
  }
  if (bottom && bb.found) {
    res.yMax = std::max(r.yMax, bb.at);
  }
  return res;
}

bool DoubleClickTracker::IsDoubleClick(unsigned int button,
                                       uint32_t window,
                                       Point p,
                                       uint32_t time_millis) {
  // Unsigned arithmetic, because X timestamps wrap around roughly every 49
  // days and a subtraction that wraps with them gives the right answer.
  const bool matches =
      have_press_ && button == button_ && window == window_ &&
      (time_millis - time_) <= kIntervalMillis &&
      std::abs(p.x - pos_.x) <= kSlopPixels &&
      std::abs(p.y - pos_.y) <= kSlopPixels;
  if (matches) {
    have_press_ = false;
    return true;
  }
  have_press_ = true;
  button_ = button;
  window_ = window;
  pos_ = p;
  time_ = time_millis;
  return false;
}
