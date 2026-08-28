// Tests for the pure half of the Windows-key mouse gestures: which of the 3x3
// grid's cells a click lands in, how far an edge may expand, and what counts
// as a double click. The handlers built on these are in drag.cc, and are
// tested against a real display in ui_test.sh.

#include "gesture.h"

#include "test.h"

namespace {

// A 300x300 window at the origin, so each cell of the grid is exactly 100
// pixels square and the boundaries are easy to name.
const Rect kWin = Rect::FromXYWH(0, 0, 300, 300);

}  // namespace

TEST(NineGridEdgeAt, EachCellGivesItsEdge) {
  struct TestCase {
    const char* name;
    Point p;
    Edge want;
  };
  const TestCase cases[] = {
      {"top left", Point{50, 50}, ETopLeft},
      {"top", Point{150, 50}, ETop},
      {"top right", Point{250, 50}, ETopRight},
      {"left", Point{50, 150}, ELeft},
      {"centre", Point{150, 150}, ENone},
      {"right", Point{250, 150}, ERight},
      {"bottom left", Point{50, 250}, EBottomLeft},
      {"bottom", Point{150, 250}, EBottom},
      {"bottom right", Point{250, 250}, EBottomRight},
  };
  for (const TestCase& tc : cases) {
    testing::Context ctx(tc.name);
    EXPECT_EQ(NineGridEdgeAt(kWin, tc.p), tc.want);
  }
}

TEST(NineGridEdgeAt, CellBoundariesFallOnExactThirds) {
  // The last pixel of the first third, and the first of the second.
  EXPECT_EQ(NineGridEdgeAt(kWin, Point{99, 150}), ELeft);
  EXPECT_EQ(NineGridEdgeAt(kWin, Point{100, 150}), ENone);
  EXPECT_EQ(NineGridEdgeAt(kWin, Point{199, 150}), ENone);
  EXPECT_EQ(NineGridEdgeAt(kWin, Point{200, 150}), ERight);
}

TEST(NineGridEdgeAt, IsRelativeToTheRectNotTheOrigin) {
  const Rect r = Rect::FromXYWH(1000, 500, 300, 300);
  EXPECT_EQ(NineGridEdgeAt(r, Point{1050, 550}), ETopLeft);
  EXPECT_EQ(NineGridEdgeAt(r, Point{1150, 650}), ENone);
  EXPECT_EQ(NineGridEdgeAt(r, Point{1250, 750}), EBottomRight);
}

TEST(NineGridEdgeAt, PointsOutsideCountAsTheNearestCell) {
  // Not reachable through a grab on the window, but the arithmetic mustn't
  // wrap round to the wrong cell if it ever happens.
  EXPECT_EQ(NineGridEdgeAt(kWin, Point{-100, -100}), ETopLeft);
  EXPECT_EQ(NineGridEdgeAt(kWin, Point{999, 999}), EBottomRight);
  EXPECT_EQ(NineGridEdgeAt(kWin, Point{150, 999}), EBottom);
}

TEST(NineGridEdgeAt, DegenerateAxisIsCentredRatherThanDividedByZero) {
  EXPECT_EQ(NineGridEdgeAt(Rect{}, Point{0, 0}), ENone);
  // No width, so nothing to be left or right of - but the vertical thirds
  // still work.
  EXPECT_EQ(NineGridEdgeAt(Rect::FromXYWH(10, 10, 0, 60), Point{10, 20}),
            ETop);
  EXPECT_EQ(NineGridEdgeAt(Rect::FromXYWH(10, 10, 0, 60), Point{10, 50}),
            EBottom);
}

namespace {

// One 1000x800 monitor, which most of the expansion tests use.
const std::vector<Rect> kOneMonitor{Rect::FromXYWH(0, 0, 1000, 800)};

}  // namespace

TEST(ExpandRect, SingleEdgeGrowsToTheMonitorAndLeavesTheOthersAlone) {
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  EXPECT_EQ(ExpandRect(r, ERight, {}, kOneMonitor),
            Rect::FromXYWH(100, 100, 900, 200));
  EXPECT_EQ(ExpandRect(r, ELeft, {}, kOneMonitor),
            Rect::FromXYWH(0, 100, 300, 200));
  EXPECT_EQ(ExpandRect(r, ETop, {}, kOneMonitor),
            Rect::FromXYWH(100, 0, 200, 300));
  EXPECT_EQ(ExpandRect(r, EBottom, {}, kOneMonitor),
            Rect::FromXYWH(100, 100, 200, 700));
}

