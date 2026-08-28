// Tests for the Windows-key arrow gestures, from the KeyPress event down to
// the focus (or the window) actually moving. navigate_test.cc covers the
// geometry these rest on; what's tested here is the wiring - that lwm asks
// for the right keys, and that a press does the thing the geometry says it
// should to the window it says it should.

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

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

// Sends a Super+Shift+<arrow> press: the same gesture, but moving the window
// rather than the focus.
void pressShiftArrow(uint8_t keycode) {
  xcb_key_press_event_t e =
      keyEvent(XCB_KEY_PRESS, keycode, SUPER_MASK | XCB_MOD_MASK_SHIFT);
  DispatchXEvent((xcb_generic_event_t*)&e);
}

// Where c's frame is in the root's stacking order, counting from the bottom.
// lwm's own popup and menu windows are in there too, so only the order of the
// frames relative to each other means anything.
int stackIndex(wmtest::World& world, const Client* c) {
  const std::vector<Window> stack = world.server().ChildrenOf(LScr::I->Root());
  const auto it = std::find(stack.begin(), stack.end(), c->parent);
  return it == stack.end() ? -1 : int(it - stack.begin());
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

TEST(GrabNavigationKeys, AsksForTheShiftedArrowsToo) {
  wmtest::World world;
  world.server().ClearCalls();
  GrabNavigationKeys();
  for (uint8_t keycode : {kLeftKey, kRightKey, kUpKey, kDownKey}) {
    testing::Context ctx(std::to_string(keycode));
    EXPECT_TRUE(
        sawCall(world, grabCall(keycode, SUPER_MASK | XCB_MOD_MASK_SHIFT)));
  }
}

TEST(HandleKeyPress, ShiftMovesTheWindowToTheEdgeOfItsMonitor) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(400, 400, 200, 200));
  ASSERT_TRUE(c != nullptr);
  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(c);
  const Rect screen = LScr::I->GetPrimaryVisibleArea(true);
  const Area size = c->FrameRect().area();
  const int yMin = c->FrameRect().yMin;

  pressShiftArrow(kLeftKey);
  EXPECT_EQ(c->FrameRect().xMin, screen.xMin);
  EXPECT_EQ(c->FrameRect().yMin, yMin) << "moving left mustn't move it up";
  EXPECT_EQ(c->FrameRect().area(), size) << "a move is not a resize";
  EXPECT_EQ(focuser->GetFocusedClient(), c)
      << "moving a window mustn't move the focus off it";

  pressShiftArrow(kDownKey);
  EXPECT_EQ(c->FrameRect().yMax, screen.yMax);
  EXPECT_EQ(c->FrameRect().xMin, screen.xMin);
}

TEST(HandleKeyPress, ShiftMovesTheWindowAndNotTheOtherOne) {
  // The gesture acts on the focused window; nothing else on screen moves.
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(400, 400, 200, 200));
  Client* other = world.MapClientWindow(Rect::FromXYWH(700, 400, 200, 200));
  ASSERT_TRUE(c != nullptr);
  ASSERT_TRUE(other != nullptr);
  LScr::I->GetFocuser()->FocusClient(c);
  const Rect before = other->FrameRect();

  pressShiftArrow(kLeftKey);
  EXPECT_EQ(other->FrameRect(), before);
}

TEST(HandleKeyPress, ShiftAtTheEdgeHandsTheWindowToTheNextMonitor) {
  wmtest::World world;
  // Two monitors side by side, as xrandr would report them.
  LScr::I->SetVisibleAreas({Rect::FromXYWH(0, 0, 640, 1024),
                            Rect::FromXYWH(640, 0, 640, 1024)});
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 300, 200, 200));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetFocuser()->FocusClient(c);

  pressShiftArrow(kRightKey);
  EXPECT_EQ(c->FrameRect().xMax, 640) << "first press stops at the monitor's "
                                         "edge";
  pressShiftArrow(kRightKey);
  EXPECT_EQ(c->FrameRect().xMin, 640) << "second press crosses the join, and "
                                         "stops the other side of it";
  pressShiftArrow(kRightKey);
  EXPECT_EQ(c->FrameRect().xMax, 1280) << "third press crosses the new "
                                          "monitor";
  pressShiftArrow(kRightKey);
  EXPECT_EQ(c->FrameRect().xMax, 1280) << "and there it stays: no third "
                                          "monitor to go to";
}

