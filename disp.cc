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

#include "client.h"
#include "debug.h"
#include "disp.h"
#include "drag.h"
#include "error.h"
#include "ewmh.h"
#include "lwm.h"
#include "hider.h"
#include "manage.h"
#include "resource.h"
#include "screen.h"
#include "shape.h"
#include "xdebugprint.h"
#include "xlib.h"

void EvExpose(XEvent* ev) {
  // Only handle the last in a group of Expose events.
  if (ev->xexpose.count != 0) {
    return;
  }

  Window w = ev->xexpose.window;

  // We don't draw on the root window so that people can have
  // their favourite Spice Girls backdrop...
  if (w == LScr::I->Root()) {
    return;
  }

  // Decide what needs redrawing: window frame or menu?
  if (w == LScr::I->Popup()) {
    size_expose();
  } else if (w == LScr::I->Menu()) {
    LScr::I->GetHider()->Paint();
  } else {
    Client* c = LScr::I->GetClient(w);
    if (c != 0) {
      c->DrawBorder();
    }
  }
}

static DragHandler* current_dragger = nullptr;

// Use this to set or clear the drag handler. Will destroy the old handler if
// one is present. The new handler's Start() function is called with ev.
void startDragging(DragHandler* handler, XEvent* ev) {
  delete current_dragger;
  current_dragger = handler;
  if (current_dragger) {
    current_dragger->Start(ev);
  }
}

// Stops the dragging, calling the Stop handler of current_dragger if there is
// one.
void stopDragging(XEvent* ev) {
  if (ev && current_dragger) {
    current_dragger->End(ev);
  }
  startDragging(nullptr, nullptr);
}

void EvButtonPress(XEvent* ev) {
  if (current_dragger) {
    LOGI() << "Already doing something";
    return;  // Already doing something.
  }
  startDragging(getDragHandlerForEvent(ev), ev);
}

void EvButtonRelease(XEvent* ev) {
  stopDragging(ev);
}

void EvCirculateRequest(XEvent* ev) {
  XCirculateRequestEvent* e = &ev->xcirculaterequest;
  Client* c = LScr::I->GetClient(e->window);
  LOGD(c) << "CirculateRequest";
  if (c == nullptr) {
    if (e->place == PlaceOnTop) {
      xlib::XRaiseWindow(e->window);
    } else {
      xlib::XLowerWindow(e->window);
    }
  } else {
    if (e->place == PlaceOnTop) {
      c->Raise();
    } else {
      c->Lower();
    }
  }
}

void EvMapRequest(XEvent* ev) {
  XMapRequestEvent* e = &ev->xmaprequest;
  Client* c = LScr::I->GetOrAddClient(e->window, false);
  LOGD(c) << "MapRequest";
  if (c->hidden) {
    c->Unhide();
  }

  switch (c->State()) {
    case WithdrawnState:
      if (c->parent == LScr::I->Root()) {
        LOGD(c) << "(map) taking management of " << WinID(c->window);
        manage(c);
        LScr::I->GetFocuser()->FocusClient(c);
        break;
      }
      if (c->framed) {
        LOGD(c) << "(map) reparenting framed window " << WinID(c->parent);
        xlib::XReparentWindow(c->window, c->parent, borderWidth(),
                              borderWidth() + textHeight());
      } else {
        LOGD(c) << "(map) reparenting unframed window " << WinID(c->parent);
        const Point p = c->ContentRect().origin();
        xlib::XReparentWindow(c->window, c->parent, p.x, p.y);
      }
      xlib::XAddToSaveSet(c->window);
    // FALLTHROUGH
    case NormalState:
      LOGD(c) << "(map) NormalState " << WinID(c->window);
      xlib::XMapWindow(c->parent);
      xlib::XMapWindow(c->window);
      c->Raise();
      c->SetState(NormalState);
      c->SendConfigureNotify();
      break;
  }
  ewmh_set_client_list();
}

