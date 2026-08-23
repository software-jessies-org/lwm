// Tests for Windows-key focus navigation, from the KeyPress event down to the
// focus actually moving. navigate_test.cc covers the geometry these rest on;
// what's tested here is the wiring - that lwm asks for the right keys, and
// that a press picks the window the geometry says it should and focuses it.

#include <sstream>

#include "client.h"
#include "disp.h"
#include "focus.h"
#include "keyboard.h"
#include "lwm.h"
#include "navigate.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

namespace {

// The keycodes the fake keyboard puts the arrows on; see
// FakeServer::GetKeyboardMapping.
constexpr uint8_t kLeftKey = 113;
constexpr uint8_t kUpKey = 111;
constexpr uint8_t kRightKey = 114;
constexpr uint8_t kDownKey = 116;

xcb_key_press_event_t keyEvent(uint8_t type, uint8_t keycode, uint16_t state) {
  xcb_key_press_event_t e{};
  e.response_type = type;
  e.detail = keycode;
  e.time = 1000;
  e.root = LScr::I->Root();
  // A key grab on the root reports the press against the root, whatever the
  // pointer happens to be over.
  e.event = LScr::I->Root();
  e.child = XCB_NONE;
  e.state = state;
  return e;
}

// Sends a Super+<arrow> press through the dispatcher, the way the event loop
// would.
void pressArrow(uint8_t keycode) {
  xcb_key_press_event_t e = keyEvent(XCB_KEY_PRESS, keycode, SUPER_MASK);
  DispatchXEvent((xcb_generic_event_t*)&e);
}

std::string grabCall(uint8_t keycode, unsigned int modifiers) {
  std::ostringstream s;
  s << "GrabKey(0x" << std::hex << LScr::I->Root() << std::dec
    << ") keycode=" << int(keycode) << " modifiers=" << modifiers;
  return s.str();
}

bool sawCall(wmtest::World& world, const std::string& want) {
  for (const std::string& call : world.server().Calls()) {
    if (call == want) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(GrabNavigationKeys, AsksForAllFourArrowsOnTheRoot) {
  wmtest::World world;
  world.server().ClearCalls();
  GrabNavigationKeys();
  for (uint8_t keycode : {kLeftKey, kRightKey, kUpKey, kDownKey}) {
    testing::Context ctx(std::to_string(keycode));
    EXPECT_TRUE(sawCall(world, grabCall(keycode, SUPER_MASK)));
  }
}

TEST(GrabNavigationKeys, RepeatsEveryGrabUnderBothLocks) {
  // Without this, the whole feature stops working the moment Num Lock is on.
  wmtest::World world;
  world.server().ClearCalls();
  GrabNavigationKeys();
  const unsigned int locks[] = {
      0,
      XCB_MOD_MASK_LOCK,
      XCB_MOD_MASK_2,
      XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
  };
  for (unsigned int lock : locks) {
    testing::Context ctx(std::to_string(lock));
    EXPECT_TRUE(sawCall(world, grabCall(kLeftKey, SUPER_MASK | lock)));
  }
}

TEST(GrabNavigationKeys, ReleasesTheOldGrabsBeforeTakingNewOnes) {
  // What makes this the right response to a MappingNotify: if the re-grab
  // didn't release first, a layout switch would leave the old keycodes
  // grabbed for ever, swallowing keys lwm no longer has any use for.
  wmtest::World world;
  GrabNavigationKeys();
  world.server().ClearCalls();
  GrabNavigationKeys();
  EXPECT_FALSE(world.server().CallsMatching("UngrabKey(").empty());
  EXPECT_FALSE(world.server().CallsMatching("GrabKey(").empty());
}

TEST(HandleKeyPress, MovesFocusToTheWindowThatWay) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* left = world.MapClientWindow(Rect::FromXYWH(50, 400, 200, 200));
  Client* right = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(left != nullptr);
  ASSERT_TRUE(right != nullptr);

  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(right);
  ASSERT_EQ(focuser->GetFocusedClient(), right);

  pressArrow(kLeftKey);
  EXPECT_EQ(focuser->GetFocusedClient(), left);

  // And back again.
  pressArrow(kRightKey);
  EXPECT_EQ(focuser->GetFocusedClient(), right);
}

TEST(HandleKeyPress, NoWindowThatWayLeavesFocusAlone) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* left = world.MapClientWindow(Rect::FromXYWH(50, 400, 200, 200));
  Client* right = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(left != nullptr);
  ASSERT_TRUE(right != nullptr);

  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(left);
  ASSERT_EQ(focuser->GetFocusedClient(), left);

  // Nothing to the left of the leftmost window, and nothing above either.
  pressArrow(kLeftKey);
  EXPECT_EQ(focuser->GetFocusedClient(), left);
  pressArrow(kUpKey);
  EXPECT_EQ(focuser->GetFocusedClient(), left);
}

TEST(HandleKeyPress, HiddenWindowsAreNotCandidates) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* left = world.MapClientWindow(Rect::FromXYWH(50, 400, 200, 200));
  Client* right = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(left != nullptr);
  ASSERT_TRUE(right != nullptr);

  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(right);
  LScr::I->GetHider()->Hide(left);
  ASSERT_TRUE(left->IsHidden());
  ASSERT_EQ(focuser->GetFocusedClient(), right);

