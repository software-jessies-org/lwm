#include "keyboard.h"

#include <map>
#include <vector>

#include "client.h"
#include "debug.h"
#include "disp.h"
#include "focus.h"
#include "log.h"
#include "navigate.h"
#include "screen.h"

namespace {

// Every grab has to be repeated under both lock modifiers and their
// combination, or the gesture stops working the moment Num Lock is on. Same
// list, and same reason, as client.cc's for the mouse gestures.
const unsigned int lockModifiers[] = {
    0,
    XCB_MOD_MASK_LOCK,
    XCB_MOD_MASK_2,
    XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
};

struct ArrowKey {
  uint32_t keysym;
  Direction dir;
};

// The modifier sets we grab each arrow under: the Windows key on its own
// moves the focus, and with Shift it moves the focused window instead.
const unsigned int kArrowModifiers[] = {
    SUPER_MASK,
    SUPER_MASK | XCB_MOD_MASK_SHIFT,
};

const ArrowKey kArrowKeys[] = {
    {kKeysymLeft, Direction::kLeft},
    {kKeysymRight, Direction::kRight},
    {kKeysymUp, Direction::kUp},
    {kKeysymDown, Direction::kDown},
};

// Which keycodes we grabbed last time, and what each one means. Kept so that
// a KeyPress is a lookup rather than a round trip to the server, and so that
// a re-grab knows what to release.
std::map<uint8_t, Direction>& grabbedKeys() {
  static std::map<uint8_t, Direction> keys;
  return keys;
}

// Moves the input focus to the next window in dir, if there is one.
void moveFocus(Client* focused, Direction dir) {
  // Every window that could take the focus, on every monitor: navigation
  // deliberately crosses screen boundaries, so nothing here filters by which
  // monitor a window is on. The two vectors are built together and indexed
  // together; PickWindowInDirection answers with an index into rects.
  std::vector<Client*> candidates;
  std::vector<Rect> rects;
  for (const auto& cit : LScr::I->Clients()) {
    Client* c = cit.second;
    // A hidden client isn't on screen to be navigated to, and a withdrawn one
    // is on its way out. Both would be invisible destinations for the focus.
    if (!c || c->IsHidden() || c->IsWithdrawn()) {
      continue;
    }
    candidates.push_back(c);
    rects.push_back(c->FrameRect());
  }

  const int idx = PickWindowInDirection(focused->FrameRect(), rects, dir);
  if (idx < 0) {
    return;  // No window that way.
  }
  LScr::I->GetFocuser()->FocusClient(candidates[idx]);
}

// Moves the focused window itself in dir: to the inner edge of its monitor,
// or on to the next monitor if it's already there. See navigate.h for the
// rules, including what happens to a window too big for where it's going.
//
// The pointer goes with the window if it was on it, keeping its place on it
// proportionally. Without that, a window moved out from under the pointer
// leaves it over whatever was behind - and under the default sloppy focus,
// the EnterNotify that follows would take the focus straight off the window
// the user is moving.
void moveWindow(Client* c, Direction dir) {
  const Rect frame = c->FrameRect();
  const Rect target =
      MoveRectInDirection(frame, dir, LScr::I->VisibleAreas(true));
  if (target == frame) {
    return;  // Nowhere to go.
  }
  const MousePos mouse = getMousePosition();
  const bool carry_pointer = frame.contains(mouse.x, mouse.y);
  LOGD(c) << "Moving " << frame << " to " << target
          << (carry_pointer ? " (with the pointer)" : "");
  // Raise first. The pointer is about to be put down inside this window's new
  // position, and a window in front of it there would take the crossing event
  // - and the focus with it - which is exactly what the raise prevents. It
  // also matches what a mouse drag does: a window the user is moving comes to
  // the front.
  c->Raise();
  // The user has placed this window by hand, so it isn't maximised any more,
  // however it got that way - the same as dragging it with the mouse.
  c->DropMaximization();
  const Rect content =
      c->HasFurniture() ? Client::ContentFromFrameRect(target) : target;
  // The client still gets the last word on its size: a window shrunk to fit a
  // small monitor is shrunk by whole character cells if that's what it has.
  c->MoveResizeTo(c->LimitResize(content));
  if (carry_pointer) {
    // Measured against where the window actually ended up rather than against
    // `target`, because LimitResize may have had something to say about it.
    xlib::XWarpPointer(
        MapPointToMovedRect(Point{mouse.x, mouse.y}, frame, c->FrameRect()));
  }
}

}  // namespace

void GrabNavigationKeys() {
  const Window root = LScr::I->Root();
  for (const auto& it : grabbedKeys()) {
    for (unsigned int mods : kArrowModifiers) {
      for (unsigned int locks : lockModifiers) {
        xlib::XUngrabKey(it.first, mods | locks, root);
      }
    }
  }
  grabbedKeys().clear();

  for (const ArrowKey& arrow : kArrowKeys) {
    for (uint8_t keycode : xlib::KeycodesForKeysym(arrow.keysym)) {
      grabbedKeys()[keycode] = arrow.dir;
      for (unsigned int mods : kArrowModifiers) {
        for (unsigned int locks : lockModifiers) {
          // Asynchronous on both devices: lwm swallows these presses whole and
          // never replays them, so there's no reason to freeze anything while
          // it makes up its mind.
          xlib::XGrabKey(keycode, mods | locks, root, false,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        }
      }
    }
  }
}

bool HandleKeyPress(xcb_key_press_event_t* ev) {
  const auto it = grabbedKeys().find(ev->detail);
  if (it == grabbedKeys().end()) {
    return false;
  }
  // The only thing ev->state is read for is Shift, which is the difference
  // between the two gestures: a passive grab matches the modifier set
  // exactly, so the only presses delivered here are the ones we asked for.
  // Super+Control+arrow, in particular, was never grabbed and still reaches
  // the application.
  const Direction dir = it->second;
  const bool move_window = ev->state & XCB_MOD_MASK_SHIFT;

  Client* focused = LScr::I->GetFocuser()->GetFocusedClient();
  if (!focused) {
    // Nothing has focus, so there's no window for "next one to the left" to
    // be relative to, and none to move either. Picking one arbitrarily would
    // be a guess, and a guess that moved focus somewhere surprising is worse
    // than doing nothing.
    return true;
  }
  if (move_window) {
    moveWindow(focused, dir);
  } else {
    moveFocus(focused, dir);
  }
  return true;
}
