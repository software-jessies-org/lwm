#include "placement.h"
#include "test.h"

TEST(AutoPlacer, CascadesDownAndRight) {
  AutoPlacer placer;
  const Rect target = Rect::FromXYWH(0, 0, 2000, 2000);
  const Point first = placer.NextPosition(Area{200, 100}, target);
  const Point second = placer.NextPosition(Area{200, 100}, target);
  EXPECT_EQ(first, (Point{100, 100}));
  EXPECT_EQ(second, (Point{100 + kAutoPlacementIncrement,
                          100 + kAutoPlacementIncrement}));
}

TEST(AutoPlacer, WrapsAroundPastMiddle) {
  AutoPlacer placer;
  const Rect target = Rect::FromXYWH(0, 0, 300, 300);
  // Middle is at x=150; keep placing until it wraps back to near the left.
  Point p{};
  for (int i = 0; i < 10; i++) {
    p = placer.NextPosition(Area{20, 20}, target);
  }
  EXPECT_TRUE(p.x < 150);
}

TEST(AutoPlacer, CentresWhenTooWideToCascadeButFits) {
  AutoPlacer placer;
  const Rect target = Rect::FromXYWH(0, 0, 1000, 1000);
  // Wide enough that cascading from x=100 would run off-screen, but it still
  // fits if centred.
  const Point p = placer.NextPosition(Area{950, 20}, target);
  EXPECT_EQ(p.x, (1000 - 950) / 2);
}

TEST(AutoPlacer, ResetsWhenPositionDriftsOutsideTarget) {
  AutoPlacer placer;
  // Place within a large area, drifting the cascade position well to the
  // right and down.
  const Rect bigTarget = Rect::FromXYWH(0, 0, 5000, 5000);
  for (int i = 0; i < 20; i++) {
    placer.NextPosition(Area{20, 20}, bigTarget);
  }
  // Now the monitor layout "changes": a much smaller target area no longer
  // contains the drifted cascade position, so placement must reset to its
  // top-left rather than placing off-screen.
  const Rect smallTarget = Rect::FromXYWH(0, 0, 400, 400);
  const Point p = placer.NextPosition(Area{20, 20}, smallTarget);
  EXPECT_EQ(p, (Point{100, 100}));
}
