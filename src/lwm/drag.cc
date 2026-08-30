#include "drag.h"

#include <sstream>

#include "client.h"
#include "debug.h"
#include "gesture.h"
#include "lwm.h"
#include "resource.h"
#include "screen.h"
#include "screenlayout.h"
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
      : window_(c->window), button_mask_(button_mask) {}

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
    // The user is placing this window by hand, so whatever maximisation it had
    // is now theirs to overrule. Idempotent, so calling it per motion event is
    // no worse than tracking whether we've done it.
    dropMaximization(c);
    moveImpl(c, mp.x - start_pos_.x, mp.y - start_pos_.y);
    return true;
  }

  virtual void moveImpl(Client* c, int dx, int dy) = 0;

  // What a drag does to the window's maximisation. Resizing always ends it -
  // there's nothing left of a maximisation the user has resized past - but a
  // move can keep half of one; see WindowMover.
  virtual void dropMaximization(Client* c) { c->DropMaximization(); }

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
  // The client's own window, which is what identifies the client for the rest
  // of the drag. It has to be the client window rather than the frame: an
  // undecorated client (one lwm gave no furniture to, such as anything that
  // draws its own title bar) has no frame, and its 'parent' is still the root
  // - for which LScr::GetClient deliberately answers nullptr. Keying off the
  // frame made every such drag cancel itself on the first motion event.
  Window window() const { return window_; }
  // Where the pointer was when the button went down.
  MousePos startPos() const { return start_pos_; }

 private:
  Window window_;
  unsigned int button_mask_;
  MousePos start_pos_;
};

// Which axes a drag keeps a window filling its monitor on. See axesToKeep.
struct FilledAxes {
  bool vert = false;
  bool horz = false;
  bool any() const { return vert || horz; }
};

class WindowMover : public WindowDragger {
 public:
  WindowMover(Client* c, unsigned int button_mask)
      : WindowDragger(c, button_mask),
        start_frame_rect_(c->FrameRect()),
        start_content_rect_(c->ContentRect()),
        fit_to_monitor_(fitsOnItsMonitor(c->FrameRect())),
        kept_(axesToKeep(c)) {}

  // A window filling its monitor on one axis only goes on filling whichever
  // monitor it's dragged onto, on that axis: that's what makes a vertically
  // maximised window dragged onto a taller monitor grow to the new monitor's
  // height instead of arriving as a short window in the middle of it, and one
  // dragged onto a shorter monitor and back again come home its old size. The
  // drag still means something, because the other axis is free.
  //
  // A window filling its monitor on *both* axes has no free axis, so keeping
  // it that way would leave the user unable to drag it anywhere but from
  // monitor to monitor. There, a drag can only mean "let this thing go", and
  // it does: whatever maximisation it had is dropped.
  virtual void dropMaximization(Client* c) {
    if (!kept_.any()) {
      c->DropMaximization();
    }
  }

  virtual void End(xcb_generic_event_t* ev) {
    WindowDragger::End(ev);
    if (!kept_.any()) {
      return;
    }
    Client* c = LScr::I->GetClient(window());
    if (!c) {
      return;
    }
    // The window is still maximised, and may be on a different monitor from
    // the one it was maximised on. Take the size it un-maximises to with it.
    const Rect frame = c->FrameRect();
    c->TranslatePreMaximizeRect(Point{frame.xMin - start_frame_rect_.xMin,
                                      frame.yMin - start_frame_rect_.yMin});
  }

  virtual void moveImpl(Client* c, int dx, int dy) {
    // Where the pointer is now: dx/dy are its offset from the press, and the
    // edge resistance below is about to adjust them. Which monitor the user is
    // dragging onto is a question about the pointer, not about the window (see
    // findDragScreen), so it has to be asked before that happens.
    const Point pointer{startPos().x + dx, startPos().y + dy};
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
    Rect frame = Rect::Translate(start_frame_rect_, Point{dx, dy});
    const std::vector<Rect> areas = LScr::I->VisibleAreas(true);
    if (!areas.empty()) {
      const Rect mon = findDragScreen(pointer, frame, areas);
      if (fit_to_monitor_) {
        // The window has been dragged somewhere; if that somewhere is a
        // monitor too small to hold it, it shrinks to fit.
        frame = ShrinkToFitGivenMonitor(frame, mon);
      }
      frame = fillMonitor(frame, mon);
    }
    // Everything is measured from where the drag started rather than from
    // where the window has got to, which is what lets a window that shrank on
    // its way to a small monitor have its size back if the user drags it home
    // again before letting go.
    //
    // The offset the content moves by comes from the frame rather than from
    // dx/dy, because a maximised axis doesn't follow the pointer: the frame is
    // where the window has actually ended up.
    const Rect content =
        (frame.area() == start_frame_rect_.area())
            ? Rect::Translate(start_content_rect_,
                              Point{frame.xMin - start_frame_rect_.xMin,
                                    frame.yMin - start_frame_rect_.yMin})
            : (c->HasFurniture() ? Client::ContentFromFrameRect(frame) : frame);
    if (content.area() == c->ContentRect().area()) {
      c->MoveTo(content);
    } else {
      // As ever, the client has the last word on its own size.
      c->MoveResizeTo(c->LimitResize(content));
    }
  }

