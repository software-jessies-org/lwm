#include "screenlayout.h"

#include <algorithm>
#include <climits>

std::vector<Rect> areasMinusStruts(std::vector<Rect> in, EWMHStrut strut) {
  // First, derive the width and height, and subtract the struts from them.
  int xMax = 0;
  int yMax = 0;
  for (const Rect& r : in) {
    if (r.xMax > xMax) {
      xMax = r.xMax;
    }
    if (r.yMax > yMax) {
      yMax = r.yMax;
    }
  }
  // Find [xy]M(in|ax), representing the total bounding box we have to clip all
  // our screens to.
  xMax -= strut.right;
  yMax -= strut.bottom;
  const int xMin = strut.left;
  const int yMin = strut.top;
  std::vector<Rect> res;
  for (const Rect& r : in) {
    res.push_back(Rect{std::max(xMin, r.xMin), std::max(yMin, r.yMin),
                       std::min(xMax, r.xMax), std::min(yMax, r.yMax)});
  }
  return res;
}

Rect PrimaryArea(const std::vector<Rect>& areas) {
  Rect res{0, 0, 0, 0};
  for (const Rect& r : areas) {
    if (r.area().num_pixels() > res.area().num_pixels()) {
      res = r;
    } else if (r.area().num_pixels() == res.area().num_pixels()) {
      // Two screens of the same size: we pick the one furthest up, and furthest
      // to the left.
      if (r.yMin < res.yMin) {
        res = r;
      } else if (r.yMin == res.yMin) {
        if (r.xMin < res.xMin) {
          res = r;
        }
      }
    }
  }
  return res;
}

int quantise(int dimension, int increment) {
  if (increment < 2) {
    return dimension;
  }
  return dimension - (dimension % increment);
}

int xMaxFrom(const std::vector<Rect>& rs) {
  int res = 0;
  for (const Rect& r : rs) {
    res = std::max(res, r.xMax);
  }
  return res;
}

Rect mirrorX(const Rect& r, int xMax) {
  return Rect{xMax - r.xMax, r.yMin, xMax - r.xMin, r.yMax};
}

std::vector<Rect> mirrorAllX(const std::vector<Rect>& rs, int xMax) {
  std::vector<Rect> res;
  for (const Rect& r : rs) {
    res.push_back(mirrorX(r, xMax));
  }
  return res;
}

Rect flipXY(const Rect& r) {
  return Rect{r.yMin, r.xMin, r.yMax, r.xMax};
}

std::vector<Rect> flipAllXY(const std::vector<Rect>& rs) {
  std::vector<Rect> res;
  for (const Rect& r : rs) {
    res.push_back(flipXY(r));
  }
  return res;
}

Rect mapLeftEdge(Rect rect,
                 const std::vector<Rect>& oldVis,
                 const std::vector<Rect>& newVis) {
  Rect oldLE;
  int oldLEOverlap = 0;
  // Find the old monitor the window overlaps the most.
  for (const Rect& r : oldVis) {
    if (r.xMin != 0) {
      continue;
    }
    const int h = Rect::Intersect(rect, r).height();
    if (h > oldLEOverlap) {
      oldLEOverlap = h;
      oldLE = r;
    }
  }
  // Find the first screen at x=0, and try to position it at roughly the same
  // height.
  for (const Rect& r : newVis) {
    if (r.xMin != 0) {
      continue;
    }
    Rect res = rect;
    // If the window was Y maximised, or its height is larger than that of the
    // new screen at this position, then maximise it.
    if (oldLEOverlap == oldLE.height() || rect.height() >= r.height()) {
      res.yMin = r.yMin;
      res.yMax = r.yMax;
    } else {
      // Window fits within the screen - ensure its Y position is relatively
      // similar to what it was.
      int yoff = rect.yMin - oldLE.yMin;
      if (yoff <= 0) {
        yoff = 0;
      } else {
        yoff = yoff * (r.height() - rect.height()) /
               (oldLE.height() - rect.height());
      }
      res.yMin = r.yMin + yoff;
      res.yMax = res.yMin + rect.height();
    }
    // Ensure the window isn't wider than the screen.
    if (res.width() > r.width()) {
      res.xMax = res.xMin + r.width();
      // ...but that means we have to ensure we don't lose it, if its xMin is
      // more than the width of the screen off the left.
      const int fixOffset = 10 - res.xMax;
      if (fixOffset > 0) {
        res.xMin += fixOffset;
        res.xMax += fixOffset;
      }
    }
    return res;
    break;
  }
  // Didn't manage to figure anything out - return same thing.
  return rect;
}

