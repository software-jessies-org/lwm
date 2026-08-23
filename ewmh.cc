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
#include "ewmh.h"
#include "lwm.h"
#include "screen.h"
#include "xlib.h"

// The following two arrays are co-indexed. The ewmh_atom_names is used to look
// up the name of a given Atom value, for debugging.
Atom ewmh_atom[EWMH_ATOM_LAST];
char const* ewmh_atom_names[EWMH_ATOM_LAST];
Atom utf8_string;

std::ostream& operator<<(std::ostream& os, const EWMHWindowState& s) {
#define D(x) os << " " #x << (s.x ? "=t" : "=f")
  D(skip_taskbar);
  D(skip_pager);
  D(fullscreen);
  D(maximized_vert);
  D(maximized_horz);
  D(above);
  D(below);
#undef D
  return os;
}

std::ostream& operator<<(std::ostream& os, const AtomName& an) {
  if (an.a == utf8_string) {
    os << "UTF8_STRING";
    return os;
  }
  for (int i = 0; i < EWMH_ATOM_LAST; i++) {
    if (an.a == ewmh_atom[i]) {
      os << ewmh_atom_names[i];
      return os;
    }
  }
  os << "Atom " << an.a;
  return os;
}

void ewmh_init() {
  // Build half a million EWMH atoms. The names are collected first and
  // interned in one batch: sixty-odd separate interns used to mean sixty-odd
  // blocking round trips at start-up, one after another.
  std::vector<std::string> names(EWMH_ATOM_LAST);
#define SET_ATOM(x)          \
  do {                       \
    names[x] = #x;           \
    ewmh_atom_names[x] = #x; \
  } while (0)
  SET_ATOM(_NET_SUPPORTED);
  SET_ATOM(_NET_CLIENT_LIST);
  SET_ATOM(_NET_CLIENT_LIST_STACKING);
  SET_ATOM(_NET_NUMBER_OF_DESKTOPS);
  SET_ATOM(_NET_DESKTOP_GEOMETRY);
  SET_ATOM(_NET_DESKTOP_VIEWPORT);
  SET_ATOM(_NET_CURRENT_DESKTOP);
  SET_ATOM(_NET_DESKTOP_NAMES);
  SET_ATOM(_NET_ACTIVE_WINDOW);
  SET_ATOM(_NET_WORKAREA);
  SET_ATOM(_NET_SUPPORTING_WM_CHECK);
  SET_ATOM(_NET_VIRTUAL_ROOTS);
  SET_ATOM(_NET_DESKTOP_LAYOUT);
  SET_ATOM(_NET_SHOWING_DESKTOP);
  SET_ATOM(_NET_CLOSE_WINDOW);
  SET_ATOM(_NET_MOVERESIZE_WINDOW);
  SET_ATOM(_NET_WM_MOVERESIZE);
  SET_ATOM(_NET_WM_NAME);
  SET_ATOM(_NET_WM_VISIBLE_NAME);
  SET_ATOM(_NET_WM_ICON_NAME);
  SET_ATOM(_NET_WM_VISIBLE_ICON_NAME);
  SET_ATOM(_NET_WM_DESKTOP);
  SET_ATOM(_NET_WM_WINDOW_TYPE);
  SET_ATOM(_NET_WM_STATE);
  SET_ATOM(_NET_WM_ALLOWED_ACTIONS);
  SET_ATOM(_NET_WM_STRUT);
  SET_ATOM(_NET_WM_ICON_GEOMETRY);
  SET_ATOM(_NET_FRAME_EXTENTS);
  SET_ATOM(_NET_WM_ICON);
  SET_ATOM(_NET_WM_PID);
  SET_ATOM(_NET_WM_HANDLED_ICONS);
  SET_ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
  SET_ATOM(_NET_WM_WINDOW_TYPE_DOCK);
  SET_ATOM(_NET_WM_WINDOW_TYPE_TOOLBAR);
  SET_ATOM(_NET_WM_WINDOW_TYPE_MENU);
  SET_ATOM(_NET_WM_WINDOW_TYPE_UTILITY);
  SET_ATOM(_NET_WM_WINDOW_TYPE_SPLASH);
  SET_ATOM(_NET_WM_WINDOW_TYPE_DIALOG);
  SET_ATOM(_NET_WM_WINDOW_TYPE_NORMAL);
  SET_ATOM(_NET_WM_STATE_MODAL);
  SET_ATOM(_NET_WM_STATE_STICKY);
  SET_ATOM(_NET_WM_STATE_MAXIMIZED_VERT);
  SET_ATOM(_NET_WM_STATE_MAXIMIZED_HORZ);
  SET_ATOM(_NET_WM_STATE_SHADED);
  SET_ATOM(_NET_WM_STATE_SKIP_TASKBAR);
  SET_ATOM(_NET_WM_STATE_SKIP_PAGER);
  SET_ATOM(_NET_WM_STATE_HIDDEN);
  SET_ATOM(_NET_WM_STATE_FULLSCREEN);
  SET_ATOM(_NET_WM_STATE_ABOVE);
  SET_ATOM(_NET_WM_STATE_BELOW);
  SET_ATOM(_NET_WM_ACTION_MOVE);
  SET_ATOM(_NET_WM_ACTION_RESIZE);
  SET_ATOM(_NET_WM_ACTION_MINIMIZE);
  SET_ATOM(_NET_WM_ACTION_SHADE);
  SET_ATOM(_NET_WM_ACTION_STICK);
  SET_ATOM(_NET_WM_ACTION_MAXIMIZE_HORIZ);
  SET_ATOM(_NET_WM_ACTION_MAXIMIZE_VERT);
  SET_ATOM(_NET_WM_ACTION_FULLSCREEN);
  SET_ATOM(_NET_WM_ACTION_CHANGE_DESKTOP);
  SET_ATOM(_NET_WM_ACTION_CLOSE);
#undef SET_ATOM
  // UTF8_STRING rides along on the same batch.
  names.push_back("UTF8_STRING");
  const std::vector<Atom> atoms = xlib::XInternAtoms(names);
  for (int i = 0; i < EWMH_ATOM_LAST; i++) {
    ewmh_atom[i] = atoms[i];
  }
  utf8_string = atoms[EWMH_ATOM_LAST];
}

