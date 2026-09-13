// Tests for the keyboard-driven unhide menu (Super+Tab), from the KeyPress
// event down to the window actually being unhidden.
//
// The menu's arithmetic lives in menulayout.cc and is tested there; what's
// tested here is the wiring, and the things which are only true once the whole
// thing is assembled: that the menu opens on the monitor the pointer is on,
// that the arrows move the pointer from item to item and stop at the ends, that
// the red highlight box follows the selection, that the menu stays above that
// box's windows, and that every way of closing the menu puts the pointer back
// where the user left it.

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "client.h"
#include "disp.h"
#include "focus.h"
#include "hider.h"
#include "keyboard.h"
#include "lwm.h"
#include "menulayout.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"
#include "xfont.h"

namespace {

// The keycodes the fake keyboard puts these on; see
// FakeServer::GetKeyboardMapping.
constexpr uint8_t kUpKey = 111;
constexpr uint8_t kDownKey = 116;
constexpr uint8_t kLeftKey = 113;
constexpr uint8_t kTabKey = 23;
constexpr uint8_t kEscapeKey = 9;
constexpr uint8_t kReturnKey = 36;
constexpr uint8_t kSpaceKey = 65;

// The height of one menu item, which is what an arrow key moves the pointer by.
int itemHeight() {
  return MenuStyle{wmtest::World::kTextHeight}.ItemHeight();
}

void pressKey(uint8_t keycode, uint16_t state) {
  xcb_key_press_event_t e{};
  e.response_type = XCB_KEY_PRESS;
  e.detail = keycode;
  e.time = 1000;
  e.root = LScr::I->Root();
  // A key grab on the root, and an active keyboard grab taken on the root,
  // both report the press against the root whatever the pointer is over.
  e.event = LScr::I->Root();
  e.child = XCB_NONE;
  e.state = state;
  DispatchXEvent((xcb_generic_event_t*)&e);
}

// Super+Tab: the gesture that opens the menu.
void pressSuperTab() {
  pressKey(kTabKey, SUPER_MASK);
}

// A plain press, which is what the menu's own keys are: while the menu is up
// lwm holds the keyboard, so nothing had to be grabbed for these.
void pressPlain(uint8_t keycode) {
  pressKey(keycode, 0);
}

// Moves the pointer as the user would, and tells lwm about it the way the
// server does: a MotionNotify against the menu window, which is what the menu
// asks for while the keyboard is driving it.
void moveMouseTo(wmtest::World& world, int x, int y) {
  world.server().SetMousePosition(x, y, 0);
  xcb_motion_notify_event_t e{};
  e.response_type = XCB_MOTION_NOTIFY;
  e.time = 1000;
  e.root = LScr::I->Root();
  e.event = LScr::I->Menu();
  e.root_x = x;
  e.root_y = y;
  DispatchXEvent((xcb_generic_event_t*)&e);
}

// The pointer leaving the menu, as the server reports it.
void sendMenuLeave() {
  xcb_leave_notify_event_t e{};
  e.response_type = XCB_LEAVE_NOTIFY;
  e.time = 1000;
  e.root = LScr::I->Root();
  e.event = LScr::I->Menu();
  e.mode = XCB_NOTIFY_MODE_NORMAL;
  e.detail = XCB_NOTIFY_DETAIL_NONLINEAR;
  DispatchXEvent((xcb_generic_event_t*)&e);
}

Point mouseAt() {
  const MousePos mp = getMousePosition();
  return Point{mp.x, mp.y};
}

Rect menuRect() {
  const xlib::WindowGeometry g = xlib::XGetGeometry(LScr::I->Menu());
  return g.rect;
}

// The four windows which draw the red highlight box, by the names
// Hider::showHighlightBox creates them under. Zero until the box has been
// needed once: they're created lazily.
Window highlightWindow(wmtest::World& world, const char* side) {
  return world.server().WindowNamed(std::string("LWM highlight ") + side);
}

// The rectangle the red highlight box is currently drawn around, reconstructed
// from the two of its windows which carry all four numbers: the left bar gives
// x, y and the height, the top bar the width.
Rect highlightedRect(wmtest::World& world) {
  const Window l = highlightWindow(world, "L");
  const Window t = highlightWindow(world, "T");
  if (!l || !t) {
    return Rect{};
  }
  const Rect lr = xlib::XGetGeometry(l).rect;
  const Rect tr = xlib::XGetGeometry(t).rect;
  return Rect::FromXYWH(lr.xMin, lr.yMin, tr.width(), lr.height());
}

// True if lwm asked the server for this key under these modifiers.
bool sawGrab(wmtest::World& world, uint8_t keycode, unsigned int modifiers) {
  std::ostringstream s;
  s << "GrabKey(0x" << std::hex << LScr::I->Root() << std::dec
    << ") keycode=" << int(keycode) << " modifiers=" << modifiers;
  const std::string want = s.str();
  for (const std::string& call : world.server().Calls()) {
    if (call == want) {
      return true;
    }
  }
  return false;
}

// Where a window sits in the root's stacking order, counting from the bottom.
int stackIndex(wmtest::World& world, Window w) {
  const std::vector<Window> stack = world.server().ChildrenOf(LScr::I->Root());
  for (int i = 0; i < int(stack.size()); i++) {
    if (stack[i] == w) {
      return i;
    }
  }
  return -1;
}

}  // namespace

