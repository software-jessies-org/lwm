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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sstream>

#include <unistd.h>

#include "client.h"
#include "cursor.h"
#include "debug.h"
#include "ewmh.h"
#include "framegeometry.h"
#include "lwm.h"
#include "resource.h"
#include "screen.h"
#include "screenlayout.h"
#include "xfont.h"
#include "xlib.h"

static int popup_width;  // The width of the size-feedback window.

// The current border width/text height, as a FrameStyle for the pure
// geometry functions in framegeometry.h. These are effectively constant for
// the process lifetime (a config change requires a full restart via SIGHUP),
// so there's no need to cache this beyond what Resources and Xft already do.
static FrameStyle CurrentFrameStyle() {
  return FrameStyle{borderWidth(), topBorderWidth(), xfont::TextHeight()};
}

// Returns the total height, in pixels, of the window title bar.
int titleBarHeight() {
  return CurrentFrameStyle().TitleBarHeight();
}

// closeBounds returns the bounding box of the close icon cross.
// If displayBounds is true, the returned box is the cross itself; if false,
// it's the active area (which extends down to the client window, and across
// to the start of the title bar).
// The reason for the difference is simple usability: particularly on large 4k
// displays, it's tricky to hit the cross itself, and easy to instead click on
// the area below and to the right of the cross, which would result in the
// window being resized. However, resizing from that position seems weird; one
// would more naturally pick the outer edge for such an action, so it makes
// more sense to have that close the window too.
Rect closeBounds(bool displayBounds) {
  return CloseBounds(CurrentFrameStyle(), displayBounds);
}

Rect titleBarBounds(int windowWidth) {
  return TitleBarBounds(CurrentFrameStyle(), windowWidth);
}

