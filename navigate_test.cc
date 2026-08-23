// Tests for the pure part of keyboard focus navigation: which window an arrow
// key picks. The grabs and the focus change built on this are in disp.cc.

#include "navigate.h"

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
