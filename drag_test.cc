// Tests for the Windows-key mouse gestures, from the ButtonPress event down
// to the window actually moving. gesture_test.cc covers the arithmetic these
// rest on; what's tested here is the wiring - that lwm asks for the clicks at
// all, picks the right handler for each button, and leaves everything else
// about a press on a client window alone.
//
// ui_test.sh does the same things against a real X server, where the passive
// grabs and the modifier state are the server's business rather than ours.

#include <algorithm>

#include "client.h"
#include "disp.h"
#include "drag.h"
#include "gesture.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

namespace {

// The client window's geometry, well clear of the screen edges so that the
// mover's edge resistance doesn't join in.
const Rect kClientRect = Rect::FromXYWH(300, 300, 300, 300);

// Timestamps, in milliseconds. Each test starts from its own base so that a
// press left over from an earlier one can't pair up with a later one: the
// double-click tracker is a static, and the fake server hands out the same
// window ids to every World.
constexpr uint32_t kSlowly = DoubleClickTracker::kIntervalMillis * 10;

xcb_button_press_event_t buttonEvent(uint8_t type,
                                     const Client* c,
                                     int button,
                                     uint16_t state,
                                     Point root,
                                     uint32_t time) {
  xcb_button_press_event_t e{};
  e.response_type = type;
  e.detail = button;
  e.time = time;
  e.root = LScr::I->Root();
  // The press arrives on the client's own window, which is where the grab in
  // Client::GrabSuperButtons is.
  e.event = c->window;
  e.child = XCB_NONE;
  e.root_x = root.x;
  e.root_y = root.y;
  e.event_x = root.x - c->ContentRect().xMin;
  e.event_y = root.y - c->ContentRect().yMin;
  e.state = state;
  return e;
}

// The state mask reported while button is held down.
uint16_t heldMask(int button) {
  switch (button) {
    case XCB_BUTTON_INDEX_2:
      return XCB_KEY_BUT_MASK_BUTTON_2;
    case XCB_BUTTON_INDEX_3:
      return XCB_KEY_BUT_MASK_BUTTON_3;
  }
  return XCB_KEY_BUT_MASK_BUTTON_1;
}

// Presses button at `from` with the given modifiers held, drags to `to`, and
// releases. Both points are root coordinates.
void drag(wmtest::World& world,
          const Client* c,
          int button,
          uint16_t modifiers,
          Point from,
          Point to,
          uint32_t time) {
  const uint16_t held = heldMask(button);
  world.server().SetMousePosition(from.x, from.y, modifiers);
  world.server().PushEvent(
      buttonEvent(XCB_BUTTON_PRESS, c, button, modifiers, from, time));
  ProcessPendingEvents();

  world.server().SetMousePosition(to.x, to.y, modifiers | held);
  xcb_motion_notify_event_t motion{};
  motion.response_type = XCB_MOTION_NOTIFY;
  motion.event = c->window;
  motion.root_x = to.x;
  motion.root_y = to.y;
  world.server().PushEvent(motion);
  ProcessPendingEvents();

  world.server().SetMousePosition(to.x, to.y, modifiers);
  world.server().PushEvent(
      buttonEvent(XCB_BUTTON_RELEASE, c, button, modifiers, to, time + 100));
  ProcessPendingEvents();
}

// Presses and releases button at p, without moving in between.
void click(wmtest::World& world,
           const Client* c,
           int button,
           uint16_t modifiers,
           Point p,
           uint32_t time) {
  world.server().SetMousePosition(p.x, p.y, modifiers);
  world.server().PushEvent(
      buttonEvent(XCB_BUTTON_PRESS, c, button, modifiers, p, time));
  ProcessPendingEvents();
  world.server().PushEvent(
      buttonEvent(XCB_BUTTON_RELEASE, c, button, modifiers, p, time + 10));
  ProcessPendingEvents();
}

void doubleClick(wmtest::World& world,
                 const Client* c,
                 int button,
                 uint16_t modifiers,
                 Point p,
                 uint32_t time) {
  click(world, c, button, modifiers, p, time);
  click(world, c, button, modifiers, p, time + 50);
}

// Where c's frame is in the root's stacking order, counting from the bottom.
// lwm's own popup and menu windows are in there too, so only the order of the
// frames relative to each other means anything.
int stackIndex(wmtest::World& world, const Client* c) {
  const std::vector<Window> stack =
      world.server().ChildrenOf(LScr::I->Root());
  const auto it = std::find(stack.begin(), stack.end(), c->parent);
  return it == stack.end() ? -1 : int(it - stack.begin());
}

// The middle of the given cell of the client area's 3x3 grid, in root
// coordinates. Column and row are 0, 1 or 2.
Point gridCell(const Client* c, int col, int row) {
  const Rect r = c->ContentRect();
  return Point{r.xMin + r.width() * (2 * col + 1) / 6,
               r.yMin + r.height() * (2 * row + 1) / 6};
}

}  // namespace