std::ostream& operator<<(std::ostream& os, const Client& c) {
  os << WinID(c.window);
  if (c.parent) {
    os << " (frame=" << WinID(c.parent) << ")";
  }
  if (c.trans) {
    os << " (trans=" << WinID(c.trans) << ")";
  }
  os << " outer=" << c.FrameRect() << " inner=" << c.ContentRect() << " ";
  if (c.hidden) {
    os << "(";
  }
  os << "\"" << c.Name() << "\"";
  if (c.hidden) {
    os << ")";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const WinID& w) {
  os << "0x" << std::hex << w.w << std::dec;
  return os;
}

Rect Client::EdgeBounds(Edge e) const {
  return EdgeBoundsFor(CurrentFrameStyle(), FrameRect(), e);
}

// Truncate names to this many characters (UTF8 characters, naturally). Much
// simpler than trying to calculate the 'best' length based on the render text
// width, which is quite unnecessary anyway.
static constexpr int maxMenuNameChars = 100;

std::string Client::MenuName() const {
  return TruncateUtf8(Name(), maxMenuNameChars);
}

void Client::Hide() {
  LScr::I->GetHider()->Hide(this);
}

void Client::Unhide() {
  LScr::I->GetHider()->Unhide(this);
}

Edge Client::EdgeAt(Window w, int x, int y) const {
  if (w != parent) {
    return EContents;
  }
  if (closeBounds(false).contains(x, y)) {  // false -> get action bounds.
    return EClose;
  }
  if (titleBarBounds(FrameRect().width()).contains(x, y)) {
    return ENone;  // Rename to ETitleBar.
  }
  const std::vector<Edge> movementEdges{ETopLeft, ETop,        ETopRight,
                                        ERight,   ELeft,       EBottomLeft,
                                        EBottom,  EBottomRight};
  for (Edge e : movementEdges) {
    if (EdgeBounds(e).contains(x, y)) {
      return e;
    }
  }
  return ENone;
}

void Client::SetIcon(xlib::ImageIcon* icon) {
  if (icon) {
    icon_ = icon;
  }
}

// The modifier combinations the Windows-key gestures have to be grabbed
// under. X matches a passive grab's modifiers exactly, so a grab on Super
// alone quietly stops working the moment Num Lock or Caps Lock is on. There
// is no "don't care" mask, so every combination of the two locks has to be
// asked for separately.
static const unsigned int superGrabModifiers[] = {
    SUPER_MASK,
    SUPER_MASK | XCB_MOD_MASK_LOCK,
    SUPER_MASK | XCB_MOD_MASK_2,
    SUPER_MASK | XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
};

void Client::GrabSuperButtons() {
  // Unframed windows are the ones lwm has decided not to put furniture on
  // (shaped windows, mostly), and the gestures are furniture.
  if (!framed) {
    return;
  }
  for (unsigned int modifiers : superGrabModifiers) {
    for (int button :
         {SUPER_MOVE_BUTTON, SUPER_RESIZE_BUTTON, SUPER_HIDE_BUTTON}) {
      // Asynchronous, so that neither pointer nor keyboard is frozen waiting
      // for lwm to allow the events through: lwm swallows these clicks
      // whole, and never replays them to the client.
      xlib::XGrabButton(button, modifiers, window, false,
                        ButtonMask | XCB_EVENT_MASK_POINTER_MOTION,
                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                        XCB_NONE);
    }
  }
}

void Client::FocusGained() {
  if (framed && Resources::I->ClickToFocus()) {
    // In click-to-focus mode, our FocusLost function will grab button events
    // on the client's window. We must relinquish this grabbing when we gain
    // focus, otherwise the client itself won't get the events when it is
    // focused.
    xlib::XUngrabButton(XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY, window);
    // That ungrab was indiscriminate, and took the Windows-key gestures with
    // it. Ask for them back.
    GrabSuperButtons();
  }
  DrawBorder();
}

void Client::FocusLost() {
  if (framed && Resources::I->ClickToFocus()) {
    // In click-to-focus mode, we need to intercept button clicks within the
    // client window, so we can give the window focus. While some applications,
    // notably java apps, will grab input focus when clicked on, xterm and
    // many others do not. Thus, we need to grab click notifications ourselves
    // so that we can properly support click-to-focus.
    xlib::XGrabButton(XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY, window, false,
                      ButtonMask, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_SYNC,
                      XCB_NONE, XCB_NONE);
  }
  DrawBorder();
}

void Client::DrawBorder() {
  if (parent == LScr::I->Root() || parent == 0 || !framed ||
      wstate.fullscreen) {
    return;
  }
  const bool active = HasFocus();

  xlib::XSetWindowBackground(
      parent, active ? LScr::I->ActiveBorder() : LScr::I->InactiveBorder());
  xlib::XClearWindow(parent);

  // Cross for the close icon.
  const Rect r = closeBounds(true);  // true -> get display bounds.
  const GC close_gc = LScr::I->GetCloseIconGC(active);
  xlib::XDrawLine(parent, close_gc, r.xMin, r.yMin, r.xMax, r.yMax);
  xlib::XDrawLine(parent, close_gc, r.xMin, r.yMax, r.xMax, r.yMin);
  const int bw = borderWidth();
  const int quarter = (titleBarHeight()) / 4;
  if (active) {
    // Give the title a nice background, and differentiate it from the
    // rest of the furniture to show it acts differently (moves the window
    // rather than resizing it).
    // However, skip the top few pixels if the 'topBorderWidth' is non-zero, to
    // show where the resize handle is.
    const int topBW = topBorderWidth();
    const int x = bw + 3 * quarter;
    const int w = FrameRect().width() - 2 * x;
    const int h = xfont::TextHeight() + bw - topBW;
    xlib::XFillRectangle(parent, LScr::I->GetTitleGC(), x, topBW, w, h);
  }

  // Find where the title stuff is going to go.
  int x = bw + 2 + (3 * quarter);
  int y = bw / 2 + xfont::TextAscent();

  // Do we have an icon? If so, draw it to the left of the title text.
  if (Icon() && Resources::I->AppIconInWindowTitle()) {
    if (active) {
      Icon()->PaintActive(parent, x, 0, titleBarHeight(), titleBarHeight());
    } else {
      Icon()->PaintInactive(parent, x, 0, titleBarHeight(), titleBarHeight());
    }
    x += titleBarHeight();  // Title bar text must come after.
  }

  // Draw window title.
  xfont::DrawString(parent, x, y, Name(),
                    active ? xfont::Colour::ACTIVE_TITLE
                           : xfont::Colour::INACTIVE_TITLE);
}

Rect Client::FrameRect() const {
  // A full-screen window has no furniture to make room for, so its frame is
  // exactly its content rect. This is not just cosmetic: it's the invariant
  // MoveTo, MoveResizeTo and EnterFullScreen all place the two windows by, and
  // ContentRectRelative below is derived from it. Get it wrong and the client
  // window sits at an offset inside its own frame, which is only invisible
  // when the frame happens to be at the screen origin.
  if (!framed || wstate.fullscreen) {
    return content_rect_;
  }
  return FrameFromContentRect(content_rect_);
}

Rect Client::ContentRectRelative() const {
  const Rect fr = FrameRect();
  return Rect::Translate(content_rect_, Point{-fr.xMin, -fr.yMin});
}

// static
Rect Client::ContentFromFrameRect(const Rect& r) {
  return ::ContentFromFrameRect(CurrentFrameStyle(), r);
}

// static
Rect Client::FrameFromContentRect(const Rect& r) {
  return ::FrameFromContentRect(CurrentFrameStyle(), r);
}

void Client::Remove() {
  if (parent != LScr::I->Root()) {
    xlib::XDestroyWindow(parent);
  }
  LScr::I->Remove(this);
  ewmh_set_client_list();
  ewmh_set_strut();
}

std::string makeSizeString(int x, int y) {
  std::ostringstream buf;
  buf << x << " x " << y;
  return buf.str();
}

void Client_SizeFeedback() {
  // Make the popup 10% wider than the widest string it needs to show.
  popup_width =
      xfont::TextWidth(makeSizeString(xlib::ScreenWidth(),
                                      xlib::ScreenHeight()));
  popup_width += popup_width / 10;

  // Put the popup in the right place to report on the window's size.
  const MousePos mp = getMousePosition();
  xlib::XMoveResizeWindow(LScr::I->Popup(), mp.x + 8, mp.y + 8, popup_width,
                          xfont::TextHeight() + 1);
  xlib::XMapRaised(LScr::I->Popup());

  // Ensure that the popup contents get redrawn. Eventually, the function
  // size_expose will get called to do the actual redraw.
  xlib::XClearArea(LScr::I->Popup(), 0, 0, 0, 0, true);
}

void size_expose() {
  Client* c = LScr::I->GetFocuser()->GetFocusedClient();
  if (!c) {
    return;
  }
  const std::string text = c->SizeString();
  const int x = (popup_width - xfont::TextWidth(text)) / 2;
  xfont::DrawString(LScr::I->Popup(), x, xfont::TextAscent() + 1, text,
                    xfont::Colour::POPUP);
}

std::string Client::SizeString() const {
  return makeSizeString(x_limiter_.DisplayableSize(content_rect_.width()),
                        y_limiter_.DisplayableSize(content_rect_.height()));
}

void Client::Lower() {
  xlib::XLowerWindow(window);
  if (framed) {
    xlib::XLowerWindow(parent);
  }
  ewmh_set_client_list();
}

void Client::Raise() {
  if (framed) {
    xlib::XRaiseWindow(parent);
  }
  xlib::XRaiseWindow(window);

  for (auto it : LScr::I->Clients()) {
    Client* tr = it.second;
    if (tr->trans != window && !(framed && tr->trans == parent)) {
      continue;
    }
    if (tr->framed) {
      xlib::XRaiseWindow(tr->parent);
    }
    xlib::XRaiseWindow(tr->window);
  }
  ewmh_set_client_list();
}

void Client::Close() {
  // Terminate the client nicely if possible. Be brutal otherwise.
  if (proto & Pdelete) {
    xlib::SendClientMessage(window, wm_protocols, wm_delete, XCB_CURRENT_TIME);
  } else {
    xlib::XKillClient(window);
  }
}

void Client::SetState(int state) {
  // WM_STATE is CARDINAL[2]/32: the state, then the icon window (which lwm
  // never provides). 32-bit words, not longs - see xlib::WindowProperty.
  const uint32_t data[2] = {uint32_t(state), XCB_NONE};

  state_ = state;
  xlib::XChangeProperty(window, wm_state, wm_state, 32, data, 2);
  ewmh_set_state(this);
}

extern void Client_ResetAllCursors() {
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    if (!c->framed) {
      continue;
    }
    xlib::XChangeWindowAttributes(
        c->parent, xlib::WindowAttrs().Cursor(LScr::I->Cursors()->Root()));
    c->cursor = ENone;
  }
}

