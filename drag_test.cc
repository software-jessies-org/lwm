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
#include "ewmh.h"
#include "gesture.h"
#include "lwm.h"
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

// Maps a client which says through _MOTIF_WM_HINTS that it draws its own
// decorations, so lwm leaves it parented to the root with no frame at all -
// the Steam launcher case. `type` is the _NET_WM_WINDOW_TYPE to advertise.
Client* mapUndecorated(wmtest::World& world, const Rect& rect, Atom type) {
  const Window w = world.server().AddClientWindow(rect);
  world.server().SetProperty32(w, motif_wm_hints, motif_wm_hints,
                               {1 << 1 /* MWM_HINTS_DECORATIONS */, 0, 0, 0,
                                0});
  if (type) {
    world.server().SetProperty32(w, ewmh_atom[_NET_WM_WINDOW_TYPE],
                                 XCB_ATOM_ATOM, {type});
  }
  xcb_map_request_event_t e{};
  e.response_type = XCB_MAP_REQUEST;
  e.parent = world.server().Root();
  e.window = w;
  world.server().PushEvent(e);
  ProcessPendingEvents();
  return LScr::I->GetClient(w, false);
}

Client* mapUndecorated(wmtest::World& world, const Rect& rect) {
  return mapUndecorated(world, rect, 0);
}

// Maps a client which resizes in whole cells, the way an xterm does. Both
// halves of the decoration toggle put the size through LimitResize, so this
// is the client that finds out whether the two halves agree.
Client* mapWithResizeIncrement(wmtest::World& world,
                               const Rect& rect,
                               int width_inc,
                               int height_inc) {
  const Window w = world.server().AddClientWindow(rect);
  xlib::FakeWindow* fw = world.server().Get(w);
  fw->normal_hints.ok = true;
  fw->normal_hints.has_resize_inc = true;
  fw->normal_hints.width_inc = width_inc;
  fw->normal_hints.height_inc = height_inc;
  xcb_map_request_event_t e{};
  e.response_type = XCB_MAP_REQUEST;
  e.parent = world.server().Root();
  e.window = w;
  world.server().PushEvent(e);
  ProcessPendingEvents();
  return LScr::I->GetClient(w, false);
}

// Super+Control+click, the decoration toggle, in the middle of the window.
void superControlClick(wmtest::World& world,
                       const Client* c,
                       int button,
                       uint32_t time) {
  click(world, c, button, SUPER_MASK | SUPER_CTRL_MASK, gridCell(c, 1, 1),
        time);
}

// Delivers the UnmapNotify a real server generates when it unmaps a client
// window on its way through a reparent (or when lwm unmaps it directly).
// FakeServer models neither, so a test that wants to know what lwm does with
// the event has to hand it over itself.
void sendUnmapNotify(wmtest::World& world, Window reported_by, Window w) {
  xcb_unmap_notify_event_t e{};
  e.response_type = XCB_UNMAP_NOTIFY;
  e.event = reported_by;
  e.window = w;
  world.server().PushEvent(e);
  ProcessPendingEvents();
}

// Where w sits in the root's stacking order, counting from the bottom, or -1
// if it isn't there. Unlike stackIndex() above this takes a window rather
// than a client, because the window that stands for a client in the stack
// changes when its frame does.
int rootStackIndex(wmtest::World& world, Window w) {
  const std::vector<Window> stack = world.server().ChildrenOf(LScr::I->Root());
  const auto it = std::find(stack.begin(), stack.end(), w);
  return it == stack.end() ? -1 : int(it - stack.begin());
}

// Sends the _NET_WM_MOVERESIZE a client sends when the user grabs its own
// title bar or resize grip.
void sendMoveResize(wmtest::World& world,
                    const Client* c,
                    EWMHDirection direction,
                    int button) {
  xcb_client_message_event_t e{};
  e.response_type = XCB_CLIENT_MESSAGE;
  e.format = 32;
  e.window = c->window;
  e.type = ewmh_atom[_NET_WM_MOVERESIZE];
  e.data.data32[0] = 0;  // x_root, ignored: we ask the server instead.
  e.data.data32[1] = 0;  // y_root, ditto.
  e.data.data32[2] = direction;
  e.data.data32[3] = button;
  e.data.data32[4] = 1;  // Source indication: a normal application.
  world.server().PushEvent(e);
  ProcessPendingEvents();
}

