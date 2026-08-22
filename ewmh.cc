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
  D(above);
  D(below);
#undef D
  return os;
}

std::ostream& operator<<(std::ostream& os, const XSizeHints& s) {
  os << "XSizeHints[";
#define D(x) (s.flags & x ? "" : "!") << #x << " "
  os << D(USPosition) << D(USSize) << D(PPosition) << D(PSize) << D(PMinSize)
     << D(PMaxSize) << D(PResizeInc) << D(PAspect) << D(PBaseSize)
     << D(PWinGravity) << "pos:" << s.width << "x" << s.height << "+" << s.x
     << "+" << s.y << " size: min=" << s.min_width << "," << s.min_height
     << "; max=" << s.max_width << "," << s.max_height
     << " aspect: min=" << s.min_aspect.x << ":" << s.min_aspect.y
     << "; max=" << s.max_aspect.x << ":" << s.max_aspect.y
     << " base=" << s.base_width << "," << s.base_height
     << " gravity=" << s.win_gravity << "]";
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
  // Build half a million EWMH atoms.
#define SET_ATOM(x)                             \
  do {                                          \
    ewmh_atom[x] = XInternAtom(dpy, #x, false); \
    ewmh_atom_names[x] = #x;                    \
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
  SET_ATOM(_NET_WM_STATE_MAXIMISED_VERT);
  SET_ATOM(_NET_WM_STATE_MAXIMISED_HORZ);
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
  utf8_string = XInternAtom(dpy, "UTF8_STRING", false);
}

EWMHWindowType ewmh_get_window_type(Window w) {
  Atom rt = 0;
  Atom* type = nullptr;
  int fmt = 0;
  unsigned long n = 0;
  unsigned long extra = 0;
  int i = XGetWindowProperty(dpy, w, ewmh_atom[_NET_WM_WINDOW_TYPE], 0, 100,
                             false, XA_ATOM, &rt, &fmt, &n, &extra,
                             (unsigned char**)&type);
  if (i != Success || type == NULL) {
    return WTypeNone;
  }
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
  XFree(type);
  return ret;
}

bool ewmh_get_window_name(Client* c) {
  Atom rt;
  char* name = nullptr;
  int fmt = 0;
  unsigned long n = 0;
  unsigned long extra = 0;
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_NAME], 0, 100,
                             false, LScr::I->GetUTF8StringAtom(), &rt, &fmt, &n,
                             &extra, (unsigned char**)&name);
  if (i != Success || name == nullptr) {
    // While modern X11 displays always work with UTF8, some VNC servers don't.
    // As I'm using 'tightvnc' for testing LWM in a window, it's actually quite
    // useful to be able to fall back to bad old non-UTF8 strings.
    i = XGetWindowProperty(dpy, c->window, XA_WM_NAME, 0, 100, false,
                           AnyPropertyType, &rt, &fmt, &n, &extra,
                           (unsigned char**)&name);
  }
  if (i != Success || name == nullptr) {
    return false;
  }
  c->SetName(std::string(name, n));
  XFree(name);
  return true;
}

bool ewmh_get_visible_window_name(Client* c) {
  Atom rt;
  char* name = nullptr;
  int fmt = 0;
  unsigned long n = 0;
  unsigned long extra = 0;
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_VISIBLE_NAME], 0,
                             100, false, LScr::I->GetUTF8StringAtom(), &rt,
                             &fmt, &n, &extra, (unsigned char**)&name);
  if (i != Success || name == nullptr) {
    return false;
  }
  c->SetVisibleName(std::string(name, n));
  XFree(name);
  return true;
}

