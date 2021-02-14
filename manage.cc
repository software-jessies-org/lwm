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

#include <signal.h>
#include <optional>

// These are Motif definitions from Xm/MwmUtil.h, but Motif isn't available
// everywhere.
#define MWM_HINTS_FUNCTIONS (1L << 0)
#define MWM_HINTS_DECORATIONS (1L << 1)
#define MWM_HINTS_INPUT_MODE (1L << 2)
#define MWM_HINTS_STATUS (1L << 3)
#define MWM_DECOR_ALL (1L << 0)
#define MWM_DECOR_BORDER (1L << 1)
#define MWM_DECOR_RESIZEH (1L << 2)
#define MWM_DECOR_TITLE (1L << 3)
#define MWM_DECOR_MENU (1L << 4)
#define MWM_DECOR_MINIMIZE (1L << 5)
#define MWM_DECOR_MAXIMIZE (1L << 6)

#include "lwm.h"

int getProperty(Window, Atom, Atom, long, unsigned char**);
int getWindowState(Window, int*);
// void applyGravity(Client*);

static Point NextAutoPosition(const Area& client_area) {
  // These are static so that the windows aren't all opened at exactly
  // the same place, but rather the opening position advances down and to
  // the right with each successive window opened.
  static unsigned int auto_x = 100;
  static unsigned int auto_y = 100;

  // First, find the primary screen area.
  const Rect scr = LScr::I->GetPrimaryVisibleArea(true);  // With struts.
  // If auto_x and auto_y are outside the main visible area, reset them.
  // This can happen after a change of monitor configuration.
  if (!scr.contains(auto_x, auto_y)) {
    auto_x = scr.xMin + 100;
    auto_y = scr.yMin + 100;
  }

  Point res{};
  if (auto_x + client_area.width > scr.xMax &&
      client_area.width <= scr.width()) {
    // If the window wouldn't fit using normal auto-placement but is small
    // enough to fit horizontally, then centre the window horizontally.
    res.x = scr.xMin + (scr.width() - client_area.width) / 2;
    auto_x = scr.xMin + 20;
  } else {
    res.x = auto_x;
    auto_x += AUTO_PLACEMENT_INCREMENT;
    if (auto_x > (scr.xMin + scr.xMax) / 2) {  // Past middle.
      auto_x = scr.xMin + 20;
    }
  }

  if (auto_y + client_area.height > scr.yMax &&
      client_area.height <= scr.height()) {
    // If the window wouldn't fit using normal auto-placement but is small
    // enough to fit vertically, then centre the window vertically.
    res.y = scr.yMin + (scr.height() - client_area.height) / 2;
    auto_y = scr.yMin + 20;
  } else {
    res.y = auto_y;
    auto_y += AUTO_PLACEMENT_INCREMENT;
    if (auto_y > (scr.yMin + scr.yMax) / 2) {  // Past middle.
      auto_y = scr.yMin + 20;
    }
  }
  return res;
}

std::optional<bool> motifWouldDecorate(Client* c) {
  unsigned long* p = 0;
  if (getProperty(c->window, motif_wm_hints, motif_wm_hints, 5L,
                  (unsigned char**)&p) <= 0) {
    return {};
  }
  if ((p[0] & MWM_HINTS_DECORATIONS) &&
      !(p[2] & (MWM_DECOR_BORDER | MWM_DECOR_ALL))) {
    return false;
  }
  XFree(p);
  return true;
}