// Moves the pointer to p with button held, and delivers the motion event that
// makes a drag in progress act on it.
void dragTo(wmtest::World& world, Point p, int button) {
  world.server().SetMousePosition(p.x, p.y, heldMask(button));
  xcb_motion_notify_event_t motion{};
  motion.response_type = XCB_MOTION_NOTIFY;
  motion.event = LScr::I->Root();
  motion.root_x = p.x;
  motion.root_y = p.y;
  world.server().PushEvent(motion);
  ProcessPendingEvents();
}

// Releases button at p, ending whatever drag is in progress. Every test that
// starts a drag must finish it: the current DragHandler is a file static in
// disp.cc, so one left running outlives the World and drives the next test's
// motion events - and the fake server hands out the same window ids each
// time, so it even finds a client to act on.
void releaseAt(wmtest::World& world, const Client* c, Point p, int button) {
  world.server().SetMousePosition(p.x, p.y, 0);
  world.server().PushEvent(
      buttonEvent(XCB_BUTTON_RELEASE, c, button, 0, p, 1000));
  ProcessPendingEvents();
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

TEST(SuperGestures, CentreDoubleClickTogglesTheWindowBackAgain) {
  // The Windows way round: the double click which made the window fill the
  // screen makes it small again, because there's nothing left to grow into.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();
  const Rect screen = LScr::I->GetPrimaryVisibleArea(true);

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              40 * kSlowly);
  ASSERT_EQ(c->FrameRect(), screen);

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              41 * kSlowly);
  EXPECT_EQ(c->FrameRect(), before)
      << "a second double click in the middle should undo the first";

  // And the toggle keeps working: expanding again notes the new geometry.
  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              42 * kSlowly);
  EXPECT_EQ(c->FrameRect(), screen);
}

TEST(SuperGestures, MiddleCentreDoubleClickTogglesToo) {
  // Button 2's centre double click expands the same way, so it un-expands the
  // same way.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();

  doubleClick(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              43 * kSlowly);
  ASSERT_EQ(c->FrameRect(), LScr::I->GetPrimaryVisibleArea(true));

  doubleClick(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              44 * kSlowly);
  EXPECT_EQ(c->FrameRect(), before);
}

TEST(SuperGestures, OnlyTheCentreCellUndoesAnExpansion) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect screen = LScr::I->GetPrimaryVisibleArea(true);

  doubleClick(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              45 * kSlowly);
  ASSERT_EQ(c->FrameRect(), screen);

  // An edge which has nowhere to grow simply doesn't grow. The user asked for
  // that one edge to move, not for the window to shrink.
  doubleClick(world, c, SUPER_RESIZE_BUTTON, SUPER_MASK, gridCell(c, 2, 1),
              46 * kSlowly);
  EXPECT_EQ(c->FrameRect(), screen);
}

TEST(SuperGestures, CentreDoubleClickUnmaximizesAMaximizedWindow) {
  // A window the client maximised itself (or that lwm maximised for it on a
  // _NET_WM_STATE message) has no room to grow either, and comes back to its
  // own recorded pre-maximise geometry - flags and all.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();
  c->SetMaximized(true, true);
  ASSERT_TRUE(c->IsMaximized());

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              47 * kSlowly);

  EXPECT_FALSE(c->IsMaximized());
  EXPECT_EQ(c->FrameRect(), before);
}

TEST(SuperGestures, ExpandingAHalfMaximizedWindowRestoresPastTheMaximization) {
  // Vertically maximised, so there's still room to grow sideways: the double
  // click expands (dropping the maximisation), and the geometry to come back
  // to is the one from before the maximisation, not the tall one.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();
  const Rect screen = LScr::I->GetPrimaryVisibleArea(true);
  c->SetMaximized(true, false);
  ASSERT_NE(c->FrameRect(), before);

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              48 * kSlowly);
  ASSERT_EQ(c->FrameRect(), screen);
  ASSERT_FALSE(c->IsMaximized());

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              49 * kSlowly);
  EXPECT_EQ(c->FrameRect(), before)
      << "the restore should skip the maximised size the window passed through";
}

