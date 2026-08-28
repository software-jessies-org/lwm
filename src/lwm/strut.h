#ifndef STRUT_H_included
#define STRUT_H_included

#include <algorithm>

/**
 * EWMH "strut", or area on each edge of the screen reserved for docking
 * bars/panels.
 */
struct EWMHStrut {
  unsigned int left;
  unsigned int right;
  unsigned int top;
  unsigned int bottom;

  bool operator==(const EWMHStrut& o) const {
    return left == o.left && right == o.right && top == o.top &&
          bottom == o.bottom;
  }
  bool operator!=(const EWMHStrut& o) const { return !(*this == o); }
};

// The element-wise maximum of two struts: the reservation needed to satisfy
// both at once. Used to combine each client's requested strut into the
// screen-wide reserved area.
inline EWMHStrut MaxStrut(const EWMHStrut& a, const EWMHStrut& b) {
  return EWMHStrut{
      std::max(a.left, b.left),
      std::max(a.right, b.right),
      std::max(a.top, b.top),
      std::max(a.bottom, b.bottom),
  };
}

#endif
