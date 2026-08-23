#include "hider.h"

#include <set>

#include "client.h"
#include "ewmh.h"
#include "lwm.h"
#include "menulayout.h"
#include "resource.h"
#include "screen.h"
#include "xfont.h"
#include "xlib.h"

namespace {

// hiddenIDFor returns the Window ID which stands for a client in the hidden_
// list and in the unhide menu. It's the client's own window: unlike the frame,
// that lasts for as long as the client does, which matters now the user can
// take a window's furniture away and give it back (see Client::SetFramed).
// LScr::GetClient resolves it either way round.
// We have a specially-named function for this so that we don't get confused
// about which Window ID we're using, as it's used in Hide, Unhide and
// OpenMenu.
Window hiddenIDFor(const Client* c) {
  return c->window;
}

// hideTargetFor returns the window Hide unmaps and Unhide maps again: the
// frame if there is one, and the client's own window if not. It is *not* the
// same as hiddenIDFor - unmapping the frame implicitly takes the client
// window inside it with it, which saves re-mapping and repositioning the
// client afterwards, and an unframed client's 'parent' is the root, which the
// server declines to unmap at all.
Window hideTargetFor(const Client* c) {
  return c->framed ? c->parent : c->window;
}

void mapAndRaise(Window w, int xmin, int ymin, int width, int height) {
  xlib::XMoveResizeWindow(w, xmin, ymin, width, height);
  xlib::XMapRaised(w);
}

MenuStyle CurrentMenuStyle() {
  return MenuStyle{xfont::TextHeight()};
}

int menuIconYPad() {
  return MenuStyle::kIconYPad;
}

int menuIconXPad() {
  return MenuStyle::kIconXPad;
}

int menuIconSize() {
  return MenuIconSize(CurrentMenuStyle());
}

int menuLHighlight() {
  return MenuLHighlight(CurrentMenuStyle());
}

int menuHighlightMargins() {
  return MenuHighlightMargins(CurrentMenuStyle());
}

int menuLMargin() {
  return MenuLMargin(CurrentMenuStyle());
}

int menuMargins() {
  return MenuMargins(CurrentMenuStyle());
}

// Returns val if it's within the range described by min and max, or min or
// max according to which side val extends off.
int clamp(int val, int min, int max) {
  if (val >= min && val < max) {
    return val;
  }
  return (val < min) ? min : max;
}

// visibleAreaAt returns the rectangle describing the current visible area which
// contains the given coordinates. This allows us to keep the popup menu within
// a single monitor at a time.
Rect visibleAreaAt(int x, int y) {
  for (const Rect& r : LScr::I->VisibleAreas(true)) {
    if (r.contains(x, y)) {
      return r;
    }
  }
  // Mouse pointer outside the screen? Weird. Anyway, just return the first one.
  return LScr::I->VisibleAreas(false)[0];
}

}  // namespace

int menuItemHeight() {
  return CurrentMenuStyle().ItemHeight();
}

void Hider::showHighlightBox(int itemIndex) {
  // If itemIndex isn't an item, actually hide the box.
  if (itemIndex < 0 || itemIndex >= open_content_.size()) {
    hideHighlightBox();
    return;
  }
  if (!highlightL) {
    // No highlight windows created yet; create them now.
    const unsigned long col =
        Resources::I->GetColour(Resources::WINDOW_HIGHLIGHT_COLOUR);
    Rect r{0, 0, 1, 1};
    highlightL = xlib::CreateNamedWindow("LWM highlight L", r, 1, col, col);
    highlightR = xlib::CreateNamedWindow("LWM highlight R", r, 1, col, col);
    highlightT = xlib::CreateNamedWindow("LWM highlight T", r, 1, col, col);
    highlightB = xlib::CreateNamedWindow("LWM highlight B", r, 1, col, col);
  }
  Client* c = LScr::I->GetClient(open_content_[itemIndex].w);
  if (!c) {
    // Client has probably gone away in the meantime; no highlight to show.
    hideHighlightBox();
    return;
  }
  const Rect r = c->FrameRect();
  mapAndRaise(highlightL, r.xMin, r.yMin, 1, r.height());
  mapAndRaise(highlightR, r.xMax, r.yMin, 1, r.height());
  mapAndRaise(highlightT, r.xMin, r.yMin, r.width(), 1);
  mapAndRaise(highlightB, r.xMin, r.yMax, r.width(), 1);
}

