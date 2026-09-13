#include "keyboard.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

#include "client.h"
#include "debug.h"
#include "disp.h"
#include "focus.h"
#include "hider.h"
#include "log.h"
#include "navigate.h"
#include "screen.h"
#include "screenlayout.h"

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

// What a key does to the unhide menu while that menu holds the keyboard. These
// aren't grabbed, and can't be: they're keys applications use, and lwm wants
// them only for the moment the menu is up. The active keyboard grab the menu
// takes is what brings them here instead.
enum class MenuKey {
  kUp,      // Previous item.
  kDown,    // Next item.
  kSelect,  // Unhide the selected window.
  kCancel,  // Close the menu, changing nothing.
};

struct MenuKeysym {
  uint32_t keysym;
  MenuKey key;
};

const MenuKeysym kMenuKeys[] = {
    {kKeysymUp, MenuKey::kUp},
    {kKeysymDown, MenuKey::kDown},
    {kKeysymReturn, MenuKey::kSelect},
    {kKeysymKPEnter, MenuKey::kSelect},
    {kKeysymSpace, MenuKey::kSelect},
    {kKeysymEscape, MenuKey::kCancel},
};

// Which keycodes we grabbed last time, and what each one means. Kept so that
// a KeyPress is a lookup rather than a round trip to the server, and so that
// a re-grab knows what to release.
std::map<uint8_t, Direction>& grabbedKeys() {
  static std::map<uint8_t, Direction> keys;
  return keys;
}

// The keycodes carrying Tab, which opens the unhide menu.
std::set<uint8_t>& menuOpenKeys() {
  static std::set<uint8_t> keys;
  return keys;
}

// The keycode-to-action map for the menu's own keys. Built at the same time as
// the grabs, and for the same reason: a layout switch moves the keycode a
// keysym sits on, and MappingNotify is when we find out.
std::map<uint8_t, MenuKey>& menuKeys() {
  static std::map<uint8_t, MenuKey> keys;
  return keys;
}

// Exactly which (keycode, modifiers) pairs we asked the server for, so that a
// re-grab releases precisely those. Keeping the list rather than recomputing it
// matters because the keys aren't all grabbed under the same modifier sets: the
// arrows want Super and Super+Shift, Tab only Super.
std::vector<std::pair<uint8_t, unsigned int>>& activeGrabs() {
  static std::vector<std::pair<uint8_t, unsigned int>> grabs;
  return grabs;
}