TEST(UnhideMenuKeys, SuperTabIsGrabbedUnderEveryLockCombination) {
  wmtest::World world;
  world.server().ClearCalls();
  GrabNavigationKeys();
  // Every lock combination, for the same reason the arrows need them: a passive
  // grab matches the modifier set exactly, so without these the gesture stops
  // working the moment Num Lock is on.
  const unsigned int locks[] = {
      0,
      XCB_MOD_MASK_LOCK,
      XCB_MOD_MASK_2,
      XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
  };
  for (unsigned int lock : locks) {
    testing::Context ctx(std::to_string(lock));
    EXPECT_TRUE(sawGrab(world, kTabKey, SUPER_MASK | lock));
  }
  // And Shift+Super+Tab isn't grabbed: there's only the one gesture.
  EXPECT_FALSE(
      sawGrab(world, kTabKey, SUPER_MASK | XCB_MOD_MASK_SHIFT));
}

TEST(UnhideMenuKeys, OpensCentredOnThePointersMonitorWithThePointerOnItsTopItem) {
  wmtest::World world(2000, 1000);
  GrabNavigationKeys();
  // Two monitors side by side. The pointer is on the right-hand one, so that's
  // where the menu has to appear - not on the primary, and not where any window
  // happens to be.
  const Rect left = Rect::FromXYWH(0, 0, 1000, 1000);
  const Rect right = Rect::FromXYWH(1000, 0, 1000, 1000);
  LScr::I->SetVisibleAreas({left, right});
  ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200)) !=
              nullptr);
  ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(100, 400, 200, 200)) !=
              nullptr);
  world.server().SetMousePosition(1600, 800, 0);

  pressSuperTab();
  ASSERT_TRUE(LScr::I->GetHider()->KeyboardMenuIsOpen());

  // Centred on the monitor the pointer was on: equal space either side of it,
  // give or take the odd pixel an odd-sized menu leaves over.
  const Rect menu = menuRect();
  EXPECT_TRUE(menu.xMin >= right.xMin && menu.xMax <= right.xMax);
  EXPECT_TRUE(std::abs((menu.xMin - right.xMin) - (right.xMax - menu.xMax)) <=
              1);
  EXPECT_TRUE(std::abs((menu.yMin - right.yMin) - (right.yMax - menu.yMax)) <=
              1);
  // Two clients, so two items.
  EXPECT_EQ(menu.height(), 2 * itemHeight());

  // And the pointer is in the middle of the top item.
  const Point p = mouseAt();
  EXPECT_EQ(p.x, menu.xMin + menu.width() / 2);
  EXPECT_EQ(p.y, menu.yMin + itemHeight() / 2);
}