void Hider::hideHighlightBox() {
  if (!highlightL) {
    // No highlight windows created; that means we have nothing to hide.
    return;
  }
  xlib::XUnmapWindow(highlightL);
  xlib::XUnmapWindow(highlightR);
  xlib::XUnmapWindow(highlightT);
  xlib::XUnmapWindow(highlightB);
}

void Hider::Hide(Client* c) {
  hidden_.push_front(hiddenIDFor(c));

  // Actually hide the window. An unframed client has nothing to hide behind,
  // so its own window is what goes - and the Client has to be warned first,
  // or the UnmapNotify that comes back looks exactly like the client
  // withdrawing the window, and lwm stops managing it.
  if (!c->framed) {
    c->ExpectUnmap();
  }
  xlib::XUnmapWindow(hideTargetFor(c));

  c->hidden = true;
  // Remove input focus, and drop from focus history.
  LScr::I->GetFocuser()->UnfocusClient(c);
  c->SetState(IconicState);
}

void Hider::Unhide(Client* c) {
  // If anyone ever hides so many windows that we notice the O(n) scan, they're
  // doing something wrong.
  for (auto it = hidden_.begin(); it != hidden_.end(); ++it) {
    if (*it == hiddenIDFor(c)) {
      hidden_.erase(it);
      c->hidden = false;
      break;
    }
  }
  // Always raise and give focus if we're trying to unhide, even if it wasn't
  // hidden.
  xlib::XMapWindow(hideTargetFor(c));
  c->Raise();
  c->SetState(NormalState);
  // Windows are given input focus when they're unhidden.
  LScr::I->GetFocuser()->FocusClient(c);
}

void Hider::OpenMenu(const xcb_button_press_event_t* e) {
  Client_ResetAllCursors();
  open_content_.clear();
  width_ = 0;

  // Add all hidden windows.
  std::set<Window> added;
  // It's possible for a client to disappear while hidden, for example if you
  // run 'sleep 5; exit' in an xterm, then hide it, you end up with a hidden
  // item with no Client. So while iterating over the hidden windows, we
  // also clean up any that have gone away.
  // Note: we have to handle the iteration carefully, as the 'erase' function
  // essentially iterates for us.
  for (std::list<Window>::iterator it = hidden_.begin(); it != hidden_.end();) {
    const Window w = *it;
    const Client* c = LScr::I->GetClient(w);
    // The following check for c->IsHidden is mainly there to avoid listing
    // 'withdrawn' windows, but as we're constructing the list of windows which
    // are iconified, this is the right check to perform here.
    // Update(2020-04-15): Actually, checking for IsHidden here breaks all
    // window hiding, and causes hidden windows not to be visible in the unhide
    // menu.
    if (c /* && c->IsHidden()*/) {
      open_content_.push_back(Item(w, true));
      added.insert(w);
      ++it;
    } else {
      it = hidden_.erase(it);  // Implicitly increments 'it'.
    }
  }

  // Add all other clients which haven't already been added.
  for (const auto& it : LScr::I->Clients()) {
    const Client* c = it.second;
    const Window w = hiddenIDFor(c);
    // The following check for c->IsNormal implicitly cuts out any windows which
    // are in withdrawn state.
    // This fixes a bug where Rhythmbox's preferences dialog would never
    // disappear from the list of windows, because it was withdrawn and kept,
    // and not destroyed.
    // To verify this bug is fixed, do the following:
    // 1: Open Rhythmbox.
    // 2: Open the Rhythmbox Preferences window.
    // 3: Verify the preferences window appears in the right-click unhide menu.
    // 4: Click on the X icon of the preferences window.
    // 5: Verify the preferences window no longer appears in the unhide menu.
    // ewmh_hasframe rather than c->framed: what's being excluded here is the
    // window types that are furniture-free by their nature (desktops, docks,
    // menus, splash screens), not the ordinary windows lwm happens not to
    // have decorated - one of which the user may well have undecorated
    // themselves, and would be surprised to find missing from this list.
    if (!ewmh_hasframe(c) || added.count(w) || !c->IsNormal()) {
      continue;
    }
    open_content_.push_back(Item(w, false));
    added.insert(w);
  }

  // Now we've got all clients in open_content_ in the right order, go through
  // and fill in their names, and find the longest.
  for (int i = 0; i < open_content_.size(); i++) {
    const Client* c = LScr::I->GetClient(open_content_[i].w);
    if (!c) {
      continue;
    }
    open_content_[i].name = c->MenuName();
    const int tw = xfont::TextWidth(open_content_[i].name) + menuMargins();
    if (tw > width_) {
      width_ = tw;
    }
  }

  height_ = open_content_.size() * menuItemHeight();

  // Arrange for centre of first menu item to be under pointer,
  // unless that would put the menu off-screen.
  // event_x/event_y are what Xlib called x/y (relative to the event window),
  // and root_x/root_y are its x_root/y_root.
  const Rect scr = visibleAreaAt(e->event_x, e->event_y);
  x_min_ = clamp(e->event_x - width_ / 2, scr.xMin, scr.xMax - width_);
  y_min_ =
      clamp(e->event_y - menuItemHeight() / 2, scr.yMin, scr.yMax - height_);

  current_item_ = itemAt(e->root_x, e->root_y);
  showHighlightBox(current_item_);
  mapAndRaise(LScr::I->Menu(), x_min_, y_min_, width_, height_);
  xlib::XChangeActivePointerGrab(ButtonMask | XCB_EVENT_MASK_BUTTON_MOTION |
                                     XCB_EVENT_MASK_OWNER_GRAB_BUTTON,
                                 XCB_NONE, XCB_CURRENT_TIME);
}