void EvUnmapNotify(XEvent* ev) {
  const XUnmapEvent& xe = ev->xunmap;
  // Don't scan the window's parents for a match - we only care about unmapping
  // of top-level client windows, so anything underneath we can ignore.
  Client* c = LScr::I->GetClient(xe.window, false);
  if (c == nullptr) {
    return;
  }
  // Be careful here. We only want to respond to unmaps on client windows that
  // we're managing. For example, if this isn't the direct client window,
  // then do nothing.
  if (c->window != xe.window) {
    return;
  }
  // Plus, when we reparent the client window to our frame, we'll receive an
  // unmap notification with window=child window, and parent=root. Check for
  // this, and ignore it.
  if (xe.event == LScr::I->Root()) {
    return;
  }
  // If we got here, then this is a client withdrawing its own window that we
  // have ourselves re-framed. We therefore withdraw ourselves.
  LOGD(c) << "Withdrawing unmapped window";
  withdraw(c);
}

void EvConfigureRequest(XEvent* ev) {
  const XConfigureRequestEvent& e = ev->xconfigurerequest;
  // There are several situations in which we can receive a configure request.
  // Two of these are:
  //  1: The client is setting up the initial size, before mapping the window.
  //  2: The client has its own built-in window parts by which the user can
  //     drag the window around (eg the Nautilus file browser).
  //
  // The basic thing we have to do is to allow the configure request to go
  // through, and affect the client's window. So let's do that.
  // Now, let's check if we're already framed and visible. If so, we also move
  // around our frame. In this case we won't bother checking if the bounds are
  // sensible, as the client's making this request.
  Client* c = LScr::I->GetClient(e.window, false);
  if (c == nullptr || c->State() != NormalState || !c->framed) {
    XWindowChanges wc{};
    wc.x = e.x;
    wc.y = e.y;
    wc.width = e.width;
    wc.height = e.height;
    wc.border_width = e.border_width;
    wc.sibling = e.above;
    wc.stack_mode = e.detail;
    xlib::XConfigureWindow(e.window, e.value_mask, &wc);
    return;
  }
  LOGD(c) << "ConfigureRequest: " << e;
  if (!c->framed) {
    return;
  }
  // Current situation with Nautilus is:
  // When dragging to move, the *first* configure request has x,y = the relative
  // position of the content window with respect to the frame window. All later
  // requests are relative to the root. When dragging to resize (bottom edge,
  // just above LWM's boundary), the first request has x, y = the frame origin,
  // which makes the client window jump up and to the left, so its origin
  // becomes that of the frame origin. Bloody weird. My best guess is that
  // there's some client notify message that should be sent in order to let the
  // client know where it should be placing things. Changing the move handling
  // to add the frame rect's origin to the provided rect does *not* work - after
  // the initial positioning, the window quickly zooms miles off out of scope of
  // the screen.
  // Now intercept the reconfigure and turn it into a move under our system.
  // Only do anything if one of the size/position values changes.
  if ((e.value_mask & (CWX | CWY | CWWidth | CWHeight)) == 0) {
    return;
  }
  Rect new_rect = c->ContentRect();
  // ICCCM section 4.1.5 says that the x and y coordinates here
  // will have been "adjusted for the border width".
  // In the case of reparenting (which we have done), it seems that this means
  // the client provides the x and y coordinates of the top-left of the parent
  // frame window (ie. the one we created and stuck the client inside).
  // The ICCCM documentation doesn't make this clear, but testing with the
  // Nautilus file manager suggests that this is the correct interpretation.
  // It's damned weird is all I can say.
  // Anyway, if you ever feel like changing this behaviour, ensure you test with
  // Nautilus. Try dragging the window about by the thing's own internal top
  // bar, or try resizing at the pixel or two just on the inside of the client
  // window. With this offset-handling hack in place, the position of the
  // window should change in a manner one might expect; without it, any drags
  // inside the client window will cause the window (frame and all) to jump up
  // and to the left by an amount corresponding to the offset from frame to
  // client window.
  const Point offset = c->ContentRectRelative().origin();
  if (e.value_mask & CWX) {
    int diff = (e.x + offset.x) - new_rect.xMin;
    new_rect.xMin += diff;
    new_rect.xMax += diff;
  }
  if (e.value_mask & CWY) {
    int diff = (e.y + offset.y) - new_rect.yMin;
    new_rect.yMin += diff;
    new_rect.yMax += diff;
  }
  if (e.value_mask & CWWidth) {
    new_rect.xMax = new_rect.xMin + e.width;
  }
  if (e.value_mask & CWHeight) {
    new_rect.yMax = new_rect.yMin + e.height;
  }

  XWindowChanges wc{};
  c->FrameRect().To(wc);
  wc.border_width = 1;
  wc.sibling = e.above;
  wc.stack_mode = e.detail;
  xlib::XConfigureWindow(e.parent, e.value_mask, &wc);
  c->SendConfigureNotify();

  c->ContentRectRelative().To(wc);
  wc.border_width = 0;
  xlib::XConfigureWindow(e.window, e.value_mask, &wc);

  if (new_rect.area() == c->ContentRect().area()) {
    c->MoveTo(new_rect);
  } else {
    c->MoveResizeTo(new_rect);
  }
}