EWMHWindowType ewmh_get_window_type(Window w) {
  xlib::WindowProperty prop = xlib::XGetWindowProperty(
      w, ewmh_atom[_NET_WM_WINDOW_TYPE], 100, XCB_ATOM_ATOM);
  if (!prop.ok()) {
    return WTypeNone;
  }
  const std::vector<uint32_t>& type = prop.Data32();
  size_t n = type.size();
  EWMHWindowType ret = WTypeNone;
  for (; n; n--) {
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_DESKTOP]) {
      ret = WTypeDesktop;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_DOCK]) {
      ret = WTypeDock;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_TOOLBAR]) {
      ret = WTypeToolbar;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_MENU]) {
      ret = WTypeMenu;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_UTILITY]) {
      ret = WTypeUtility;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_SPLASH]) {
      ret = WTypeSplash;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_DIALOG]) {
      ret = WTypeDialog;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_NORMAL]) {
      ret = WTypeNormal;
      break;
    }
  }
  return ret;
}

bool ewmh_get_window_name(Client* c) {
  xlib::WindowProperty prop = xlib::XGetWindowProperty(
      c->window, ewmh_atom[_NET_WM_NAME], 100, LScr::I->GetUTF8StringAtom());
  if (!prop.ok()) {
    // While modern X11 displays always work with UTF8, some VNC servers don't.
    // As I'm using 'tightvnc' for testing LWM in a window, it's actually quite
    // useful to be able to fall back to bad old non-UTF8 strings.
    prop = xlib::XGetWindowProperty(c->window, XCB_ATOM_WM_NAME, 100,
                                    xlib::kAnyPropertyType);
  }
  if (!prop.ok()) {
    return false;
  }
  c->SetName(prop.Data8());
  return true;
}

bool ewmh_get_visible_window_name(Client* c) {
  xlib::WindowProperty prop =
      xlib::XGetWindowProperty(c->window, ewmh_atom[_NET_WM_VISIBLE_NAME], 100,
                               LScr::I->GetUTF8StringAtom());
  if (!prop.ok()) {
    return false;
  }
  c->SetVisibleName(prop.Data8());
  return true;
}

xlib::ImageIcon* ewmh_get_window_icon(Client* c) {
  // Max allowed size for a window icon is 1MiB.
  xlib::WindowProperty prop = xlib::XGetWindowProperty(
      c->window, ewmh_atom[_NET_WM_ICON], 1 << 20, XCB_ATOM_CARDINAL);
  if (!prop.ok()) {
    return nullptr;
  }
  if (prop.bytes_after > 0) {
    fprintf(stderr, "Icon size too large: %d bytes extra\n",
            int(prop.bytes_after));
    return nullptr;
  }
  // _NET_WM_ICON is a CARDINAL[] of 32-bit ARGB pixels. Reading it as
  // anything wider than 32 bits gets you an icon made of noise.
  return xlib::ImageIcon::CreateFromPixels(prop.Data32().data(),
                                           prop.Data32().size());
}

