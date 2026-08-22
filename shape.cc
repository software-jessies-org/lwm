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

#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif

/*ARGSUSED*/
extern void setShape(Client* c) {
#ifdef SHAPE
  if (!shape) {
    return;
  }
  XShapeSelectInput(dpy, c->window, ShapeNotifyMask);
  int order;
  int n;
  XRectangle* rect =
      XShapeGetRectangles(dpy, c->window, ShapeBounding, &n, &order);
  if (n > 1) {
    int border = borderWidth();
    XShapeCombineShape(dpy, c->parent, ShapeBounding, border - 1, border - 1,
                       c->window, ShapeBounding, ShapeSet);
  }
  XFree(rect);
#else
  c = c;
#endif
}

/*ARGSUSED*/
extern int shapeEvent(XEvent* ev) {
#ifdef SHAPE
  if (shape && ev->type == shape_event) {
    XShapeEvent* e = (XShapeEvent*)ev;
    Client* c = LScr::I->GetClient(e->window);
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
  int n;
  int order;
  XFree(XShapeGetRectangles(dpy, w, ShapeBounding, &n, &order));
  return (n > 1);
#else
  w = w;
  return 0;
#endif
}

extern int serverSupportsShapes() {
#ifdef SHAPE
  int shape_error;
  int res = XShapeQueryExtension(dpy, &shape_event, &shape_error);
  LOGI() << "Shape extension supported: " << res << " (event " << shape_event
         << ")";
  return res;
#else
  LOGI() << "Shape support not enabled";
  return 0;
#endif
}