TEST(SuperGestures, NothingToRestoreLeavesTheWindowWhereItIs) {
  // A window which was born filling the screen has never been expanded, so
  // there's no recorded geometry to go back to. It must stay put rather than
  // be moved to whatever an empty Rect means.
  wmtest::World world;
  const Rect screen = LScr::I->GetPrimaryVisibleArea(true);
  Client* c = world.MapClientWindow(screen);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              50 * kSlowly);

  EXPECT_EQ(c->ContentRect(), before);
}

TEST(SuperGestures, TheRestoreOnlyHappensOnce) {
  // Un-expanding spends the record, so a window the user has since grown by
  // hand doesn't jump back to a geometry from before that.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();

  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              51 * kSlowly);
  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              52 * kSlowly);
  ASSERT_EQ(c->FrameRect(), before);

  // The window is small again, so this one expands rather than restoring.
  // What it must not do is put the window back to a stale rect.
  doubleClick(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, gridCell(c, 1, 1),
              53 * kSlowly);
  EXPECT_EQ(c->FrameRect(), LScr::I->GetPrimaryVisibleArea(true));
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

// ---------------------------------------------------------------------------
// Undecorated windows.
//
// A client that draws its own title bar and resize grip asks lwm for no
// decorations, and lwm obliges by giving it no frame. That must not also cost
// it every other way of being moved: with no furniture to drag, the Windows-
// key gestures and _NET_WM_MOVERESIZE are the only two left, and for a while
// neither of them worked, which pinned such a window to the screen for good.
// ---------------------------------------------------------------------------

TEST(Undecorated, ClicksOnAnUndecoratedWindowAreGrabbed) {
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->framed);

  std::ostringstream prefix;
  prefix << "GrabButton(0x" << std::hex << c->window << ")";
  EXPECT_FALSE(world.server().CallsMatching(prefix.str()).empty())
      << "an undecorated window got no gesture grabs, so it can't be moved";
}

TEST(Undecorated, WindowTypesWithNoFurnitureByNatureGetNoGrabs) {
  // A dock isn't undecorated because it asked to draw its own title bar; it's
  // undecorated because dragging a dock around is meaningless. It should still
  // be left alone.
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect,
                             ewmh_atom[_NET_WM_WINDOW_TYPE_DOCK]);
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->framed);

  std::ostringstream prefix;
  prefix << "GrabButton(0x" << std::hex << c->window << ")";
  EXPECT_TRUE(world.server().CallsMatching(prefix.str()).empty());
}

TEST(Undecorated, SuperDragMovesAnUndecoratedWindow) {
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  drag(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, Point{400, 400},
       Point{450, 470}, 10000);

  EXPECT_EQ(c->ContentRect(), Rect::Translate(before, Point{50, 70}));
}

// ---------------------------------------------------------------------------
// Super+Control+click: turning lwm's furniture on and off for a window that
// already exists. The window's outer extent is what's preserved, so the
// client window grows into the space the furniture was using and shrinks back
// out of it again.
// ---------------------------------------------------------------------------

TEST(Decorations, TheToggleIsGrabbed) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);

  // Under every combination of the two lock modifiers, same as the plain
  // Super gestures: X matches a passive grab's modifiers exactly.
  std::ostringstream prefix;
  prefix << "GrabButton(0x" << std::hex << c->window << ")";
  const std::vector<std::string> grabs =
      world.server().CallsMatching(prefix.str());
  for (unsigned int locks :
       {0u, uint32_t(XCB_MOD_MASK_LOCK), uint32_t(XCB_MOD_MASK_2),
        uint32_t(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)}) {
    std::ostringstream want;
    want << prefix.str() << " button=" << SUPER_DECORATE_BUTTON
         << " modifiers=" << (SUPER_MASK | SUPER_CTRL_MASK | locks);
    bool found = false;
    for (const std::string& got : grabs) {
      found = found || got == want.str();
    }
    EXPECT_TRUE(found) << "no grab matching '" << want.str() << "'";
  }
}