TEST(ExpandRect, CornerGrowsBothOfItsEdges) {
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  EXPECT_EQ(ExpandRect(r, ETopLeft, {}, kOneMonitor),
            Rect::FromXYWH(0, 0, 300, 300));
  EXPECT_EQ(ExpandRect(r, EBottomRight, {}, kOneMonitor),
            Rect::FromXYWH(100, 100, 900, 700));
}

TEST(ExpandRect, CentreGrowsEveryEdge) {
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  EXPECT_EQ(ExpandRect(r, ENone, {}, kOneMonitor), kOneMonitor[0]);
}

TEST(ExpandRect, StopsAtAWindowInTheWay) {
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  // Directly to the right of r, and overlapping it vertically.
  const Rect blocker = Rect::FromXYWH(600, 150, 100, 100);
  EXPECT_EQ(ExpandRect(r, ERight, {blocker}, kOneMonitor),
            Rect::FromXYWH(100, 100, 500, 200));
  // The nearest of several is the one that counts.
  const Rect farther = Rect::FromXYWH(800, 150, 100, 100);
  EXPECT_EQ(ExpandRect(r, ERight, {farther, blocker}, kOneMonitor),
            Rect::FromXYWH(100, 100, 500, 200));
}

TEST(ExpandRect, IgnoresWindowsWhichDoNotOverlapOnTheOtherAxis) {
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  // To the right of r, but entirely below it: growing rightwards will never
  // touch it, so it must not stop the expansion.
  const Rect below = Rect::FromXYWH(600, 500, 100, 100);
  EXPECT_EQ(ExpandRect(r, ERight, {below}, kOneMonitor),
            Rect::FromXYWH(100, 100, 900, 200));
  // Exactly abutting r's bottom edge counts as no overlap, so this one
  // doesn't block a sideways expansion either.
  const Rect abutting = Rect::FromXYWH(600, 300, 100, 100);
  EXPECT_EQ(ExpandRect(r, ERight, {abutting}, kOneMonitor),
            Rect::FromXYWH(100, 100, 900, 200));
}

TEST(ExpandRect, IgnoresWindowsBehindTheEdgeBeingExpanded) {
  const Rect r = Rect::FromXYWH(400, 100, 200, 200);
  const Rect toTheLeft = Rect::FromXYWH(100, 150, 100, 100);
  EXPECT_EQ(ExpandRect(r, ERight, {toTheLeft}, kOneMonitor),
            Rect::FromXYWH(400, 100, 600, 200));
  // ...but it does stop a leftward expansion.
  EXPECT_EQ(ExpandRect(r, ELeft, {toTheLeft}, kOneMonitor),
            Rect::FromXYWH(200, 100, 400, 200));
}

TEST(ExpandRect, AnOverlappingWindowBlocksNothing) {
  // Already on top of r: there's no gap to fill, and neither of its edges is
  // beyond r's, so it can't be a barrier for either direction.
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  const Rect overlapping = Rect::FromXYWH(200, 150, 200, 100);
  EXPECT_EQ(ExpandRect(r, ERight, {overlapping}, kOneMonitor),
            Rect::FromXYWH(100, 100, 900, 200));
}

TEST(ExpandRect, ObstaclesAreOnlyConsultedWhenGivenToUs) {
  // The "expand to the monitor, ignoring other windows" gesture is just this
  // function with an empty obstacle list.
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  const Rect blocker = Rect::FromXYWH(600, 150, 100, 100);
  EXPECT_EQ(ExpandRect(r, ENone, {blocker}, kOneMonitor),
            Rect::FromXYWH(0, 0, 600, 800));
  EXPECT_EQ(ExpandRect(r, ENone, {}, kOneMonitor), kOneMonitor[0]);
}

TEST(ExpandRect, StaysOnTheMonitorTheWindowIsOn) {
  // Two 500-wide monitors side by side. A window on the left one must expand
  // to the join, not across it.
  const std::vector<Rect> monitors{Rect::FromXYWH(0, 0, 500, 800),
                                   Rect::FromXYWH(500, 0, 500, 800)};
  const Rect onLeft = Rect::FromXYWH(100, 100, 200, 200);
  EXPECT_EQ(ExpandRect(onLeft, ENone, {}, monitors),
            Rect::FromXYWH(0, 0, 500, 800));
  const Rect onRight = Rect::FromXYWH(600, 100, 200, 200);
  EXPECT_EQ(ExpandRect(onRight, ENone, {}, monitors),
            Rect::FromXYWH(500, 0, 500, 800));
}