bool ewmh_hasframe(Client* c) {
  switch (c->wtype) {
    case WTypeDesktop:
    case WTypeDock:
    case WTypeMenu:
    case WTypeSplash:
      return false;
    default:
      return true;
  }
}

void ewmh_get_state(Client* c) {
  if (c == NULL) {
    return;
  }
  xlib::WindowProperty prop = xlib::XGetWindowProperty(
      c->window, ewmh_atom[_NET_WM_STATE], 100, XCB_ATOM_ATOM);
  if (!prop.ok()) {
    return;
  }
  const std::vector<uint32_t>& state = prop.Data32();
  size_t n = state.size();
  c->wstate.skip_taskbar = false;
  c->wstate.skip_pager = false;
  c->wstate.fullscreen = false;
  c->wstate.maximized_vert = false;
  c->wstate.maximized_horz = false;
  c->wstate.above = false;
  c->wstate.below = false;
  for (; n; n--) {
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_SKIP_TASKBAR]) {
      c->wstate.skip_taskbar = true;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_SKIP_PAGER]) {
      c->wstate.skip_pager = true;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_FULLSCREEN]) {
      c->wstate.fullscreen = true;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]) {
      c->wstate.maximized_vert = true;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]) {
      c->wstate.maximized_horz = true;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_ABOVE]) {
      c->wstate.above = true;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_BELOW]) {
      c->wstate.below = true;
    }
  }
}

bool new_state(unsigned long action, bool current) {
  enum Action { remove, add, toggle };
  switch (action) {
    case remove:
      return false;
    case add:
      return true;
    case toggle:
      return !current;
  }
  fprintf(stderr, "%s: bad action in _NET_WM_STATE (%d)\n", argv0, (int)action);
  return current;
}

void ewmh_change_state(Client* c, unsigned long action, unsigned long atom) {
  Atom* a = (Atom*)&atom;

  if (atom == 0) {
    return;
  }
  if (*a == ewmh_atom[_NET_WM_STATE_SKIP_TASKBAR]) {
    c->wstate.skip_taskbar = new_state(action, c->wstate.skip_taskbar);
  }
  if (*a == ewmh_atom[_NET_WM_STATE_SKIP_PAGER]) {
    c->wstate.skip_pager = new_state(action, c->wstate.skip_pager);
  }
  if (*a == ewmh_atom[_NET_WM_STATE_FULLSCREEN]) {
    bool was_fullscreen = c->wstate.fullscreen;

    c->wstate.fullscreen = new_state(action, c->wstate.fullscreen);
    if (!was_fullscreen && c->wstate.fullscreen) {
      LOGD(c) << "Entering full-screen mode";
      c->EnterFullScreen();
    }
    if (was_fullscreen && !c->wstate.fullscreen) {
      LOGD(c) << "Exiting full-screen mode";
      c->ExitFullScreen();
    }
  }
  // Both maximisation axes go through Client::SetMaximized, which needs to see
  // the transition (it remembers the un-maximised geometry on the way in), so
  // the new values are worked out first and handed over rather than assigned.
  if (*a == ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]) {
    c->SetMaximized(new_state(action, c->wstate.maximized_vert),
                    c->wstate.maximized_horz);
  }
  if (*a == ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]) {
    c->SetMaximized(c->wstate.maximized_vert,
                    new_state(action, c->wstate.maximized_horz));
  }
  if (*a == ewmh_atom[_NET_WM_STATE_ABOVE]) {
    c->wstate.above = new_state(action, c->wstate.above);
  }
  if (*a == ewmh_atom[_NET_WM_STATE_BELOW]) {
    c->wstate.below = new_state(action, c->wstate.below);
  }
  ewmh_set_state(c);

  // may have to shuffle windows in the stack after a change of state
  ewmh_set_client_list();
}

void ewmh_set_state(Client* c) {
  if (c == NULL) {
    return;
  }
#define MAX_ATOMS 8
  Atom a[MAX_ATOMS];
  int atoms = 0;
  if (!c->IsWithdrawn()) {
    if (c->hidden) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_HIDDEN];
    }
    if (c->wstate.skip_taskbar) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_SKIP_TASKBAR];
    }
    if (c->wstate.skip_pager) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_SKIP_PAGER];
    }
    if (c->wstate.fullscreen) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_FULLSCREEN];
    }
    if (c->wstate.maximized_vert) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT];
    }
    if (c->wstate.maximized_horz) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ];
    }
    if (c->wstate.above) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_ABOVE];
    }
    if (c->wstate.below) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_BELOW];
    }
  }
  if (atoms > MAX_ATOMS) {
    panic("too many atoms! Change MAX_ATOMS in ewmh_set_state");
  }
  xlib::XChangeProperty(c->window, ewmh_atom[_NET_WM_STATE], XCB_ATOM_ATOM, 32,
                        a, atoms);