/*ARGSUSED*/
void manage(Client* c) {
  LOGD(c) << ">>> manage";
  // get the EWMH window type, as this might overrule some hints
  c->wtype = ewmh_get_window_type(c->window);
  // get in the initial EWMH state
  ewmh_get_state(c);
  // set EWMH allowable actions, now we intend to manage this window
  ewmh_set_allowed(c);
  // is this window to have a frame?
  c->framed = ewmh_hasframe(c);
  // Java JFrames with 'setUndecorated(true)' use motif WM hints (specifically
  // MWM_DECOR_ALL) to disable borders. They also set _OL_DECOR_DEL, but LWM
  // doesn't interpret those.
  // What Java does _not_ do is to use the EWMH window type to remove borders.
  // So rather than relying exclusively on the EWMH type when the window type
  // is present, we also add the motif hint if it's there.
  // Changed on 2021-02-14 to make Evergreen's auto-completion popup be
  // correctly drawn without borders. Hopefully this won't have any negative
  // side-effects.
  if (auto motif_decor = motifWouldDecorate(c); motif_decor) {
    c->framed = *motif_decor;
  }
  if (isShaped(c->window)) {
    c->framed = false;
  }

  // get the EWMH strut - if there is one
  ewmh_get_strut(c);

  // Get the hints, window name, and normal hints (see ICCCM section 4.1.2.3).
  XWMHints* hints = XGetWMHints(dpy, c->window);
  if (Resources::I->ProcessAppIcons()) {
    if (hints) {
      c->SetIcon(xlib::ImageIcon::Create(hints->icon_pixmap, hints->icon_mask));
    }
    c->SetIcon(ewmh_get_window_icon(c));
  }

  getWindowName(c);
  getVisibleWindowName(c);

  // Scan the list of atoms on WM_PROTOCOLS to see which of the
  // protocols that we understand the client is prepared to
  // participate in. (See ICCCM section 4.1.2.7.)
  Atom* protocols;
  int num_protocols;
  if (XGetWMProtocols(dpy, c->window, &protocols, &num_protocols) != 0) {
    for (int p = 0; p < num_protocols; p++) {
      if (protocols[p] == wm_delete) {
        c->proto |= Pdelete;
      } else if (protocols[p] == wm_take_focus) {
        c->proto |= Ptakefocus;
      }
    }
    XFree(protocols);
  }

  // Get the WM_TRANSIENT_FOR property (see ICCCM section 4.1.2.6).
  getTransientFor(c);

  // Work out details for the Client structure from the hints.
  if (hints && (hints->flags & InputHint)) {
    c->accepts_focus = hints->input;
  }

  int state;
  if (!getWindowState(c->window, &state)) {
    state = hints ? hints->initial_state : NormalState;
  }

  // Sort out the window's position.
  xlib::WindowGeometry geom = xlib::XGetGeometry(c->window);
  if (!geom.ok) {
    LOGE() << "Failed to get geometry for " << WinID(c->window);
    return;
  }
  // OpenGL programs (according to an old comment in here) can apparently
  // appear with 0 size, while their minimum sizes are larger than this.
  // Therefore, use the client's size limitations to ensure the original
  // size is sane.
  Rect rect = c->LimitResize(geom.rect);

  // If the position is zero, we assume there's none specified and we have
  // to invent a good position ourselves. However, we only do this for framed
  // windows, as it's perfectly reasonable for a launcher (eg gummiband) to
  // want to place itself at the origin of the screen.
  if (c->framed && rect.xMin == 0 && rect.yMin == 0) {
    Point p = NextAutoPosition(rect.area());
    rect = Rect::Translate(rect, p);
  }
  // There was code here which called 'applyGravity' if there was a user-
  // -specified position, or is_initialising was set. Apparently this is
  // in accordance with section 4.1.2.3 of the ICCCM.

  if (hints) {
    XFree(hints);
  }

  if (c->framed) {
    c->FurnishAt(rect);
  }

  // Stupid X11 doesn't let us change border width in the above
  // call. It's a window attribute, but it's somehow second-class.
  //
  // As pointed out by Adrian Colley, we can't change the window
  // border width at all for InputOnly windows.
  const XWindowAttributes current_attr = xlib::XGetWindowAttributes(c->window);
  if (current_attr.c_class != InputOnly) {
    XSetWindowBorderWidth(dpy, c->window, 0);
  }

  XSetWindowAttributes attr;
  attr.event_mask = ColormapChangeMask | EnterWindowMask | PropertyChangeMask |
                    FocusChangeMask;
  attr.win_gravity = StaticGravity;
  attr.do_not_propagate_mask = ButtonMask;
  xlib::XChangeWindowAttributes(
      c->window, CWEventMask | CWWinGravity | CWDontPropagate, &attr);

  if (c->framed) {
    xlib::XReparentWindow(c->window, c->parent, borderWidth(),
                          borderWidth() + textHeight());
  }

  setShape(c);

  XAddToSaveSet(dpy, c->window);
  if (state == IconicState) {
    c->Hide();
  } else {
    // Map the new window in the relevant state.
    c->hidden = false;
    xlib::XMapWindow(c->parent);
    xlib::XMapWindow(c->window);
    c->SetState(NormalState);
  }

  if (c->wstate.fullscreen) {
    c->EnterFullScreen();
  }

  if (!c->HasFocus()) {
    c->FocusLost();
  }
  LOGD(c) << "<<< manage";
}

