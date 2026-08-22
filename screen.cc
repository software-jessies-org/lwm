#include "client.h"
#include "cursor.h"
#include "debug.h"
#include "ewmh.h"
#include "manage.h"
#include "resource.h"
#include "screen.h"
#include "screenlayout.h"
#include "xlib.h"

// The static LScr instance.
LScr* LScr::I;

LScr::LScr(Display* dpy)
    : dpy_(dpy),
      root_(RootWindow(dpy, kOnlyScreenIndex)),
      width_(DisplayWidth(dpy, kOnlyScreenIndex)),
      height_(DisplayHeight(dpy, kOnlyScreenIndex)),
      cursor_map_(new CursorMap(dpy)),
      utf8_string_atom_(xlib::XInternAtom("UTF8_STRING")),
      strut_{0, 0, 0, 0} {
  visible_areas_ = std::vector<Rect>(1, Rect{0, 0, width_, height_});
}

void LScr::Init() {
  active_border_ = Resources::I->GetColour(Resources::BORDER_COLOUR);
  inactive_border_ = Resources::I->GetColour(Resources::INACTIVE_BORDER_COLOUR);

  // The graphics context used for the menu is a simple exclusive OR which will
  // toggle pixels between black and white. This allows us to implement
  // highlights really easily.
  XGCValues gv;
  gv.foreground = black() ^ white();
  gv.background = white();
  gv.function = GXxor;
  gv.line_width = 2;
  gv.subwindow_mode = IncludeInferiors;
  const unsigned long gv_mask =
      GCForeground | GCBackground | GCFunction | GCLineWidth | GCSubwindowMode;
  menu_gc_ = xlib::XCreateGC(root_, gv_mask, &gv);

  // The GC used for the close button is the same as for the menu, except it
  // uses GXcopy, not GXxor, so we draw the chosen colour correctly.
  gv.foreground = Resources::I->GetColour(Resources::CLOSE_ICON_COLOUR);
  gv.background = white();
  gv.function = GXcopy;
  gc_ = xlib::XCreateGC(root_, gv_mask, &gv);
  xlib::XSetLineAttributes(gc_, 2, LineSolid, CapProjecting, JoinMiter);

  gv.foreground =
      Resources::I->GetColour(Resources::INACTIVE_CLOSE_ICON_COLOUR);
  inactive_gc_ = xlib::XCreateGC(root_, gv_mask, &gv);
  xlib::XSetLineAttributes(inactive_gc_, 2, LineSolid, CapProjecting,
                           JoinMiter);

  // The title bar.
  gv.foreground = Resources::I->GetColour(Resources::TITLE_BG_COLOUR);
  title_gc_ = xlib::XCreateGC(root_, gv_mask, &gv);

  // Create the popup window, to be used for the resize feedback window,
  // and the menu window.
  XSetWindowAttributes attr;
  attr.event_mask = ButtonMask | ButtonMotionMask | ExposureMask;
  const unsigned int fg = Resources::I->GetColour(Resources::POPUP_TEXT_COLOUR);
  const unsigned int bg =
      Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR);
  Rect r{0, 0, 1, 1};
  popup_ = xlib::CreateNamedWindow("LWM size popup", r, 1, fg, bg);
  xlib::XChangeWindowAttributes(popup_, CWEventMask, &attr);
  menu_ = xlib::CreateNamedWindow("LWM unhide menu", r, 1, fg, bg);
  xlib::XChangeWindowAttributes(menu_, CWEventMask, &attr);

  // Announce our interest in the root_ window.
  attr.cursor = cursor_map_->Root();
  attr.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ColormapChangeMask | ButtonPressMask | ButtonReleaseMask |
                    PropertyChangeMask | EnterWindowMask;
  xlib::XChangeWindowAttributes(root_, CWCursor | CWEventMask, &attr);

  // Tell all the applications what icon sizes we prefer.
  xlib::ImageIcon::ConfigureIconSizes();

  // Make sure all our communication to the server got through.
  xlib::XSync(false);
  ScanWindowTree();
  InitEWMH();
}

