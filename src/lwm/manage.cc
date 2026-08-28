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

#include "client.h"
#include "debug.h"
#include "error.h"
#include "ewmh.h"
#include "lwm.h"
#include "manage.h"
#include "resource.h"
#include "screen.h"
#include "screenlayout.h"
#include "session.h"
#include "shape.h"
#include "xfont.h"
#include "xlib.h"

int getWindowState(Window, int*);
// void applyGravity(Client*);

std::optional<bool> motifWouldDecorate(Client* c) {
  // _MOTIF_WM_HINTS is a format-32 property of 5 words, whose type is the atom
  // of the same name; read it as 32-bit words. See the note on
  // xlib::WindowProperty.
  //
  // Mind the argument order. The Xlib-era getProperty() this replaced took
  // (window, property, type, length); xlib::XGetWindowProperty takes
  // (window, property, length, type). Getting it the wrong way round doesn't
  // fail loudly: X answers a type mismatch with the *real* type and format and
  // an empty value, so prop.ok() is false and this reads as "the window has no
  // Motif hints" for every window that has them.
  const xlib::WindowProperty prop = xlib::XGetWindowProperty(
      c->window, motif_wm_hints, 5L, motif_wm_hints);
  const std::vector<uint32_t>& p = prop.Data32();
  if (!prop.ok() || p.size() < 3) {
    return {};
  }
  if ((p[0] & MWM_HINTS_DECORATIONS) &&
      !(p[2] & (MWM_DECOR_BORDER | MWM_DECOR_ALL))) {
    return false;
  }
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
  const xlib::WMHints hints = xlib::XGetWMHints(c->window);
  if (Resources::I->ProcessAppIcons()) {
    if (hints.ok) {
      c->SetIcon(xlib::ImageIcon::Create(hints.icon_pixmap, hints.icon_mask));
    }
    c->SetIcon(ewmh_get_window_icon(c));
  }

  getWindowName(c);
  getVisibleWindowName(c);

  // Scan the list of atoms on WM_PROTOCOLS to see which of the
  // protocols that we understand the client is prepared to
  // participate in. (See ICCCM section 4.1.2.7.)
  for (const Atom proto : xlib::XGetWMProtocols(c->window)) {
    if (proto == wm_delete) {
      c->proto |= Pdelete;
    } else if (proto == wm_take_focus) {
      c->proto |= Ptakefocus;
    }
  }

  // Get the WM_TRANSIENT_FOR property (see ICCCM section 4.1.2.6).
  getTransientFor(c);

  // Work out details for the Client structure from the hints.
  if (hints.has_input) {
    c->accepts_focus = hints.input;
  }

  int state;
  if (!getWindowState(c->window, &state)) {
    state = hints.has_initial_state ? hints.initial_state : NormalState;
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
    Point p = LScr::I->NextAutoPosition(rect.area());
    rect = Rect::Translate(rect, p);
  }
  // There was code here which called 'applyGravity' if there was a user-
  // -specified position, or is_initialising was set. Apparently this is
  // in accordance with section 4.1.2.3 of the ICCCM.

  if (c->framed) {
    c->FurnishAt(rect);
  } else {
    // An undecorated window the exact size of a monitor, or of a monitor's
    // work area, is going full screen the borderless way, and may have got the
    // geometry slightly wrong; put it on the monitor. See SnapToMonitor. This
    // has to happen here as well as in EvConfigureRequest, because a client
    // that gets the geometry it wants at creation time never sends us a
    // configure request at all. Clients with struts of their own are exempt:
    // a panel filling the work area is doing what it means to.
    if (!c->HasStruts()) {
      const Rect snapped =
          SnapToMonitor(c->ContentRect(), LScr::I->VisibleAreas(false),
                        LScr::I->Strut());
      if (snapped != c->ContentRect()) {
        LOGD(c) << "Snapping undecorated full-monitor window to " << snapped;
        // MoveResizeTo, not MoveTo: a window sized to the work area has to
        // grow over the panel as well as move.
        c->MoveResizeTo(snapped);
      }
    }
  }

  // Stupid X11 doesn't let us change border width in the above
  // call. It's a window attribute, but it's somehow second-class.
  //
  // As pointed out by Adrian Colley, we can't change the window
  // border width at all for InputOnly windows.
  const xlib::WindowAttributes current_attr =
      xlib::XGetWindowAttributes(c->window);
  if (!current_attr.input_only) {
    xlib::XSetWindowBorderWidth(c->window, 0);
  }

  xlib::XChangeWindowAttributes(
      c->window,
      xlib::WindowAttrs()
          .EventMask(XCB_EVENT_MASK_COLOR_MAP_CHANGE |
                     XCB_EVENT_MASK_ENTER_WINDOW |
                     XCB_EVENT_MASK_PROPERTY_CHANGE |
                     XCB_EVENT_MASK_FOCUS_CHANGE)
          .WinGravity(XCB_GRAVITY_STATIC)
          .DontPropagate(ButtonMask));

  if (c->framed) {
    xlib::XReparentWindow(c->window, c->parent, borderWidth(),
                          borderWidth() + xfont::TextHeight());
  }
  // The client window is the only place a Windows-key gesture can start, so
  // it's the window the grabs go on. Reparenting doesn't disturb them.
  c->GrabSuperButtons();

  setShape(c);

  xlib::XAddToSaveSet(c->window);
  if (state == IconicState) {
    c->Hide();
  } else {
    // Map the new window in the relevant state.
    c->hidden = false;
    xlib::XMapWindow(c->parent);
    xlib::XMapWindow(c->window);
    c->SetState(NormalState);
  }

  // A client says how it wants to start by setting _NET_WM_STATE before it maps
  // the window, and ewmh_get_state has already read it into c->wstate. Clear
  // the maximisation flags again before handing them to SetMaximized: it needs
  // to see the transition, because that's when it records the geometry to
  // restore to.
  if (c->IsMaximized()) {
    const bool vert = c->wstate.maximized_vert;
    const bool horz = c->wstate.maximized_horz;
    c->wstate.maximized_vert = false;
    c->wstate.maximized_horz = false;
    c->SetMaximized(vert, horz);
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
  // xlib::XGetTransientForHint returns None both on failure and when there is
  // no transient window.
  // It is therefore vitally important to, on failure, set c->trans to None.
  // If this is not done, it causes a really annoying bug in Terminator, such
  // that if you open a window from another, then open a modal dialog from the
  // second, then the first window will now start considering the second
  // terminal to be its 'trans' window. This is caused by the wacky way in
  // which Java implements modal dialogs.
  // Anyway, you have been warned: do not remove the setting of c->trans to
  // None on failure!
  c->trans = xlib::XGetTransientForHint(c->window);
  if (c->trans != XCB_NONE) {
    LOGD(c) << "Transient for window " << WinID(c->trans);
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

  xlib::XRemoveFromSaveSet(c->window);
  c->SetState(WithdrawnState);

  // Sync, and ignore any errors the requests above provoked. X11 sends us an
  // UnmapNotify before it sends us a DestroyNotify, so we can get here without
  // knowing whether the relevant window still exists. The Sync matters: it's
  // what guarantees those errors have arrived (and so are still inside this
  // scope's sequence range) before we stop ignoring them.
  ScopedIgnoreBadWindow ignorer;
  xlib::Sync();
}

/*ARGSUSED*/
void Terminate(int signal) {
  // Set all clients free.
  Client_FreeAll();

  // Give up the input focus and the colourmap.
  xlib::XSetInputFocus(XCB_INPUT_FOCUS_POINTER_ROOT,
                       XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
  // Closing the display dumps a load of BadMatch errors into the log. That's
  // unhelpful spam on the way out, so say we're not interested.
  ScopedIgnoreBadMatch ignorer;
  xlib::CloseDisplay();
  session_end();

  if (signal == SIGHUP) {
    forceRestart = true;
  } else if (signal) {
    exit(EXIT_FAILURE);
  } else {
    exit(EXIT_SUCCESS);
  }
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
  // WM_STATE is CARDINAL[2]/32: the state, then the icon window.
  const xlib::WindowProperty prop =
      xlib::XGetWindowProperty(w, wm_state, 2L, wm_state);
  if (!prop.ok() || prop.Data32().empty()) {
    return 0;
  }
  *state = int(prop.Data32()[0]);
  return 1;
}
