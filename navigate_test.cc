// Tests for the pure part of the arrow-key gestures: which window an arrow
// key picks the focus up from, and where Shift+arrow puts a window. The grabs
// and the focus change built on these are in keyboard.cc.

#include "navigate.h"

#include <vector>

#include "test.h"

namespace {

// A 100x100 window centred exactly on the given point. Everything here is
// stated in terms of centres, because that's all PickWindowInDirection looks
// at; the size is arbitrary and identical throughout so it can't matter.
Rect centredAt(int cx, int cy) {
  return Rect::FromXYWH(cx - 50, cy - 50, 100, 100);
}

// The window with focus, in the middle of a 1080p screen's worth of room.
const Rect kFrom = centredAt(550, 550);

}  // namespace

TEST(PickWindowInDirection, PicksTheWindowThatWay) {
  const std::vector<Rect> candidates = {
      centredAt(150, 550),  // 0: left
      centredAt(950, 550),  // 1: right
      centredAt(550, 150),  // 2: above
      centredAt(550, 950),  // 3: below
  };
  struct TestCase {
    const char* name;
    Direction dir;
    int want;
  };
  const TestCase cases[] = {
      {"left", Direction::kLeft, 0},
      {"right", Direction::kRight, 1},
      {"up", Direction::kUp, 2},
      {"down", Direction::kDown, 3},
  };
  for (const TestCase& tc : cases) {
    testing::Context ctx(tc.name);
    EXPECT_EQ(PickWindowInDirection(kFrom, candidates, tc.dir), tc.want);
  }
}

TEST(PickWindowInDirection, NothingThatWayGivesMinusOne) {
  // One window, off to the left. Every other direction must come up empty
  // rather than settling for it.
  const std::vector<Rect> candidates = {centredAt(150, 550)};
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), 0);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kRight), -1);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kUp), -1);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kDown), -1);
}

TEST(PickWindowInDirection, EmptyCandidateListGivesMinusOne) {
  EXPECT_EQ(PickWindowInDirection(kFrom, {}, Direction::kLeft), -1);
}

TEST(PickWindowInDirection, OutsideTheConeDoesNotCount) {
  // 400 to the left, but 450 up: further vertically than horizontally, so the
  // left cone doesn't reach it. The up cone does.
  const std::vector<Rect> candidates = {centredAt(150, 100)};
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), -1);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kUp), 0);
}

TEST(PickWindowInDirection, ExactlyDiagonalBelongsToBothCones) {
  // 400 left and 400 up: |dx| == |dy|, which both cones include. This is what
  // stops the four cones leaving gaps between them.
  const std::vector<Rect> candidates = {centredAt(150, 150)};
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), 0);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kUp), 0);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kRight), -1);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kDown), -1);
}

TEST(PickWindowInDirection, LevelCentreIsNotInEitherCone) {
  // Directly above, so dx is 0: neither the left nor the right cone takes it,
  // however far away it is.
  const std::vector<Rect> candidates = {centredAt(550, 100)};
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), -1);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kRight), -1);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kUp), 0);
}

TEST(PickWindowInDirection, NearestCentreWins) {
  const std::vector<Rect> candidates = {
      centredAt(50, 550),   // 0: 500 away
      centredAt(350, 550),  // 1: 200 away, so this one
      centredAt(150, 550),  // 2: 400 away
  };
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), 1);
}

TEST(PickWindowInDirection, DistanceIsToTheCentreNotTheEdge) {
  // A wide window whose near edge is closer, but whose centre is further off,
  // than the narrow one beyond it. The centre is what decides.
  const std::vector<Rect> candidates = {
      Rect::FromXYWH(0, 500, 400, 100),  // 0: centre at x=200, 350 away
      centredAt(300, 550),               // 1: centre at x=300, 250 away
  };
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), 1);
}

TEST(PickWindowInDirection, TiesGoToTheEarlierCandidate) {
  const std::vector<Rect> candidates = {
      centredAt(150, 550),
      centredAt(150, 550),
  };
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), 0);
}

