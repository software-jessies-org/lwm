#include "framegeometry.h"

Rect CloseBounds(const FrameStyle& style, bool displayBounds) {
  const int quarter = (style.border_width + style.text_height) / 4;
  const int cMin = quarter + 2;
  const int cMax = displayBounds ? 3 * quarter : style.TitleBarHeight();
  return Rect{cMin, cMin, cMax, cMax};
}

Rect TitleBarBounds(const FrameStyle& style, int windowWidth) {
  const int x = style.TitleBarHeight();
  return Rect{x, style.top_border_width, windowWidth - x,
             style.TitleBarHeight()};
}

// Our use of -1 for the x/y min value on the left and top edges, and the +1
// on the width/height for the right/bottom edges looks funny. The reason for
// doing this is because our furniture window includes a 1 pixel border, which
// exists outside our normal window coordinates. If we don't add these -/+1
// hacks, we end up with the wrong behaviour when the pointer is within this
// border, which looks funny.
Rect EdgeBoundsFor(const FrameStyle& style, const Rect& frameRect, Edge e) {
  const int inset = style.TitleBarHeight();
  Rect res{inset, inset, frameRect.width() - inset, frameRect.height() - inset};
  if (isLeftEdge(e)) {
    res.xMin = -1;
    res.xMax = inset;
  } else if (isRightEdge(e)) {
    res.xMin = frameRect.width() - inset;
    res.xMax = frameRect.width() + 1;
  }
  if (isTopEdge(e)) {
    res.yMin = -1;
    res.yMax = inset;
  } else if (isBottomEdge(e)) {
    res.yMin = frameRect.height() - inset;
    res.yMax = frameRect.height() + 1;
  }
  return res;
}

Rect ContentFromFrameRect(const FrameStyle& style, const Rect& r) {
  Rect res = r;
  res.xMin += style.border_width;
  res.yMin += style.TitleBarHeight();
  res.xMax -= style.border_width;
  res.yMax -= style.border_width;
  return res;
}

Rect FrameFromContentRect(const FrameStyle& style, const Rect& r) {
  Rect res = r;
  res.xMin -= style.border_width;
  res.yMin -= style.TitleBarHeight();
  res.xMax += style.border_width;
  res.yMax += style.border_width;
  return res;
}