TEST(UnhideMenuKeys, MenuStaysAboveTheRedHighlightBoxWindows) {
  // The four highlight windows are raised to the top every time the selection
  // changes. If the menu didn't go back above them, a window whose frame lay
  // under the menu would leave one of them drawn across it - and, because the
  // pointer is genuinely inside the menu under keyboard control, the pointer
  // would count as having left the menu, which closes it. So the menu must be
  // in front of all four, both when it opens and after every move.
  wmtest::World world;
  GrabNavigationKeys();
  // Two windows placed so that their frames cover the middle of the screen,
  // which is exactly where the menu is about to open.
  Client* a = world.MapClientWindow(Rect::FromXYWH(100, 100, 1000, 800));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 200, 900, 700));
  ASSERT_TRUE(a != nullptr);
  ASSERT_TRUE(b != nullptr);
  world.server().SetMousePosition(640, 512, 0);

  pressSuperTab();
  ASSERT_TRUE(LScr::I->GetHider()->KeyboardMenuIsOpen());

  const char* sides[] = {"L", "R", "T", "B"};
  for (const char* side : sides) {
    testing::Context ctx(std::string("on open, side ") + side);
    const Window hl = highlightWindow(world, side);
    ASSERT_TRUE(hl != 0);
    EXPECT_TRUE(stackIndex(world, LScr::I->Menu()) > stackIndex(world, hl));
  }

  // And again after an arrow key has moved the box to the other window.
  pressPlain(kDownKey);
  for (const char* side : sides) {
    testing::Context ctx(std::string("after moving, side ") + side);
    const Window hl = highlightWindow(world, side);
    ASSERT_TRUE(hl != 0);
    EXPECT_TRUE(stackIndex(world, LScr::I->Menu()) > stackIndex(world, hl));
  }
}

TEST(UnhideMenuKeys, TheRedBoxIsDrawnRoundTheSelectedWindow) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* a = world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200));
  Client* b = world.MapClientWindow(Rect::FromXYWH(700, 600, 300, 150));
  ASSERT_TRUE(a != nullptr);
  ASSERT_TRUE(b != nullptr);
  world.server().SetMousePosition(640, 512, 0);

  pressSuperTab();
  // Neither client is hidden, so they're listed in Clients() order, which is by
  // window id: a first, then b.
  EXPECT_EQ(highlightedRect(world), a->FrameRect());
  pressPlain(kDownKey);
  EXPECT_EQ(highlightedRect(world), b->FrameRect());
  pressPlain(kUpKey);
  EXPECT_EQ(highlightedRect(world), a->FrameRect());
}

TEST(UnhideMenuKeys, ArrowsMoveThePointerOneItemAtATime) {
  wmtest::World world;
  GrabNavigationKeys();
  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(50 + 50 * i, 50, 200,
                                                     200)) != nullptr);
  }
  world.server().SetMousePosition(640, 512, 0);
  pressSuperTab();

  const Rect menu = menuRect();
  const int top = menu.yMin + itemHeight() / 2;
  EXPECT_EQ(mouseAt().y, top);
  pressPlain(kDownKey);
  EXPECT_EQ(mouseAt().y, top + itemHeight());
  pressPlain(kDownKey);
  EXPECT_EQ(mouseAt().y, top + 2 * itemHeight());
  pressPlain(kUpKey);
  EXPECT_EQ(mouseAt().y, top + itemHeight());
  // The x never changes: the pointer stays down the middle of the menu.
  EXPECT_EQ(mouseAt().x, menu.xMin + menu.width() / 2);
}

