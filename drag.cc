#include "drag.h"

#include <sstream>

#include "client.h"
#include "debug.h"
#include "gesture.h"
#include "lwm.h"
#include "resource.h"
#include "screen.h"
#include "xlib.h"

namespace {

// Max distance between click and release, for closing, iconising etc.
#define MAX_CLICK_DISTANCE 4

// Whether a press at start and a release at end count as a click. X reports
// presses and releases and has no opinion on which pairs of them are clicks,
// so anything that acts on a click rather than a drag has to decide for
// itself, and they all decide the same way: the pointer must have stayed
// where it was put.
bool isClick(MousePos start, MousePos end) {
  return std::max(std::abs(start.x - end.x), std::abs(start.y - end.y)) <=
         MAX_CLICK_DISTANCE;
}

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
  // button_mask is the bit which must stay set in the pointer's state mask
  // for the drag to continue; see the race described in Move below. It
  // depends on which button started the drag, so it can't simply be
  // MOVING_BUTTON_MASK: the Windows-key resize gesture drags with button 2,
  // and could as easily be given a button that isn't in that mask at all.
  WindowDragger(Client* c, unsigned int button_mask)
      : window_(c->parent), button_mask_(button_mask) {}

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
    if (!c || !(mp.modMask & button_mask_)) {
      // Either the client window closed underneath us, or we're somehow not
      // holding the mouse button down, despite not having seen the unclick
      // properly. In any case, cancel dragging.
      End(ev);
      return false;
    }
    // The user is placing this window by hand, so it isn't maximised any more
    // however it got that way. Idempotent, so calling it per motion event is
    // no worse than tracking whether we've done it.
    c->DropMaximization();
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

 protected:
  // LWM's frame window.
  Window window() const { return window_; }
  // Where the pointer was when the button went down.
  MousePos startPos() const { return start_pos_; }

 private:
  Window window_;
  unsigned int button_mask_;
  MousePos start_pos_;
};

class WindowMover : public WindowDragger {
 public:
  WindowMover(Client* c, unsigned int button_mask)
      : WindowDragger(c, button_mask),
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

// The Windows-key move gesture doubles as a click: a press which goes nowhere
// hasn't moved the window anywhere either, and raises it instead. That's what
// a button 1 press on the frame does, and the point of these gestures is that
// the whole window acts like the frame.
class WindowMoverRaiser : public WindowMover {
 public:
  WindowMoverRaiser(Client* c, unsigned int button_mask)
      : WindowMover(c, button_mask) {}

  virtual void End(xcb_generic_event_t* ev) {
    WindowMover::End(ev);
    if (!isClick(startPos(), getMousePosition())) {
      return;  // A real drag: the window has already gone where it was put.
    }
    Client* c = LScr::I->GetClient(window());
    if (c) {
      LOGD(c) << "Raising (user action)";
      c->Raise();
    }
  }
};

class WindowResizer : public WindowDragger {
 public:
  WindowResizer(Client* c, Edge edge, unsigned int button_mask)
      : WindowDragger(c, button_mask),
        edge_(edge),
        start_content_rect_(c->ContentRect()) {}

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

// WindowExpander is the double-click half of the Windows-key gestures: it
// grows the window in one shot rather than following the mouse, so all the
// work happens on the press and the rest of the click is simply absorbed.
//
// The edges to expand come from the same 3x3 grid the resize gesture uses,
// with the centre square meaning "all of them". Windows in the way stop the
// expansion unless ignore_obstacles_ is set, which is what makes button 2's
// double click a "fill the monitor" and button 1's a "fill the space".
class WindowExpander : public DragHandler {
 public:
  WindowExpander(Client* c, Edge edge, bool ignore_obstacles)
      : window_(c->parent), edge_(edge), ignore_obstacles_(ignore_obstacles) {}

  virtual void Start(xcb_generic_event_t*) {
    Client* c = LScr::I->GetClient(window_);
    if (!c) {
      return;
    }
    const Rect frame = c->FrameRect();
    const Rect grown =
        ExpandRect(frame, edge_, obstacles(c), LScr::I->VisibleAreas(true));
    LOGD(c) << "Expanding " << frame << " to " << grown;
    if (grown == frame) {
      return;
    }
    c->DropMaximization();
    Rect content = c->framed ? Client::ContentFromFrameRect(grown) : grown;
    // The client still gets the last word on its size: expanding into a gap
    // an xterm can only half fill leaves the rest of the gap empty.
    c->MoveResizeTo(c->LimitResize(content));
  }

  virtual bool Move(xcb_generic_event_t*) { return true; }
  virtual void End(xcb_generic_event_t*) {}