TEST(SuperGestures, ClicksOnTheClientWindowAreGrabbed) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);

  // All three gesture buttons, on the client's own window (not the frame),
  // under every combination of the two lock modifiers - X matches a passive
  // grab's modifiers exactly, so Num Lock would otherwise turn the feature
  // off.
  std::ostringstream prefix;
  prefix << "GrabButton(0x" << std::hex << c->window << ")";
  const std::vector<std::string> grabs =
      world.server().CallsMatching(prefix.str());
  for (int button :
       {SUPER_MOVE_BUTTON, SUPER_RESIZE_BUTTON, SUPER_HIDE_BUTTON}) {
    for (unsigned int locks : {0u, uint32_t(XCB_MOD_MASK_LOCK),
                               uint32_t(XCB_MOD_MASK_2),
                               uint32_t(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)}) {
      std::ostringstream want;
      want << prefix.str() << " button=" << button
           << " modifiers=" << (SUPER_MASK | locks);
      bool found = false;
      for (const std::string& got : grabs) {
        found = found || got == want.str();
      }
      EXPECT_TRUE(found) << "no grab matching '" << want.str() << "'";
    }
  }
}

TEST(SuperGestures, LeftDragMovesTheWindow) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  drag(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
       Point{gridCell(c, 1, 1).x + 40, gridCell(c, 1, 1).y + 25}, kSlowly);

  EXPECT_EQ(c->ContentRect(), Rect::Translate(before, Point{40, 25}))
      << "the window should have followed the mouse exactly";
}

TEST(SuperGestures, LeftDragMovesFromAnyPartOfTheWindow) {
  // Unlike the resize gesture, the 3x3 grid means nothing to a move: it's the
  // whole window that travels wherever the press started.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  const Point from = gridCell(c, 0, 0);
  drag(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, from,
       Point{from.x - 30, from.y + 10}, 2 * kSlowly);

  EXPECT_EQ(c->ContentRect(), Rect::Translate(before, Point{-30, 10}));
}

TEST(SuperGestures, MiddleDragResizesTheEdgeUnderThePointer) {
  struct TestCase {
    const char* name;
    int col;
    int row;
    Point by;
    Rect want_delta;  // How each of the four edges should move.
  };
  const TestCase cases[] = {
      {"right edge", 2, 1, Point{50, 30}, Rect{0, 0, 50, 0}},
      {"left edge", 0, 1, Point{-50, 30}, Rect{-50, 0, 0, 0}},
      {"top edge", 1, 0, Point{50, -30}, Rect{0, -30, 0, 0}},
      {"bottom edge", 1, 2, Point{50, 30}, Rect{0, 0, 0, 30}},
      {"top left corner", 0, 0, Point{-50, -30}, Rect{-50, -30, 0, 0}},
      {"bottom right corner", 2, 2, Point{50, 30}, Rect{0, 0, 50, 30}},
      {"top right corner", 2, 0, Point{50, -30}, Rect{0, -30, 50, 0}},
      {"bottom left corner", 0, 2, Point{-50, 30}, Rect{-50, 0, 0, 30}},
  };
  uint32_t time = 3 * kSlowly;
  for (const TestCase& tc : cases) {
    testing::Context ctx(tc.name);
    wmtest::World world;
    Client* c = world.MapClientWindow(kClientRect);
    ASSERT_TRUE(c != nullptr);
    const Rect before = c->ContentRect();

    const Point from = gridCell(c, tc.col, tc.row);
    drag(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, from,
         Point{from.x + tc.by.x, from.y + tc.by.y}, time);
    time += kSlowly;

    const Rect want{before.xMin + tc.want_delta.xMin,
                    before.yMin + tc.want_delta.yMin,
                    before.xMax + tc.want_delta.xMax,
                    before.yMax + tc.want_delta.yMax};
    EXPECT_EQ(c->ContentRect(), want);
  }
}