#undef MAX_ATOMS
}

void ewmh_set_allowed(Client* c) {
  // FIXME: this is dumb - the allowed actions should be calculated
  // but for now, anything goes.
  Atom action[6];

  action[0] = ewmh_atom[_NET_WM_ACTION_MOVE];
  action[1] = ewmh_atom[_NET_WM_ACTION_RESIZE];
  action[2] = ewmh_atom[_NET_WM_ACTION_FULLSCREEN];
  action[3] = ewmh_atom[_NET_WM_ACTION_CLOSE];
  action[4] = ewmh_atom[_NET_WM_ACTION_MAXIMIZE_HORIZ];
  action[5] = ewmh_atom[_NET_WM_ACTION_MAXIMIZE_VERT];
  xlib::XChangeProperty(c->window, ewmh_atom[_NET_WM_ALLOWED_ACTIONS],
                        XCB_ATOM_ATOM, 32, action, 6);
}

// _NET_FRAME_EXTENTS is CARDINAL[4]/32: left, right, top, bottom. It tells a
// client how much space lwm's furniture takes up around it, so that a client
// which cares where its *frame* lands, or which wants to size itself to fit a
// monitor with decorations included, can do the arithmetic itself instead of
// guessing. Wine, GTK and Chromium all read it; without it they assume zero
// and are a title bar out.
//
// The bulk of the numbers is the difference between the frame rect and the
// content rect, which needs no special-casing for either kind of undecorated
// window: Client::FrameRect returns the content rect itself when the client
// isn't framed or is full screen, so every extent comes out zero.
//
// On top of that goes the frame window's own X border. That lives *outside*
// the frame's coordinate space, so FrameRect() doesn't include it and neither
// does any of lwm's internal geometry - but it's a black pixel drawn all the
// way round the frame (see LScr::Furnish), so from the client's point of view
// it is one more pixel of border on every side, and this property is defined
// in terms of what's on the screen. Full screen drops it to zero for exactly
// the same reason it drops the rest of the furniture: see
// Client::EnterFullScreen.
void ewmh_set_frame_extents(Client* c) {
  if (c == nullptr) {
    return;
  }
  // A withdrawn window has no frame, so it gets zeroes rather than a stale set
  // of extents, in the same way ewmh_set_state publishes an empty state.
  uint32_t data[4] = {};
  if (!c->IsWithdrawn()) {
    const Rect frame = c->FrameRect();
    const Rect content = c->ContentRect();
    const int xborder =
        (c->framed && !c->wstate.fullscreen) ? kFrameBorderWidth : 0;
    data[0] = content.xMin - frame.xMin + xborder;  // left
    data[1] = frame.xMax - content.xMax + xborder;  // right
    data[2] = content.yMin - frame.yMin + xborder;  // top
    data[3] = frame.yMax - content.yMax + xborder;  // bottom
  }
  xlib::XChangeProperty(c->window, ewmh_atom[_NET_FRAME_EXTENTS],
                        XCB_ATOM_CARDINAL, 32, data, 4);
}

void ewmh_set_strut() {
  // find largest reserved areas
  EWMHStrut strut{0, 0, 0, 0};
  for (auto it : LScr::I->Clients()) {
    strut = MaxStrut(strut, it.second->strut);
  }
  if (!LScr::I->ChangeStrut(strut)) {
    return;  // No change; we're done.
  }

  // set the new workarea
  uint32_t data[4];
  data[0] = strut.left;
  data[1] = strut.top;
  data[2] = xlib::ScreenWidth() - (strut.left + strut.right);
  data[3] = xlib::ScreenHeight() - (strut.top + strut.bottom);
  xlib::XChangeProperty(LScr::I->Root(), ewmh_atom[_NET_WORKAREA],
                        XCB_ATOM_CARDINAL, 32, data, 4);

  // ensure no window fully occupy reserved areas
  // XXXXXXXXXXXXXXXXXXXX FIX THIS! Should probably treat the changing of
  // struts in the same way as we deal with RandR changes.
  //  for (auto it : LScr::I->Clients()) {
  //    Client* c = it.second;
  //    int x = c->size.x;
  //    int y = c->size.y;
  //
  //    if (c->wstate.fullscreen) {
  //      continue;
  //    }
  //    Client_MakeSane(c, ENone, x, y, 0, 0);
  //    LOGD(c) << "MakeSane done; y=" << c->size.y << "; framed=" << c->framed;
  //    if (c->framed) {
  //      xlib::XMoveWindow(c->parent, c->size.x, c->size.y - xfont::TextHeight());
  //    } else {
  //      xlib::XMoveWindow(c->parent, c->size.x, c->size.y);
  //    }
  //    c->SendConfigureNotify();
  //  }
}