TEST(Decorations, TheClickTogglesTheFurnitureAndKeepsTheOuterExtent) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  ASSERT_TRUE(c->HasFurniture());
  const Rect outer = c->FrameRect();
  const Rect inner = c->ContentRect();
  ASSERT_NE(outer, inner) << "the test needs a client with real furniture";

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 60 * kSlowly);

  EXPECT_FALSE(c->HasFurniture());
  EXPECT_EQ(c->ContentRect(), outer)
      << "the client should have grown into the space the furniture was using";
  EXPECT_EQ(c->FrameRect(), outer) << "and the outer extent should not move";

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 61 * kSlowly);

  EXPECT_TRUE(c->HasFurniture());
  EXPECT_EQ(c->FrameRect(), outer);
  EXPECT_EQ(c->ContentRect(), inner) << "and back to exactly where it started";
}

TEST(Decorations, AClientWithSizeIncrementsGetsItsExactGeometryBack) {
  // Both halves of the toggle put the size through LimitResize, and a client
  // with increments rounds down each time, so doing the arithmetic in both
  // directions used to cost an xterm a row and a column per round trip.
  wmtest::World world;
  Client* c = mapWithResizeIncrement(world, kClientRect, 7, 13);
  ASSERT_TRUE(c != nullptr);
  ASSERT_TRUE(c->HasFurniture());
  const Rect content = c->ContentRect();

  for (int i = 0; i < 3; i++) {
    superControlClick(world, c, SUPER_DECORATE_BUTTON, (110 + 2 * i) * kSlowly);
    ASSERT_FALSE(c->HasFurniture());
    superControlClick(world, c, SUPER_DECORATE_BUTTON,
                      (111 + 2 * i) * kSlowly);
    ASSERT_TRUE(c->HasFurniture());
    EXPECT_EQ(c->ContentRect(), content)
        << "the window shrank on round trip " << (i + 1);
  }
}

TEST(Decorations, MovingAnUndecoratedWindowGivesUpTheRememberedGeometry) {
  // The remembered geometry is only good while nothing has touched the
  // window since. Once something has, the arithmetic is the only honest
  // answer, even if it costs a client with increments a row.
  wmtest::World world;
  Client* c = mapWithResizeIncrement(world, kClientRect, 7, 13);
  ASSERT_TRUE(c != nullptr);

  const Rect before = c->ContentRect();

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 120 * kSlowly);
  ASSERT_FALSE(c->HasFurniture());
  const Rect moved = Rect::Translate(c->ContentRect(), Point{100, 40});
  c->MoveTo(moved);

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 121 * kSlowly);

  EXPECT_TRUE(c->HasFurniture());
  EXPECT_NE(c->ContentRect(), before)
      << "the toggle put the window back where it was before the move";
  EXPECT_EQ(Rect::Intersect(c->FrameRect(), moved), c->FrameRect())
      << "and it should be inside the outer extent it was actually given";
}

TEST(Decorations, TheFrameStaysAndShrinksOntoTheClientWindow) {
  // Turning the furniture off used to reparent the client back out to the
  // root and destroy the frame. It doesn't: the frame stays, shrunk to the
  // size of the client window and stripped of its border, which is the same
  // shape it takes while its client is full screen.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Window frame = c->parent;
  ASSERT_TRUE(frame != LScr::I->Root());

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 62 * kSlowly);

  EXPECT_EQ(c->parent, frame) << "the frame should have survived the toggle";
  EXPECT_TRUE(c->framed) << "still framed - it just has no furniture on it";
  ASSERT_TRUE(world.server().Get(frame) != nullptr);
  EXPECT_TRUE(world.server().Get(frame)->mapped);
  EXPECT_EQ(LScr::I->GetClient(frame), c) << "and is still registered as ours";
  // The client window fills the frame exactly, so nothing of the frame shows
  // and every pointer event lands on the client.
  EXPECT_EQ(world.server().Get(frame)->rect, c->ContentRect());
  EXPECT_EQ(world.server().Get(frame)->border_width, 0)
      << "the frame's X border would draw a line round an undecorated window";
  EXPECT_EQ(world.server().Get(c->window)->rect,
            Rect::FromXYWH(0, 0, c->ContentRect().width(),
                           c->ContentRect().height()));
  EXPECT_EQ(world.server().Get(c->window)->parent, frame);
  // Still fully managed, and still focused: nothing was unmapped.
  EXPECT_TRUE(world.server().Get(c->window)->mapped);
  EXPECT_EQ(LScr::I->GetClient(c->window, false), c);
  EXPECT_EQ(c->State(), NormalState);
  EXPECT_EQ(world.server().FocusedWindow(), c->window);

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 63 * kSlowly);

  EXPECT_EQ(c->parent, frame) << "and the same frame comes back";
  EXPECT_EQ(world.server().Get(frame)->rect, c->FrameRect());
  EXPECT_EQ(world.server().Get(frame)->border_width, kFrameBorderWidth);
  EXPECT_EQ(world.server().Get(c->window)->rect, c->ContentRectRelative());
}

