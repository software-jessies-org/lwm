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

// Cursor names, as found in the "cursors" directory of an icon theme. These
// are the traditional X names rather than the newer freedesktop ones
// ("default", "nw-resize", ...): themes reliably ship the old names, whether
// as files or as symlinks to the new ones, and libxcb-cursor knows how to map
// each of them onto the core cursor font if the theme doesn't have it at all.
constexpr char kLeftPtr[] = "left_ptr";
constexpr char kTopLeftCorner[] = "top_left_corner";
constexpr char kTopSide[] = "top_side";
constexpr char kTopRightCorner[] = "top_right_corner";
constexpr char kRightSide[] = "right_side";
constexpr char kFleur[] = "fleur";
constexpr char kLeftSide[] = "left_side";
constexpr char kBottomLeftCorner[] = "bottom_left_corner";
constexpr char kBottomSide[] = "bottom_side";
constexpr char kBottomRightCorner[] = "bottom_right_corner";
constexpr char kXCursor[] = "X_cursor";

}  // namespace

CursorMap::CursorMap() {
  // These come from the user's cursor theme, so they're drawn at whatever size
  // that theme is configured for (XCURSOR_SIZE, or the Xcursor.size resource,
  // or a size derived from the screen height). Using the core cursor font
  // instead, as lwm used to, gave a 16-pixel pointer over lwm's frames and the
  // root window while every toolkit-drawn window got the themed one, which on
  // a high resolution display is a glaring difference.
  root_ = xlib::CreateNamedCursor(kLeftPtr);

#define MC(e, s) edges_[e] = xlib::CreateNamedCursor(s)
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
