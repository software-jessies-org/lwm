#include "client.h"
#include "cursor.h"
#include "debug.h"
#include "ewmh.h"
#include "manage.h"
#include "resource.h"
#include "screen.h"
#include "error.h"
#include "screenlayout.h"
#include "xlib.h"

// The static LScr instance.
LScr* LScr::I;

LScr::LScr()
    : root_(xlib::Root()),
      width_(xlib::ScreenWidth()),
      height_(xlib::ScreenHeight()),
      cursor_map_(new CursorMap()),
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
  // Note the ValueList builders: XCB takes a bare array of values which must
  // be in increasing order of their mask bits, and checks nothing, so nothing
  // here writes one by hand.
  xlib::GCValues menu_gv;
  menu_gv.Foreground(black() ^ white())
      .Background(white())
      .Function(XCB_GX_XOR)
      .LineWidth(2)
      .SubwindowMode(XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS);
  menu_gc_ = xlib::XCreateGC(root_, menu_gv);

  // The GC used for the close button is the same as for the menu, except it
  // uses GXcopy, not GXxor, so we draw the chosen colour correctly. The line
  // style is set at creation now, rather than by a follow-up
  // XSetLineAttributes.
  xlib::GCValues close_gv;
  close_gv.Foreground(Resources::I->GetColour(Resources::CLOSE_ICON_COLOUR))
      .Background(white())
      .Function(XCB_GX_COPY)
      .LineWidth(2)
      .LineStyle(XCB_LINE_STYLE_SOLID)
      .CapStyle(XCB_CAP_STYLE_PROJECTING)
      .JoinStyle(XCB_JOIN_STYLE_MITER)
      .SubwindowMode(XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS);
  gc_ = xlib::XCreateGC(root_, close_gv);

  xlib::GCValues inactive_gv;
  inactive_gv
      .Foreground(
          Resources::I->GetColour(Resources::INACTIVE_CLOSE_ICON_COLOUR))
      .Background(white())
      .Function(XCB_GX_COPY)
      .LineWidth(2)
      .LineStyle(XCB_LINE_STYLE_SOLID)
      .CapStyle(XCB_CAP_STYLE_PROJECTING)
      .JoinStyle(XCB_JOIN_STYLE_MITER)
      .SubwindowMode(XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS);
  inactive_gc_ = xlib::XCreateGC(root_, inactive_gv);

  // The title bar.
  xlib::GCValues title_gv;
  title_gv.Foreground(Resources::I->GetColour(Resources::TITLE_BG_COLOUR))
      .Background(white())
      .Function(XCB_GX_COPY)
      .LineWidth(2)
      .SubwindowMode(XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS);
  title_gc_ = xlib::XCreateGC(root_, title_gv);

  // Create the popup window, to be used for the resize feedback window,
  // and the menu window.
  const uint32_t popup_events = kPopupEventMask;
  const unsigned int fg = Resources::I->GetColour(Resources::POPUP_TEXT_COLOUR);
  const unsigned int bg =
      Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR);
  Rect r{0, 0, 1, 1};
  popup_ = xlib::CreateNamedWindow("LWM size popup", r, 1, fg, bg);
  xlib::XChangeWindowAttributes(popup_,
                                xlib::WindowAttrs().EventMask(popup_events));
  menu_ = xlib::CreateNamedWindow("LWM unhide menu", r, 1, fg, bg);
  xlib::XChangeWindowAttributes(menu_,
                                xlib::WindowAttrs().EventMask(popup_events));

  // Announce our interest in the root window. SubstructureRedirect is the
  // part only one client may hold, so this is also how we find out whether
  // another window manager is already running - and unlike Xlib, we get the
  // answer here rather than as a mystery BadAccess in an error handler.
  const uint32_t root_events =
      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_COLOR_MAP_CHANGE |
      XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
      XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_ENTER_WINDOW;
  if (!xlib::SelectRootEvents(root_, root_events)) {
    panic("another window manager is already running.");
  }
  xlib::XChangeWindowAttributes(
      root_, xlib::WindowAttrs().Cursor(cursor_map_->Root()));

  // Tell all the applications what icon sizes we prefer.
  xlib::ImageIcon::ConfigureIconSizes();

  // Make sure all our communication to the server got through.
  xlib::Sync();
  ScanWindowTree();
  InitEWMH();
}

