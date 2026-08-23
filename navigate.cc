#include "navigate.h"

#include <cstdlib>
#include <cstdint>

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