extern void Client_FreeAll() {
  // Take a copy of the client pointers, as releasing the clients may mutate
  // the underlying clients map.
  std::vector<Client*> clients;
  for (auto it : LScr::I->Clients()) {
    clients.push_back(it.second);
  }
  for (auto c : clients) {
    c->Release();
  }
}

Client::Client(Window w,
               const xlib::WindowAttributes& attr,
               const DimensionLimiter& x_limiter,
               const DimensionLimiter& y_limiter)
    : window(w),
      parent(LScr::I->Root()),
      content_rect_(attr.rect),
      x_limiter_(x_limiter),
      y_limiter_(y_limiter),
      original_border_width_(attr.border_width) {}

void Client::Release() {
  // Reparent the client window to the root, to elide our furniture window.
  const Rect cr = ContentRect();
  LOGD(this) << "Client::Release " << cr;
  if (!framed) {
    return;
  }
  xlib::XReparentWindow(window, LScr::I->Root(), cr.xMin, cr.yMin);
  if (hidden) {
    // The window was iconised, so map it back into view so it isn't lost
    // forever, but lower it so it doesn't jump all over the foreground.
    xlib::XMapWindow(window);
    xlib::XLowerWindow(window);
  }

  // Give it back its initial border width.
  xlib::XConfigureWindow(
      window, xlib::WindowChanges().BorderWidth(original_border_width_));
}

