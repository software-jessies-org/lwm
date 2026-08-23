#ifndef GEOMETRY_H_included
#define GEOMETRY_H_included

#include <iostream>
#include <string>
#include <vector>

#include "edge.h"
#include "sizelimits.h"

struct Point {
  int x;
  int y;

  inline bool operator==(const Point& o) const { return x == o.x && y == o.y; }
  inline bool operator!=(const Point& o) const { return !operator==(o); }

  // Returns a - b. (This said "b - a" until 2026-08-23; the code always did
  // a - b, and Sub had no callers to disagree with it.)
  static Point Sub(Point a, Point b);
};

struct Area {
  int width;
  int height;

  inline bool operator==(const Area& o) const {
    return width == o.width && height == o.height;
  }
  inline bool operator!=(const Area& o) const { return !operator==(o); }

  int num_pixels() const { return width * height; }
};

struct Rect {
  int xMin;
  int yMin;
  int xMax;
  int yMax;

  bool contains(int x, int y) const {
    return x >= xMin && y >= yMin && x < xMax && y < yMax;
  }

  int width() const { return xMax - xMin; }
  int height() const { return yMax - yMin; }
  Area area() const { return Area{width(), height()}; }
  bool empty() const { return area().num_pixels() == 0; }

  Point origin() const { return Point{xMin, yMin}; }
  Point middle() const { return Point{(xMin + xMax) / 2, (yMin + yMax) / 2}; }

  inline bool operator==(const Rect& o) const {
    return xMin == o.xMin && yMin == o.yMin && xMax == o.xMax && yMax == o.yMax;
  }

  inline bool operator!=(const Rect& o) const { return !operator==(o); }

  static Rect FromXYWH(int x, int y, int w, int h);

  template <typename T>
  static Rect From(const T& t) {
    return FromXYWH(t.x, t.y, t.width, t.height);
  }

  template <typename T>
  void To(T& t) {
    t.x = xMin;
    t.y = yMin;
    t.width = width();
    t.height = height();
  }

  // Returns a new Rect which is shifted by the given x and y translation.
  static Rect Translate(Rect r, Point p);

  // Returns the intersection of the two rectangles or, if they don't intersect,
  // the empty rectangle 0,0,0,0.
  static Rect Intersect(const Rect& a, const Rect& b);

  // Parse rectangles in X11 style (1280x960+23+25).
  // Returns the canonical empty rectangle if parsing fails.
  static Rect Parse(std::string str);
};

std::ostream& operator<<(std::ostream& os, const Point& p);
std::ostream& operator<<(std::ostream& os, const Rect& r);
std::ostream& operator<<(std::ostream& os, const Area& a);
std::ostream& operator<<(std::ostream& os, const std::vector<Rect>& rs);

#endif
