/*
 * lwm, a window manager for X11
 * Copyright (C) 1997-2016 Elliott Hughes, James Carter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <stdio.h>

#include "cursor.h"

namespace {

// Glyph indices into the standard "cursor" font. Xlib spelled these XC_left_ptr
// and so on in <X11/cursorfont.h>; XCB ships no equivalent header, and these
// are fixed by the font itself rather than by any library, so here they are.
// Each cursor occupies two glyphs, the image and its mask, which is why they
// go up in twos.
constexpr unsigned int kLeftPtr = 68;
constexpr unsigned int kTopLeftCorner = 134;
constexpr unsigned int kTopSide = 138;
constexpr unsigned int kTopRightCorner = 136;
constexpr unsigned int kRightSide = 96;
constexpr unsigned int kFleur = 52;
constexpr unsigned int kLeftSide = 70;
constexpr unsigned int kBottomLeftCorner = 12;
constexpr unsigned int kBottomSide = 16;
constexpr unsigned int kBottomRightCorner = 14;
constexpr unsigned int kXCursor = 0;

const char kCursorFG[] = "Black";  //"Medium Turquoise";
const char kCursorBG[] = "White";  //"Navy Blue";

}  // namespace

CursorMap::CursorMap() {
  // Unlike Xlib's XCreateFontCursor, the colours are supplied when the cursor
  // is created, so there's no follow-up recolouring step.
  const unsigned long fg = xlib::ColourByName(kCursorFG);
  const unsigned long bg = xlib::ColourByName(kCursorBG);
  root_ = xlib::CreateFontCursor(kLeftPtr, fg, bg);

#define MC(e, s) edges_[e] = xlib::CreateFontCursor(s, fg, bg)
  MC(ETopLeft, kTopLeftCorner);
  MC(ETop, kTopSide);
  MC(ETopRight, kTopRightCorner);
  MC(ERight, kRightSide);
  MC(ENone, kFleur);
  MC(ELeft, kLeftSide);
  MC(EBottomLeft, kBottomLeftCorner);
  MC(EBottom, kBottomSide);
  MC(EBottomRight, kBottomRightCorner);
  MC(EClose, kXCursor);
#undef MC
}

Cursor CursorMap::ForEdge(Edge e) const {
  auto it = edges_.find(e);
  return (it == edges_.end()) ? Root() : it->second;
}