 private:
  // The frames of the other windows on screen, which are what the expansion
  // stops at. Hidden and withdrawn windows aren't on screen, so they don't
  // get in the way.
  std::vector<Rect> obstacles(const Client* self) const {
    std::vector<Rect> res;
    if (ignore_obstacles_) {
      return res;
    }
    for (const auto& it : LScr::I->Clients()) {
      const Client* c = it.second;
      if (c == self || c->hidden || !c->IsNormal()) {
        continue;
      }
      res.push_back(c->FrameRect());
    }
    return res;
  }

  // LWM's frame window.
  Window window_;
  const Edge edge_;
  const bool ignore_obstacles_;
};

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
    if (!isClick(start_pos_, getMousePosition())) {
      return;  // Cancelled by mouse pointer having moved too far away.
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

// The bit set in a pointer state mask while the given button is held down.
unsigned int buttonStateMask(int button) {
  switch (button) {
    case XCB_BUTTON_INDEX_1:
      return XCB_KEY_BUT_MASK_BUTTON_1;
    case XCB_BUTTON_INDEX_2:
      return XCB_KEY_BUT_MASK_BUTTON_2;
    case XCB_BUTTON_INDEX_3:
      return XCB_KEY_BUT_MASK_BUTTON_3;
    case XCB_BUTTON_INDEX_4:
      return XCB_KEY_BUT_MASK_BUTTON_4;
    case XCB_BUTTON_INDEX_5:
      return XCB_KEY_BUT_MASK_BUTTON_5;
  }
  return 0;
}

// The presses which might turn out to be the first half of a double click.
// Only the Windows-key gestures use this; the rest of lwm's mouse handling
// has no interest in double clicks.
DoubleClickTracker super_clicks;

// The gestures the user gets by holding the Windows key and clicking on the
// window itself, rather than on lwm's furniture:
//
//   button 1 drag          move the window
//   button 2 drag          resize the nearest edge or corner
//   button 1 click         raise the window, as a button 1 press on the
//                          furniture does
//   button 1 double click  expand the nearest edge or corner up to whatever
//                          is in the way
//   button 2 double click  the same, but ignoring other windows, so it
//                          expands to the monitor
//   button 3 click         hide the window, as a button 3 click on the
//                          furniture does
//
// A click is a drag that went nowhere, so the move and raise gestures are one
// handler which decides between them on the button release.
//
// "Nearest edge or corner" means the 3x3 grid in gesture.h, laid over the
// part of the window the gesture works on - the client's own window, not the
// frame, since that's the whole area a press can arrive from. Its centre
// square resizes nothing, but expands everything.
DragHandler* getSuperDragHandler(Client* c,
                                 const xcb_button_press_event_t* e) {
  if (e->detail == SUPER_HIDE_BUTTON) {
    // Hiding cares about neither the grid nor double clicks, so it goes
    // first: WindowHider waits for the release, and only acts if the pointer
    // has stayed put, exactly as it does on the frame.
    return new WindowHider(c);
  }
  const Edge edge =
      NineGridEdgeAt(c->ContentRect(), Point{e->root_x, e->root_y});
  const bool double_click = super_clicks.IsDoubleClick(
      e->detail, c->window, Point{e->root_x, e->root_y}, e->time);
  if (double_click) {
    return new WindowExpander(c, edge, e->detail == SUPER_RESIZE_BUTTON);
  }
  const bool moving = e->detail == SUPER_MOVE_BUTTON;
  if (!moving && edge == ENone) {
    return nullptr;  // Middle of the grid: no edge to resize.
  }
  // The press may have come through any of several passive grabs, whose event
  // masks are none of this code's business - click-to-focus installs one of
  // its own on the same window, and it asks for no motion events at all.
  // Restate what this drag needs on the active grab, which is also how the
  // pointer gets the right shape while the drag is going on.
  xlib::XChangeActivePointerGrab(
      ButtonMask | XCB_EVENT_MASK_POINTER_MOTION_HINT |
          XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_OWNER_GRAB_BUTTON,
      LScr::I->Cursors()->ForEdge(moving ? ENone : edge), XCB_CURRENT_TIME);
  const unsigned int mask = buttonStateMask(e->detail);
  if (moving) {
    return new WindowMoverRaiser(c, mask);
  }
  return new WindowResizer(c, edge, mask);
}

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
    // A press on the client's own window. lwm only sees these at all when
    // the user is holding the Windows key (Client::GrabSuperButtons), or in
    // click-to-focus mode, where the click is just a request for focus.
    if (e->state & SUPER_MASK) {
      return getSuperDragHandler(c, e);
    }
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
    return new WindowMover(c, MOVING_BUTTON_MASK);
  }
  if (e->detail == RESHAPE_BUTTON) {
    c->Raise();
    if (edge == ENone) {
      return new WindowMover(c, MOVING_BUTTON_MASK);
    }
    return new WindowResizer(c, edge, MOVING_BUTTON_MASK);
  }
  return nullptr;
}
