#include "drag.h"

#include <sstream>

#include "client.h"
#include "debug.h"
#include "lwm.h"
#include "resource.h"
#include "screen.h"
#include "xlib.h"

namespace {

class MenuDragger : public DragHandler {
 public:
  MenuDragger() = default;

  virtual void Start(xcb_generic_event_t* ev) {
    LScr::I->GetHider()->OpenMenu((const xcb_button_press_event_t*)ev);
  }

  virtual bool Move(xcb_generic_event_t* ev) {
    LScr::I->GetHider()->MouseMotion((const xcb_motion_notify_event_t*)ev);
    return true;
  }

  virtual void End(xcb_generic_event_t* ev) {
    LScr::I->GetHider()->MouseRelease((const xcb_button_release_event_t*)ev);
  }
};

// WindowDragger handles the shared bit of actions which involve dragging, such
// as moving or resizing windows.
// This class keeps track of where the mouse pointer was when the button was
// pressed, and where it ends up, and sends the offset from one to the other
// to the subclass.
// Subclasses must implement the moveImpl function.
class WindowDragger : public DragHandler {
 public:
  WindowDragger(Client* c) : window_(c->parent) {}

  virtual void Start(xcb_generic_event_t*) {
    start_pos_ = getMousePosition();
    LOGD(LScr::I->GetClient(window_))
        << "Window drag from " << start_pos_.x << ", " << start_pos_.y;
  }

  virtual bool Move(xcb_generic_event_t* ev) {
    Client* c = LScr::I->GetClient(window_);
    MousePos mp = getMousePosition();
    // Cancel everything if either the client has disappeared (window closed
    // while we were dragging it), or if we're somehow no longer holding the
    // mouse button down. This latter hack prevents randomly dragging around
    // windows due to a race condition in X.
    if (!c || !(mp.modMask & MOVING_BUTTON_MASK)) {
      // Either the client window closed underneath us, or we're somehow not
      // holding the mouse button down, despite not having seen the unclick
      // properly. In any case, cancel dragging.
      End(ev);
      return false;
    }
    moveImpl(c, mp.x - start_pos_.x, mp.y - start_pos_.y);
    return true;
  }

  virtual void moveImpl(Client* c, int dx, int dy) = 0;

  virtual void End(xcb_generic_event_t*) {
    MousePos mp = getMousePosition();
    LOGD(LScr::I->GetClient(window_))
        << "Window drag to " << mp.x << ", " << mp.y << " (moved "
        << (mp.x - start_pos_.x) << ", " << (mp.y - start_pos_.y) << ")";
    // Unmapping the popup only has an effect if it's open (so if this is a
    // resize instead of a move), but it doesn't hurt to always close it.
    xlib::XUnmapWindow(LScr::I->Popup());
  }

 private:
  // LWM's frame window.
  Window window_;
  MousePos start_pos_;
};

class WindowMover : public WindowDragger {
 public:
  WindowMover(Client* c)
      : WindowDragger(c),
        start_frame_rect_(c->FrameRect()),
        start_content_rect_(c->ContentRect()) {}