TEST(HandleKeyPress, ShiftOntoASmallerMonitorShrinksTheWindow) {
  wmtest::World world;
  // A big monitor with a small one beside it.
  LScr::I->SetVisibleAreas({Rect::FromXYWH(0, 0, 900, 1000),
                            Rect::FromXYWH(900, 0, 300, 300)});
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(20, 20, 700, 600));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetFocuser()->FocusClient(c);

  pressShiftArrow(kRightKey);  // To the big monitor's right edge.
  ASSERT_EQ(c->FrameRect().xMax, 900);
  pressShiftArrow(kRightKey);  // And across to the small one.

  const Rect got = c->FrameRect();
  EXPECT_TRUE(got.width() <= 300) << "too wide for the monitor: " << got;
  EXPECT_TRUE(got.height() <= 300) << "too tall for the monitor: " << got;
  EXPECT_EQ(got.xMax, 1200);
  EXPECT_EQ(got.yMin, 0);
}

TEST(HandleKeyPress, ShiftWithNothingFocusedIsNotACrash) {
  wmtest::World world;
  GrabNavigationKeys();
  ASSERT_EQ(LScr::I->GetFocuser()->GetFocusedClient(), nullptr);
  xcb_key_press_event_t e = keyEvent(XCB_KEY_PRESS, kLeftKey,
                                     SUPER_MASK | XCB_MOD_MASK_SHIFT);
  EXPECT_TRUE(HandleKeyPress(&e));
}

TEST(HandleKeyPress, ThePointerGoesWithTheWindow) {
  // Without this the window slides out from under the pointer, and the
  // EnterNotify on whatever was behind it takes the focus with it.
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(400, 400, 200, 200));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetFocuser()->FocusClient(c);

  const Rect before = c->FrameRect();
  const Point p{before.xMin + before.width() / 4,
                before.yMin + before.height() / 2};
  world.server().SetMousePosition(p.x, p.y, 0);

  pressShiftArrow(kLeftKey);

  const Rect after = c->FrameRect();
  const MousePos mp = getMousePosition();
  EXPECT_TRUE(after.contains(mp.x, mp.y)) << "pointer at " << mp.x << ","
                                          << mp.y << " left " << after;
  // A plain move, so the pointer keeps its exact place on the window.
  EXPECT_EQ(mp.x - after.xMin, p.x - before.xMin);
  EXPECT_EQ(mp.y - after.yMin, p.y - before.yMin);
}

TEST(HandleKeyPress, ThePointerKeepsItsProportionalPlaceOnAShrunkWindow) {
  wmtest::World world;
  LScr::I->SetVisibleAreas({Rect::FromXYWH(0, 0, 900, 1000),
                            Rect::FromXYWH(900, 0, 300, 300)});
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(20, 20, 700, 600));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetFocuser()->FocusClient(c);

  const Rect before = c->FrameRect();
  // Three quarters of the way across, one quarter of the way down.
  const Point p{before.xMin + before.width() * 3 / 4,
                before.yMin + before.height() / 4};
  world.server().SetMousePosition(p.x, p.y, 0);

  pressShiftArrow(kRightKey);  // To the big monitor's edge.
  pressShiftArrow(kRightKey);  // Across to the small one, shrinking to fit.

  const Rect after = c->FrameRect();
  ASSERT_TRUE(after.width() < before.width());
  const MousePos mp = getMousePosition();
  EXPECT_TRUE(after.contains(mp.x, mp.y)) << "pointer at " << mp.x << ","
                                          << mp.y << " left " << after;
  // Percentages, because the pixel it lands on depends on the rounding.
  EXPECT_NEAR((mp.x - after.xMin) * 100 / after.width(), 75, 2);
  EXPECT_NEAR((mp.y - after.yMin) * 100 / after.height(), 25, 2);
}