 private:
  // Stretches the frame over `mon`, the monitor the pointer is on, on
  // whichever axes the window is staying maximised on. This is the whole of
  // "still maximised": the monitor is asked for afresh on every motion event,
  // so crossing onto a bigger one grows the window as it arrives, and coming
  // back to a smaller one shrinks it again.
  Rect fillMonitor(Rect frame, const Rect& mon) const {
    if (kept_.vert) {
      frame.yMin = mon.yMin;
      frame.yMax = mon.yMax;
    }
    if (kept_.horz) {
      frame.xMin = mon.xMin;
      frame.xMax = mon.xMax;
    }
    return frame;
  }

  // The axes on which the window is filling the monitor it's on as it stands,
  // and so the ones the drag keeps it filling.
  //
  // This deliberately does not ask whether the client set
  // _NET_WM_STATE_MAXIMIZED_VERT. That flag is how a *client* says it is
  // maximised, and it's far from the only way a window gets that way: lwm's
  // own expand gesture fills a monitor without setting it, and so does a user
  // dragging an edge to the top and bottom of the screen. What matters to a
  // drag is what the window looks like, so that's what's measured - with the
  // flags folded in, because a client which says it is maximised is, even if
  // its own size rules leave it a pixel short.
  //
  // "Filling" means as big as the monitor lets this particular client be: an
  // xterm maximises to a whole number of character cells, which is a few
  // pixels short of the screen, and it would be a strange rule that treated
  // that as not maximised. LimitResize is what knows the difference.
  static FilledAxes axesToKeep(Client* c) {
    FilledAxes res;
    const std::vector<Rect> areas = LScr::I->VisibleAreas(true);
    if (areas.empty()) {
      return res;
    }
    const Rect frame = c->FrameRect();
    const Rect mon = findBestScreenFor(frame, areas);
    const Area full = maximizedFrameArea(c, mon);
    res.vert = c->wstate.maximized_vert || frame.height() == full.height;
    res.horz = c->wstate.maximized_horz || frame.width() == full.width;
    if (res.vert && res.horz) {
      // Nothing left to drag it by; see dropMaximization.
      return FilledAxes{};
    }
    return res;
  }

  // The size the window would be if it were maximised on `mon`: the monitor,
  // less whatever the client's own size rules refuse.
  static Area maximizedFrameArea(Client* c, const Rect& mon) {
    const Rect content =
        c->HasFurniture() ? Client::ContentFromFrameRect(mon) : mon;
    const Rect limited = c->LimitResize(content);
    return (c->HasFurniture() ? Client::FrameFromContentRect(limited) : limited)
        .area();
  }

  // Whether the window fitted on the monitor it started the drag on. If it
  // didn't - a client which asked to be bigger than the screen, and got it -
  // then shrinking it to fit as it's dragged would be lwm second-guessing a
  // size the user never chose, so a drag leaves such a window's size alone.
  static bool fitsOnItsMonitor(const Rect& frame) {
    const std::vector<Rect> areas = LScr::I->VisibleAreas(true);
    if (areas.empty()) {
      return false;
    }
    const Rect mon = findBestScreenFor(frame, areas);
    return frame.width() <= mon.width() && frame.height() <= mon.height();
  }

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
  const bool fit_to_monitor_;
  // The axes this drag keeps the window filling its monitor on. At most one of
  // them is ever set; see axesToKeep and dropMaximization.
  const FilledAxes kept_;
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

// ChordedDrag adds the two chords to a drag: with the drag's own button still
// held, a click of button 1 raises the window and carries on, and a click of
// button 3 iconises it and abandons the drag. They're the meanings those two
// buttons have on their own, offered without having to let go first - which
// matters most for the resize, where the edge being dragged can easily be
// behind something else, and where finding that out ought not to cost the
// drag.
//
// Both are clicks in the same sense as everywhere else in lwm: the pointer
// has to stay where the chord button went down, so a chord the user thinks
// better of costs nothing, and simply resting a finger on another button
// while dragging on doesn't quietly do something.
//
// It's a mixin because the two drags that have chords - the resize and the
// centre-square move - have no ancestor of their own below WindowDragger,
// which the frame drags and _NET_WM_MOVERESIZE use too, and those have no
// chords.
template <class Base>
class ChordedDrag : public Base {
 public:
  using Base::Base;