// get _NET_WM_STRUT and if it is available recalculate the screens
// reserved areas. the EWMH spec isn't clear about what we should do
// about hidden windows. It seems silly to reserve space for an invisible
// window, but the spec allows it. Ho Hum...		jfc
void ewmh_get_strut(Client* c) {
  if (c == nullptr) {
    return;
  }
  xlib::WindowProperty prop = xlib::XGetWindowProperty(
      c->window, ewmh_atom[_NET_WM_STRUT], 5, XCB_ATOM_CARDINAL);
  const std::vector<uint32_t>& strut = prop.Data32();
  if (!prop.ok() || strut.size() < 4) {
    return;
  }
  c->strut.left = strut[0];
  c->strut.right = strut[1];
  c->strut.top = strut[2];
  c->strut.bottom = strut[3];
  ewmh_set_strut();
}

// fix stack forces each window on the screen to be in the right place in
// the window stack as indicated in the EWMH spec version 1.2 (section 7.10).
void fix_stack() {
  // this is pretty dumb. we should query the tree and only move
  // those windows that require it. doing it regardless like this
  // causes the desktop to flicker

  // first lower clients with _NET_WM_STATE_BELOW
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    if (!c->wstate.below) {
      continue;
    }
    c->Lower();
  }

  // lower desktops - they are always the lowest
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    if (c->wtype != WTypeDesktop) {
      continue;
    }
    c->Lower();
    break;  // only one desktop, surely
  }

  // raise clients with _NET_WM_STATE_ABOVE and docks
  // (unless marked with _NET_WM_STATE_BELOW)
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    if (!(c->wstate.above || (c->wtype == WTypeDock && !c->wstate.below))) {
      continue;
    }
    c->Raise();
  }

  // raise fullscreens - they're always on top
  // Misam Saki reports problems with this and believes fullscreens
  // should not be automatically raised.
  //
  // However if the code below is removed then the panel is raised above
  // fullscreens, which is not desirable.
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    if (!c->wstate.fullscreen) {
      continue;
    }
    c->Raise();
  }
}

bool valid_for_client_list(Client* c) {
  return !c->IsWithdrawn();
}

// update_client_list updates the properties on the root window used by
// task lists and pagers.
//
// it should be called whenever the window stack is modified, or when clients
// are hidden or unhidden.
void ewmh_set_client_list() {
  static bool recursion_stop;
  if (recursion_stop) {
    return;
  }
  recursion_stop = true;
  fix_stack();
  int no_clients = 0;
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    if (valid_for_client_list(c)) {
      no_clients++;
    }
  }
  // Window IDs are 32 bits on the wire, and these are format-32 properties.
  std::vector<uint32_t> client_list(no_clients);
  std::vector<uint32_t> stacked_client_list(no_clients);
  if (no_clients > 0) {
    int i = no_clients - 1;  // array starts with oldest
    for (auto it : LScr::I->Clients()) {
      Client* c = it.second;
      if (valid_for_client_list(c)) {
        client_list[i] = c->window;
        i--;
        if (i < 0) {
          break;
        }
      }
    }

    xlib::WindowTree wt = xlib::WindowTree::Query(LScr::I->Root());
    int ci = 0;
    for (Window win : wt.children) {
      Client* c = LScr::I->GetClient(win);
      if (!c) {
        continue;
      }
      if (valid_for_client_list(c)) {
        stacked_client_list[ci] = c->window;
        ci++;
        if (ci >= no_clients) {
          break;
        }
      }
    }
  }
  xlib::XChangeProperty(LScr::I->Root(), ewmh_atom[_NET_CLIENT_LIST],
                        XCB_ATOM_WINDOW, 32, client_list.data(), no_clients);
  xlib::XChangeProperty(LScr::I->Root(), ewmh_atom[_NET_CLIENT_LIST_STACKING],
                        XCB_ATOM_WINDOW, 32, stacked_client_list.data(),
                        no_clients);
  recursion_stop = false;
}