void LScr::InitEWMH() {
  // Announce EWMH compatibility on the screen.
  Rect r{-200, -200, 1, 1};
  ewmh_compat_ = xlib::CreateNamedWindow("LWM EWMH", r, 0, 0, 0);
  xlib::XChangeProperty(ewmh_compat_, ewmh_atom[_NET_WM_NAME],
                        utf8_string_atom_, 8, "lwm", 3);
  // The check window must carry _NET_SUPPORTING_WM_CHECK pointing at itself, as
  // well as the copy on the root below. Clients are told to read the property
  // on the root, then read it again on the window it names, and to believe
  // there's an EWMH window manager running only if both agree - that's what
  // makes a stale property left behind by a crashed WM detectable. Without this
  // line, Wine logged "Invalid _NET_SUPPORTING_WM_CHECK window" and SDL and
  // friends conclude they're running with no WM to co-operate with.
  xlib::XChangeProperty(ewmh_compat_, ewmh_atom[_NET_SUPPORTING_WM_CHECK],
                        XCB_ATOM_WINDOW, 32, &ewmh_compat_, 1);
  // Map it. Nothing is ever drawn in it, and at -200,-200 it's nowhere near
  // the screen, but it has to be viewable because it's where the input focus
  // goes when no client will take it (see Focuser::ReallyFocusClient), and
  // XSetInputFocus on an unmapped window is a BadMatch. Mapping it doesn't
  // hand it to the window management code: it's one of our own windows, so
  // GetOrAddClient refuses it.
  xlib::XMapWindow(ewmh_compat_);

  // set root window properties. Note the 32-bit arrays: these properties are
  // format 32, and that means uint32_t, not long.
  xlib::XChangeProperty(root_, ewmh_atom[_NET_SUPPORTED], XCB_ATOM_ATOM, 32,
                        ewmh_atom, EWMH_ATOM_LAST);

  xlib::XChangeProperty(root_, ewmh_atom[_NET_SUPPORTING_WM_CHECK],
                        XCB_ATOM_WINDOW, 32, &ewmh_compat_, 1);

  uint32_t data[4];
  data[0] = 1;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_NUMBER_OF_DESKTOPS],
                        XCB_ATOM_CARDINAL, 32, data, 1);

  data[0] = width_;
  data[1] = height_;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_DESKTOP_GEOMETRY],
                        XCB_ATOM_CARDINAL, 32, data, 2);

  data[0] = 0;
  data[1] = 0;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_DESKTOP_VIEWPORT],
                        XCB_ATOM_CARDINAL, 32, data, 2);

  data[0] = 0;
  xlib::XChangeProperty(root_, ewmh_atom[_NET_CURRENT_DESKTOP],
                        XCB_ATOM_CARDINAL, 32, data, 1);

  ewmh_set_strut();
  ewmh_set_client_list();
}