  virtual bool ChordPress(const xcb_button_press_event_t* e) {
    if (e->detail != SUPER_CHORD_RAISE_BUTTON &&
        e->detail != SUPER_CHORD_HIDE_BUTTON) {
      return false;
    }
    chord_button_ = e->detail;
    chord_pos_ = getMousePosition();
    return true;
  }

  virtual bool ChordRelease(const xcb_button_release_event_t* e) {
    if (e->detail != chord_button_) {
      // Not a button this drag took the press of, so it's the one the drag
      // itself is running on: this release is the end of the drag.
      return false;
    }
    const unsigned int button = chord_button_;
    chord_button_ = 0;
    if (!isClick(chord_pos_, getMousePosition())) {
      return true;  // The pointer wandered off: a change of mind. Drag on.
    }
    Client* c = LScr::I->GetClient(this->window());
    if (!c) {
      return true;
    }
    if (button == SUPER_CHORD_RAISE_BUTTON) {
      LOGD(c) << "Raising (user action, chord)";
      c->Raise();
      return true;
    }
    LOGD(c) << "Hiding (user action, chord)";
    c->Hide();
    // Returning false ends the drag, which is the point: there's nothing left
    // on screen to go on dragging. The window keeps whatever the drag had
    // done to it by now, and comes back that way when it's unhidden.
    return false;
  }