TEST(UnhideMenuKeys, ArrowsAreClampedToTheEndsOfTheMenu) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* a = world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200));
  Client* b = world.MapClientWindow(Rect::FromXYWH(400, 50, 200, 200));
  ASSERT_TRUE(a != nullptr);
  ASSERT_TRUE(b != nullptr);
  world.server().SetMousePosition(640, 512, 0);
  pressSuperTab();

  // Up on the top item does nothing at all: it doesn't wrap round to the
  // bottom, and it doesn't move the pointer off the menu.
  const Point top = mouseAt();
  pressPlain(kUpKey);
  EXPECT_EQ(mouseAt(), top);
  EXPECT_EQ(highlightedRect(world), a->FrameRect());
  EXPECT_TRUE(LScr::I->GetHider()->KeyboardMenuIsOpen());

  // And likewise down on the bottom one.
  pressPlain(kDownKey);
  const Point bottom = mouseAt();
  ASSERT_TRUE(bottom != top);
  pressPlain(kDownKey);
  EXPECT_EQ(mouseAt(), bottom);
  EXPECT_EQ(highlightedRect(world), b->FrameRect());
  EXPECT_TRUE(LScr::I->GetHider()->KeyboardMenuIsOpen());
}

TEST(UnhideMenuKeys, MovingThePointerOverTheMenuSelectsTheItemUnderIt) {
  // The arrows work by moving the pointer, so the pointer has to be what the
  // selection follows - including when the user moves it themselves.
  wmtest::World world;
  GrabNavigationKeys();
  Client* a = world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200));
  Client* b = world.MapClientWindow(Rect::FromXYWH(700, 600, 300, 150));
  ASSERT_TRUE(a != nullptr);
  ASSERT_TRUE(b != nullptr);
  world.server().SetMousePosition(640, 512, 0);
  pressSuperTab();
  ASSERT_EQ(highlightedRect(world), a->FrameRect());

  const Rect menu = menuRect();
  moveMouseTo(world, menu.xMin + 5, menu.yMin + itemHeight() + 2);
  EXPECT_EQ(highlightedRect(world), b->FrameRect());
  // And an arrow key from there carries on from where the pointer is, rather
  // than from wherever the last key left the selection.
  pressPlain(kUpKey);
  EXPECT_EQ(highlightedRect(world), a->FrameRect());
}

TEST(UnhideMenuKeys, EscapeClosesTheMenuAndPutsThePointerBack) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetHider()->Hide(c);
  ASSERT_TRUE(c->IsHidden());

  const Point was{640, 512};
  world.server().SetMousePosition(was.x, was.y, 0);
  pressSuperTab();
  ASSERT_TRUE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  ASSERT_TRUE(world.server().IsMapped(LScr::I->Menu()));
  ASSERT_TRUE(mouseAt() != was);

  pressPlain(kEscapeKey);
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_FALSE(world.server().IsMapped(LScr::I->Menu()));
  EXPECT_EQ(mouseAt(), was);
  // Escape changes nothing else: the window it was pointing at stays hidden.
  EXPECT_TRUE(c->IsHidden());
  // And the keyboard goes back to the applications.
  EXPECT_TRUE(world.server().DidCall("UngrabKeyboard("));
}

TEST(UnhideMenuKeys, ThePointerLeavingTheMenuClosesIt) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetHider()->Hide(c);

  const Point was{640, 512};
  world.server().SetMousePosition(was.x, was.y, 0);
  pressSuperTab();
  ASSERT_TRUE(LScr::I->GetHider()->KeyboardMenuIsOpen());

  sendMenuLeave();
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_FALSE(world.server().IsMapped(LScr::I->Menu()));
  EXPECT_EQ(mouseAt(), was);
  EXPECT_TRUE(c->IsHidden());

  // The LeaveNotify which the closing itself generates (the menu is unmapped
  // from under the pointer, and then the pointer is warped home) must not be
  // read as anything: the menu has already gone.
  sendMenuLeave();
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_EQ(mouseAt(), was);
}