void Client::EnterFullScreen() {
  pre_full_screen_content_rect_ = content_rect_;
  // For now, just find the 'main screen' and use that.
  // Ideally, we'd actually try to find the largest contiguous rectangle, as
  // someone might be using two identical-sized monitors next to each other
  // to get a bigger view of what they're killing, but for now we'll save that
  // for another day.
  const Rect scr = LScr::I->GetPrimaryVisibleArea(false);  // Without struts.
  content_rect_ = scr;
  if (framed) {
    // Drop the frame's own X border. It lives outside the frame's coordinate
    // space, so leaving it on would put the whole window a pixel down and to
    // the right of the monitor, with a pixel of frame showing above and to the
    // left of a game that asked to fill the screen exactly.
    xlib::XConfigureWindow(parent, xlib::WindowChanges().BorderWidth(0));
    // wstate.fullscreen is already set by the time we get here, so FrameRect()
    // is the screen rect (there's no furniture to make room for) and
    // ContentRectRelative() is the origin of the frame's coordinate space.
    //
    // Both of those matter. The client window is *reparented*, so its position
    // has to be given relative to the frame; this used to pass the root
    // coordinates instead, which is the same thing only while the screen being
    // filled starts at x=0. On a two-monitor layout with the primary on the
    // right, it offset the client inside its frame by the width of the other
    // monitor, and the client was clipped to the part of the frame that was
    // left.
    xlib::XMoveResizeWindow(parent, FrameRect());
    xlib::XMoveResizeWindow(window, ContentRectRelative());
  } else {
    xlib::XMoveResizeWindow(window, content_rect_);
  }
  xlib::XRaiseWindow(framed ? parent : window);
  SendConfigureNotify();
}