void LScr::ScanWindowTree() {
  xlib::WindowTree wt = xlib::WindowTree::Query(root_);
  // Ask about every candidate window in one go. Adopting the windows that
  // are already on screen used to be three blocking round trips apiece.
  std::vector<Window> candidates;
  for (const Window w : wt.children) {
    if (!xlib::IsLWMWindow(w)) {
      candidates.push_back(w);
    }
  }
  const std::vector<xlib::WindowInfo> infos = xlib::QueryWindows(candidates);
  for (size_t i = 0; i < candidates.size(); i++) {
    AddClient(candidates[i], true, infos[i]);
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
  c = AddClient(w, is_startup_scan, xlib::QueryWindows({w})[0]);
  DebugCLI::NotifyClientAdd(c);
  return c;
}

Client* LScr::AddClient(Window w,
                        bool is_startup_scan,
                        const xlib::WindowInfo& info) {
  const xlib::WindowAttributes& attr = info.attributes;
  if (!attr.ok || attr.override_redirect) {
    return nullptr;
  }
  // The following check prevents us from making random stuff visible, like the
  // currently-not-visible menu window of gummiband, or the icon-containing
  // windows of Java apps.
  if (is_startup_scan && !attr.viewable) {
    return nullptr;
  }
  const xlib::NormalHints& size = info.normal_hints;
  DimensionLimiter xdl;
  DimensionLimiter ydl;
  if (size.ok) {
    xdl = DimensionLimiter(size.has_min_size ? size.min_width : 0,
                           size.has_max_size ? size.max_width : 0,
                           size.has_base_size ? size.base_width : 0,
                           size.has_resize_inc ? size.width_inc : 1);
    ydl = DimensionLimiter(size.has_min_size ? size.min_height : 0,
                           size.has_max_size ? size.max_height : 0,
                           size.has_base_size ? size.base_height : 0,
                           size.has_resize_inc ? size.height_inc : 1);
  }
  Client* c = new Client(w, attr, xdl, ydl);
  // LOGI() << "New client " << attr.width << "x" << attr.height << "+" <<
  // attr.x
  //       << "+" << attr.y << ", g = " << attr.win_gravity;
  // Register the client *before* managing it. manage() reads the window's
  // _NET_WM_STRUT and calls ewmh_set_strut(), which recomputes the screen's
  // reservation by folding over Clients() - so a client that isn't in the map
  // yet contributes nothing, and its own strut is dropped from that pass.
  // Today InitEWMH() re-runs ewmh_set_strut() once the start-up scan is over,
  // which covers for the omission; this ordering means the intermediate state
  // is right too, rather than depending on that second call.
  clients_[w] = c;
  // Call manage if we know the window is already mapped (scanned at start-up).
  if (is_startup_scan) {
    manage(c);
  }
  return c;
}

void LScr::Furnish(Client* c) {
  std::ostringstream name;
  name << "LWM frame for " << WinID(c->window);
  LOGD(c) << "Creating frame for client, at " << c->FrameRect();
  c->parent = xlib::CreateNamedWindow(name.str(), c->FrameRect(),
                                      kFrameBorderWidth, black(), white());
  // DO NOT SET PointerMotionHint! Doing so allows X to send just one
  // notification to the window until the key or button state changes. This
  // prevents us from properly updating the cursor as we move the pointer around
  // our window furniture.
  const uint32_t frame_events =
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_ENTER_WINDOW |
      XCB_EVENT_MASK_LEAVE_WINDOW | ButtonMask |
      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_POINTER_MOTION;
  xlib::XChangeWindowAttributes(c->parent,
                                xlib::WindowAttrs().EventMask(frame_events));
  parents_[c->parent] = c;
  DebugCLI::NotifyFrameCreated(c);
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

Client* LScr::FullScreenOccupantOf(const Rect& area) const {
  if (area.empty()) {
    return nullptr;
  }
  for (const auto& it : clients_) {
    Client* c = it.second;
    // A hidden or withdrawn window covers nothing, whatever its geometry says.
    if (!c->IsNormal()) {
      continue;
    }
    // The desktop background, and any panel, is meant to fill the screen and
    // is no reason to send anything anywhere.
    if (c->wtype == WTypeDesktop || c->wtype == WTypeDock || c->HasStruts()) {
      continue;
    }
    const Rect r = c->ContentRect();
    if (c->wstate.fullscreen) {
      // Taken at its word if most of the monitor is under it. The slack is for
      // a client whose idea of the monitor is a pixel or two out, not for one
      // which is somewhere else entirely.
      if (Rect::Intersect(r, area).area().num_pixels() * 2 >
          area.area().num_pixels()) {
        return c;
      }
      continue;
    }
    if (r.xMin <= area.xMin && r.yMin <= area.yMin && r.xMax >= area.xMax &&
        r.yMax >= area.yMax) {
      return c;
    }
  }
  return nullptr;
}

Rect LScr::PlacementAreaFor(const Client* c) const {
  // The monitors, and their work areas, are index-aligned: whether a monitor
  // is taken is decided on the whole of it, but what a window is placed in is
  // the part of it no panel has claimed.
  const std::vector<Rect> areas = VisibleAreas(false);
  const std::vector<Rect> work_areas = VisibleAreas(true);
  const Rect primary = PrimaryArea(work_areas);
  std::vector<Rect> free_areas;
  for (size_t i = 0; i < areas.size(); i++) {
    // SameProgramAs is symmetric, so which way round this is asked doesn't
    // matter; it's asked of the occupant because that one is never null.
    const Client* occupant = FullScreenOccupantOf(areas[i]);
    if (occupant == nullptr || occupant->SameProgramAs(c)) {
      free_areas.push_back(work_areas[i]);
    }
  }
  if (free_areas.empty()) {
    return primary;  // Everything is taken; there's nowhere better to go.
  }
  // PrimaryArea picks the best of whatever it's given, by the same rule that
  // picked the primary monitor out of the whole set. So when the primary is
  // among the free areas this returns it, and the answer only differs when it
  // isn't - no separate "is the primary taken?" test needed.
  const Rect res = PrimaryArea(free_areas);
  if (res != primary) {
    LOGI() << "Primary monitor is full screen for another program; placing new "
              "window on " << res << " instead";
  }
  return res;
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
  // Note: the *new* areas. This used to measure visible_areas_, the ones being
  // replaced, and so left width_/height_ describing the previous layout.
  for (const Rect& r : visible_areas) {
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
    // coordinates (if the client is framed). A full-screen client's frame *is*
    // its content rect (see Client::FrameRect), so there's nothing to subtract
    // in that case - doing so would shrink it by the furniture size on every
    // monitor change.
    if (c->framed && !c->wstate.fullscreen) {
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
