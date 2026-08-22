#include "edge.h"

bool isLeftEdge(Edge e) {
  return e == ETopLeft || e == ELeft || e == EBottomLeft;
}

bool isRightEdge(Edge e) {
  return e == ETopRight || e == ERight || e == EBottomRight;
}

bool isTopEdge(Edge e) {
  return e == ETopLeft || e == ETop || e == ETopRight;
}

bool isBottomEdge(Edge e) {
  return e == EBottomLeft || e == EBottom || e == EBottomRight;
}