TEST(HandleKeyPress, APointerElsewhereIsLeftAlone) {
  // Nothing has moved out from under it, so there's nothing to fix - and
  // dragging the pointer across the screen after a window the user isn't
  // pointing at would be worse than leaving it.
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(400, 400, 200, 200));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetFocuser()->FocusClient(c);
  world.server().SetMousePosition(5, 5, 0);
  world.server().ClearCalls();

  pressShiftArrow(kLeftKey);

  const MousePos mp = getMousePosition();
  EXPECT_EQ(mp.x, 5);
  EXPECT_EQ(mp.y, 5);
  EXPECT_TRUE(world.server().CallsMatching("WarpPointer(").empty());
}

TEST(HandleKeyPress, TheMovedWindowComesToTheFront) {
  // The pointer is about to be put down inside the window's new position, so
  // anything in front of it there would take the crossing event, and the
  // focus with it.
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(400, 400, 200, 200));
  ASSERT_TRUE(c != nullptr);
  // Mapped second, so it starts above the first.
  Client* above = world.MapClientWindow(Rect::FromXYWH(100, 400, 200, 200));
  ASSERT_TRUE(above != nullptr);
  ASSERT_TRUE(stackIndex(world, c) < stackIndex(world, above));

  Focuser* focuser = LScr::I->GetFocuser();
  focuser->FocusClient(c);
  pressShiftArrow(kLeftKey);

  EXPECT_TRUE(stackIndex(world, c) > stackIndex(world, above))
      << "the window being moved should have been raised";
  EXPECT_EQ(focuser->GetFocusedClient(), c);
}

TEST(HandleKeyPress, ThePointerFollowsTheFocus) {
  // Without this the focus is somewhere the pointer isn't, and with sloppy
  // focus the next twitch of the mouse hands it straight back.
  wmtest::World world;
  GrabNavigationKeys();
  Client* left = world.MapClientWindow(Rect::FromXYWH(50, 400, 200, 200));
  Client* right = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(left != nullptr);
  ASSERT_TRUE(right != nullptr);
  LScr::I->GetFocuser()->FocusClient(right);
  world.server().SetMousePosition(1000, 500, 0);

  pressArrow(kLeftKey);
  ASSERT_EQ(LScr::I->GetFocuser()->GetFocusedClient(), left);
  // Nothing is in the way, so it's the middle of the whole window.
  const MousePos mp = getMousePosition();
  EXPECT_EQ((Point{mp.x, mp.y}), left->FrameRect().middle());
}

TEST(HandleKeyPress, TheWindowNavigatedToComesToTheFront) {
  // The focus is no use on a window buried under another one, and the pointer
  // is about to be put down on this one.
  wmtest::World world;
  GrabNavigationKeys();
  Client* target = world.MapClientWindow(Rect::FromXYWH(50, 400, 200, 200));
  // Overlapping it, and mapped second so it starts in front of it. It sits
  // further from the window the focus starts on, so it isn't the one the
  // arrow picks.
  Client* over = world.MapClientWindow(Rect::FromXYWH(0, 450, 200, 200));
  Client* focused = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(target != nullptr);
  ASSERT_TRUE(over != nullptr);
  ASSERT_TRUE(focused != nullptr);
  ASSERT_TRUE(stackIndex(world, target) < stackIndex(world, over));
  ASSERT_FALSE(Rect::Intersect(target->FrameRect(), over->FrameRect()).empty());
  LScr::I->GetFocuser()->FocusClient(focused);

  pressArrow(kLeftKey);
  ASSERT_EQ(LScr::I->GetFocuser()->GetFocusedClient(), target);
  EXPECT_TRUE(stackIndex(world, target) > stackIndex(world, over))
      << "the window navigated to should have been raised";
  // And so the whole of it is there for the pointer to land in the middle of.
  const MousePos mp = getMousePosition();
  EXPECT_EQ((Point{mp.x, mp.y}), target->FrameRect().middle());
}