TEST(PickWindowInDirection, TheFocusedWindowIsNeverPicked) {
  // Callers are allowed to leave the focused window in the list: its centre
  // is our own, so no cone contains it.
  const std::vector<Rect> candidates = {kFrom, centredAt(150, 550)};
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kLeft), 1);
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kRight), -1);
}

TEST(PickWindowInDirection, WindowsOnOtherMonitorsAreJustMoreCandidates) {
  // Nothing here knows about monitors: a window on the screen to the right is
  // picked by exactly the same rule as one on this screen, only further away.
  const std::vector<Rect> candidates = {
      centredAt(2470, 550),  // 0: second monitor, 1920 to the right
      centredAt(950, 550),   // 1: this monitor, 400 to the right
  };
  EXPECT_EQ(PickWindowInDirection(kFrom, candidates, Direction::kRight), 1);
  // With the near one gone, focus crosses to the other monitor.
  EXPECT_EQ(PickWindowInDirection(kFrom, {candidates[0]}, Direction::kRight), 0);
}

namespace {

// Two monitors side by side: a big one on the left, a smaller one on the
// right whose top is level with it. Matching the common laptop-plus-external
// arrangement, the right hand one is both narrower and shorter.
const std::vector<Rect> kTwoMonitors = {
    Rect::FromXYWH(0, 0, 1000, 800),
    Rect::FromXYWH(1000, 0, 600, 500),
};

}  // namespace

TEST(MoveRectInDirection, GoesToTheInnerEdgeOfItsOwnMonitor) {
  const std::vector<Rect> areas = {Rect::FromXYWH(0, 0, 1000, 800)};
  const Rect win = Rect::FromXYWH(400, 300, 200, 100);
  struct TestCase {
    const char* name;
    Direction dir;
    Rect want;
  };
  const TestCase cases[] = {
      {"left", Direction::kLeft, Rect::FromXYWH(0, 300, 200, 100)},
      {"right", Direction::kRight, Rect::FromXYWH(800, 300, 200, 100)},
      {"up", Direction::kUp, Rect::FromXYWH(400, 0, 200, 100)},
      {"down", Direction::kDown, Rect::FromXYWH(400, 700, 200, 100)},
  };
  for (const TestCase& tc : cases) {
    testing::Context ctx(tc.name);
    EXPECT_EQ(MoveRectInDirection(win, tc.dir, areas), tc.want);
  }
}

TEST(MoveRectInDirection, MonitorEdgesAreNotTheScreenOrigin) {
  // The monitor a window is on decides where it stops, not the desktop as a
  // whole: a window on the right hand monitor moved left stops at that
  // monitor's left edge.
  const Rect win = Rect::FromXYWH(1200, 100, 200, 100);
  EXPECT_EQ(MoveRectInDirection(win, Direction::kLeft, kTwoMonitors),
            Rect::FromXYWH(1000, 100, 200, 100));
}

TEST(MoveRectInDirection, AlreadyAtTheEdgeMovesToTheNextMonitor) {
  // Flush against the left monitor's right edge, so the next press hands it
  // over - and it arrives just the other side of the join, against the right
  // monitor's left edge, rather than jumping that monitor's whole width.
  const Rect win = Rect::FromXYWH(800, 100, 200, 100);
  const Rect crossed =
      MoveRectInDirection(win, Direction::kRight, kTwoMonitors);
  EXPECT_EQ(crossed, Rect::FromXYWH(1000, 100, 200, 100));
  // Only then does it carry on to the far side of its new monitor.
  EXPECT_EQ(MoveRectInDirection(crossed, Direction::kRight, kTwoMonitors),
            Rect::FromXYWH(1400, 100, 200, 100));
}