TEST(UnhideMenuKeys, ReturnUnhidesAndRaisesTheSelectedWindow) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* front = world.MapClientWindow(Rect::FromXYWH(400, 400, 300, 300));
  Client* hidden = world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200));
  ASSERT_TRUE(front != nullptr);
  ASSERT_TRUE(hidden != nullptr);
  LScr::I->GetHider()->Hide(hidden);
  ASSERT_TRUE(hidden->IsHidden());
  // The hidden window is listed first, so it's the one the menu opens on.
  const Point was{640, 512};
  world.server().SetMousePosition(was.x, was.y, 0);
  pressSuperTab();
  ASSERT_EQ(highlightedRect(world), hidden->FrameRect());

  pressPlain(kReturnKey);
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_FALSE(world.server().IsMapped(LScr::I->Menu()));
  EXPECT_FALSE(hidden->IsHidden());
  // Raised, which for two clients means it's the front one of the two.
  EXPECT_TRUE(stackIndex(world, hidden->parent) >
              stackIndex(world, front->parent));
  // And the pointer is back where the user left it.
  EXPECT_EQ(mouseAt(), was);
}

TEST(UnhideMenuKeys, SpaceSelectsTheSameWayReturnDoes) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* a = world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200));
  Client* b = world.MapClientWindow(Rect::FromXYWH(700, 600, 300, 150));
  ASSERT_TRUE(a != nullptr);
  ASSERT_TRUE(b != nullptr);
  LScr::I->GetHider()->Hide(a);
  LScr::I->GetHider()->Hide(b);
  // hidden_ is a push_front list, so the most recently hidden comes first.
  world.server().SetMousePosition(640, 512, 0);
  pressSuperTab();
  ASSERT_EQ(highlightedRect(world), b->FrameRect());
  pressPlain(kDownKey);
  ASSERT_EQ(highlightedRect(world), a->FrameRect());

  pressPlain(kSpaceKey);
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_FALSE(a->IsHidden());
  // Only the selected one: the other stays hidden.
  EXPECT_TRUE(b->IsHidden());
}

TEST(UnhideMenuKeys, KeysTheMenuDoesntUseAreSwallowed) {
  // lwm holds the keyboard while the menu is up, so a key it doesn't recognise
  // can't be passed on to anyone - but it mustn't be allowed to fall through to
  // the navigation gestures either, which would move the focus out from under
  // a menu the user is still reading.
  wmtest::World world;
  GrabNavigationKeys();
  Client* left = world.MapClientWindow(Rect::FromXYWH(50, 400, 200, 200));
  Client* right = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(left != nullptr);
  ASSERT_TRUE(right != nullptr);
  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(right);
  world.server().SetMousePosition(640, 512, 0);
  pressSuperTab();

  // Super+Left would normally move the focus. Not while the menu is up.
  pressKey(kLeftKey, SUPER_MASK);
  EXPECT_EQ(focuser->GetFocusedClient(), right);
  EXPECT_TRUE(LScr::I->GetHider()->KeyboardMenuIsOpen());
}

TEST(UnhideMenuKeys, ASecondSuperTabDoesNotReopenTheMenu) {
  // In particular it must not overwrite the pointer position the menu is going
  // to put the pointer back to: that's where the user was before any of this,
  // not the middle of the top menu item.
  wmtest::World world;
  GrabNavigationKeys();
  ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200)) !=
              nullptr);
  ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(400, 50, 200, 200)) !=
              nullptr);
  const Point was{640, 512};
  world.server().SetMousePosition(was.x, was.y, 0);
  pressSuperTab();
  pressPlain(kDownKey);
  const Point selected = mouseAt();

  pressSuperTab();
  // Still on the item the user had picked, rather than back at the top.
  EXPECT_EQ(mouseAt(), selected);
  pressPlain(kEscapeKey);
  EXPECT_EQ(mouseAt(), was);
}