void EvConfigureNotify(XEvent*) {}

void EvDestroyNotify(XEvent* ev) {
  Window w = ev->xdestroywindow.window;
  // Request the client, but without scanning this window's parents for it.
  // The window is gone, so any attempt to scan the window tree will result in
  // errors.
  Client* c = LScr::I->GetClient(w, false);
  if (c == 0) {
    return;
  }
  ScopedIgnoreBadWindow ignorer;
  c->Remove();
}

void EvClientMessage(XEvent* ev) {
  XClientMessageEvent* e = &ev->xclient;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }
  if (e->message_type == wm_change_state) {
    if (e->format == 32 && e->data.l[0] == IconicState && c->IsNormal()) {
      LOGD(c) << "Client message: requested hide";
      c->Hide();
    }
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_STATE] && e->format == 32) {
    LOGD(c) << "Client message: WM state change: " << e->data.l[0] << " -> "
            << e->data.l[1] << ", " << e->data.l[2];
    ewmh_change_state(c, e->data.l[0], e->data.l[1]);
    ewmh_change_state(c, e->data.l[0], e->data.l[2]);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_ACTIVE_WINDOW] && e->format == 32) {
    LOGD(c) << "Client message: requested set active: unhiding";
    // An EWMH enabled application has asked for this client to be made the
    // active window. Unhide also raises and gives focus to the window.
    c->Unhide();
    return;
  }
  if (e->message_type == ewmh_atom[_NET_CLOSE_WINDOW] && e->format == 32) {
    LOGD(c) << "Client message: requested close";
    c->Close();
    return;
  }
  if (e->message_type == ewmh_atom[_NET_MOVERESIZE_WINDOW] && e->format == 32) {
    XEvent ev;

    // FIXME: ok, so this is a bit of a hack
    ev.xconfigurerequest.window = e->window;
    ev.xconfigurerequest.x = e->data.l[1];
    ev.xconfigurerequest.y = e->data.l[2];
    ev.xconfigurerequest.width = e->data.l[3];
    ev.xconfigurerequest.height = e->data.l[4];
    ev.xconfigurerequest.value_mask = 0;
    if (e->data.l[0] & (1 << 8)) {
      ev.xconfigurerequest.value_mask |= CWX;
    }
    if (e->data.l[0] & (1 << 9)) {
      ev.xconfigurerequest.value_mask |= CWY;
    }
    if (e->data.l[0] & (1 << 10)) {
      ev.xconfigurerequest.value_mask |= CWWidth;
    }
    if (e->data.l[0] & (1 << 11)) {
      ev.xconfigurerequest.value_mask |= CWHeight;
    }
    LOGD(c) << "Client message: move/resize -> " << ev.xconfigurerequest
            << " (flags " << ev.xconfigurerequest.value_mask << ")";
    EvConfigureRequest(&ev);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_MOVERESIZE] && e->format == 32) {
    LOGD(c) << "Client message: requested _NET_WM_MOVERESIZE";
    Edge edge = E_LAST;
    EWMHDirection direction = (EWMHDirection)e->data.l[2];

    // before we can do any resizing, make the window visible
    if (c->IsHidden()) {
      c->Unhide();
    }
    xlib::XMapWindow(c->parent);
    c->Raise();
    // FIXME: we're ignoring x_root, y_root and button!
    switch (direction) {
      case DSizeTopLeft:
        edge = ETopLeft;
        break;
      case DSizeTop:
        edge = ETop;
        break;
      case DSizeTopRight:
        edge = ETopRight;
        break;
      case DSizeRight:
        edge = ERight;
        break;
      case DSizeBottomRight:
        edge = EBottomRight;
        break;
      case DSizeBottom:
        edge = EBottom;
        break;
      case DSizeBottomLeft:
        edge = EBottomLeft;
        break;
      case DSizeLeft:
        edge = ELeft;
        break;
      case DMove:
        edge = ENone;
        break;
      case DSizeKeyboard:
        // FIXME: don't know how to deal with this
        edge = E_LAST;
        break;
      case DMoveKeyboard:
        edge = E_LAST;
        break;
      default:
        edge = E_LAST;
        fprintf(stderr,
                "%s: received _NET_WM_MOVERESIZE"
                " with bad direction",
                argv0);
        break;
    }
    switch (edge) {
      case E_LAST:
        break;
      case ENone:
        // Should do a move, but this currently can't work because we only allow
        // the move to continue while a mouse button is pressed. We should
        // consider adding back this functionality, but for now it's not used
        // and won't work.
        break;
      default:
        // Same here, this functionality can't work right now. Need to fix it.
        break;
    }
  }
}

