#include "navigate.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "screenlayout.h"

int PickWindowInDirection(const Rect& from,
                          const std::vector<Rect>& candidates,
                          Direction dir) {
  const Point origin = from.middle();
  int best = -1;
  int64_t best_dist = 0;
  for (int i = 0; i < (int)candidates.size(); i++) {
    const Point p = candidates[i].middle();
    const int dx = p.x - origin.x;
    const int dy = p.y - origin.y;
    // Which delta the direction is 'along', and which way it must point. The
    // cone test is the same shape for all four directions once we've picked
    // those out; doing it this way keeps the four cases from drifting apart.
    int along = 0;
    int across = 0;
    switch (dir) {
      case Direction::kLeft:
        along = -dx;
        across = dy;
        break;
      case Direction::kRight:
        along = dx;
        across = dy;
        break;
      case Direction::kUp:
        along = -dy;
        across = dx;
        break;
      case Direction::kDown:
        along = dy;
        across = dx;
        break;
    }
    // along > 0 rather than >= 0: a centre level with ours on this axis is
    // not in the cone, and neither is our own centre.
    if (along <= 0 || along < std::abs(across)) {
      continue;
    }
    // Squared distance, to keep this in integers. Widened first: two
    // coordinates a screen's width apart square to more than an int holds.
    const int64_t dist = (int64_t)dx * dx + (int64_t)dy * dy;
    if (best == -1 || dist < best_dist) {
      best = i;
      best_dist = dist;
    }
  }
  return best;
}

namespace {

// The four directions differ only in which axis a window moves along, and
// which way along it. Everything below therefore works on this view of a
// rect - the pair of coordinates along the axis dir names, and the pair
// across it - which is what keeps the four cases from drifting apart in the
// same way PickWindowInDirection's cone test does.
struct Axes {
  int lo;
  int hi;
  int aLo;
  int aHi;
};

bool horizontal(Direction dir) {
  return dir == Direction::kLeft || dir == Direction::kRight;
}

// True for the two directions in which coordinates get smaller.
bool towardsLower(Direction dir) {
  return dir == Direction::kLeft || dir == Direction::kUp;
}

Direction opposite(Direction dir) {
  switch (dir) {
    case Direction::kLeft:
      return Direction::kRight;
    case Direction::kRight:
      return Direction::kLeft;
    case Direction::kUp:
      return Direction::kDown;
    case Direction::kDown:
      return Direction::kUp;
  }
  return dir;  // Unreachable; the switch is exhaustive.
}

Axes pack(const Rect& r, Direction dir) {
  return horizontal(dir) ? Axes{r.xMin, r.xMax, r.yMin, r.yMax}
                         : Axes{r.yMin, r.yMax, r.xMin, r.xMax};
}

Rect unpack(const Axes& a, Direction dir) {
  return horizontal(dir) ? Rect{a.lo, a.aLo, a.hi, a.aHi}
                         : Rect{a.aLo, a.lo, a.aHi, a.hi};
}

// Puts r against the far edge of mon on the side dir names, shrinking it if
// mon is too small to hold it. Across the direction of travel the window
// stays where it was, except that it too must fit on the monitor: a window
// moved to a shorter screen is pulled up onto it rather than left hanging off
// the bottom.
Rect placeAgainstEdge(const Rect& r, const Rect& mon, Direction dir) {
  const Axes w = pack(r, dir);
  const Axes m = pack(mon, dir);
  Axes res;
  const int len = std::min(w.hi - w.lo, m.hi - m.lo);
  if (towardsLower(dir)) {
    res.lo = m.lo;
    res.hi = m.lo + len;
  } else {
    res.hi = m.hi;
    res.lo = m.hi - len;
  }
  const int across = std::min(w.aHi - w.aLo, m.aHi - m.aLo);
  res.aLo = std::max(m.aLo, std::min(w.aLo, m.aHi - across));
  res.aHi = res.aLo + across;
  return unpack(res, dir);
}

// The index in areas of the monitor next to cur in direction dir, or -1 if
// there isn't one. A monitor qualifies if its leading edge is beyond cur's;
// of those, one which shares some of cur's span across the direction of
// travel beats one which doesn't (monitors side by side are neighbours even
// if a third is nearer as the crow flies), then the nearest along the
// direction wins, then the nearest across it.
int monitorInDirection(const Rect& cur,
                       Direction dir,
                       const std::vector<Rect>& areas) {
  const Axes c = pack(cur, dir);
  const int cMid = (c.aLo + c.aHi) / 2;
  int best = -1;
  bool bestOverlaps = false;
  int bestAlong = 0;
  int bestAcross = 0;
  for (int i = 0; i < (int)areas.size(); i++) {
    const Axes a = pack(areas[i], dir);
    const int along = towardsLower(dir) ? (c.lo - a.lo) : (a.hi - c.hi);
    if (along <= 0) {
      continue;  // Not beyond us: either the same monitor, or behind us.
    }
    const bool overlaps = a.aHi > c.aLo && a.aLo < c.aHi;
    const int across = std::abs((a.aLo + a.aHi) / 2 - cMid);
    const bool better =
        best == -1 || (overlaps && !bestOverlaps) ||
        (overlaps == bestOverlaps &&
         (along < bestAlong || (along == bestAlong && across < bestAcross)));
    if (!better) {
      continue;
    }
    best = i;
    bestOverlaps = overlaps;
    bestAlong = along;
    bestAcross = across;
  }
  return best;
}

// One axis of MapPointToMovedRect. The multiplication is done first, and
// widened, so that a window most of a desktop wide doesn't overflow it.
int mapCoord(int p, int fromMin, int fromLen, int toMin, int toLen) {
  if (fromLen <= 0) {
    return toMin;
  }
  return toMin + (int)((int64_t)(p - fromMin) * toLen / fromLen);
}

}  // namespace