 private:
  // The chord button currently held, or 0. Button numbers start at 1, so 0
  // matches no release.
  unsigned int chord_button_ = 0;
  // Where the pointer was when that button went down.
  MousePos chord_pos_ = {};
};

// WindowExpander is the double-click half of the Windows-key gestures: it
// grows the window in one shot rather than following the mouse, so all the
// work happens on the press and the rest of the click is simply absorbed.
//
// The edges to expand come from the same 3x3 grid the resize gesture uses,
// with the centre square meaning "all of them". Windows in the way stop the
// expansion unless ignore_obstacles_ is set, which is what makes button 2's
// double click a "fill the monitor" and button 1's a "fill the space".
//
// The centre square is a toggle: when there's nothing left to grow into, it
// puts the window back to the size it had before it was expanded instead.
class WindowExpander : public DragHandler {
 public:
  WindowExpander(Client* c, Edge edge, bool ignore_obstacles)
      : window_(c->window), edge_(edge), ignore_obstacles_(ignore_obstacles) {}

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
      // Nowhere left to grow. From the centre square, that makes this the
      // other half of the gesture: a window which is already as big as it
      // goes shrinks back to the size it had before it was expanded, so that
      // one double click in the middle of a window toggles it. From an edge
      // there's no such reading - the user asked for that one edge to grow,
      // and it can't - so nothing happens, as before.
      if (edge_ == ENone) {
        c->Unexpand();
      }
      return;
    }
    // Before the maximisation goes, while it can still say where the window
    // was before it was maximised.
    c->NotePreExpandRect();
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
  WindowClicker(Client* c) : window_(c->window) {}
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
  // The client's own window; see WindowDragger::window() for why this can't be
  // the frame.
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

// Super+Control+click: turn lwm's furniture on or off for this window. Like
// the other clicks it waits for the release and does nothing if the pointer
// wandered off in between, so a change of mind costs nothing.
class WindowDecorationToggler : public WindowClicker {
 public:
  WindowDecorationToggler(Client* c) : WindowClicker(c) {}
  virtual void act(Client* c) {
    LOGD(c) << "Toggling decorations (user action)";
    c->SetFurniture(!c->HasFurniture());
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
//   button 1 press         raise the window, as a button 1 press on the
//                          furniture does
//   button 1 drag          raise the window and move it
//   button 2 drag          resize the nearest edge or corner, or, from the
//                          centre square, move the window without raising it
//   button 1 double click  expand the nearest edge or corner up to whatever
//                          is in the way
//   button 2 double click  the same, but ignoring other windows, so it
//                          expands to the monitor
//   button 3 click         hide the window, as a button 3 click on the
//                          furniture does
//
// and, while a button 2 drag is running, with button 2 still held (see
// ChordedDrag):
//
//   button 1 click         raise the window, and go on dragging
//   button 3 click         hide the window, which ends the drag
//
// Holding Control as well switches to a second, much smaller set, which so
// far has one gesture in it:
//
//   button 1 click         turn lwm's furniture on or off for this window
//
// A double click in the centre square grows every edge, and grows nothing
// when the window is already as big as it goes - which is when it shrinks the
// window back to its pre-expansion size instead.
//
// The two ways of moving a window differ only in the stacking order: button 1
// brings it to the front, and button 2's centre square doesn't, which is the
// same pair of choices the frame offers with its reshape and move buttons.
//
// "Nearest edge or corner" means the 3x3 grid in gesture.h, laid over the
// part of the window the gesture works on - the client's own window, not the
// frame, since that's the whole area a press can arrive from. Its centre
// square resizes nothing, so that's the square that moves.
DragHandler* getSuperDragHandler(Client* c,
                                 const xcb_button_press_event_t* e) {
  if (e->state & SUPER_CTRL_MASK) {
    // Super+Control, which is a gesture set of its own rather than a variation
    // on the ones below: none of the grid, the double clicks or the drags
    // means anything here. Only one button does anything, and the others
    // deliberately do nothing rather than something the user didn't ask for.
    if (e->detail == SUPER_DECORATE_BUTTON) {
      return new WindowDecorationToggler(c);
    }
    return nullptr;
  }
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
  // The centre of the grid names no edge, so there's nothing there for button
  // 2 to resize. It moves the window instead - the one thing the grid's
  // middle can mean - and, unlike button 1, leaves the stacking order alone,
  // so that a window can be nudged into place without coming to the front.
  const bool centre_moving = !moving && edge == ENone;
  // The press may have come through any of several passive grabs, whose event
  // masks are none of this code's business - click-to-focus installs one of
  // its own on the same window, and it asks for no motion events at all.
  // Restate what this drag needs on the active grab, which is also how the
  // pointer gets the right shape while the drag is going on. It is also what
  // makes the chords possible: the presses of the other buttons arrive
  // through this grab, and would otherwise go to the application.
  xlib::XChangeActivePointerGrab(
      ButtonMask | XCB_EVENT_MASK_POINTER_MOTION_HINT |
          XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_OWNER_GRAB_BUTTON,
      LScr::I->Cursors()->ForEdge((moving || centre_moving) ? ENone : edge),
      XCB_CURRENT_TIME);
  const unsigned int mask = buttonStateMask(e->detail);
  if (moving) {
    // Raised here rather than at the end of the gesture, so that the window
    // the user is dragging is in front of the others while they do it, and so
    // that a press which turns out to be a click has already done its work.
    LOGD(c) << "Raising (user action)";
    c->Raise();
    return new WindowMover(c, mask);
  }
  if (centre_moving) {
    return new ChordedDrag<WindowMover>(c, mask);
  }
  return new ChordedDrag<WindowResizer>(c, edge, mask);
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

DragHandler* getMoveResizeHandler(Client* c, Edge edge, int button) {
  // WindowDragger follows the pointer until the button it started with comes
  // back up, so a button we can't find in a pointer state mask is one whose
  // release we would never recognise: the drag would then run for ever, and
  // because lwm refuses to start a second drag while one is in progress, every
  // later mouse gesture would be refused too. Decline instead.
  const unsigned int mask = buttonStateMask(button);
  if (!mask) {
    return nullptr;
  }
  // Deliberately the plain WindowMover and WindowResizer: the raise and the
  // chords belong to the Windows-key bindings, where the user is holding a
  // button lwm chose. Here the button is the client's choice, the caller has
  // already raised the window on the client's behalf, and a click of another
  // button is the application's business.
  if (edge == ENone) {
    return new WindowMover(c, mask);
  }
  return new WindowResizer(c, edge, mask);
}

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