int Hider::itemAt(int x, int y) const {
  x -= x_min_;
  y -= y_min_;
  if (x < 0 || y < 0 || x >= width_ || y >= height_) {
    return -1;
  }
  return y / menuItemHeight();
}

void Hider::Paint() {
  // We have to repaint from scratch. While this can cause a little flickering,
  // it's necessary to first blank the window background, so that we don't
  // corrupt our display when the red highlight box windows open and close over
  // the top of the menu.
  xlib::XClearWindow(LScr::I->Menu());
  const int itemHeight = menuItemHeight();
  const auto popup = LScr::I->Menu();
  const auto gc = LScr::I->GetMenuGC();
  for (int i = 0; i < open_content_.size(); i++) {
    const int y = i * itemHeight;
    const int textY = y + xfont::TextAscent() + MenuStyle::kYPadding / 2;
    xfont::DrawString(popup, menuLMargin(), textY, open_content_[i].name,
                      xfont::Colour::POPUP);
    // Show a dotted line to separate the last hidden window from the first
    // non-hidden one.
    if (!open_content_[i].hidden && (i == 0 || open_content_[i - 1].hidden)) {
      xlib::XChangeGC(gc, xlib::GCValues()
                              .LineWidth(1)
                              .LineStyle(XCB_LINE_STYLE_ON_OFF_DASH)
                              .CapStyle(XCB_CAP_STYLE_BUTT)
                              .JoinStyle(XCB_JOIN_STYLE_MITER));
      xlib::XDrawLine(popup, gc, 0, y, width_, y);
    }

    Client* c = LScr::I->GetClient(open_content_[i].w);
    if (c && c->Icon() && Resources::I->AppIconInUnhideMenu()) {
      c->Icon()->PaintMenu(popup, menuIconXPad(), y + menuIconYPad(),
                           menuIconSize(), menuIconSize());
    }
  }
  drawHighlight(current_item_);
}

void Hider::drawHighlight(int itemIndex) {
  if (itemIndex == -1) {
    return;
  }
  const int ih = menuItemHeight();
  const int y = itemIndex * ih;
  xlib::XFillRectangle(LScr::I->Menu(), LScr::I->GetMenuGC(), menuLHighlight(),
                       y, width_ - menuHighlightMargins(), ih);
}

void Hider::MouseMotion(const xcb_motion_notify_event_t* ev) {
  const int old = current_item_;  // Old menu position.
  current_item_ = itemAt(ev->root_x, ev->root_y);
  if (current_item_ != old) {
    // In order to avoid too much flickering, and to avoid weird corruption
    // in our popup window, we first make the red highlight box disappear,
    // then update the menu item highlight (by EORing the old and new highlight
    // positions), and then finally reopen the red highlight box in its new
    // position. This seems to be the smoothest and least flickery/error-prone
    // way of updating the menu.
    hideHighlightBox();
    drawHighlight(old);
    drawHighlight(current_item_);
    showHighlightBox(current_item_);
  }
}

void Hider::MouseRelease(const xcb_button_release_event_t* ev) {
  hideHighlightBox();
  const int n = itemAt(ev->root_x, ev->root_y);
  xlib::XUnmapWindow(LScr::I->Menu());
  if (n < 0) {
    return;  // User just released the mouse without having selected anything.
  }
  Client* c = LScr::I->GetClient(open_content_[n].w);
  if (c == nullptr) {
    return;  // Window must have disappeared, and we've lost the client.
  }
  Unhide(c);
}