  virtual void moveImpl(Client* c, int dx, int dy) {
    Rect r = Rect::Translate(start_frame_rect_, Point{dx, dy});
    // Implement edge resistance for all of the visible areas. There can be
    // several if we're using multiple monitors with xrandr, and they can be
    // offset from each other. However, for each box, ensure that some part of
    // the window is interacting with an edge.
    // We're going to assume we always have to take struts into account, because
    // if a window has struts set, it should be movable anyway.
    // We only pick the first spotted edge correction factor. Otherwise if there
    // are two screens, say, next to each other, with the same bottom location,
    // a window that is dragged towards the bottom over the boundary will be
    // affected twice, resulting in weird effects (the window moving upwards as
    // it's dragged downwards).
    int orig_dx = dx;
    int orig_dy = dy;
    for (const auto& vis : LScr::I->VisibleAreas(true)) {
      // Check for top/bottom if the horizontal location of the window overlaps
      // with that of the screen area.
      if ((orig_dy == dy) && (r.xMin < vis.xMax) && (r.xMax > vis.xMin)) {
        dy += getResistanceOffset(vis.yMin - r.yMin);  // Top.
        dy -= getResistanceOffset(r.yMax - vis.yMax);  // Bottom.
      }
      // Check for left/right if the vertical location of the window overlaps
      // with that of the screen area.
      if ((orig_dx == dx) && (r.yMin < vis.yMax) && (r.yMax > vis.yMin)) {
        dx += getResistanceOffset(vis.xMin - r.xMin);  // Left.
        dx -= getResistanceOffset(r.xMax - vis.xMax);  // Right.
      }
    }
    c->MoveTo(Rect::Translate(start_content_rect_, Point{dx, dy}));
  }

 private:
  // The diff is expected to be the difference between a window position and
  // some barrier (eg edge of a screen). If that difference is within
  // 0..EDGE_RESIST, we return it; otherwise we return 0. This makes the code to
  // apply edge resistance a simple matter of subtracting or adding the returned
  // value.
  int getResistanceOffset(int diff) {
    if (diff <= 0 || diff > EDGE_RESIST) {
      return 0;
    }
    return diff;
  }

  const Rect start_frame_rect_;
  const Rect start_content_rect_;
};

class WindowResizer : public WindowDragger {
 public:
  WindowResizer(Client* c, Edge edge)
      : WindowDragger(c), edge_(edge), start_content_rect_(c->ContentRect()) {}

  virtual void moveImpl(Client* c, int dx, int dy) {
    Client_SizeFeedback();
    Rect ns = start_content_rect_;
    // Vertical.
    if (isTopEdge(edge_)) {
      ns.yMin += dy;
    }
    if (isBottomEdge(edge_)) {
      ns.yMax += dy;
    }

    // Horizontal.
    if (isLeftEdge(edge_)) {
      ns.xMin += dx;
    }
    if (isRightEdge(edge_)) {
      ns.xMax += dx;
    }
    // The client knows what the rules are regarding min/max size, and size
    // increments (eg. integer numbers of characters in xterm). Let it limit our
    // suggested size, so as to avoid unpleasantness.
    ns = c->LimitResize(ns);
    c->MoveResizeTo(ns);
  }

 private:
  const Edge edge_;
  const Rect start_content_rect_;
};

// Max distance between click and release, for closing, iconising etc.
#define MAX_CLICK_DISTANCE 4

// WindowClicker is a dragger that handles cases when we want to deal with
// simple clicks.
// Such actions include closing, hiding or lowering the window, but we only take
// action after the mouse is released, in case the user changes his or her mind.
// This class also checks the distance the mouse has moved, and only if it is
// released close to where it was pressed will it trigger the action.
class WindowClicker : public DragHandler {
 public:
  WindowClicker(Client* c) : window_(c->parent) {}
  virtual void Start(xcb_generic_event_t*) { start_pos_ = getMousePosition(); }
  virtual bool Move(xcb_generic_event_t*) { return true; }

  virtual void End(xcb_generic_event_t*) {
    MousePos mp = getMousePosition();
    const int dx = std::abs(start_pos_.x - mp.x);
    const int dy = std::abs(start_pos_.y - mp.y);
    if (std::max(dx, dy) > MAX_CLICK_DISTANCE) {
      // Cancelled by mouse pointer having moved too far away.
      return;
    }
    Client* c = LScr::I->GetClient(window_);
    // Check if client still exists.
    if (c) {
      act(c);
    }
  }

  virtual void act(Client* c) = 0;

