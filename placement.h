#ifndef PLACEMENT_H_included
#define PLACEMENT_H_included

#include "geometry.h"

// How many pixels to move the auto-placement location down and to the right,
// after each window is placed.
inline constexpr int kAutoPlacementIncrement = 40;

// Chooses positions for auto-placed windows (those with no position hint of
// their own): cascades down and to the right within a target area, wrapping
// back to the top-left once past the middle, and centring windows that are
// too big to cascade but would still fit if centred.
class AutoPlacer {
 public:
  // Returns the top-left position for a window of the given size within
  // target, and advances internal state so the next call cascades further.
  // If the previous cascade position has drifted outside target (e.g.
  // because the monitor layout changed), placement resets to its top-left
  // corner first.
  Point NextPosition(const Area& client_area, const Rect& target);

 private:
  unsigned int next_x_ = 100;
  unsigned int next_y_ = 100;
};

#endif
