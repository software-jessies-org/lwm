#include <stdio.h>
#include <stdlib.h>

#include "geometry.h"

std::ostream& operator<<(std::ostream& os, const Point& p) {
  os << p.x << "," << p.y;
  return os;
}

std::ostream& operator<<(std::ostream& os, const Area& a) {
  os << a.width << "x" << a.height;
  return os;
}

std::ostream& operator<<(std::ostream& os, const Rect& r) {
  os << (r.xMax - r.xMin) << "x" << (r.yMax - r.yMin);
  if (r.xMin >= 0) {
    os << "+";
  }
  os << r.xMin;
  if (r.yMin >= 0) {
    os << "+";
  }
  os << r.yMin;
  return os;
}

std::ostream& operator<<(std::ostream& os, const std::vector<Rect>& rs) {
  os << "rects[";
  for (int i = 0; i < rs.size(); i++) {
    if (i) {
      os << " ";
    }
    os << rs[i];
  }
  os << "]";
  return os;
}

// static
Point Point::Sub(Point a, Point b) {
  return Point{a.x - b.x, a.y - b.y};
}

// static
Rect Rect::FromXYWH(int x, int y, int w, int h) {
  return Rect{x, y, x + w, y + h};
}

// static
Rect Rect::Intersect(const Rect& a, const Rect& b) {
  Rect res = Rect{std::max(a.xMin, b.xMin), std::max(a.yMin, b.yMin),
                  std::min(a.xMax, b.xMax), std::min(a.yMax, b.yMax)};
  if (res.xMax > res.xMin && res.yMax > res.yMin) {
    return res;
  }
  // No intersection.
  res = Rect{0, 0, 0, 0};
  return res;
}

// static
Rect Rect::Translate(Rect r, Point p) {
  return Rect{r.xMin + p.x, r.yMin + p.y, r.xMax + p.x, r.yMax + p.y};
}

int parseInt(const std::string& s) {
  const char* cs = s.c_str();
  if (cs[0] == '+') {
    cs++;
  }
  return atoi(cs);
}

// static
Rect Rect::Parse(std::string v) {
  Rect res = {};
  int sep = v.find('x');
  if (sep == std::string::npos) {
    return res;
  }
  const int w = parseInt(v.substr(0, sep));
  v = v.substr(sep + 1);
  sep = v.find_first_of("+-");
  if (sep == std::string::npos) {
    return res;
  }
  const int h = parseInt(v.substr(0, sep));
  v = v.substr(sep);
  sep = v.find_last_of("+-");
  if (sep == std::string::npos || sep == 0) {
    return res;
  }
  const int x = parseInt(v.substr(0, sep));
  const int y = parseInt(v.substr(sep));
  if (h != 0 && w != 0) {
    res.xMin = x;
    res.yMin = y;
    res.xMax = x + w;
    res.yMax = y + h;
  }
  return res;
}