TEST(MoveRectInDirection, AndBackAgain) {
  // Each press undoes one press the other way, so a window walks back along
  // the same three positions it walked out on.
  const Rect win = Rect::FromXYWH(1400, 100, 200, 100);
  const Rect once = MoveRectInDirection(win, Direction::kLeft, kTwoMonitors);
  EXPECT_EQ(once, Rect::FromXYWH(1000, 100, 200, 100));
  const Rect twice = MoveRectInDirection(once, Direction::kLeft, kTwoMonitors);
  EXPECT_EQ(twice, Rect::FromXYWH(800, 100, 200, 100));
  EXPECT_EQ(MoveRectInDirection(twice, Direction::kLeft, kTwoMonitors),
            Rect::FromXYWH(0, 100, 200, 100));
}

TEST(MoveRectInDirection, NowhereToGoLeavesTheWindowAlone) {
  const Rect win = Rect::FromXYWH(1400, 100, 200, 100);
  EXPECT_EQ(MoveRectInDirection(win, Direction::kRight, kTwoMonitors), win);
  // Nor is the monitor next door reachable by moving up or down: it's beside
  // this one, not above or below it.
  EXPECT_EQ(MoveRectInDirection(Rect::FromXYWH(1400, 0, 200, 100),
                                Direction::kUp, kTwoMonitors),
            Rect::FromXYWH(1400, 0, 200, 100));
}

TEST(MoveRectInDirection, NoMonitorsAtAllIsNotACrash) {
  const Rect win = Rect::FromXYWH(100, 100, 200, 100);
  EXPECT_EQ(MoveRectInDirection(win, Direction::kLeft, {}), win);
}

TEST(MoveRectInDirection, TooBigForTheNewMonitorShrinksToFit) {
  // 900x700 fits the left monitor and nothing like fits the right one, so
  // moving it across shrinks it on both axes.
  const Rect win = Rect::FromXYWH(100, 50, 900, 700);
  const Rect moved = MoveRectInDirection(win, Direction::kRight, kTwoMonitors);
  EXPECT_EQ(moved, Rect::FromXYWH(1000, 0, 600, 500))
      << "the window should fill the small monitor rather than hang off it";
}

TEST(MoveRectInDirection, ShrinkingOneAxisLeavesTheOtherAlone) {
  // Short enough for the small monitor, too wide for it.
  const Rect win = Rect::FromXYWH(100, 100, 900, 200);
  EXPECT_EQ(MoveRectInDirection(win, Direction::kRight, kTwoMonitors),
            Rect::FromXYWH(1000, 100, 600, 200));
}

TEST(MoveRectInDirection, ArrivalIsPulledOntoTheNewMonitor) {
  // The window fits the small monitor, but sits below its bottom edge. Moving
  // it across has to pull it up, or it would vanish into the dead space under
  // the shorter screen.
  const Rect win = Rect::FromXYWH(800, 600, 200, 100);
  EXPECT_EQ(MoveRectInDirection(win, Direction::kRight, kTwoMonitors),
            Rect::FromXYWH(1000, 400, 200, 100));
}

TEST(MoveRectInDirection, PicksTheNeighbourItSharesAnEdgeWith) {
  // Three monitors: two stacked on the left, one on the right level with the
  // lower of them. From the lower left monitor, "right" must mean the one
  // beside it, even though the other is nearer the origin.
  const std::vector<Rect> areas = {
      Rect::FromXYWH(0, 0, 800, 400),      // 0: top left
      Rect::FromXYWH(0, 400, 800, 400),    // 1: bottom left
      Rect::FromXYWH(800, 400, 800, 400),  // 2: bottom right
  };
  const Rect win = Rect::FromXYWH(600, 500, 200, 100);
  EXPECT_EQ(MoveRectInDirection(win, Direction::kRight, areas),
            Rect::FromXYWH(800, 500, 200, 100));
}