TEST(Decorations, TheToggleNeitherUnmapsNorReparents) {
  // This is the whole reason the frame stays. A reparent to the root is the
  // ICCCM's "I have stopped managing this window", and clients that believe
  // it - Java's XAWT is one - stop trusting the geometry they're given and
  // put themselves back to the size they last knew. It also unmaps the client
  // window on the way, which lwm then has to tell apart from a withdrawal.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  std::ostringstream reparent;
  reparent << "ReparentWindow(0x" << std::hex << c->window << ")";
  std::ostringstream unmap;
  unmap << "UnmapWindow(0x" << std::hex << c->window << ")";
  world.server().ClearCalls();

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 64 * kSlowly);
  superControlClick(world, c, SUPER_DECORATE_BUTTON, 65 * kSlowly);

  EXPECT_TRUE(world.server().CallsMatching(reparent.str()).empty())
      << "the toggle reparented the client window";
  EXPECT_TRUE(world.server().CallsMatching(unmap.str()).empty())
      << "the toggle unmapped the client window";
  // So an UnmapNotify arriving now is the client's own doing, and nothing the
  // toggle did should have left an expected unmap lying around to swallow it.
  sendUnmapNotify(world, c->parent, c->window);
  EXPECT_EQ(c->State(), WithdrawnState);
}

TEST(Decorations, TheFirstFrameIsStillCountedAsAnUnmapWeAskedFor) {
  // The one reparent left in the toggle is the first frame for a window lwm
  // chose not to decorate. That still unmaps the client window on its way,
  // and that UnmapNotify still has to be told apart from a withdrawal.
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->framed);

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 66 * kSlowly);
  ASSERT_TRUE(c->framed);
  sendUnmapNotify(world, LScr::I->Root(), c->window);

  EXPECT_EQ(c->State(), NormalState)
      << "the window was withdrawn by its own reparent";
  EXPECT_EQ(LScr::I->GetClient(c->window, false), c);

  // One expected unmap, no more: the next event is the client's own doing.
  sendUnmapNotify(world, c->parent, c->window);
  EXPECT_EQ(c->State(), WithdrawnState);
}

TEST(Decorations, TheToggleKeepsTheWindowsPlaceInTheStack) {
  wmtest::World world;
  Client* below = world.MapClientWindow(Rect::FromXYWH(50, 50, 100, 100));
  Client* c = world.MapClientWindow(kClientRect);
  Client* above = world.MapClientWindow(Rect::FromXYWH(700, 50, 100, 100));
  ASSERT_TRUE(below != nullptr && c != nullptr && above != nullptr);
  c->Raise();
  above->Raise();
  ASSERT_TRUE(rootStackIndex(world, below->parent) <
              rootStackIndex(world, c->parent));
  ASSERT_TRUE(rootStackIndex(world, c->parent) <
              rootStackIndex(world, above->parent));

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 67 * kSlowly);

  EXPECT_TRUE(rootStackIndex(world, below->parent) <
              rootStackIndex(world, c->parent))
      << "losing the furniture lowered the window";
  EXPECT_TRUE(rootStackIndex(world, c->parent) <
              rootStackIndex(world, above->parent))
      << "losing the furniture raised the window";

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 68 * kSlowly);

  EXPECT_TRUE(rootStackIndex(world, below->parent) <
              rootStackIndex(world, c->parent));
  EXPECT_TRUE(rootStackIndex(world, c->parent) <
              rootStackIndex(world, above->parent))
      << "gaining the furniture raised the window";
}