 private:
  // LWM's frame window.
  Window window_;
  MousePos start_pos_;
};

class WindowCloser : public WindowClicker {
 public:
  WindowCloser(Client* c) : WindowClicker(c) {}
  virtual void act(Client* c) {
    LOGD(c) << "Closing (user action)";
    c->Close();
  }
};

class WindowHider : public WindowClicker {
 public:
  WindowHider(Client* c) : WindowClicker(c) {}
  virtual void act(Client* c) {
    LOGD(c) << "Hiding (user action)";
    c->Hide();
  }
};

class WindowLowerer : public WindowClicker {
 public:
  WindowLowerer(Client* c) : WindowClicker(c) {}
  virtual void act(Client* c) {
    LOGD(c) << "Lowering (user action)";
    c->Lower();
  }
};

class ShellRunner : public DragHandler {
 public:
  explicit ShellRunner(int button) : button_(button) {}
  virtual void Start(xcb_generic_event_t*) { shell(button_); }
  virtual bool Move(xcb_generic_event_t*) { return false; }
  virtual void End(xcb_generic_event_t*) {}

 private:
  int button_;
};

void RunConfiguredAltCommand(Window w, Edge edge, int button) {
  // For now, we only run commands on the title bar, so we have two cases to
  // deal with. If and when we end up being able to configure any button on any
  // Edge, this will need a more elegant solution.
  if (edge != ENone) {
    return;
  }
  std::string command;
  if (button == XCB_BUTTON_INDEX_1) {
    command = Resources::I->Get(Resources::ALT_BUTTON1_TITLE_COMMAND);
  } else if (button == XCB_BUTTON_INDEX_2) {
    command = Resources::I->Get(Resources::ALT_BUTTON2_TITLE_COMMAND);
  }
  if (command.empty()) {
    return;
  }
  std::ostringstream oss;
  oss << command << " " << WinID(w);
  RunCommand(oss.str());
}

}  // namespace

DragHandler* getDragHandlerForEvent(const xcb_button_press_event_t* e) {
  // Deal with root window button presses.
  if (e->event == e->root) {
    if (e->detail == XCB_BUTTON_INDEX_3) {
      return new MenuDragger;
    }
    return new ShellRunner(e->detail);
  }

  Client* c = LScr::I->GetClient(e->event);
  if (c == nullptr) {
    return nullptr;
  }
  if (Resources::I->ClickToFocus()) {
    LScr::I->GetFocuser()->FocusClient(c);
  }

  // move this test up to disable scroll to focus
  if (e->detail >= 4 && e->detail <= 7) {
    return nullptr;
  }
  const Edge edge = c->EdgeAt(e->event, e->event_x, e->event_y);
  if (edge == EContents) {
    return nullptr;
  }

  // If the user has alt held, then we run special configured commands as
  // configured in the user's xresources.
  if (e->state & XCB_MOD_MASK_1) {
    RunConfiguredAltCommand(c->window, edge, e->detail);
  }

  if (edge == EClose) {
    return new WindowCloser(c);
  }

  // Somewhere in the rest of the frame.
  if (e->detail == HIDE_BUTTON) {
    if (e->state & XCB_MOD_MASK_SHIFT) {
      return new WindowLowerer(c);
    }
    return new WindowHider(c);
  }
  if (e->detail == MOVE_BUTTON) {
    // If we're moving the window because the user has used the 'move' button
    // (generally middle), then force the mouse pointer to turn into the move
    // pointer, even if it's over an area of the window furniture which usually
    // has another pointer.
    xlib::XChangeActivePointerGrab(
        ButtonMask | XCB_EVENT_MASK_POINTER_MOTION_HINT |
            XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_OWNER_GRAB_BUTTON,
        LScr::I->Cursors()->ForEdge(ENone), XCB_CURRENT_TIME);
    return new WindowMover(c);
  }
  if (e->detail == RESHAPE_BUTTON) {
    c->Raise();
    if (edge == ENone) {
      return new WindowMover(c);
    }
    return new WindowResizer(c, edge);
  }
  return nullptr;
}