void EvPropertyNotify(XEvent* ev) {
  XPropertyEvent* e = &ev->xproperty;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }
  // This function can be called for a window that has already been destroyed.
  // That's fine; we'll expect functions we call to handle errors properly, but
  // we'll stomp on the printing of error logs.
  ScopedIgnoreBadWindow ignorer;

  if (e->atom == _mozilla_url || e->atom == XA_WM_NAME) {
    LOGD(c) << "Property change: XA_WM_NAME";
    getWindowName(c);
  } else if (e->atom == ewmh_atom[_NET_WM_VISIBLE_NAME]) {
    LOGD(c) << "Property change: _NET_WM_VISIBLE_NAME";
    getVisibleWindowName(c);
  } else if (e->atom == XA_WM_TRANSIENT_FOR) {
    LOGD(c) << "Property change: XA_WM_TRANSIENT_FOR";
    getTransientFor(c);
  } else if (e->atom == XA_WM_NORMAL_HINTS) {
    LOGD(c) << "Property change: XA_WM_NORMAL_HINTS";
    // XXXXXXXXXXXXXXXXXX Reset the hints used for window sizing.
    // getNormalHints(c);
  } else if (e->atom == ewmh_atom[_NET_WM_STRUT]) {
    LOGD(c) << "Property change: _NET_WM_STRUT";
    ewmh_get_strut(c);
  } else if (e->atom == ewmh_atom[_NET_WM_STATE]) {
    const EWMHWindowState old = c->wstate;
    ewmh_get_state(c);
    LOGD(c) << "Property change: _NET_WM_STATE:" << diff{old, c->wstate};
    if (c->wstate.fullscreen && !old.fullscreen) {
      c->EnterFullScreen();
    } else if (!c->wstate.fullscreen && old.fullscreen) {
      c->ExitFullScreen();
    }
  }
}

void EvReparentNotify(XEvent* ev) {
  XReparentEvent* e = &ev->xreparent;
  if (e->event != LScr::I->Root() || e->override_redirect ||
      e->parent == LScr::I->Root()) {
    return;
  }

  Client* c = LScr::I->GetClient(e->window);
  LOGD(c) << "ReparentNotify to " << WinID(c->parent);
  if (c != 0 && (c->parent == LScr::I->Root() || c->IsWithdrawn())) {
    c->Remove();
  }
}