TEST(Decorations, TheOtherButtonsDoNothingWithControlHeld) {
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->FrameRect();

  for (int button : {SUPER_RESIZE_BUTTON, SUPER_HIDE_BUTTON}) {
    superControlClick(world, c, button, (70 + button) * kSlowly);
    EXPECT_TRUE(c->HasFurniture())
        << "button " << button << " toggled the furniture";
    EXPECT_FALSE(c->IsHidden()) << "button " << button << " hid the window";
    EXPECT_EQ(c->FrameRect(), before);
  }
}

TEST(Decorations, AWindowLwmChoseNotToDecorateCanBeGivenFurniture) {
  // The Steam launcher case, in reverse: the client said it draws its own
  // title bar, the user disagrees. This is the one direction that still has
  // to reparent, because there's no frame to grow.
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->framed);
  ASSERT_EQ(c->parent, LScr::I->Root());
  const Rect outer = c->FrameRect();

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 80 * kSlowly);

  EXPECT_TRUE(c->HasFurniture());
  ASSERT_TRUE(c->parent != LScr::I->Root());
  EXPECT_EQ(LScr::I->GetClient(c->parent), c)
      << "the new frame must resolve to this client";
  EXPECT_EQ(world.server().Get(c->window)->parent, c->parent);
  EXPECT_TRUE(world.server().Get(c->parent)->mapped);
  EXPECT_EQ(c->FrameRect(), outer)
      << "the outer extent should be preserved in this direction too";
  EXPECT_EQ(c->ContentRect(), Client::ContentFromFrameRect(outer));
  EXPECT_EQ(world.server().Get(c->parent)->rect, c->FrameRect());
  EXPECT_EQ(world.server().Get(c->window)->rect, c->ContentRectRelative());
  EXPECT_EQ(world.server().FocusedWindow(), c->window)
      << "the reparent handed the focus back to PointerRoot";

  // And off again, which from here on is the cheap version: the frame it just
  // gained is the one it keeps.
  const Window frame = c->parent;
  superControlClick(world, c, SUPER_DECORATE_BUTTON, 81 * kSlowly);

  EXPECT_FALSE(c->HasFurniture());
  EXPECT_EQ(c->parent, frame);
  EXPECT_EQ(c->ContentRect(), outer);
}

TEST(Decorations, FurnitureFreeWindowTypesAreLeftAlone) {
  // A dock has no furniture because a dock with a title bar is nonsense, not
  // because anyone chose. It gets no gesture grabs either, so this can only
  // be reached by calling in directly - but the refusal is what stops a
  // future caller doing something silly.
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect,
                             ewmh_atom[_NET_WM_WINDOW_TYPE_DOCK]);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  c->SetFurniture(true);

  EXPECT_FALSE(c->framed);
  EXPECT_EQ(c->parent, LScr::I->Root());
  EXPECT_EQ(c->ContentRect(), before);
}

TEST(Decorations, AFullScreenWindowIsLeftAlone) {
  // Full screen has already taken the furniture off, and the geometry that
  // means anything is the one it's standing in front of. Both belong to
  // ExitFullScreen.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Window frame = c->parent;
  c->wstate.fullscreen = true;
  c->EnterFullScreen();
  const Rect full = c->ContentRect();

  superControlClick(world, c, SUPER_DECORATE_BUTTON, 90 * kSlowly);

  EXPECT_TRUE(c->framed);
  EXPECT_EQ(c->parent, frame);
  EXPECT_EQ(c->ContentRect(), full);

  // And it comes back out of full screen with the furniture it went in with.
  c->wstate.fullscreen = false;
  c->ExitFullScreen();
  EXPECT_TRUE(c->HasFurniture());
  EXPECT_EQ(c->ContentRect(), kClientRect);
  EXPECT_EQ(world.server().Get(frame)->border_width, kFrameBorderWidth);
}