void getTransientFor(Client* c) {
  Window trans = None;
  // XGetTransientForHint returns a Status indicating success or failure.
  // It is important to realise, however, that a zero status does not
  // necessarily indicate an error, but also occurs when there is no transient
  // window.
  // It is therefore vitally important to, on failure, set c->trans to None.
  // If this is not done, it causes a really annoying bug in Terminator, such
  // that if you open a window from another, then open a modal dialog from the
  // second, then the first window will now start considering the second
  // terminal to be its 'trans' window. This is caused by the wacky way in
  // which Java implements modal dialogs.
  // Anyway, you have been warned: do not remove the setting of c->trans to
  // None on failure!
  if (XGetTransientForHint(dpy, c->window, &trans)) {
    LOGD(c) << "Transient for window " << WinID(trans);
    c->trans = trans;
  } else {
    c->trans = None;
  }
}

void withdraw(Client* c) {
  if (c->parent != LScr::I->Root()) {
    xlib::XUnmapWindow(c->parent);
    // This seems to make no sense. Surely we just want to unmap our frame,
    // but we certainly shouldn't then reparent our frame to the root. That's
    // just weird.
    //    xlib::XReparentWindow(c->parent, LScr::I->Root(), c->size.x,
    //    c->size.y);
  }

  XRemoveFromSaveSet(dpy, c->window);
  c->SetState(WithdrawnState);

  // Flush and ignore any errors. X11 sends us an UnmapNotify before it
  // sends us a DestroyNotify. That means we can get here without knowing
  // whether the relevant window still exists.
  ScopedIgnoreBadWindow ignorer;
  XSync(dpy, false);
}

/*ARGSUSED*/
void Terminate(int signal) {
  // Set all clients free.
  Client_FreeAll();

  // Give up the input focus and the colourmap.
  XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
  // XCloseDisplay (or rather, XSync as called by XCloseDisplay) dumps a load
  // of BadMatch errors into the error handler. That's unhelpful spam, so
  // inform the error handler that it should ignore them.
  ScopedIgnoreBadMatch ignorer;
  XCloseDisplay(dpy);
  session_end();

  if (signal == SIGHUP) {
    forceRestart = true;
  } else if (signal) {
    exit(EXIT_FAILURE);
  } else {
    exit(EXIT_SUCCESS);
  }
}

int getProperty(Window w, Atom a, Atom type, long len, unsigned char** p) {
  Atom real_type = 0;
  int format = 0;
  unsigned long n = 0;
  unsigned long extra = 0;

  // len is in 32-bit multiples.
  int status = XGetWindowProperty(dpy, w, a, 0L, len, false, type, &real_type,
                                  &format, &n, &extra, p);
  if (status != Success || *p == 0) {
    return -1;
  }
  if (n == 0 && p) {
    XFree(*p);
  }
  // could check real_type, format, extra here...
  return n;
}

void getWindowName(Client* c) {
  if (!c) {
    return;
  }
  const std::string old_name = c->Name();
  ewmh_get_window_name(c);
  if (old_name != c->Name()) {
    c->DrawBorder();
  }
}

void getVisibleWindowName(Client* c) {
  if (!c) {
    return;
  }
  const std::string old_name = c->Name();
  ewmh_get_visible_window_name(c);
  if (old_name != c->Name()) {
    c->DrawBorder();
  }
}

int getWindowState(Window w, int* state) {
  long* p = 0;

  if (getProperty(w, wm_state, wm_state, 2L, (unsigned char**)&p) <= 0) {
    return 0;
  }
  *state = (int)*p;
  XFree(p);
  return 1;
}
