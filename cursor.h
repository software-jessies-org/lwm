#ifndef LWM_CURSOR_H_included
#define LWM_CURSOR_H_included

#include <map>

#include "edge.h"
#include "xlib.h"

class CursorMap {
 public:
  explicit CursorMap(Display* dpy);

  // Root() returns the standard pointer cursor we use most places, including
  // over the root window.
  Cursor Root() const { return root_; }

  // ForEdge returns the cursor appropriate to the given edge. This may be
  // arrows for the resizing areas, or a nice big 'X' for EClose.
  // Returns the same as Root() if there's no specific cursor for some edge.
  Cursor ForEdge(Edge e) const;

 private:
  Cursor root_;
  std::map<Edge, Cursor> edges_;
};

#endif  // LWM_CURSOR_H_included
