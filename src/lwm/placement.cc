#include "placement.h"

Point AutoPlacer::NextPosition(const Area& client_area, const Rect& target) {
  // If next_x_/next_y_ are outside the target area, reset them. This can
  // happen after a change of monitor configuration.
  if (!target.contains(next_x_, next_y_)) {
    next_x_ = target.xMin + 100;
    next_y_ = target.yMin + 100;
  }

  Point res{};
  if (next_x_ + client_area.width > target.xMax &&
      client_area.width <= target.width()) {
    // If the window wouldn't fit using normal auto-placement but is small
    // enough to fit horizontally, then centre the window horizontally.
    res.x = target.xMin + (target.width() - client_area.width) / 2;
    next_x_ = target.xMin + 20;
  } else {
    res.x = next_x_;
    next_x_ += kAutoPlacementIncrement;
    if (next_x_ > (target.xMin + target.xMax) / 2) {  // Past middle.
      next_x_ = target.xMin + 20;
    }
  }

  if (next_y_ + client_area.height > target.yMax &&
      client_area.height <= target.height()) {
    // If the window wouldn't fit using normal auto-placement but is small
    // enough to fit vertically, then centre the window vertically.
    res.y = target.yMin + (target.height() - client_area.height) / 2;
    next_y_ = target.yMin + 20;
  } else {
    res.y = next_y_;
    next_y_ += kAutoPlacementIncrement;
    if (next_y_ > (target.yMin + target.yMax) / 2) {  // Past middle.
      next_y_ = target.yMin + 20;
    }
  }
  return res;
}