void LScr::InitEWMH() {
  // Announce EWMH compatibility on the screen.
  Rect r{-200, -200, 1, 1};
  ewmh_compat_ = xlib::CreateNamedWindow("LWM EWMH", r, 0, 0, 0);
  xlib::XChangeProperty(ewmh_compat_, ewmh_atom[_NET_WM_NAME],
                        utf8_string_atom_, XA_CURSOR, PropModeReplace,
                        (const unsigned char*)"lwm", 3);

  // set root window properties
  xlib::XChangeProperty(root_, ewmh_atom[_NET_SUPPORTED], XA_ATOM, 32,
                        PropModeReplace, (unsigned char*)ewmh_atom,
                        EWMH_ATOM_LAST);

  xlib::XChangeProperty(root_, ewmh_atom[_NET_SUPPORTING_WM_CHECK], XA_WINDOW,
                        32, PropModeReplace, (unsigned char*)&ewmh_compat_, 1);

  unsigned long data[4];
  data[0] = 1;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_NUMBER_OF_DESKTOPS],
                        XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char*)data, 1);

  data[0] = width_;
  data[1] = height_;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_DESKTOP_GEOMETRY], XA_CARDINAL,
                        32, PropModeReplace, (unsigned char*)data, 2);

  data[0] = 0;
  data[1] = 0;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_DESKTOP_VIEWPORT], XA_CARDINAL,
                        32, PropModeReplace, (unsigned char*)data, 2);

  data[0] = 0;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_CURRENT_DESKTOP], XA_CARDINAL,
                        32, PropModeReplace, (unsigned char*)data, 1);

  ewmh_set_strut();
  ewmh_set_client_list();
}

void LScr::ScanWindowTree() {
  xlib::WindowTree wt = xlib::WindowTree::Query(dpy_, root_);
  for (const Window w : wt.children) {
    if (!xlib::IsLWMWindow(w)) {
      AddClient(w, true);
    }
  }
  // Tell all the clients they don't have input focus. This has two effects:
  // 1: the client will respond by drawing its border (always)
  // 2: if we're in click-to-focus mode, the client will grab input events, so
  //    that it can detect clicks within the window being managed.
  // We do that now, after we've scanned the window tree, so everything is in
  // its final state.
  for (auto it : clients_) {
    it.second->FocusLost();
  }
}

Client* LScr::GetOrAddClient(Window w, bool is_startup_scan) {
  if (xlib::IsLWMWindow(w)) {
    return nullptr;  // No client for our own windows.
  }
  Client* c = GetClient(w);
  if (c) {
    return c;
  }
  c = AddClient(w, is_startup_scan);
  DebugCLI::NotifyClientAdd(c);
  return c;
}

Client* LScr::AddClient(Window w, bool is_startup_scan) {
  const XWindowAttributes attr = xlib::XGetWindowAttributes(w);
  if (attr.override_redirect) {
    return nullptr;
  }
  // The following check prevents us from making random stuff visible, like the
  // currently-not-visible menu window of gummiband, or the icon-containing
  // windows of Java apps.
  if (is_startup_scan && attr.map_state != IsViewable) {
    return nullptr;
  }
  XSizeHints size;
  long msize;
  DimensionLimiter xdl;
  DimensionLimiter ydl;
  if (xlib::XGetWMNormalHints(w, &size, &msize)) {
    xdl = DimensionLimiter(size.flags & PMinSize ? size.min_width : 0,
                           size.flags & PMaxSize ? size.max_width : 0,
                           size.flags & PBaseSize ? size.base_width : 0,
                           size.flags & PResizeInc ? size.width_inc : 1);
    ydl = DimensionLimiter(size.flags & PMinSize ? size.min_height : 0,
                           size.flags & PMaxSize ? size.max_height : 0,
                           size.flags & PBaseSize ? size.base_height : 0,
                           size.flags & PResizeInc ? size.height_inc : 1);
  }
  Client* c = new Client(w, attr, xdl, ydl);
  // LOGI() << "New client " << attr.width << "x" << attr.height << "+" <<
  // attr.x
  //       << "+" << attr.y << ", g = " << attr.win_gravity;
  // Call manage if we know the window is already mapped (scanned at start-up).
  if (is_startup_scan) {
    manage(c);
  }
  clients_[w] = c;
  return c;
}

void LScr::Furnish(Client* c) {
  std::ostringstream name;
  name << "LWM frame for " << WinID(c->window);
  LOGD(c) << "Creating frame for client, at " << c->FrameRect();
  c->parent =
      xlib::CreateNamedWindow(name.str(), c->FrameRect(), 1, black(), white());
  XSetWindowAttributes attr;
  // DO NOT SET PointerMotionHintMask! Doing so allows X to send just one
  // notification to the window until the key or button state changes. This
  // prevents us from properly updating the cursor as we move the pointer around
  // our window furniture.
  attr.event_mask = ExposureMask | EnterWindowMask | LeaveWindowMask |
                    ButtonMask | SubstructureRedirectMask |
                    SubstructureNotifyMask | PointerMotionMask;
  xlib::XChangeWindowAttributes(c->parent, CWEventMask, &attr);
  parents_[c->parent] = c;
}