bool mapEdges(Rect rect,
              const std::vector<Rect>& oldVis,
              const std::vector<Rect>& newVis,
              Rect* newRect) {
  // If the window abuts the left edge of the screen, or extends beyond it,
  // keep it there, but ensure that its Y span is within the new vertical area
  // of the left edge, and that it's no wider than that screen.
  if (rect.xMin <= 0) {
    *newRect = mapLeftEdge(rect, oldVis, newVis);
    return true;
  }
  // Same for the right edge. Because this code is a bit complicated, we
  // implement this by mirroring the before/after screens horizontally, then
  // calling mapLeftEdge, before mirroring back the resulting rect.
  const int oldXMax = xMaxFrom(oldVis);
  const int newXMax = xMaxFrom(newVis);
  if (rect.xMax >= oldXMax) {
    Rect res = mapLeftEdge(mirrorX(rect, oldXMax), mirrorAllX(oldVis, oldXMax),
                           mirrorAllX(newVis, newXMax));
    *newRect = mirrorX(res, newXMax);
    return true;
  }
  return false;
}

Rect tallestScreenAtX(const std::vector<Rect>& vis, int x) {
  int maxHeight = 0;
  Rect res = Rect{0, 0, 0, 0};
  Rect rightmost = Rect{0, 0, 0, 0};
  for (const Rect& r : vis) {
    if (r.xMax > rightmost.xMax) {
      rightmost = r;
    }
    if (x < r.xMin || x > r.xMax) {
      continue;
    }
    if (r.height() > maxHeight) {
      maxHeight = r.height();
      res = r;
    }
  }
  return res.empty() ? rightmost : res;
}

Rect sourceScreen(const std::vector<Rect>& vis, const Rect& r) {
  int area = 0;
  Rect res;
  for (const Rect& scr : vis) {
    const int ra = Rect::Intersect(scr, r).area().num_pixels();
    if (ra > area) {
      area = ra;
      res = scr;
    }
  }
  if (area) {
    return res;
  }
  // No overlap found anywhere; fall back to...
  return tallestScreenAtX(vis, r.xMin);
}

Rect forceWithinRectX(Rect r, Rect target) {
  Rect res = r;
  if (res.width() >= target.width()) {
    res.xMin = target.xMin;
    res.xMax = target.xMax;
    return res;
  }
  if (res.xMin < target.xMin) {
    res.xMin = target.xMin;
  } else if (res.xMin > target.xMax - r.width()) {
    res.xMin = target.xMax - r.width();
  }
  res.xMax = res.xMin + r.width();
  return res;
}

int scale(int pos, int size, int oldMax, int newMax) {
  if (oldMax <= size || newMax <= size) {
    return pos;
  }
  return pos * (newMax - size) / (oldMax - size);
}

int maybeScaleDown(int v, int oldMax, int newMax) {
  return (newMax >= oldMax) ? v : (v * newMax / oldMax);
}

Rect MapToNewAreas(Rect rect,
                   const std::vector<Rect>& oldVis,
                   const std::vector<Rect>& newVis) {
  Rect res;
  const int oldXMax = xMaxFrom(oldVis);
  const int newXMax = xMaxFrom(newVis);
  // If this window is height-maximised on any of the original visible areas,
  // we deal with it specially.
  for (const Rect& r : oldVis) {
    if (Rect::Intersect(r, rect).height() == r.height()) {
      res.xMin = scale(rect.xMin, rect.width(), oldXMax, newXMax);
      // Find the highest screen at the middle X location, and use that as a
      // target.
      const int scaledWidth = maybeScaleDown(rect.width(), oldXMax, newXMax);
      const Rect target = tallestScreenAtX(newVis, res.xMin + scaledWidth / 2);
      res.xMax = res.xMin + rect.width();
      res = forceWithinRectX(res, target);
      res.yMin = target.yMin;
      res.yMax = target.yMax;
      return res;
    }
  }
  // Deal with windows that are up against the X or Y extremes of the
  // old visible area. To save on code, which do this by using mirroring and
  // flipping, and just implement code for the x=0 edge.
  if (mapEdges(rect, oldVis, newVis, &res)) {
    return res;
  }
  if (mapEdges(flipXY(rect), flipAllXY(oldVis), flipAllXY(newVis), &res)) {
    return flipXY(res);
  }
  // If we got here, the window is floating about within some screen.
  // One thing we can be reasonably certain of is that if the window was
  // maximised in either dimension, it will have been taken care of by one of
  // the mapEdges calls. So we can happily ignore that.
  // Possibly the simplest approach now is to:
  // Map the X position according to old vs new X extents.
  res.xMin = scale(rect.xMin, rect.width(), oldXMax, newXMax);
  // Find the highest screen at the middle X location, and use that as a target.
  const int scaledWidth = maybeScaleDown(rect.width(), oldXMax, newXMax);
  const Rect target = tallestScreenAtX(newVis, res.xMin + scaledWidth / 2);
  // If the window is too wide to fit in the screen, force it down to occupy
  // the whole screen area.
  if (rect.width() >= target.width()) {
    res.xMin = target.xMin;
    res.xMax = target.xMax;
  } else {
    // Window is small enough to fit in the target window. However, we want
    // to ensure it's entirely within one monitor, so adjust accordingly.
    res.xMax = res.xMin + rect.width();
    res = forceWithinRectX(res, target);
  }
  // At this point, res has xMin and xMax set correctly. Now deal with the Y
  // coordinate.
  // Again, if the window is too tall to fit in the monitor, force its y
  // coordinates.
  if (rect.height() > target.height()) {
    res.yMin = target.yMin;
    res.yMax = target.yMax;
  } else {
    // Find the screen that the old rect mostly intersected (or just pick one at
    // its x position).
    const Rect source = sourceScreen(oldVis, rect);
    // If the window was off the top or bottom, push it entirely within the
    // target.
    if (rect.yMin < source.yMin) {
      res.yMin = target.yMin;
    } else if (rect.yMax >= source.yMax) {
      res.yMin = target.yMax - rect.height();
    } else {
      // Window was entirely within the Y scope of the source. Scale it so it
      // occupies the same kind of Y position in the target.
      res.yMin = target.yMin + (rect.yMin - source.yMin) *
                                   (target.height() - rect.height()) /
                                   (source.height() - rect.height());
    }
    res.yMax = res.yMin + rect.height();
  }
  return res;
}