// Asks for one keycode under the given modifier set, and under both lock
// modifiers and their combination: a passive grab matches the modifier state
// exactly, so without these the gesture stops working the moment Num Lock is
// on.
void grabKey(uint8_t keycode, unsigned int modifiers) {
  for (unsigned int locks : lockModifiers) {
    // Asynchronous on both devices: lwm swallows these presses whole and never
    // replays them, so there's no reason to freeze anything while it makes up
    // its mind.
    xlib::XGrabKey(keycode, modifiers | locks, LScr::I->Root(), false,
                   XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    activeGrabs().push_back({keycode, modifiers | locks});
  }
}

// The frames of the windows in front of c, bottom-most first: everything
// which could be hiding part of it. The stacking order comes from the server
// rather than from anything lwm remembers, because that's the only place it
// is actually kept - a client can restack itself, and lwm's own fix_stack
// does the same on its behalf.
std::vector<Rect> windowsInFrontOf(const Client* c) {
  std::vector<Rect> res;
  bool found = false;
  // Children of the root, bottom-most first, so everything after c in the
  // list is in front of it. Windows which aren't clients (lwm's own popup and
  // menu, and anything override-redirect) are skipped: the first two are only
  // up while the user is using them, and nothing here is asked at that moment.
  for (Window w : xlib::WindowTree::Query(LScr::I->Root()).children) {
    Client* other = LScr::I->GetClient(w);
    if (!other) {
      continue;
    }
    if (other == c) {
      found = true;
      continue;
    }
    if (!found || other->IsHidden() || other->IsWithdrawn()) {
      continue;  // Behind c, or not on screen to be hiding anything.
    }
    res.push_back(other->FrameRect());
  }
  return res;
}

// Puts the pointer on the newly focused window: in the middle of the largest
// piece of it which is both on screen and not covered by another window.
//
// The window has been raised by the time this is called, so usually nothing
// is covering it and this is simply its middle. What survives a raise is a
// window's own transients - Client::Raise takes them up with it, and they end
// up in front of it, which is the point of them - so a window with a dialog
// over it still has to be measured rather than assumed. Under the default
// sloppy focus the pointer landing on that dialog would send lwm an
// EnterNotify and take the focus straight back off the window the user asked
// for. If there is no uncovered piece at all, the pointer stays where it is:
// that's better than undoing the very gesture that moved it.
void warpPointerTo(Client* c) {
  // The part of the frame which is on screen at all. VisibleAreas(true)
  // excludes the space panels have reserved, so a window under a panel
  // doesn't get the pointer parked on the panel - which, for a panel which is
  // override-redirect and so not a client, is the only thing that keeps it off.
  const Rect frame = c->FrameRect();
  const Rect onScreen = Rect::Intersect(
      frame, findBestScreenFor(frame, LScr::I->VisibleAreas(true)));
  const Rect visible = LargestVisibleRect(onScreen, windowsInFrontOf(c));
  if (visible.empty()) {
    LOGD(c) << "Not warping the pointer: no visible part of " << frame;
    return;
  }
  xlib::XWarpPointer(visible.middle());
}

// Where the window these keys last raised came from in the stacking order.
//
// Raising the window the focus moves to is right for the window you're going
// to, and wrong for the ones you pass over on the way: flipping between two
// windows either side of a third leaves that third one parked on top of both,
// which is not what the user asked for by pressing an arrow twice. So the
// raise is remembered, and undone by the next one - each arrow press puts the
// last window it raised back where it was before raising the new one. A
// window flipped across therefore comes to the front for as long as it holds
// the focus, and drops back out of the way as soon as the focus leaves it.
//
// There's only ever one note. Each raise consumes the previous one, so at
// most one window is ever up out of its place because of these keys.
struct RaiseNote {
  Window client = 0;  // The raised client's window; 0 for "no note".
  Window below = 0;   // Where it was: what it sat directly above.
};

RaiseNote& raiseNote() {
  static RaiseNote note;
  return note;
}

// Puts the noted window back where it came from, and forgets it. A note for a
// window which has since been closed is simply dropped: GetClient with
// scan_parents off is the lookup that answers "is this still a client of
// ours", without asking the server about a window which may not exist.
void restoreLastRaised() {
  const RaiseNote note = raiseNote();
  raiseNote() = RaiseNote{};
  if (!note.client) {
    return;
  }
  if (Client* c = LScr::I->GetClient(note.client, false)) {
    c->RestoreStackPosition(note.below);
  }
}

// Moves the input focus to the next window in dir, if there is one, and
// raises it: the focus is no use on a window buried under another, and having
// asked for it by name the user means the window in front, not just the one
// typing goes to.
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
  Client* target = candidates[idx];
  // Unless the target is the window the note is already about - a press which
  // has come back to a window we raised and which then lost the focus some
  // other way, to the mouse. Raising it again doesn't change where it
  // originally came from, so the note stands.
  if (raiseNote().client != target->window) {
    // Whatever the last press raised goes back down before this one raises
    // anything, so that the position noted for the target is where it really
    // sits now, rather than where it sits under a window that's on its way
    // back down.
    restoreLastRaised();
    raiseNote() = RaiseNote{target->window, target->StackPosition()};
  }
  LScr::I->GetFocuser()->FocusClient(target);
  target->Raise();
  // After the raise, so that the part of the window the pointer can be put on
  // is the part it has once it's in front.
  warpPointerTo(target);
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
  //
  // Anything the arrow keys raised goes back where it came from first -
  // including this window, if it's the one the note is about. Putting it back
  // only to raise it again on the next line is a no-op the user never sees;
  // what matters is that the note is cleared, because a window the user has
  // moved on purpose has earned its place at the front and shouldn't drop
  // back down when the focus flips away from it.
  restoreLastRaised();
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
  for (const auto& grab : activeGrabs()) {
    xlib::XUngrabKey(grab.first, grab.second, root);
  }
  activeGrabs().clear();
  grabbedKeys().clear();
  menuOpenKeys().clear();
  menuKeys().clear();

  for (const ArrowKey& arrow : kArrowKeys) {
    for (uint8_t keycode : xlib::KeycodesForKeysym(arrow.keysym)) {
      grabbedKeys()[keycode] = arrow.dir;
      for (unsigned int mods : kArrowModifiers) {
        grabKey(keycode, mods);
      }
    }
  }
  // Super+Tab opens the unhide menu. Shift isn't grabbed with it: there's only
  // one thing to do, and the menu it opens takes the keyboard for itself the
  // moment it's up.
  for (uint8_t keycode : xlib::KeycodesForKeysym(kKeysymTab)) {
    menuOpenKeys().insert(keycode);
    grabKey(keycode, SUPER_MASK);
  }
  // The menu's own keys are looked up, not grabbed: see kMenuKeys.
  for (const MenuKeysym& mk : kMenuKeys) {
    for (uint8_t keycode : xlib::KeycodesForKeysym(mk.keysym)) {
      menuKeys()[keycode] = mk.key;
    }
  }
}

void ForgetNavigationState() {
  raiseNote() = RaiseNote{};
}

bool HandleKeyPress(xcb_key_press_event_t* ev) {
  Hider* hider = LScr::I->GetHider();
  if (hider->KeyboardMenuIsOpen()) {
    // lwm holds an active keyboard grab, so this press is the menu's whatever
    // it is. Modifiers are ignored: the user reaching for Escape with a finger
    // still on Shift means Escape.
    const auto mk = menuKeys().find(ev->detail);
    if (mk == menuKeys().end()) {
      return true;  // Not a key the menu uses. Swallow it; nobody else can act
                    // on it while we hold the keyboard.
    }
    switch (mk->second) {
      case MenuKey::kUp:
        hider->KeyboardMenuMove(-1);
        break;
      case MenuKey::kDown:
        hider->KeyboardMenuMove(1);
        break;
      case MenuKey::kSelect:
        hider->KeyboardMenuSelect();
        break;
      case MenuKey::kCancel:
        hider->KeyboardMenuClose(true);
        break;
    }
    return true;
  }
  if (menuOpenKeys().count(ev->detail)) {
    // Not in the middle of a mouse gesture. The button-3 unhide menu is itself
    // a drag, and it uses the very window this would put up, so opening one
    // over the other would leave the keyboard menu believing it owns a window
    // the button release is about to unmap - and holding a keyboard grab that
    // nothing was left to release.
    if (!IsDragging()) {
      hider->OpenMenuForKeyboard();
    }
    return true;
  }
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