Client* LScr::GetClient(Window w, bool scan_parents) const {
  if (w == 0 || w == Root()) {
    return nullptr;
  }
  const auto it = parents_.find(w);
  if (it != parents_.end()) {
    return it->second;
  }
  while (w) {
    const auto it = clients_.find(w);
    if (it != clients_.end()) {
      return it->second;
    }
    // scan_parents must be disabled when we're responding to a DestroyNotify
    // event. We'll get a notification of the 'c->window' window as well, but
    // we should just silently ignore the destruction of all its subwindows.
    // If we fail to do this, the ParentOf is going to fail, because the window
    // doesn't exist any more.
    if (!scan_parents) {
      return nullptr;
    }
    w = xlib::WindowTree::ParentOf(w);
  }
  return nullptr;
}

void LScr::Remove(Client* c) {
  focuser_.UnfocusClient(c);
  auto it = clients_.find(c->window);
  if (it == clients_.end()) {
    return;
  }
  parents_.erase(it->second->parent);
  clients_.erase(it);
  DebugCLI::NotifyClientRemove(c);
  delete c;
}

Rect LScr::GetPrimaryVisibleArea(bool withStruts) const {
  return PrimaryArea(VisibleAreas(withStruts));
}

std::vector<Rect> LScr::VisibleAreas(bool withStruts) const {
  if (!withStruts) {
    return visible_areas_;
  }
  return areasMinusStruts(visible_areas_, strut_);
}

struct moveData {
  Client* c;
  Rect r;
};

// How to do this:
// Find the old visible area containing the window.
// Scale window centre to new display w/h, and map that to new visible area.
// Sort out new position/size according to mapping from old to new visible area.
// Once all the internal sizes are updated, and we have a list of actions to
// take, switch in the new visible_areas_, and then send all the size change/
// configure notify requests.
void LScr::SetVisibleAreas(std::vector<Rect> visible_areas) {
  int nScrWidth = 0;
  int nScrHeight = 0;
  for (const Rect& r : visible_areas_) {
    if (r.xMax > nScrWidth) {
      nScrWidth = r.xMax;
    }
    if (r.yMax > nScrHeight) {
      nScrHeight = r.yMax;
    }
  }

  const std::vector<Rect> oldVis = areasMinusStruts(visible_areas_, strut_);
  const std::vector<Rect> newVis = areasMinusStruts(visible_areas, strut_);

  std::vector<moveData> moves;

  // Now, go through the windows and adjust their sizes and locations to
  // conform to the new screen layout.
  for (auto it : clients_) {
    Client* c = it.second;
    // Ignore clients that set struts; we expect these to watch for screen
    // changes for themselves, and move their windows if necessary.
    // Of course, if we were to move them, we'd want to be using the strutless
    // visible areas, not the ones with the struts removed, otherwise we'd
    // reposition strutty windows so they don't intersect their own struts,
    // which is wrong.
    if (c->HasStruts()) {
      // If this client has set a strut, it's reserved an area of the screen for
      // it to place its own window in. As such, we must avoid forcing that
      // window into the visible area with struts excluded, as doing so would
      // prevent the client from placing its window in its own reserved area.
      // A better approach may be to use the visible areas *without* the struts
      // removed in order to potential force strutted windows into the visible
      // area of the screen. However, as they're reserving a window edge
      // already, they probably should be listening for xrandr events and moving
      // their windows appropriately, in which case there's nothing for us to
      // do here.
      continue;
    }

    Rect newRect = MapToNewAreas(c->FrameRect(), oldVis, newVis);

    // Now we have newRect, which describes where we'd like to put the window,
    // including its frame. Translate that down to the client window
    // coordinates (if the client is framed).
    if (c->framed) {
      newRect = Client::ContentFromFrameRect(newRect);
    }
    newRect = c->LimitResize(newRect);
    moves.push_back(moveData{c, newRect});
  }

  // Now we've determined what we need to do with the windows, we should put the
  // new screen geometry in place so that it can be used properly during the
  // window position updates.
  visible_areas_ = visible_areas;
  width_ = nScrWidth;
  height_ = nScrHeight;

  // All set up now, let's move all the windows around.
  for (moveData& move : moves) {
    move.c->MoveResizeTo(move.r);
  }
}

bool LScr::ChangeStrut(const EWMHStrut& strut) {
  if (strut == strut_) {
    return false;  // No change.
  }
  strut_ = strut;
  return true;
}