Rect MoveRectInDirection(const Rect& frame,
                         Direction dir,
                         const std::vector<Rect>& areas) {
  if (areas.empty()) {
    return frame;
  }
  const Rect cur = findBestScreenFor(frame, areas);
  const Rect edge = placeAgainstEdge(frame, cur, dir);
  if (edge != frame) {
    return edge;  // There was still room to move on this monitor.
  }
  // Already flush against the edge, so this press is the one that hands the
  // window to the next monitor along. It arrives against that monitor's near
  // edge - the one it just crossed - so it ends up beside where it was rather
  // than jumping the whole width of the new monitor. Getting to the far side
  // of that monitor is the next press's job, which is why this is
  // placeAgainstEdge in the *opposite* direction.
  const int next = monitorInDirection(cur, dir, areas);
  if (next < 0) {
    return frame;
  }
  return placeAgainstEdge(frame, areas[next], opposite(dir));
}

Point MapPointToMovedRect(Point p, const Rect& from, const Rect& to) {
  return Point{
      mapCoord(p.x, from.xMin, from.width(), to.xMin, to.width()),
      mapCoord(p.y, from.yMin, from.height(), to.yMin, to.height()),
  };
}

namespace {

// The distinct coordinates, in order, that `r`'s own edges and the occluders'
// edges cut the interval [lo, hi] into. Coordinates outside it are the
// occluders' business, not ours: they've already been clipped away.
std::vector<int> gridLines(int lo, int hi, const std::vector<int>& cuts) {
  std::vector<int> res;
  res.reserve(cuts.size() + 2);
  res.push_back(lo);
  res.push_back(hi);
  for (int c : cuts) {
    res.push_back(c);
  }
  std::sort(res.begin(), res.end());
  res.erase(std::unique(res.begin(), res.end()), res.end());
  return res;
}

// One bar of the histogram sweep below: a run of free space `height` pixels
// deep whose left edge is the grid line `left`.
struct Bar {
  int left;
  int height;
};

}  // namespace