TEST(MoveRectInDirection, VerticallyStackedMonitors) {
  const std::vector<Rect> areas = {
      Rect::FromXYWH(0, 0, 800, 400),
      Rect::FromXYWH(0, 400, 800, 400),
  };
  // Already at the top of the lower monitor, so up means the one above it -
  // arriving against its bottom edge, the one it just crossed.
  const Rect win = Rect::FromXYWH(100, 400, 200, 100);
  const Rect crossed = MoveRectInDirection(win, Direction::kUp, areas);
  EXPECT_EQ(crossed, Rect::FromXYWH(100, 300, 200, 100));
  EXPECT_EQ(MoveRectInDirection(crossed, Direction::kUp, areas),
            Rect::FromXYWH(100, 0, 200, 100));
}

TEST(MapPointToMovedRect, APureMoveCarriesThePointWithIt) {
  const Rect from = Rect::FromXYWH(100, 100, 200, 100);
  const Rect to = Rect::FromXYWH(500, 700, 200, 100);
  // Every corner and the middle, so a sign error can't hide.
  EXPECT_EQ(MapPointToMovedRect(Point{100, 100}, from, to), (Point{500, 700}));
  EXPECT_EQ(MapPointToMovedRect(Point{150, 120}, from, to), (Point{550, 720}));
  EXPECT_EQ(MapPointToMovedRect(Point{299, 199}, from, to), (Point{699, 799}));
}

TEST(MapPointToMovedRect, AResizeKeepsThePointInProportion) {
  // Half the width and half the height: a point 3/4 of the way across stays
  // 3/4 of the way across.
  const Rect from = Rect::FromXYWH(0, 0, 400, 200);
  const Rect to = Rect::FromXYWH(1000, 500, 200, 100);
  EXPECT_EQ(MapPointToMovedRect(Point{300, 100}, from, to),
            (Point{1150, 550}));
  EXPECT_EQ(MapPointToMovedRect(Point{0, 0}, from, to), (Point{1000, 500}));
}

TEST(MapPointToMovedRect, TheResultStaysInsideTheNewRect) {
  // Whatever the shape change, a point inside `from` has to come out inside
  // `to`: the whole purpose is to leave the pointer on the window.
  const Rect from = Rect::FromXYWH(0, 0, 1000, 1000);
  const Rect to = Rect::FromXYWH(50, 60, 37, 11);
  for (int x : {0, 1, 500, 998, 999}) {
    for (int y : {0, 1, 500, 998, 999}) {
      testing::Context ctx(std::to_string(x) + "," + std::to_string(y));
      const Point got = MapPointToMovedRect(Point{x, y}, from, to);
      EXPECT_TRUE(to.contains(got.x, got.y)) << "landed at " << got;
    }
  }
}

TEST(MapPointToMovedRect, AnEmptyRectHasNoProportionsToKeep) {
  const Rect from = Rect::FromXYWH(100, 100, 0, 0);
  const Rect to = Rect::FromXYWH(500, 500, 200, 100);
  EXPECT_EQ(MapPointToMovedRect(Point{100, 100}, from, to), (Point{500, 500}));
}

TEST(LargestVisibleRect, NothingInTheWayLeavesTheWholeWindow) {
  const Rect win = Rect::FromXYWH(100, 100, 400, 300);
  EXPECT_EQ(LargestVisibleRect(win, {}), win);
  // A window which misses it entirely is no more in the way than none at all.
  EXPECT_EQ(LargestVisibleRect(win, {Rect::FromXYWH(600, 600, 100, 100)}), win);
}

TEST(LargestVisibleRect, AWindowOverTheRightHandSideLeavesTheLeft) {
  const Rect win = Rect::FromXYWH(0, 0, 400, 300);
  // Covers the right third, top to bottom and beyond, so the answer is the
  // rest: the occluder is clipped to the window before anything else happens.
  const Rect over = Rect::FromXYWH(300, -50, 200, 500);
  EXPECT_EQ(LargestVisibleRect(win, {over}), Rect::FromXYWH(0, 0, 300, 300));
}

TEST(LargestVisibleRect, PicksTheBiggerOfThePiecesAWindowIsCutInto) {
  const Rect win = Rect::FromXYWH(0, 0, 400, 300);
  // A band across the middle, leaving 100 pixels above it and 150 below. Both
  // are the full width, so the taller one wins.
  const Rect band = Rect::FromXYWH(0, 100, 400, 50);
  EXPECT_EQ(LargestVisibleRect(win, {band}), Rect::FromXYWH(0, 150, 400, 150));
}