TEST(HandleKeyPress, ThePointerAvoidsATransientInFront) {
  // A raise doesn't get a window out from under its own dialogs: Client::Raise
  // takes those up with it and leaves them in front, which is what they're
  // for. So the middle of the window may still be covered, and the pointer
  // landing there would give the dialog the focus that was just moved off it.
  wmtest::World world;
  GrabNavigationKeys();
  Client* target = world.MapClientWindow(Rect::FromXYWH(50, 400, 400, 400));
  // A dialog over the lower half of it, mapped second so it starts in front.
  // It's far enough down the screen not to be the nearer of the two to the
  // window the focus starts on.
  Client* dialog = world.MapClientWindow(Rect::FromXYWH(50, 620, 400, 380));
  Client* focused = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(target != nullptr);
  ASSERT_TRUE(dialog != nullptr);
  ASSERT_TRUE(focused != nullptr);
  dialog->trans = target->window;
  ASSERT_FALSE(
      Rect::Intersect(target->FrameRect(), dialog->FrameRect()).empty());
  LScr::I->GetFocuser()->FocusClient(focused);

  pressArrow(kLeftKey);
  ASSERT_EQ(LScr::I->GetFocuser()->GetFocusedClient(), target);
  ASSERT_TRUE(stackIndex(world, target) < stackIndex(world, dialog))
      << "the dialog should have been raised along with its parent";

  const MousePos mp = getMousePosition();
  EXPECT_TRUE(target->FrameRect().contains(mp.x, mp.y))
      << "pointer at " << mp.x << "," << mp.y << " left " << target->FrameRect();
  EXPECT_FALSE(dialog->FrameRect().contains(mp.x, mp.y))
      << "pointer at " << mp.x << "," << mp.y << " landed on the dialog in "
      << "front, at " << dialog->FrameRect();
}

TEST(HandleKeyPress, ACompletelyCoveredWindowLeavesThePointerAlone) {
  // There is nowhere on this window to put the pointer that isn't on the
  // dialog in front of it, and putting it there would undo the gesture.
  // Better to move the focus alone than to move the focus and then lose it.
  wmtest::World world;
  GrabNavigationKeys();
  const Rect where = Rect::FromXYWH(50, 400, 200, 200);
  Client* target = world.MapClientWindow(where);
  // Exactly on top of it, and a transient of it, so the raise takes it along
  // rather than getting the window out from under it.
  Client* dialog = world.MapClientWindow(where);
  Client* focused = world.MapClientWindow(Rect::FromXYWH(900, 400, 200, 200));
  ASSERT_TRUE(target != nullptr);
  ASSERT_TRUE(dialog != nullptr);
  ASSERT_TRUE(focused != nullptr);
  dialog->trans = target->window;
  ASSERT_EQ(target->FrameRect(), dialog->FrameRect());
  LScr::I->GetFocuser()->FocusClient(focused);
  world.server().SetMousePosition(1000, 500, 0);
  world.server().ClearCalls();

  pressArrow(kLeftKey);
  // The two are the same distance away, so the tie goes to the first of them;
  // if that ever changes, this test is no longer testing what it says.
  ASSERT_EQ(LScr::I->GetFocuser()->GetFocusedClient(), target);
  ASSERT_TRUE(stackIndex(world, target) < stackIndex(world, dialog));
  const MousePos mp = getMousePosition();
  EXPECT_EQ((Point{mp.x, mp.y}), (Point{1000, 500}));
  EXPECT_TRUE(world.server().CallsMatching("WarpPointer(").empty());
}

TEST(HandleKeyPress, NoWindowThatWayLeavesThePointerAlone) {
  wmtest::World world;
  GrabNavigationKeys();
  Client* c = world.MapClientWindow(Rect::FromXYWH(50, 400, 200, 200));
  ASSERT_TRUE(c != nullptr);
  LScr::I->GetFocuser()->FocusClient(c);
  world.server().SetMousePosition(700, 700, 0);

  pressArrow(kRightKey);
  const MousePos mp = getMousePosition();
  EXPECT_EQ((Point{mp.x, mp.y}), (Point{700, 700}));
}