TEST(SuperGestures, MiddleDragInTheCentreCellDoesNothing) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  const Point from = gridCell(c, 1, 1);
  drag(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, from,
       Point{from.x + 50, from.y + 30}, 12 * kSlowly);

  EXPECT_EQ(c->ContentRect(), before)
      << "the centre square of the grid is not an edge to resize";
}

TEST(SuperGestures, LeftDoubleClickInTheMiddleFillsTheScreen) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              13 * kSlowly);

  EXPECT_EQ(c->FrameRect(), LScr::I->GetPrimaryVisibleArea(true))
      << "with nothing else on screen, every edge should reach the monitor";
}

TEST(SuperGestures, LeftDoubleClickOnlyExpandsTheEdgeItPicked) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();
  const Rect screen = LScr::I->GetPrimaryVisibleArea(true);

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 2, 1),
              14 * kSlowly);

  EXPECT_EQ(c->FrameRect(),
            (Rect{before.xMin, before.yMin, screen.xMax, before.yMax}));
}

TEST(SuperGestures, LeftDoubleClickStopsAtAnotherWindow) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  // A second window to the right of the first, overlapping it vertically.
  Client* blocker = world.MapClientWindow(Rect::FromXYWH(800, 350, 200, 200));
  ASSERT_TRUE(blocker != nullptr);
  const Rect before = c->FrameRect();

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 2, 1),
              15 * kSlowly);

  EXPECT_EQ(c->FrameRect(), (Rect{before.xMin, before.yMin,
                                  blocker->FrameRect().xMin, before.yMax}));
}

TEST(SuperGestures, MiddleDoubleClickIgnoresOtherWindows) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  Client* blocker = world.MapClientWindow(Rect::FromXYWH(800, 350, 200, 200));
  ASSERT_TRUE(blocker != nullptr);
  const Rect before = c->FrameRect();
  const Rect screen = LScr::I->GetPrimaryVisibleArea(true);

  doubleClick(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, gridCell(c, 2, 1),
              16 * kSlowly);

  EXPECT_EQ(c->FrameRect(),
            (Rect{before.xMin, before.yMin, screen.xMax, before.yMax}))
      << "button 2's double click expands to the monitor regardless";
}

TEST(SuperGestures, MiddleDoubleClickInTheCentreCellFillsTheScreen) {
  // The centre square resizes nothing on a drag, but expands everything on a
  // double click.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);

  doubleClick(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              17 * kSlowly);

  EXPECT_EQ(c->FrameRect(), LScr::I->GetPrimaryVisibleArea(true));
}

TEST(SuperGestures, ExpansionStaysOnOneMonitor) {
  wmtest::World world;
  // Two monitors side by side, as xrandr would report them.
  LScr::I->SetVisibleAreas({Rect::FromXYWH(0, 0, 640, 1024),
                            Rect::FromXYWH(640, 0, 640, 1024)});
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 300, 200, 200));
  ASSERT_TRUE(c != nullptr);

  doubleClick(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              18 * kSlowly);

  EXPECT_EQ(c->FrameRect(), Rect::FromXYWH(0, 0, 640, 1024))
      << "expanding must stop at the edge of the monitor the window is on";
}

TEST(SuperGestures, TwoSlowClicksAreNotADoubleClick) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();

  const Point p = gridCell(c, 1, 1);
  click(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, p, 19 * kSlowly);
  click(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, p,
        19 * kSlowly + DoubleClickTracker::kIntervalMillis + 100);

  EXPECT_EQ(c->FrameRect(), before);
}

TEST(SuperGestures, LeftClickRaisesTheWindow) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  // Mapped second, so it starts above the first.
  Client* above = world.MapClientWindow(Rect::FromXYWH(800, 350, 200, 200));
  ASSERT_TRUE(above != nullptr);
  ASSERT_TRUE(stackIndex(world, c) < stackIndex(world, above));
  const Rect before = c->ContentRect();

  click(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
        34 * kSlowly);

  EXPECT_TRUE(stackIndex(world, c) > stackIndex(world, above))
      << "a press that went nowhere should have raised the window";
  EXPECT_EQ(c->ContentRect(), before) << "and not moved it";
}

