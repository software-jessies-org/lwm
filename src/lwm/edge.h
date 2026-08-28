#ifndef EDGE_H_included
#define EDGE_H_included

/**
Window edge, used in resizing. The `edge' ENone is used to signify a
window move rather than a resize. The code is sufficiently similar that
this isn't a special case to be treated separately.
*/
enum Edge {
  ETopLeft,
  ETop,
  ETopRight,
  ERight,
  ENone,
  ELeft,
  EBottomLeft,
  EBottom,
  EBottomRight,
  EClose,     // Special 'Edge' to denote the close icon.
  EContents,  // Special again: not any action, it's the client window.
  E_LAST
};

bool isLeftEdge(Edge e);
bool isRightEdge(Edge e);
bool isTopEdge(Edge e);
bool isBottomEdge(Edge e);

#endif
