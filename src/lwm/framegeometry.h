#ifndef FRAMEGEOMETRY_H_included
#define FRAMEGEOMETRY_H_included

#include "edge.h"
#include "geometry.h"

// The measurements needed to convert between a client's content rect and
// its on-screen frame, and to lay out the frame's furniture (title bar,
// close icon, resize edges). Built once from Resources and the loaded font
// (see CurrentFrameStyle() in client.cc); passed by value/const-ref so the
// geometry functions below don't need to reach for those globals themselves.
struct FrameStyle {
  int border_width = 0;
  int top_border_width = 0;
  int text_height = 0;

  int TitleBarHeight() const { return text_height + border_width; }
};

// The bounding box of the close icon. If displayBounds is true, this is the
// drawn cross itself; if false, it's the (deliberately larger) clickable
// area, which extends down to the client window and across to the start of
// the title bar - see the comment at the call site in client.cc.
Rect CloseBounds(const FrameStyle& style, bool displayBounds);

// The bounding box of the title bar (the draggable "move" area), for a
// frame of the given total width.
Rect TitleBarBounds(const FrameStyle& style, int windowWidth);

// Maps a point on the frame (given as an Edge - see Client::EdgeAt) to the
// bounding box of that edge's hit-test region, within a frame of the given
// size. The -1/+1 fudges on the outer edges account for the frame's own
// 1px X border, which sits outside its normal coordinate space.
Rect EdgeBoundsFor(const FrameStyle& style, const Rect& frameRect, Edge e);

// Converts between a client's frame rect and its content rect, in whichever
// coordinate space r is already in (both are root-relative, or both are
// frame-relative).
Rect ContentFromFrameRect(const FrameStyle& style, const Rect& r);
Rect FrameFromContentRect(const FrameStyle& style, const Rect& r);

#endif