Rect Client::MaximizedRect(const Rect& restored) const {
  if (!IsMaximized()) {
    return restored;
  }
  // Maximisation is about the *frame*: what fills the screen is the window
  // furniture and all, so the arithmetic happens in frame coordinates and is
  // converted back at the end. Doing it on the content rect would push the
  // title bar off the top of the screen.
  const Rect frame = framed ? FrameFromContentRect(restored) : restored;
  // With struts: unlike a full-screen window, a maximised one leaves the
  // panels alone. That is what the whole strut mechanism is for.
  const Rect area = findBestScreenFor(frame, LScr::I->VisibleAreas(true));
  Rect res = frame;
  if (wstate.maximized_horz) {
    res.xMin = area.xMin;
    res.xMax = area.xMax;
  }
  if (wstate.maximized_vert) {
    res.yMin = area.yMin;
    res.yMax = area.yMax;
  }
  return framed ? ContentFromFrameRect(res) : res;
}

void Client::SetMaximized(bool vert, bool horz) {
  if (!IsMaximized()) {
    // Going from un-maximised to maximised on at least one axis: this is the
    // geometry to come back to. Note it's taken before the flags change, so
    // that setting the second axis while the first is already set doesn't
    // overwrite it with a rect that's already half screen-sized. And while the
    // window is full screen, content_rect_ is the screen - the geometry that
    // means anything is the one full screen is standing in front of.
    pre_maximize_content_rect_ =
        wstate.fullscreen ? pre_full_screen_content_rect_ : content_rect_;
  }
  wstate.maximized_vert = vert;
  wstate.maximized_horz = horz;
  LOGD(this) << "SetMaximized vert=" << vert << " horz=" << horz;
  if (wstate.fullscreen) {
    // Full screen wins while it lasts; ExitFullScreen applies whatever the
    // maximisation flags say by then.
    return;
  }
  Rect target = MaximizedRect(pre_maximize_content_rect_);
  if (!IsMaximized()) {
    // Un-maximising, so target is the geometry we saved on the way in - which
    // may be stale, because the monitor layout can have changed while the
    // window was maximised. Don't restore a window onto a monitor that isn't
    // there any more.
    const bool f = framed;
    target = makeVisible(f ? FrameFromContentRect(target) : target,
                         LScr::I->VisibleAreas(true));
    if (f) {
      target = ContentFromFrameRect(target);
    }
  }
  // The client still gets the last word on its size, so an xterm maximises to
  // a whole number of character cells rather than to the exact screen height.
  MoveResizeTo(LimitResize(target));
}

void Client::DropMaximization() {
  if (!IsMaximized()) {
    return;
  }
  LOGD(this) << "Dropping maximisation (user moved or resized the window)";
  wstate.maximized_vert = false;
  wstate.maximized_horz = false;
  ewmh_set_state(this);
}

void Client::ExitFullScreen() {
  content_rect_ = IsMaximized()
                      ? MaximizedRect(pre_maximize_content_rect_)
                      : pre_full_screen_content_rect_;
  if (framed) {
    xlib::XConfigureWindow(
        parent, xlib::WindowChanges().BorderWidth(kFrameBorderWidth));
    xlib::XMoveResizeWindow(parent, FrameRect());
    // The client window is reparented, so its position is relative to the
    // *frame*, not to the root. If we move it to 'content_rect_', it ends up
    // offset within the frame window by the frame origin coordinates.
    xlib::XMoveResizeWindow(window, ContentRectRelative());
    DrawBorder();  // The furniture is visible again.
  } else {
    xlib::XMoveResizeWindow(window, content_rect_);
  }
  SendConfigureNotify();
}

void Client::SendConfigureNotify() {
  xcb_configure_notify_event_t ce{};
  ce.response_type = XCB_CONFIGURE_NOTIFY;
  ce.event = window;
  ce.window = window;
  content_rect_.To(ce);
  ce.border_width = framed ? 0 : original_border_width_;
  ce.above_sibling = XCB_NONE;
  ce.override_redirect = 0;
  LOGD(this) << "Sending config notify, r=" << content_rect_ << " to "
             << WinID(window);
  xlib::SendEvent(window, false, XCB_EVENT_MASK_STRUCTURE_NOTIFY, ce);
}

bool Client::HasFocus() const {
  return this == LScr::I->GetFocuser()->GetFocusedClient();
}

// static
Client* Client::FocusedClient() {
  return LScr::I->GetFocuser()->GetFocusedClient();
}