TEST(Decorations, FullScreenLeavesAnUndecoratedWindowUndecorated) {
  // Full screen takes the furniture off and puts it back. It must not put
  // back furniture the user had already turned off - including the frame's
  // own X border, which is the only part of it ExitFullScreen touches
  // directly.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  superControlClick(world, c, SUPER_DECORATE_BUTTON, 95 * kSlowly);
  ASSERT_FALSE(c->HasFurniture());
  const Rect undecorated = c->ContentRect();

  c->wstate.fullscreen = true;
  c->EnterFullScreen();
  c->wstate.fullscreen = false;
  c->ExitFullScreen();

  EXPECT_FALSE(c->HasFurniture());
  EXPECT_EQ(c->ContentRect(), undecorated);
  EXPECT_EQ(world.server().Get(c->parent)->rect, undecorated);
  EXPECT_EQ(world.server().Get(c->parent)->border_width, 0);
}

TEST(Decorations, AHiddenWindowIsLeftAlone) {
  // The Hider is holding the window by whichever of the two this would map or
  // unmap. Nothing can reach this today - a hidden window can't be clicked -
  // but half-unhiding a window would be a miserable thing to debug.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Window frame = c->parent;
  c->Hide();
  ASSERT_TRUE(c->IsHidden());

  c->SetFurniture(false);

  EXPECT_TRUE(c->HasFurniture());
  EXPECT_EQ(c->parent, frame);
  EXPECT_FALSE(world.server().Get(frame)->mapped) << "still hidden";
}

TEST(Decorations, AnUndecoratedWindowStillHidesAndUnhides) {
  // A window with no furniture still has a frame, so hiding unmaps that, the
  // same as any other window.
  wmtest::World world;
  Client* c = world.MapClientWindow(kClientRect);
  ASSERT_TRUE(c != nullptr);
  superControlClick(world, c, SUPER_DECORATE_BUTTON, 100 * kSlowly);
  ASSERT_FALSE(c->HasFurniture());

  c->Hide();
  EXPECT_TRUE(c->IsHidden());
  EXPECT_FALSE(world.server().Get(c->parent)->mapped);
  EXPECT_EQ(c->State(), IconicState);

  c->Unhide();
  EXPECT_FALSE(c->IsHidden());
  EXPECT_TRUE(world.server().Get(c->parent)->mapped);
  EXPECT_EQ(c->State(), NormalState);
}

TEST(Decorations, AWindowThatNeverGetsAFrameStillHidesAndUnhides) {
  // Hiding unmaps the frame, and a client lwm never framed hasn't got one:
  // its own window goes instead. Getting that wrong left the window on
  // screen, marked hidden, with no way back.
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->framed);

  c->Hide();
  EXPECT_TRUE(c->IsHidden());
  EXPECT_FALSE(world.server().Get(c->window)->mapped)
      << "the client window is the only thing there is to unmap";
  // The unmap lwm asked for isn't the client withdrawing the window.
  sendUnmapNotify(world, LScr::I->Root(), c->window);
  EXPECT_EQ(c->State(), IconicState);

  c->Unhide();
  EXPECT_FALSE(c->IsHidden());
  EXPECT_TRUE(world.server().Get(c->window)->mapped);
  EXPECT_EQ(c->State(), NormalState);
}

TEST(MoveResize, MoveRequestFollowsThePointer) {
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  // The user has pressed button 1 on the client's own title bar; the client
  // hands the drag to us from there.
  world.server().SetMousePosition(400, 400, XCB_KEY_BUT_MASK_BUTTON_1);
  sendMoveResize(world, c, DMove, XCB_BUTTON_INDEX_1);
  dragTo(world, Point{460, 440}, XCB_BUTTON_INDEX_1);

  EXPECT_EQ(c->ContentRect(), Rect::Translate(before, Point{60, 40}));

  releaseAt(world, c, Point{460, 440}, XCB_BUTTON_INDEX_1);
  // The grab we took to receive those events has to go back, or no other
  // client sees the mouse again.
  EXPECT_FALSE(world.server().CallsMatching("UngrabPointer()").empty());
}