TEST(SuperGestures, LeftDragDoesNotRaiseTheWindow) {
  // Moving a window without bringing it to the front is deliberate: it's what
  // the middle button on the frame is for, and the whole point of these
  // gestures is that the window behaves like its own furniture.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  Client* above = world.MapClientWindow(Rect::FromXYWH(800, 350, 200, 200));
  ASSERT_TRUE(above != nullptr);
  const int before = stackIndex(world, c);

  const Point from = gridCell(c, 1, 1);
  drag(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, from,
       Point{from.x + 40, from.y + 25}, 35 * kSlowly);

  EXPECT_TRUE(stackIndex(world, c) < stackIndex(world, above));
  EXPECT_EQ(stackIndex(world, c), before);
}

TEST(SuperGestures, RightClickHidesTheWindow) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  click(world, c, SUPER_HIDE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
        23 * kSlowly);

  EXPECT_TRUE(c->hidden) << "button 3 should hide, as it does on the frame";
  EXPECT_EQ(c->ContentRect(), before)
      << "hiding a window shouldn't move or resize it";
}

TEST(SuperGestures, RightClickHidesFromAnyCellOfTheGrid) {
  // The 3x3 grid picks an edge for the resize and expand gestures; hiding
  // has no edge to pick, so it happens wherever the click lands.
  uint32_t time = 24 * kSlowly;
  for (int col = 0; col < 3; col++) {
    for (int row = 0; row < 3; row++) {
      testing::Context ctx(std::to_string(col) + "," + std::to_string(row));
      wmtest::World world;
      Client* c = world.MapClientWindow(kClientRect);
      ASSERT_TRUE(c != nullptr);

      click(world, c, SUPER_HIDE_BUTTON, SUPER_MASK, gridCell(c, col, row),
            time);
      time += kSlowly;

      EXPECT_TRUE(c->hidden);
    }
  }
}

TEST(SuperGestures, RightDragDoesNotHideTheWindow) {
  // WindowHider only acts if the pointer stayed where it was pressed, so a
  // press the user thought better of and dragged away from does nothing at
  // all - and, unlike button 2, it doesn't resize on the way either.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  const Point from = gridCell(c, 2, 1);
  drag(world, c, SUPER_HIDE_BUTTON, SUPER_MASK, from,
       Point{from.x + 50, from.y + 30}, 33 * kSlowly);

  EXPECT_FALSE(c->hidden);
  EXPECT_EQ(c->ContentRect(), before);
}

TEST(SuperGestures, WithoutTheWindowsKeyNothingHappens) {
  // Click-to-focus mode grabs the client window too, so a press on it can
  // reach lwm with no modifiers at all. It must still be nothing but a click
  // as far as the window's geometry is concerned.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  const Point from = gridCell(c, 2, 1);
  drag(world, c, SUPER_MOVE_BUTTON, 0, from, Point{from.x + 40, from.y + 25},
       20 * kSlowly);
  drag(world, c, SUPER_RESIZE_BUTTON, 0, from, Point{from.x + 40, from.y + 25},
       21 * kSlowly);
  doubleClick(world, c, SUPER_MOVE_BUTTON, 0, from, 22 * kSlowly);
  click(world, c, SUPER_HIDE_BUTTON, 0, from, 22 * kSlowly + 500);

  EXPECT_EQ(c->ContentRect(), before);
  EXPECT_FALSE(c->hidden);
}

TEST(SuperGestures, DraggingAMaximizedWindowUnmaximizesIt) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  c->SetMaximized(true, true);
  ASSERT_TRUE(c->IsMaximized());
  const Rect maximized = c->ContentRect();

  const Point from = gridCell(c, 1, 1);
  drag(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, from,
       Point{from.x + 40, from.y + 25}, kSlowly);

  EXPECT_FALSE(c->IsMaximized())
      << "a window the user has dragged isn't maximised any more";
  // Only the size is asserted, not the position: a maximised window is flush
  // with the screen edges, so WindowMover's edge resistance has opinions about
  // how far it actually travels. What matters here is that dropping the state
  // leaves the window the size it was rather than restoring it.
  EXPECT_EQ(c->ContentRect().area(), maximized.area());
}