Rect Client::LimitResize(const Rect& suggested) {
  Rect res = suggested;
  x_limiter_.Limit(content_rect_.xMin, content_rect_.xMax, res.xMin, res.xMax);
  y_limiter_.Limit(content_rect_.yMin, content_rect_.yMax, res.yMin, res.yMax);
  return res;
}

void Client::MoveTo(const Rect& new_content_rect) {
  if (content_rect_.width() != new_content_rect.width() ||
      content_rect_.height() != new_content_rect.height()) {
    LOGF() << "Invalid move from " << content_rect_ << " to "
           << new_content_rect << " (size mismatch)";
  }
  if (content_rect_.xMin == new_content_rect.xMin &&
      content_rect_.yMin == new_content_rect.yMin) {
    return;  // Move to same place. AKA NOP.
  }
  content_rect_ = new_content_rect;
  if (framed) {
    // Moving the frame carries the client window along inside it.
    xlib::XMoveWindow(parent, FrameRect().origin());
  } else {
    // Nothing to carry it: an unframed client is a child of the root, so it's
    // the window that has to move. This used to update content_rect_ and stop,
    // which made MoveTo a no-op on screen for every unframed client.
    xlib::XMoveWindow(window, content_rect_.origin());
  }
  // Do I need to send a configure notify? According to this:
  // https://tronche.com/gui/x/xlib/events/window-state-change/configure.html
  // ...it looks like the job of the X server itself.
  LOGD(this) << "MoveTo " << new_content_rect;
  SendConfigureNotify();
}

void Client::MoveResizeTo(const Rect& new_content_rect) {
  if (new_content_rect == content_rect_) {
    // Nothing to do.
    return;
  }
  const bool move_client = content_rect_.origin() != new_content_rect.origin();
  content_rect_ = new_content_rect;
  if (framed) {
    xlib::XMoveResizeWindow(parent, FrameRect());
    if (move_client) {
      // Client was resized towards the top/left. We need to move and resize it
      // so it stays within the right offset of its frame. Coordinates are
      // relative to the frame window.
      const Rect fr = FrameRect();
      Rect r = Rect::Translate(content_rect_, Point{-fr.xMin, -fr.yMin});
      xlib::XMoveResizeWindow(window, r);
    } else {
      // Client was only resized at the bottom and/or right. No move necessary.
      xlib::XResizeWindow(window, content_rect_.area());
    }
  } else {
    xlib::XMoveResizeWindow(window, content_rect_);
  }
  // Do I need to send a configure notify? According to this:
  // https://tronche.com/gui/x/xlib/events/window-state-change/configure.html
  // ...it looks like the job of the X server itself.
  LOGD(this) << "MoveResizeTo " << new_content_rect;
  SendConfigureNotify();
}

void Client::FurnishAt(Rect rect) {
  bool is_visible = false;
  for (const Rect& area : LScr::I->VisibleAreas(true)) {
    // If there's any overlap with one of the displays, that's OK.
    if (!Rect::Intersect(rect, area).empty()) {
      is_visible = true;
      break;
    }
  }
  if (!is_visible) {
    // Oh no, the client tried to open the window outside the visible areas
    // (ImageMagick's 'display' program does this sometimes, when the total
    // screen area is non-rectangular - so when there are multiple monitors).
    // Just try to centre the window within the 'main' screen, which is
    // generally the biggest one, and the one the user most cares about.
    Rect scr = LScr::I->GetPrimaryVisibleArea(true);
    // We must work in terms of the frame rect, then transform back to content
    // rect, otherwise we run the risk of the client's content being within the
    // screen, but having no window move/resize widgets visible.
    rect = FrameFromContentRect(rect);
    const int w = std::min(rect.width(), scr.width());
    const int h = std::min(rect.height(), scr.height());
    const int x_off = (scr.width() - w) / 2;
    const int y_off = (scr.height() - h) / 2;
    const int x = scr.xMin + x_off;
    const int y = scr.yMin + y_off;
    rect = Rect::FromXYWH(x, y, w, h);
    rect = ContentFromFrameRect(rect);
  }
  content_rect_ = rect;
  content_rect_ = LimitResize(rect);
  LScr::I->Furnish(this);
}

