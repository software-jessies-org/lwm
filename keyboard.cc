#include "keyboard.h"

#include <map>
#include <vector>

#include "client.h"
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

}  // namespace

void GrabNavigationKeys() {
  const Window root = LScr::I->Root();
  for (const auto& it : grabbedKeys()) {
    for (unsigned int locks : lockModifiers) {
      xlib::XUngrabKey(it.first, SUPER_MASK | locks, root);
    }
  }
  grabbedKeys().clear();

  for (const ArrowKey& arrow : kArrowKeys) {
    for (uint8_t keycode : xlib::KeycodesForKeysym(arrow.keysym)) {
      grabbedKeys()[keycode] = arrow.dir;
      for (unsigned int locks : lockModifiers) {
        // Asynchronous on both devices: lwm swallows these presses whole and
        // never replays them, so there's no reason to freeze anything while
        // it makes up its mind.
        xlib::XGrabKey(keycode, SUPER_MASK | locks, root, false,
                       XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
      }
    }
  }
}

bool HandleKeyPress(xcb_key_press_event_t* ev) {
  const auto it = grabbedKeys().find(ev->detail);
  if (it == grabbedKeys().end()) {
    return false;
  }
  // No check on ev->state beyond this: a passive grab matches the modifier
  // set exactly, so the only presses delivered here are the ones we asked
  // for. Super+Control+arrow, in particular, was never grabbed and still
  // reaches the application.
  const Direction dir = it->second;

  Focuser* focuser = LScr::I->GetFocuser();
  Client* focused = focuser->GetFocusedClient();
  if (!focused) {
    // Nothing has focus, so there's no window for "next one to the left" to
    // be relative to. Picking one arbitrarily would be a guess, and a guess
    // that moved focus somewhere surprising is worse than doing nothing.
    return true;
  }

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
    return true;  // Ours, but there's no window that way.
  }
  focuser->FocusClient(candidates[idx]);
  return true;
}
