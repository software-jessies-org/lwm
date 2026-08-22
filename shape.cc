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

#include <time.h>

#include <unistd.h>

#include "client.h"
#include "lwm.h"
#include "screen.h"
#include "shape.h"
#include "xlib.h"

#ifdef SHAPE
#include <xcb/shape.h>
#endif

/*ARGSUSED*/
extern void setShape(Client* c) {
#ifdef SHAPE
  if (!shape) {
    return;
  }
  xlib::XShapeSelectInput(c->window);
  if (xlib::XShapeCountRectangles(c->window) > 1) {
    int border = borderWidth();
    xlib::XShapeCombineShape(c->parent, border - 1, border - 1, c->window);
  }
#else
  c = c;
#endif
}

/*ARGSUSED*/
extern int shapeEvent(xcb_generic_event_t* ev) {
#ifdef SHAPE
  // Extension events don't have fixed numbers: the server allocates a base at
  // run time and the extension's events are numbered from it. shape_event is
  // that base, filled in by serverSupportsShapes().
  if (shape && (ev->response_type & 0x7f) == shape_event) {
    const xcb_shape_notify_event_t* e = (const xcb_shape_notify_event_t*)ev;
    Client* c = LScr::I->GetClient(e->affected_window);
    if (c != 0) {
      setShape(c);
    }
    return 1;
  }
#else
  ev = ev;
#endif
  return 0;
}

/*ARGSUSED*/
extern int isShaped(Window w) {
#ifdef SHAPE
  return xlib::XShapeCountRectangles(w) > 1;
#else
  w = w;
  return 0;
#endif
}

extern int serverSupportsShapes() {
#ifdef SHAPE
  const int first_event = xlib::XShapeQueryExtension();
  if (first_event < 0) {
    LOGI() << "Shape extension not supported by the server";
    return 0;
  }
  shape_event = first_event;
  LOGI() << "Shape extension supported (event " << shape_event << ")";
  return 1;
#else
  LOGI() << "Shape support not enabled";
  return 0;
#endif
}