  // The only window to the left is hidden, so there's nowhere to go.
  pressArrow(kLeftKey);
  EXPECT_EQ(focuser->GetFocusedClient(), right);
}

TEST(HandleKeyPress, PicksTheNearestOfSeveral) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* far_left = world.MapClientWindow(Rect::FromXYWH(0, 400, 150, 150));
  Client* near_left = world.MapClientWindow(Rect::FromXYWH(400, 400, 150, 150));
  Client* focused = world.MapClientWindow(Rect::FromXYWH(900, 400, 150, 150));
  ASSERT_TRUE(far_left != nullptr);
  ASSERT_TRUE(near_left != nullptr);
  ASSERT_TRUE(focused != nullptr);

  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(focused);
  pressArrow(kLeftKey);
  EXPECT_EQ(focuser->GetFocusedClient(), near_left);
  // A second press carries on past it, rather than sticking.
  pressArrow(kLeftKey);
  EXPECT_EQ(focuser->GetFocusedClient(), far_left);
}

TEST(HandleKeyPress, VerticalNavigationUsesTheVerticalCone) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* above = world.MapClientWindow(Rect::FromXYWH(500, 50, 200, 150));
  Client* below = world.MapClientWindow(Rect::FromXYWH(500, 750, 200, 150));
  Client* middle = world.MapClientWindow(Rect::FromXYWH(500, 400, 200, 150));
  ASSERT_TRUE(above != nullptr);
  ASSERT_TRUE(below != nullptr);
  ASSERT_TRUE(middle != nullptr);

  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(middle);
  pressArrow(kUpKey);
  EXPECT_EQ(focuser->GetFocusedClient(), above);
  pressArrow(kDownKey);
  EXPECT_EQ(focuser->GetFocusedClient(), middle);
  pressArrow(kDownKey);
  EXPECT_EQ(focuser->GetFocusedClient(), below);
}

TEST(HandleKeyPress, AKeyWeNeverGrabbedIsNotOurs) {
  wmtest::World world;
  GrabNavigationKeys();
  xcb_key_press_event_t e = keyEvent(XCB_KEY_PRESS, 38 /* 'a' */, SUPER_MASK);
  EXPECT_FALSE(HandleKeyPress(&e));
}

TEST(HandleKeyPress, NothingFocusedIsNotACrash) {
  wmtest::World world;
  GrabNavigationKeys();
  // No clients at all, so nothing has focus and there is no window for
  // "left of" to be measured from.
  ASSERT_EQ(LScr::I->GetFocuser()->GetFocusedClient(), nullptr);
  xcb_key_press_event_t e = keyEvent(XCB_KEY_PRESS, kLeftKey, SUPER_MASK);
  EXPECT_TRUE(HandleKeyPress(&e));
  EXPECT_EQ(LScr::I->GetFocuser()->GetFocusedClient(), nullptr);
}
