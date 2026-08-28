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
#include "keyboard.h"
#include "lwm.h"
#include "hider.h"
#include "manage.h"
#include "resource.h"
#include "screen.h"
#include "screenlayout.h"
#include "shape.h"
#include "xdebugprint.h"
#include "xfont.h"
#include "xlib.h"

// Handlers below take the raw event and cast it themselves, which keeps the
// dispatch table at the bottom of the file a straight one-liner per type.
// There is no equivalent of Xlib's XAnyEvent: the window a given event is
// about sits at a different offset in each struct, and is variously called
// `window` or `event` depending on the type. That is why each handler names
// its own struct rather than sharing a common accessor.

void EvExpose(xcb_generic_event_t* ev) {
  const xcb_expose_event_t* e = (const xcb_expose_event_t*)ev;
  // Only handle the last in a group of Expose events.
  if (e->count != 0) {
    return;
  }

  Window w = e->window;

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

// Set while the drag in progress is one lwm had to take an explicit pointer
// grab for; see EvClientMessage's _NET_WM_MOVERESIZE handling. Every other
// drag begins with a button press lwm received itself, which comes with an
// implicit grab the server tears down on the button release, so there is
// nothing for lwm to give back.
static bool drag_owns_pointer_grab = false;

// Use this to set or clear the drag handler. Will destroy the old handler if
// one is present. The new handler's Start() function is called with ev.
//
// This is the one place a drag ends, so it is also where the pointer grab is
// returned. Every path that finishes a drag must come through here.
void startDragging(DragHandler* handler, xcb_generic_event_t* ev) {
  delete current_dragger;
  if (drag_owns_pointer_grab) {
    xlib::XUngrabPointer(XCB_CURRENT_TIME);
    drag_owns_pointer_grab = false;
  }
  current_dragger = handler;
  if (current_dragger) {
    current_dragger->Start(ev);
  }
}

// Stops the dragging, calling the Stop handler of current_dragger if there is
// one.
void stopDragging(xcb_generic_event_t* ev) {
  if (ev && current_dragger) {
    current_dragger->End(ev);
  }
  startDragging(nullptr, nullptr);
}

void EvButtonPress(xcb_generic_event_t* ev) {
  if (current_dragger) {
    LOGI() << "Already doing something";
    return;  // Already doing something.
  }
  startDragging(getDragHandlerForEvent((xcb_button_press_event_t*)ev), ev);
}

void EvButtonRelease(xcb_generic_event_t* ev) {
  stopDragging(ev);
}

void EvCirculateRequest(xcb_generic_event_t* ev) {
  const xcb_circulate_request_event_t* e =
      (const xcb_circulate_request_event_t*)ev;
  Client* c = LScr::I->GetClient(e->window);
  LOGD(c) << "CirculateRequest";
  if (c == nullptr) {
    if (e->place == XCB_PLACE_ON_TOP) {
      xlib::XRaiseWindow(e->window);
    } else {
      xlib::XLowerWindow(e->window);
    }
  } else {
    if (e->place == XCB_PLACE_ON_TOP) {
      c->Raise();
    } else {
      c->Lower();
    }
  }
}

void EvMapRequest(xcb_generic_event_t* ev) {
  const xcb_map_request_event_t* e = (const xcb_map_request_event_t*)ev;
  Client* c = LScr::I->GetOrAddClient(e->window, false);
  if (c == nullptr) {
    // Nothing we can manage. In practice this means the window was destroyed
    // between the server sending us this request and our asking about it, so
    // the attributes came back empty; GetOrAddClient also declines our own
    // windows and override-redirect ones.
    return;
  }
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
        // Not the furniture offset spelled out: a window whose furniture the
        // user turned off is framed too, and its client window belongs at the
        // frame's origin.
        const Point p = c->ContentRectRelative().origin();
        xlib::XReparentWindow(c->window, c->parent, p.x, p.y);
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

void EvUnmapNotify(xcb_generic_event_t* ev) {
  const xcb_unmap_notify_event_t* e = (const xcb_unmap_notify_event_t*)ev;
  // Don't scan the window's parents for a match - we only care about unmapping
  // of top-level client windows, so anything underneath we can ignore.
  Client* c = LScr::I->GetClient(e->window, false);
  if (c == nullptr) {
    return;
  }
  // Be careful here. We only want to respond to unmaps on client windows that
  // we're managing. For example, if this isn't the direct client window,
  // then do nothing.
  if (c->window != e->window) {
    return;
  }
  // An unmap lwm itself asked for. That's either a direct one - hiding a
  // client with no frame to unmap in its place - or the server's own doing on
  // the way through a reparent, which is how the decoration toggle works. The
  // event is identical either way, so Client counts the ones we cause and we
  // tick them off here.
  if (c->TakeExpectedUnmap()) {
    LOGD(c) << "Ignoring an unmap we asked for";
    return;
  }
  // Plus, when we reparent the client window to our frame, we'll receive an
  // unmap notification with window=child window, and parent=root. Check for
  // this, and ignore it.
  //
  // Only for clients we actually reparented, though. An unframed client (a
  // dock, a menu, a splash screen - see ewmh_hasframe) is left where it is, a
  // direct child of the root, so the root's SubstructureNotify is the *only*
  // way we ever hear that it has unmapped itself. Dropping that used to leave
  // such a client in our save-set for ever, and when a client disconnects the
  // X server maps every unmapped window in its save-set - so lwm's own exit
  // reopened gummiband's closed menu, which then looked like the next lwm
  // making it visible at start-up.
  if (c->parent != LScr::I->Root() && e->event == LScr::I->Root()) {
    return;
  }
  // If we got here, then this is a client withdrawing a window we manage. We
  // therefore withdraw ourselves.
  LOGD(c) << "Withdrawing unmapped window";
  withdraw(c);
}

// The geometry an unframed client's configure request should really be
// granted at: what it asked for, unless SnapToMonitor puts it on a monitor.
//
// Only a request that names all four of x, y, width and height is considered.
// That's what a client sizing itself to a monitor sends, and it means we never
// have to combine the request with lwm's own idea of where the window is -
// which, for an unframed client, is only ever the geometry it had when we
// adopted it, because these requests are passed through rather than applied
// through Client.
//
// A client that sets a strut is exempt: it has reserved a piece of the screen
// for its own window, so a panel that happens to be the size of the work area
// is doing exactly what it means to, and blowing it up to the size of the
// monitor would be lwm covering the screen with its own furniture.
static Rect snapUndecoratedFullScreen(
    const Client* c,
    const xcb_configure_request_event_t& e) {
  const Rect asked = Rect::FromXYWH(e.x, e.y, e.width, e.height);
  const uint16_t needed = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                          XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
  if (c == nullptr || c->framed || c->HasStruts() ||
      (e.value_mask & needed) != needed) {
    return asked;
  }
  const Rect snapped =
      SnapToMonitor(asked, LScr::I->VisibleAreas(false), LScr::I->Strut());
  if (snapped != asked) {
    LOGD(c) << "Snapping undecorated full-monitor request " << asked << " to "
            << snapped;
  }
  return snapped;
}

// Shared by the real ConfigureRequest event and by _NET_MOVERESIZE_WINDOW,
// which is defined as meaning the same thing.
static void handleConfigureRequest(const xcb_configure_request_event_t& e) {
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
    // Pass the request straight through, honouring exactly the fields the
    // client named - with one exception. An undecorated window asking to be
    // exactly the size of a monitor, or of a monitor's work area, is going
    // full screen the borderless way, and if it's got the geometry slightly
    // wrong it can only be by mistake, so put it on the monitor. See
    // SnapToMonitor.
    const Rect granted = snapUndecoratedFullScreen(c, e);
    xlib::WindowChanges wc;
    wc.FromRequestMask(e.value_mask, granted.xMin, granted.yMin,
                       granted.width(), granted.height(), e.border_width,
                       e.sibling, e.stack_mode);
    xlib::XConfigureWindow(e.window, wc);
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
  if ((e.value_mask &
       (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
        XCB_CONFIG_WINDOW_HEIGHT)) == 0) {
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
  if (e.value_mask & XCB_CONFIG_WINDOW_X) {
    int diff = (e.x + offset.x) - new_rect.xMin;
    new_rect.xMin += diff;
    new_rect.xMax += diff;
  }
  if (e.value_mask & XCB_CONFIG_WINDOW_Y) {
    int diff = (e.y + offset.y) - new_rect.yMin;
    new_rect.yMin += diff;
    new_rect.yMax += diff;
  }
  if (e.value_mask & XCB_CONFIG_WINDOW_WIDTH) {
    new_rect.xMax = new_rect.xMin + e.width;
  }
  if (e.value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
    new_rect.yMax = new_rect.yMin + e.height;
  }

  // The frame takes the client's stacking request, but our own geometry and
  // border. Only the fields the client asked about are sent, same as before.
  // A full-screen frame has no border, so that it lines up with the monitor
  // exactly (see Client::EnterFullScreen); don't hand it one back here.
  const int frame_border = c->wstate.fullscreen ? 0 : kFrameBorderWidth;
  const Rect frame = c->FrameRect();
  xlib::WindowChanges frame_wc;
  frame_wc.FromRequestMask(e.value_mask, frame.xMin, frame.yMin, frame.width(),
                           frame.height(), frame_border, e.sibling,
                           e.stack_mode);
  xlib::XConfigureWindow(e.parent, frame_wc);
  c->SendConfigureNotify();

  const Rect content = c->ContentRectRelative();
  xlib::WindowChanges content_wc;
  content_wc.FromRequestMask(e.value_mask, content.xMin, content.yMin,
                             content.width(), content.height(), 0, e.sibling,
                             e.stack_mode);
  xlib::XConfigureWindow(e.window, content_wc);

  if (new_rect.area() == c->ContentRect().area()) {
    c->MoveTo(new_rect);
  } else {
    c->MoveResizeTo(new_rect);
  }
}

void EvConfigureRequest(xcb_generic_event_t* ev) {
  handleConfigureRequest(*(const xcb_configure_request_event_t*)ev);
}

void EvConfigureNotify(xcb_generic_event_t*) {}

void EvDestroyNotify(xcb_generic_event_t* ev) {
  const xcb_destroy_notify_event_t* e = (const xcb_destroy_notify_event_t*)ev;
  // Request the client, but without scanning this window's parents for it.
  // The window is gone, so any attempt to scan the window tree will result in
  // errors.
  Client* c = LScr::I->GetClient(e->window, false);
  if (c == 0) {
    return;
  }
  ScopedIgnoreBadWindow ignorer;
  c->Remove();
}

void EvClientMessage(xcb_generic_event_t* ev) {
  const xcb_client_message_event_t* e = (const xcb_client_message_event_t*)ev;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }
  // Note data32, not Xlib's data.l: the wire format is five 32-bit words, and
  // Xlib's `long` array was only ever a 64-bit-widened view of them.
  const uint32_t* data = e->data.data32;
  if (e->type == wm_change_state) {
    if (e->format == 32 && data[0] == IconicState && c->IsNormal()) {
      LOGD(c) << "Client message: requested hide";
      c->Hide();
    }
    return;
  }
  if (e->type == ewmh_atom[_NET_WM_STATE] && e->format == 32) {
    LOGD(c) << "Client message: WM state change: " << data[0] << " -> "
            << data[1] << ", " << data[2];
    ewmh_change_state(c, data[0], data[1]);
    ewmh_change_state(c, data[0], data[2]);
    return;
  }
  if (e->type == ewmh_atom[_NET_ACTIVE_WINDOW] && e->format == 32) {
    LOGD(c) << "Client message: requested set active: unhiding";
    // An EWMH enabled application has asked for this client to be made the
    // active window. Unhide also raises and gives focus to the window.
    c->Unhide();
    return;
  }
  if (e->type == ewmh_atom[_NET_CLOSE_WINDOW] && e->format == 32) {
    LOGD(c) << "Client message: requested close";
    c->Close();
    return;
  }
  if (e->type == ewmh_atom[_NET_MOVERESIZE_WINDOW] && e->format == 32) {
    // FIXME: ok, so this is a bit of a hack
    xcb_configure_request_event_t req{};
    req.window = e->window;
    req.parent = c->parent;
    req.x = data[1];
    req.y = data[2];
    req.width = data[3];
    req.height = data[4];
    req.value_mask = 0;
    if (data[0] & (1 << 8)) {
      req.value_mask |= XCB_CONFIG_WINDOW_X;
    }
    if (data[0] & (1 << 9)) {
      req.value_mask |= XCB_CONFIG_WINDOW_Y;
    }
    if (data[0] & (1 << 10)) {
      req.value_mask |= XCB_CONFIG_WINDOW_WIDTH;
    }
    if (data[0] & (1 << 11)) {
      req.value_mask |= XCB_CONFIG_WINDOW_HEIGHT;
    }
    LOGD(c) << "Client message: move/resize -> " << req << " (flags "
            << req.value_mask << ")";
    handleConfigureRequest(req);
    return;
  }
  if (e->type == ewmh_atom[_NET_WM_MOVERESIZE] && e->format == 32) {
    LOGD(c) << "Client message: requested _NET_WM_MOVERESIZE";
    Edge edge = E_LAST;
    EWMHDirection direction = (EWMHDirection)data[2];

    if (direction == DMoveResizeCancel) {
      // Only cancel a drag we started for this client; a stale cancel must not
      // interrupt whatever the user is doing to some other window.
      if (current_dragger && drag_owns_pointer_grab) {
        LOGD(c) << "_NET_WM_MOVERESIZE: cancelled by client";
        startDragging(nullptr, nullptr);
      }
      return;
    }
    if (current_dragger) {
      // The user is already dragging something. Whatever this client thinks is
      // happening, taking the pointer away mid-gesture would be worse.
      LOGD(c) << "_NET_WM_MOVERESIZE: ignored, a drag is already in progress";
      return;
    }

    // before we can do any resizing, make the window visible
    if (c->IsHidden()) {
      c->Unhide();
    }
    if (c->framed) {
      xlib::XMapWindow(c->parent);
    }
    c->Raise();
    // x_root and y_root (data[0] and data[1]) are ignored on purpose: the drag
    // handlers ask the server where the pointer is when they start, which is
    // both more current than the coordinates in the message and correct even
    // if the client rounded them.
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
    if (edge == E_LAST) {
      return;  // A direction we don't implement; nothing to start.
    }
    // The press that began this drag went to the client, not to lwm, so there
    // is no implicit grab bringing us the motion and release events the drag
    // needs. Take an explicit one, with the cursor for the edge, exactly as
    // the Windows-key gestures do.
    //
    // Two details. The grab goes on the root, not on the client: a resize drag
    // routinely takes the pointer outside the window being resized, and the
    // events have to keep coming when it does. And owner_events is false, so
    // that every button and motion event comes to lwm for the duration rather
    // than being delivered to whichever of our own windows it lands on.
    //
    // Asynchronous, like the gesture grabs: freezing the pointer until lwm
    // replays the events would stall the client for the whole drag.
    if (!xlib::XGrabPointer(
            LScr::I->Root(), false, ButtonMask | XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
            LScr::I->Cursors()->ForEdge(edge), XCB_CURRENT_TIME)) {
      LOGD(c) << "_NET_WM_MOVERESIZE: could not grab the pointer";
      return;
    }
    // data[3] is the button the user is holding. A handler is only returned if
    // it's one whose release we can recognise; see getMoveResizeHandler.
    DragHandler* handler = getMoveResizeHandler(c, edge, data[3]);
    if (!handler) {
      xlib::XUngrabPointer(XCB_CURRENT_TIME);
      return;
    }
    startDragging(handler, ev);
    drag_owns_pointer_grab = true;
  }
}

void EvPropertyNotify(xcb_generic_event_t* ev) {
  const xcb_property_notify_event_t* e =
      (const xcb_property_notify_event_t*)ev;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }
  // This function can be called for a window that has already been destroyed.
  // That's fine; we'll expect functions we call to handle errors properly, but
  // we'll stomp on the printing of error logs.
  ScopedIgnoreBadWindow ignorer;

  if (e->atom == _mozilla_url || e->atom == XCB_ATOM_WM_NAME) {
    LOGD(c) << "Property change: XA_WM_NAME";
    getWindowName(c);
  } else if (e->atom == ewmh_atom[_NET_WM_VISIBLE_NAME]) {
    LOGD(c) << "Property change: _NET_WM_VISIBLE_NAME";
    getVisibleWindowName(c);
  } else if (e->atom == XCB_ATOM_WM_TRANSIENT_FOR) {
    LOGD(c) << "Property change: XA_WM_TRANSIENT_FOR";
    getTransientFor(c);
  } else if (e->atom == XCB_ATOM_WM_NORMAL_HINTS) {
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

void EvReparentNotify(xcb_generic_event_t* ev) {
  const xcb_reparent_notify_event_t* e =
      (const xcb_reparent_notify_event_t*)ev;
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

void EvFocusIn(xcb_generic_event_t* ev) {
  const xcb_focus_in_event_t* e = (const xcb_focus_in_event_t*)ev;
  // In practice, GetInputFocus returns the child window that actually has
  // focus (in Java apps, the 'FocusProxy' window), while the event reports
  // the top-level window.
  xlib::FocusWindow focus = xlib::XGetInputFocus();
  Window focus_window = focus.window;
  // There seems to be a bug in the Xserver, whereupon for the first focus-in
  // event we receive, GetInputFocus returns focus_window==1, which doesn't
  // correspond to any actual window. In this case, fall back to the window
  // which was specified in the event itself.
  // Without this hack, the first time we change focus after running LWM, we
  // get a spurious error due to trying to look up the parents of window 1.
  if (focus_window == 1) {
    focus_window = e->event;
  }
  Client* c = LScr::I->GetClient(focus_window);
  // A window which has just been hidden generates a FocusIn as the server
  // moves the focus off it, and can still be what GetInputFocus reports.
  // Chasing it would mean asking to focus an unmapped window, which X answers
  // with a BadMatch.
  if (c && !c->IsHidden()) {
    LOGD(c) << "  focusing client; focus window = " << WinID(focus_window);
    LScr::I->GetFocuser()->FocusClient(c);
  }
}

void EvFocusOut(xcb_generic_event_t*) {}

void EvEnterNotify(xcb_generic_event_t* ev) {
  const xcb_enter_notify_event_t* e = (const xcb_enter_notify_event_t*)ev;
  if (current_dragger) {
    return;
  }
  LScr::I->GetFocuser()->EnterWindow(e->event);
  // We receive enter events for our client windows too. When we do, we need
  // to switch the mouse pointer's shape to the default pointer.
  // If we don't do this, then for apps like Rhythmbox which don't
  // aggressively set the pointer to their preferred shape, we end up showing
  // silly icons, such as the 'resize corner' icon, while hovering over the
  // middle of the application window.
  Client* c = LScr::I->GetClient(e->event);
  if (c == nullptr) {
    return;
  }
  if (e->event != c->parent) {
    // TODO: add a SetCursor method to Client, so we don't have to keep
    // repeating this code everywhere.
    xlib::XChangeWindowAttributes(
        c->parent, xlib::WindowAttrs().Cursor(LScr::I->Cursors()->Root()));
    // Record that the current cursor is whatever the child window says it is.
    // This has to be different from any Edge we want to trigger when the mouse
    // crosses window furniture, otherwise we may fail to trigger a cursor
    // switch. For example, were we to set this to ENone, if the mouse were to
    // cross from the client window into the title bar, we'd fail to switch to
    // the 'move window' cursor.
    c->cursor = EContents;
  }
}

void EvMotionNotify(xcb_generic_event_t* ev) {
  if (current_dragger) {
    // A handler returning false has cancelled itself (the client vanished, or
    // the button is no longer down). Retire it through startDragging rather
    // than just forgetting it, so that it's destroyed and any pointer grab it
    // owns is handed back.
    if (!current_dragger->Move(ev)) {
      startDragging(nullptr, nullptr);
    }
    return;
  }
  const xcb_motion_notify_event_t* e = (const xcb_motion_notify_event_t*)ev;
  Client* c = LScr::I->GetClient(e->event);
  if (c == nullptr) {
    return;
  }
  if ((e->event == c->parent) && (e->child != c->window)) {
    Edge edge = c->EdgeAt(e->event, e->event_x, e->event_y);
    if (edge != EContents && c->cursor != edge) {
      xlib::XChangeWindowAttributes(
          c->parent,
          xlib::WindowAttrs().Cursor(LScr::I->Cursors()->ForEdge(edge)));
      c->cursor = edge;
    }
  }
}

static void EvKeyPress(xcb_generic_event_t* ev) {
  // The only keys lwm grabs are the Windows-key navigation ones, so an
  // unrecognised press means the grab and this handler have got out of step.
  // Nothing to do about it but let it go.
  if (!HandleKeyPress((xcb_key_press_event_t*)ev)) {
    LOGI() << "unexpected KeyPress, keycode "
           << int(((xcb_key_press_event_t*)ev)->detail);
  }
}

static void EvMappingNotify(xcb_generic_event_t* ev) {
  xcb_mapping_notify_event_t* e = (xcb_mapping_notify_event_t*)ev;
  // A pointer remapping can't move a keysym, so it can't invalidate a key
  // grab. The other two can: the keycode an arrow sits on changes with the
  // layout, and which modifier is Super changes with the modifier map.
  if (e->request == XCB_MAPPING_POINTER) {
    return;
  }
  GrabNavigationKeys();
}

extern void DispatchXEvent(xcb_generic_event_t* ev) {
  // Bit 0x80 means the event was sent by another client with SendEvent rather
  // than generated by the server. lwm treats both the same, so mask it off.
  switch (ev->response_type & 0x7f) {
#define EV(xcb_name, handler) \
  case xcb_name:              \
    Ev##handler(ev);          \
    break

    EV(XCB_EXPOSE, Expose);
    EV(XCB_MOTION_NOTIFY, MotionNotify);
    EV(XCB_BUTTON_PRESS, ButtonPress);
    EV(XCB_BUTTON_RELEASE, ButtonRelease);
    EV(XCB_FOCUS_IN, FocusIn);
    EV(XCB_FOCUS_OUT, FocusOut);
    EV(XCB_MAP_REQUEST, MapRequest);
    EV(XCB_CONFIGURE_REQUEST, ConfigureRequest);
    EV(XCB_UNMAP_NOTIFY, UnmapNotify);
    EV(XCB_DESTROY_NOTIFY, DestroyNotify);
    EV(XCB_CLIENT_MESSAGE, ClientMessage);
    EV(XCB_PROPERTY_NOTIFY, PropertyNotify);
    EV(XCB_REPARENT_NOTIFY, ReparentNotify);
    EV(XCB_ENTER_NOTIFY, EnterNotify);
    EV(XCB_CIRCULATE_REQUEST, CirculateRequest);
    EV(XCB_CONFIGURE_NOTIFY, ConfigureNotify);
    EV(XCB_KEY_PRESS, KeyPress);
    EV(XCB_MAPPING_NOTIFY, MappingNotify);
#undef EV

    // Response type 0 is not an event at all: it's an error report. Xlib
    // delivered these to a global handler at an unpredictable time; XCB puts
    // them in the queue in sequence, which is what makes the suppression in
    // error.cc precise rather than approximate.
    case 0:
      HandleXError((const xcb_generic_error_t*)ev);
      break;

    case XCB_LEAVE_NOTIFY:
    case XCB_CREATE_NOTIFY:
    case XCB_GRAVITY_NOTIFY:
    case XCB_MAP_NOTIFY:
    // Grabbing a key gets us its release as well as its press. The navigation
    // gestures act on the press alone, so the release is of no interest -
    // but it has to be named here, or it would be logged as unknown.
    case XCB_KEY_RELEASE:
    case XCB_SELECTION_CLEAR:
    case XCB_SELECTION_NOTIFY:
    case XCB_SELECTION_REQUEST:
    case XCB_NO_EXPOSURE:
      break;
    default:
      LOGI_IF(!shapeEvent(ev) && !randrEvent(ev))
          << "unknown event " << int(ev->response_type & 0x7f);
  }
}

extern void ProcessPendingEvents() {
  while (xcb_generic_event_t* ev = xlib::NextEvent()) {
    // Every event carries the sequence number of the last request the server
    // had processed when it was generated, so this is the moment at which we
    // can tell that an error-suppression range is finished with.
    RetireIgnoredSequences(ev->full_sequence);
    DispatchXEvent(ev);
    free(ev);
  }
}