Rect LargestVisibleRect(const Rect& r, const std::vector<Rect>& occluders) {
  const Rect kNone{0, 0, 0, 0};
  if (r.empty()) {
    return kNone;
  }
  // Clip the occluders to r first. Everything after this works within r, and
  // an occluder which only overlapped a corner of it would otherwise drag
  // grid lines outside r along with it.
  std::vector<Rect> obs;
  std::vector<int> xCuts;
  std::vector<int> yCuts;
  for (const Rect& o : occluders) {
    const Rect clipped = Rect::Intersect(o, r);
    if (clipped.empty()) {
      continue;
    }
    obs.push_back(clipped);
    xCuts.push_back(clipped.xMin);
    xCuts.push_back(clipped.xMax);
    yCuts.push_back(clipped.yMin);
    yCuts.push_back(clipped.yMax);
  }
  if (obs.empty()) {
    return r;  // Nothing in the way: the whole window is the answer.
  }
  const std::vector<int> xs = gridLines(r.xMin, r.xMax, xCuts);
  const std::vector<int> ys = gridLines(r.yMin, r.yMax, yCuts);
  const int nx = (int)xs.size() - 1;  // Columns of the grid.
  const int ny = (int)ys.size() - 1;  // Rows of it.

  // Which cells of the grid an occluder covers. A cell is covered or it isn't:
  // the grid lines were chosen so that no occluder edge can pass through the
  // middle of one.
  std::vector<bool> covered(nx * ny, false);
  for (const Rect& o : obs) {
    const int x0 = std::lower_bound(xs.begin(), xs.end(), o.xMin) - xs.begin();
    const int x1 = std::lower_bound(xs.begin(), xs.end(), o.xMax) - xs.begin();
    const int y0 = std::lower_bound(ys.begin(), ys.end(), o.yMin) - ys.begin();
    const int y1 = std::lower_bound(ys.begin(), ys.end(), o.yMax) - ys.begin();
    for (int row = y0; row < y1; row++) {
      for (int col = x0; col < x1; col++) {
        covered[row * nx + col] = true;
      }
    }
  }

  // The largest free rectangle in the grid, row by row from the top. `heights`
  // is how far up the free space above each column reaches from the bottom of
  // the current row, measured in pixels rather than in cells so that the area
  // compared is the real one; a rectangle of free cells is then a run of
  // columns taken to the depth of the shallowest of them, which is the classic
  // largest-rectangle-in-a-histogram problem.
  //
  // The stack holds the bars still capable of growing wider, always increasing
  // in height from the bottom of the stack up. A shallower column closes off
  // every bar deeper than itself: each is popped and measured, and the last
  // one popped hands its left edge to the new bar, because the space it
  // occupied is free to the new bar's lesser depth as well.
  std::vector<int> heights(nx, 0);
  std::vector<Bar> stack;
  Rect best = kNone;
  int64_t bestArea = 0;
  for (int row = 0; row < ny; row++) {
    const int rowHeight = ys[row + 1] - ys[row];
    for (int col = 0; col < nx; col++) {
      heights[col] = covered[row * nx + col] ? 0 : heights[col] + rowHeight;
    }
    const int bottom = ys[row + 1];
    stack.clear();
    // One past the last column, with nothing in it, so that the columns still
    // on the stack when the row ends are measured too.
    for (int col = 0; col <= nx; col++) {
      const int h = (col < nx) ? heights[col] : 0;
      int left = col;
      while (!stack.empty() && stack.back().height >= h) {
        const Bar bar = stack.back();
        stack.pop_back();
        const Rect cand{xs[bar.left], bottom - bar.height, xs[col], bottom};
        const int64_t area = (int64_t)cand.width() * cand.height();
        if (area > bestArea) {
          bestArea = area;
          best = cand;
        }
        left = bar.left;
      }
      if (h > 0) {
        stack.push_back(Bar{left, h});
      }
    }
  }
  return best;
}