void EvFocusIn(XEvent* ev) {
  // In practice, XGetInputFocus returns the child window that actually has
  // focus (in Java apps, the 'FocusProxy' window), while the XEvent reports
  // the top-level window.
  xlib::FocusWindow focus = xlib::XGetInputFocus();
  Window focus_window = focus.window;
  // There seems to be a bug in the Xserver, whereupon for the first focus-in
  // event we receive, XGetInputFocus returns focus_window==1, which doesn't
  // correspond to any actual window. In this case, fall back to the window
  // which was specified in the event itself.
  // Without this hack, the first time we change focus after running LWM, we
  // get a spurious error due to trying to look up the parents of window 1.
  if (focus_window == 1) {
    focus_window = ev->xfocus.window;
  }
  Client* c = LScr::I->GetClient(focus_window);
  if (c) {
    LOGD(c) << "  focusing client; focus window = " << WinID(focus_window);
    LScr::I->GetFocuser()->FocusClient(c);
  }
}

void EvFocusOut(XEvent*) {}

void EvEnterNotify(XEvent* ev) {
  if (current_dragger) {
    return;
  }
  LScr::I->GetFocuser()->EnterWindow(ev->xcrossing.window);
  // We receive enter events for our client windows too. When we do, we need
  // to switch the mouse pointer's shape to the default pointer.
  // If we don't do this, then for apps like Rhythmbox which don't
  // aggressively set the pointer to their preferred shape, we end up showing
  // silly icons, such as the 'resize corner' icon, while hovering over the
  // middle of the application window.
  Client* c = LScr::I->GetClient(ev->xcrossing.window);
  if (c == nullptr) {
    return;
  }
  if (ev->xcrossing.window != c->parent) {
    // TODO: add a SetCursor method to Client, so we don't have to keep
    // repeating this code everywhere.
    XSetWindowAttributes attr;
    attr.cursor = LScr::I->Cursors()->Root();
    xlib::XChangeWindowAttributes(c->parent, CWCursor, &attr);
    // Record that the current cursor is whatever the child window says it is.
    // This has to be different from any Edge we want to trigger when the mouse
    // crosses window furniture, otherwise we may fail to trigger a cursor
    // switch. For example, were we to set this to ENone, if the mouse were to
    // cross from the client window into the title bar, we'd fail to switch to
    // the 'move window' cursor.
    c->cursor = EContents;
  }
}

void EvMotionNotify(XEvent* ev) {
  if (current_dragger) {
    if (!current_dragger->Move(ev)) {
      current_dragger = nullptr;
    }
    return;
  }
  XMotionEvent* e = &ev->xmotion;
  Client* c = LScr::I->GetClient(e->window);
  if (c == nullptr) {
    return;
  }
  if ((e->window == c->parent) && (e->subwindow != c->window)) {
    Edge edge = c->EdgeAt(e->window, e->x, e->y);
    if (edge != EContents && c->cursor != edge) {
      XSetWindowAttributes attr;
      attr.cursor = LScr::I->Cursors()->ForEdge(edge);
      xlib::XChangeWindowAttributes(c->parent, CWCursor, &attr);
      c->cursor = edge;
    }
  }
}

extern void DispatchXEvent(XEvent* ev) {
  switch (ev->type) {
#define EV(x)  \
  case x:      \
    Ev##x(ev); \
    break

    EV(Expose);
    EV(MotionNotify);
    EV(ButtonPress);
    EV(ButtonRelease);
    EV(FocusIn);
    EV(FocusOut);
    EV(MapRequest);
    EV(ConfigureRequest);
    EV(UnmapNotify);
    EV(DestroyNotify);
    EV(ClientMessage);
    EV(PropertyNotify);
    EV(ReparentNotify);
    EV(EnterNotify);
    EV(CirculateRequest);
    EV(ConfigureNotify);
#undef EV

    case LeaveNotify:
    case CreateNotify:
    case GravityNotify:
    case MapNotify:
    case MappingNotify:
    case SelectionClear:
    case SelectionNotify:
    case SelectionRequest:
    case NoExpose:
      break;
    default:
      LOGI_IF(!shapeEvent(ev)) << "unknown event " << ev->type;
  }
}