TEST(UnhideMenuKeys, SuperTabIsIgnoredWhileAMouseMenuIsUp) {
  // The button-3 unhide menu is a drag, and it uses the very window Super+Tab
  // would put up. Opening one over the other would leave the keyboard menu
  // owning a window the button release is about to unmap, and holding a
  // keyboard grab with nothing left to release it.
  wmtest::World world;
  GrabNavigationKeys();
  ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200)) !=
              nullptr);
  world.server().SetMousePosition(640, 512, 0);

  // Button 3 on the root, which is what opens the mouse-driven menu.
  xcb_button_press_event_t press{};
  press.response_type = XCB_BUTTON_PRESS;
  press.detail = XCB_BUTTON_INDEX_3;
  press.time = 1000;
  press.root = LScr::I->Root();
  press.event = LScr::I->Root();
  press.child = XCB_NONE;
  press.root_x = 640;
  press.root_y = 512;
  press.event_x = 640;
  press.event_y = 512;
  DispatchXEvent((xcb_generic_event_t*)&press);
  ASSERT_TRUE(IsDragging());

  world.server().ClearCalls();
  pressSuperTab();
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_FALSE(world.server().DidCall("GrabKeyboard("));

  // Let go, or the drag outlives this test: current_dragger is a static, and
  // nothing but a button release retires it.
  xcb_button_release_event_t release{};
  release.response_type = XCB_BUTTON_RELEASE;
  release.detail = XCB_BUTTON_INDEX_3;
  release.time = 1100;
  release.root = LScr::I->Root();
  release.event = LScr::I->Root();
  release.child = XCB_NONE;
  release.root_x = 0;
  release.root_y = 0;
  DispatchXEvent((xcb_generic_event_t*)&release);
  ASSERT_FALSE(IsDragging());
}

TEST(UnhideMenuKeys, NoWindowsMeansNoMenu) {
  wmtest::World world;
  GrabNavigationKeys();
  world.server().SetMousePosition(640, 512, 0);
  world.server().ClearCalls();
  pressSuperTab();
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_FALSE(world.server().IsMapped(LScr::I->Menu()));
  // And with nothing on screen we haven't taken the keyboard off anyone.
  EXPECT_FALSE(world.server().DidCall("GrabKeyboard("));
  // Nor moved their pointer.
  EXPECT_EQ(mouseAt(), (Point{640, 512}));
}

TEST(UnhideMenuKeys, ARefusedKeyboardGrabMeansNoMenu) {
  // Without the keyboard there's no way to drive the menu and no way to close
  // it with Escape, so a menu we couldn't grab for would be a trap.
  wmtest::World world;
  GrabNavigationKeys();
  ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200)) !=
              nullptr);
  world.server().SetMousePosition(640, 512, 0);
  world.server().SetKeyboardGrabStatus(XCB_GRAB_STATUS_ALREADY_GRABBED);

  pressSuperTab();
  EXPECT_FALSE(LScr::I->GetHider()->KeyboardMenuIsOpen());
  EXPECT_FALSE(world.server().IsMapped(LScr::I->Menu()));
  EXPECT_EQ(mouseAt(), (Point{640, 512}));
}

TEST(UnhideMenuKeys, TheMenuAsksForMotionAndLeaveWhileItHasTheKeyboard) {
  // With no button held there is no button-motion to hear, so the pointer
  // moving over the menu and leaving it both have to be asked for outright -
  // and handed back afterwards, or lwm goes on being told about pointer
  // movement it has no use for.
  wmtest::World world;
  GrabNavigationKeys();
  ASSERT_TRUE(world.MapClientWindow(Rect::FromXYWH(50, 50, 200, 200)) !=
              nullptr);
  world.server().SetMousePosition(640, 512, 0);

  pressSuperTab();
  uint32_t mask = world.server().EventMaskOf(LScr::I->Menu());
  EXPECT_TRUE((mask & XCB_EVENT_MASK_POINTER_MOTION) != 0);
  EXPECT_TRUE((mask & XCB_EVENT_MASK_LEAVE_WINDOW) != 0);

  pressPlain(kEscapeKey);
  EXPECT_EQ(world.server().EventMaskOf(LScr::I->Menu()), kPopupEventMask);
}
