#include "sizelimits.h"

// Any window outside these size ranges are just plain silly.
// Revisit MAX_MAX if we ever get to such a silly situation that we need more
// than 10 thousand pixels on a side.
#define MIN_MIN 10
#define MAX_MAX 10000

DimensionLimiter::DimensionLimiter(int min, int max, int base, int increment)
    : min_(min), max_(max), base_(base), increment_(increment) {
  // Fix any dodgy numbers.
  if (min_ < MIN_MIN) {
    min_ = MIN_MIN;
  }
  if (max_ < min_ || max_ > MAX_MAX) {
    max_ = MAX_MAX;
  }
  if (increment_ < 1) {
    increment_ = 1;
  }
}

void DimensionLimiter::Limit(int old_min,
                             int old_max,
                             int& new_min,
                             int& new_max) const {
  int size = new_max - new_min;
  // In all the cases further down, size_delta must be set to the value to *add*
  // to the size in order to make it conform to the rules.
  int size_delta = 0;
  if (size < min_) {
    size_delta = min_ - size;
  } else if (size > max_) {
    size_delta = max_ - size;
  } else {
    size_delta = -((size - base_) % increment_);
  }
  if (size_delta == 0) {
    return;
  }
  // Need to fix the size. To do that, we must pick which of min and max have
  // changed, and adjust it accordingly. If neither has change (unlikely) it
  // doesn't matter which we pick.
  // Note that the size_delta is a delta in *size*, so it should be added to
  // new_max, *or* subtracted from new_min.
  // It makes sense to pass in old_max from an API point of view (symmetric
  // with old_min), even though only old_min is actually needed to tell which
  // end moved.
  (void)old_max;
  if (new_min != old_min) {
    new_min -= size_delta;
  } else {
    new_max += size_delta;
  }
}

int DimensionLimiter::DisplayableSize(int v) const {
  return (v - base_) / increment_;
}