TEST(LargestVisibleRect, ComparesAreasAndNotJustWidthsOrHeights) {
  const Rect win = Rect::FromXYWH(0, 0, 400, 300);
  // The occluder leaves a strip 40 wide down the whole 300 of the left side
  // (12000 pixels), and one 400 wide but only 20 deep along the bottom (8000).
  // The wider piece is the smaller one.
  const Rect over = Rect::FromXYWH(40, 0, 360, 280);
  EXPECT_EQ(LargestVisibleRect(win, {over}), Rect::FromXYWH(0, 0, 40, 300));
}

TEST(LargestVisibleRect, FindsAPieceNoSingleOccluderDefines) {
  // Two occluders, in opposite corners, with the free space between them
  // bounded by an edge of each: the answer's edges come from two different
  // windows, which is why this can't be done one occluder at a time.
  const Rect win = Rect::FromXYWH(0, 0, 300, 300);
  const Rect topLeft = Rect::FromXYWH(0, 0, 100, 100);
  const Rect bottomRight = Rect::FromXYWH(200, 200, 100, 100);
  // The answer is the 200x200 block to the right of the first and above the
  // second: its left edge comes from one occluder and its bottom edge from the
  // other. It beats the full-width band between them (300x100) on area.
  EXPECT_EQ(LargestVisibleRect(win, {topLeft, bottomRight}),
            Rect::FromXYWH(100, 0, 200, 200));
}

TEST(LargestVisibleRect, StepsAroundOverlappingOccluders) {
  // Occluders which cover each other are no different from ones which don't:
  // covered is covered, however many times over.
  const Rect win = Rect::FromXYWH(0, 0, 200, 200);
  const Rect a = Rect::FromXYWH(0, 0, 150, 150);
  const Rect b = Rect::FromXYWH(50, 50, 150, 100);
  // Everything above y=150 is covered by one or the other, leaving the bottom.
  EXPECT_EQ(LargestVisibleRect(win, {a, b}), Rect::FromXYWH(0, 150, 200, 50));
}

TEST(LargestVisibleRect, ACompletelyCoveredWindowHasNoVisibleRect) {
  const Rect win = Rect::FromXYWH(100, 100, 200, 200);
  const Rect over = Rect::FromXYWH(50, 50, 400, 400);
  EXPECT_TRUE(LargestVisibleRect(win, {over}).empty());
  // Two occluders which cover it only between them count just as much.
  const Rect top = Rect::FromXYWH(100, 100, 200, 100);
  const Rect bottom = Rect::FromXYWH(100, 200, 200, 100);
  EXPECT_TRUE(LargestVisibleRect(win, {top, bottom}).empty());
}

TEST(LargestVisibleRect, AnEmptyWindowHasNoVisibleRect) {
  EXPECT_TRUE(LargestVisibleRect(Rect{0, 0, 0, 0}, {}).empty());
  EXPECT_TRUE(LargestVisibleRect(Rect::FromXYWH(10, 10, 0, 50), {}).empty());
}

TEST(LargestVisibleRect, TheMiddleOfTheAnswerIsInsideTheWindow) {
  // What the caller actually uses is the middle of the rectangle, so it had
  // better be a point on the window and not on any of the occluders.
  const Rect win = Rect::FromXYWH(0, 0, 500, 400);
  const std::vector<Rect> occluders = {
      Rect::FromXYWH(-20, -20, 200, 200),
      Rect::FromXYWH(300, 100, 400, 100),
      Rect::FromXYWH(100, 300, 100, 300),
  };
  const Point p = LargestVisibleRect(win, occluders).middle();
  EXPECT_TRUE(win.contains(p.x, p.y));
  for (const Rect& o : occluders) {
    EXPECT_FALSE(o.contains(p.x, p.y));
  }
}