int absDist(int min1, int max1, int min2, int max2) {
  if (min1 > max2) {
    return min1 - max2;
  }
  if (min2 > max1) {
    return min2 - max1;
  }
  return 0;
}

Rect SnapToMonitor(const Rect& r, const std::vector<Rect>& areas) {
  // Only an exact size match counts. A window one pixel off a monitor's size
  // isn't trying to cover it, and guessing on its behalf would be worse than
  // leaving it alone.
  Rect best{};
  int best_overlap = 0;
  for (const Rect& area : areas) {
    if (area.area() != r.area()) {
      continue;
    }
    const int overlap = Rect::Intersect(r, area).area().num_pixels();
    if (overlap > best_overlap) {
      best_overlap = overlap;
      best = area;
    }
  }
  // Require the window to be mostly on the monitor already. Because r is
  // exactly one monitor's size, at most one monitor can hold more than half of
  // it, so this both picks the target unambiguously and stops us dragging a
  // window across the desk because its size happened to match.
  if (best_overlap * 2 <= r.area().num_pixels()) {
    return r;
  }
  return Rect::Translate(r, Point::Sub(best.origin(), r.origin()));
}

Rect findBestScreenFor(const Rect& r, const std::vector<Rect>& areas) {
  // First try to find the one with the largest overlap.
  Rect res{};
  int ra = 0;
  for (Rect v : areas) {
    const int area = Rect::Intersect(r, v).area().num_pixels();
    if (area > ra) {
      ra = area;
      res = v;
    }
  }
  // If we found an overlapping screen, return it.
  if (ra) {
    return res;
  }
  // Found no overlapping screens. Try again, this time picking the screen
  // closest to the query rectangle.
  int rd = INT_MAX;
  for (Rect v : areas) {
    // These will be absolute distances.
    const int xd = absDist(r.xMin, r.xMax, v.xMin, v.xMax);
    const int yd = absDist(r.yMin, r.yMax, v.yMin, v.yMax);
    // Just sum the distances; it'll do.
    const int d = xd + yd;
    if (d < rd) {
      rd = d;
      res = v;
    }
  }
  return res;
}

Rect makeVisible(Rect r, const std::vector<Rect>& areas) {
  const Rect scr = findBestScreenFor(r, areas);
  Point translation{};
  if (r.width() >= scr.width()) {
    r.xMin = scr.xMin;
    r.xMax = scr.xMax;
  } else if (r.xMax > scr.xMax) {
    translation.x = scr.xMax - r.xMax;
  } else if (r.xMin < scr.xMin) {
    translation.x = scr.xMin - r.xMin;
  }
  if (r.height() >= scr.height()) {
    r.yMin = scr.yMin;
    r.yMax = scr.yMax;
  } else if (r.yMax > scr.yMax) {
    translation.y = scr.yMax - r.yMax;
  } else if (r.yMin < scr.yMin) {
    translation.y = scr.yMin - r.yMin;
  }
  return Rect::Translate(r, translation);
}

// One axis of ShrinkToFitMonitor: given the window's span lo..hi and the
// monitor's mLo..mHi, writes back the span the window should have. Does
// nothing at all if it already fits, which is what keeps a window that's
// merely hanging off the edge of a screen where the user put it.
void shrinkAxisToFit(int* lo, int* hi, int mLo, int mHi) {
  if (*hi - *lo <= mHi - mLo) {
    return;
  }
  *lo = mLo;
  *hi = mHi;
}

Rect ShrinkToFitGivenMonitor(Rect r, const Rect& mon) {
  shrinkAxisToFit(&r.xMin, &r.xMax, mon.xMin, mon.xMax);
  shrinkAxisToFit(&r.yMin, &r.yMax, mon.yMin, mon.yMax);
  return r;
}

Rect ShrinkToFitMonitor(Rect r, const std::vector<Rect>& areas) {
  if (areas.empty()) {
    return r;
  }
  return ShrinkToFitGivenMonitor(r, findBestScreenFor(r, areas));
}

Rect findDragScreen(Point pointer,
                    const Rect& r,
                    const std::vector<Rect>& areas) {
  for (const Rect& v : areas) {
    if (v.contains(pointer.x, pointer.y)) {
      return v;
    }
  }
  return findBestScreenFor(r, areas);
}