TEST(ExpandRect, PicksTheMonitorItOverlapsMost) {
  const std::vector<Rect> monitors{Rect::FromXYWH(0, 0, 500, 800),
                                   Rect::FromXYWH(500, 0, 500, 800)};
  // Straddles the join, but is mostly on the right-hand monitor.
  const Rect straddling = Rect::FromXYWH(450, 100, 200, 200);
  EXPECT_EQ(ExpandRect(straddling, ERight, {}, monitors),
            Rect::FromXYWH(450, 100, 550, 200));
}

TEST(ExpandRect, EdgesNeverMoveInwards) {
  // A window hanging off the left of the monitor. Expanding leftwards has
  // nowhere to go, and must not "expand" by shrinking to the monitor edge.
  const Rect r = Rect::FromXYWH(-50, 100, 200, 200);
  EXPECT_EQ(ExpandRect(r, ELeft, {}, kOneMonitor), r);
  // Growing the other way still works.
  EXPECT_EQ(ExpandRect(r, ERight, {}, kOneMonitor),
            Rect::FromXYWH(-50, 100, 1050, 200));
  // And a window bigger than its monitor is left entirely alone.
  const Rect huge = Rect::FromXYWH(-100, -100, 1200, 1000);
  EXPECT_EQ(ExpandRect(huge, ENone, {}, kOneMonitor), huge);
}

TEST(ExpandRect, WithNoMonitorsOnlyTheObstaclesConstrainIt) {
  // Not something lwm does, but the function must not invent a barrier at
  // zero (or run off to infinity) when it's told about no monitors at all.
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  EXPECT_EQ(ExpandRect(r, ENone, {}, {}), r);
  const Rect blocker = Rect::FromXYWH(600, 150, 100, 100);
  EXPECT_EQ(ExpandRect(r, ERight, {blocker}, {}),
            Rect::FromXYWH(100, 100, 500, 200));
}

TEST(DoubleClickTracker, TwoQuickPressesInTheSamePlaceAreADoubleClick) {
  DoubleClickTracker t;
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1000));
  EXPECT_TRUE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1100));
}

TEST(DoubleClickTracker, ADoubleClickIsConsumed) {
  // Otherwise a triple click would fire the action twice, and a click-and-
  // hold after one would be interpreted as another.
  DoubleClickTracker t;
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1000));
  EXPECT_TRUE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1100));
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1200));
  EXPECT_TRUE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1300));
}

TEST(DoubleClickTracker, TooSlowIsTwoSingleClicks) {
  DoubleClickTracker t;
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1000));
  const uint32_t tooLate = 1000 + DoubleClickTracker::kIntervalMillis + 1;
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{10, 10}, tooLate));
  // The second press became the new first one, so a prompt third completes a
  // double click with it.
  EXPECT_TRUE(t.IsDoubleClick(1, 0x100, Point{10, 10}, tooLate + 10));
}

TEST(DoubleClickTracker, SmallPointerDriftIsToleratedButLargeIsNot) {
  DoubleClickTracker t;
  const int slop = DoubleClickTracker::kSlopPixels;
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{100, 100}, 1000));
  EXPECT_TRUE(
      t.IsDoubleClick(1, 0x100, Point{100 + slop, 100 - slop}, 1100));
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{100, 100}, 2000));
  EXPECT_FALSE(
      t.IsDoubleClick(1, 0x100, Point{100 + slop + 1, 100}, 2100));
}

TEST(DoubleClickTracker, DifferentButtonOrWindowIsNotADoubleClick) {
  DoubleClickTracker t;
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{10, 10}, 1000));
  EXPECT_FALSE(t.IsDoubleClick(3, 0x100, Point{10, 10}, 1050));
  EXPECT_FALSE(t.IsDoubleClick(3, 0x200, Point{10, 10}, 1100));
  // ...and the window one left a valid first press behind it.
  EXPECT_TRUE(t.IsDoubleClick(3, 0x200, Point{10, 10}, 1150));
}

TEST(DoubleClickTracker, TimestampWraparoundIsNotAMissedClick) {
  // X timestamps wrap round to zero roughly every 49 days, and a subtraction
  // that wraps with them gives the right interval.
  DoubleClickTracker t;
  const uint32_t before = 0xfffffff0;
  EXPECT_FALSE(t.IsDoubleClick(1, 0x100, Point{10, 10}, before));
  EXPECT_TRUE(t.IsDoubleClick(1, 0x100, Point{10, 10}, before + 100));
}