xlib::ImageIcon* ewmh_get_window_icon(Client* c) {
  Atom rt;
  unsigned long* data = NULL;
  int fmt = 0;
  unsigned long n = 0;
  unsigned long extra = 0;
  // Max allowed size for a window icon is 1MiB.
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_ICON], 0,
                             1 << 20, false, XA_CARDINAL, &rt, &fmt, &n, &extra,
                             (unsigned char**)&data);
  if (i != Success || data == nullptr) {
    return nullptr;
  }
  xlib::XFreer data_freer(data);
  if (extra > 0) {
    fprintf(stderr, "Icon size too large: %d bytes extra\n", int(extra));
    return nullptr;
  }
  return xlib::ImageIcon::CreateFromPixels(data, n);
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
  Atom rt = 0;
  Atom* state = nullptr;
  int fmt = 0;
  unsigned long n = 0;
  unsigned long extra = 0;
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_STATE], 0, 100,
                             false, XA_ATOM, &rt, &fmt, &n, &extra,
                             (unsigned char**)&state);
  if (i != Success || state == NULL) {
    return;
  }
  c->wstate.skip_taskbar = false;
  c->wstate.skip_pager = false;
  c->wstate.fullscreen = false;
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
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_ABOVE]) {
      c->wstate.above = true;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_BELOW]) {
      c->wstate.below = true;
    }
  }
  XFree(state);
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
#define MAX_ATOMS 6
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
  XChangeProperty(dpy, c->window, ewmh_atom[_NET_WM_STATE], XA_ATOM, 32,
                  PropModeReplace, (unsigned char*)a, atoms);
#undef MAX_ATOMS
}

void ewmh_set_allowed(Client* c) {
  // FIXME: this is dumb - the allowed actions should be calculated
  // but for now, anything goes.
  Atom action[4];

  action[0] = ewmh_atom[_NET_WM_ACTION_MOVE];
  action[1] = ewmh_atom[_NET_WM_ACTION_RESIZE];
  action[2] = ewmh_atom[_NET_WM_ACTION_FULLSCREEN];
  action[3] = ewmh_atom[_NET_WM_ACTION_CLOSE];
  XChangeProperty(dpy, c->window, ewmh_atom[_NET_WM_ALLOWED_ACTIONS], XA_ATOM,
                  32, PropModeReplace, (unsigned char*)action, 4);
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
  unsigned long data[4];
  data[0] = strut.left;
  data[1] = strut.top;
  data[2] = DisplayWidth(dpy, 0) - (strut.left + strut.right);
  data[3] = DisplayHeight(dpy, 0) - (strut.top + strut.bottom);
  XChangeProperty(dpy, LScr::I->Root(), ewmh_atom[_NET_WORKAREA], XA_CARDINAL,
                  32, PropModeReplace, (unsigned char*)data, 4);

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
  //      xlib::XMoveWindow(c->parent, c->size.x, c->size.y - textHeight());
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
  Atom rt = 0;
  unsigned long* strut = nullptr;
  int fmt = 0;
  unsigned long n = 0;
  unsigned long extra = 0;
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_STRUT], 0, 5,
                             false, XA_CARDINAL, &rt, &fmt, &n, &extra,
                             (unsigned char**)&strut);
  if (i != Success || strut == nullptr || n < 4) {
    if (strut) {
      XFree(strut);
    }
    return;
  }
  c->strut.left = (unsigned int)strut[0];
  c->strut.right = (unsigned int)strut[1];
  c->strut.top = (unsigned int)strut[2];
  c->strut.bottom = (unsigned int)strut[3];
  XFree(strut);
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
  Window* client_list = NULL;
  Window* stacked_client_list = NULL;
  if (no_clients > 0) {
    client_list = (Window*)malloc(sizeof(Window) * no_clients);
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

    stacked_client_list = (Window*)malloc(sizeof(Window) * no_clients);

    xlib::WindowTree wt = xlib::WindowTree::Query(dpy, LScr::I->Root());
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
  XChangeProperty(dpy, LScr::I->Root(), ewmh_atom[_NET_CLIENT_LIST], XA_WINDOW,
                  32, PropModeReplace, (unsigned char*)client_list, no_clients);
  XChangeProperty(dpy, LScr::I->Root(), ewmh_atom[_NET_CLIENT_LIST_STACKING],
                  XA_WINDOW, 32, PropModeReplace,
                  (unsigned char*)stacked_client_list, no_clients);
  free(client_list);
  free(stacked_client_list);
  recursion_stop = false;
}