TEST(MoveResize, ResizeRequestDragsTheNamedEdge) {
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  world.server().SetMousePosition(600, 600, XCB_KEY_BUT_MASK_BUTTON_1);
  sendMoveResize(world, c, DSizeBottomRight, XCB_BUTTON_INDEX_1);
  dragTo(world, Point{640, 660}, XCB_BUTTON_INDEX_1);

  const Rect after = c->ContentRect();
  EXPECT_EQ(after.xMin, before.xMin);
  EXPECT_EQ(after.yMin, before.yMin);
  EXPECT_EQ(after.xMax, before.xMax + 40);
  EXPECT_EQ(after.yMax, before.yMax + 60);

  releaseAt(world, c, Point{640, 660}, XCB_BUTTON_INDEX_1);
}

TEST(MoveResize, ADragThatCannotBeTrackedIsRefused) {
  // Direction DSizeKeyboard has no button behind it, so there is no release
  // for lwm to wait for. Starting a drag anyway would leave one running for
  // ever, and lwm refuses to start a second while one is in progress - so the
  // symptom would be that every later mouse gesture stopped working.
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  world.server().SetMousePosition(400, 400, 0);
  sendMoveResize(world, c, DSizeKeyboard, 0);

  // A later gesture still works, which is what says no drag was left stuck.
  drag(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, Point{400, 400},
       Point{430, 400}, 20000);
  EXPECT_EQ(c->ContentRect(), Rect::Translate(before, Point{30, 0}));
}

TEST(MoveResize, AFailedPointerGrabLeavesNoDragRunning) {
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  world.server().SetPointerGrabStatus(XCB_GRAB_STATUS_ALREADY_GRABBED);
  world.server().SetMousePosition(400, 400, XCB_KEY_BUT_MASK_BUTTON_1);
  sendMoveResize(world, c, DMove, XCB_BUTTON_INDEX_1);
  dragTo(world, Point{460, 440}, XCB_BUTTON_INDEX_1);
  EXPECT_EQ(c->ContentRect(), before) << "moved despite not holding the pointer";

  // And the gestures still work afterwards.
  world.server().SetPointerGrabStatus(XCB_GRAB_STATUS_SUCCESS);
  drag(world, c, SUPER_MOVE_BUTTON, SUPER_MASK, Point{400, 400},
       Point{430, 400}, 30000);
  EXPECT_EQ(c->ContentRect(), Rect::Translate(before, Point{30, 0}));
}

TEST(MoveResize, CancelStopsTheDragAndReturnsTheGrab) {
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);

  world.server().SetMousePosition(400, 400, XCB_KEY_BUT_MASK_BUTTON_1);
  sendMoveResize(world, c, DMove, XCB_BUTTON_INDEX_1);
  dragTo(world, Point{460, 440}, XCB_BUTTON_INDEX_1);
  const Rect moved = c->ContentRect();

  world.server().ClearCalls();
  sendMoveResize(world, c, DMoveResizeCancel, XCB_BUTTON_INDEX_1);
  EXPECT_FALSE(world.server().CallsMatching("UngrabPointer()").empty());

  // Further motion no longer moves the window.
  dragTo(world, Point{500, 500}, XCB_BUTTON_INDEX_1);
  EXPECT_EQ(c->ContentRect(), moved);
}

TEST(MoveResize, ARequestIsIgnoredWhileTheUserIsAlreadyDragging) {
  wmtest::World world;
  Client* c = mapUndecorated(world, kClientRect);
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  // Start a Windows-key move, and leave it in progress.
  world.server().SetMousePosition(400, 400, SUPER_MASK);
  world.server().PushEvent(buttonEvent(XCB_BUTTON_PRESS, c, SUPER_MOVE_BUTTON,
                                       SUPER_MASK, Point{400, 400}, 40000));
  ProcessPendingEvents();

  world.server().ClearCalls();
  sendMoveResize(world, c, DSizeBottomRight, XCB_BUTTON_INDEX_1);
  EXPECT_TRUE(world.server().CallsMatching("GrabPointer(").empty())
      << "took the pointer away from a gesture already in progress";

  // The user's own drag is still the one in charge.
  dragTo(world, Point{450, 400}, SUPER_MOVE_BUTTON);
  EXPECT_EQ(c->ContentRect(), Rect::Translate(before, Point{50, 0}));

  releaseAt(world, c, Point{450, 400}, SUPER_MOVE_BUTTON);
}
