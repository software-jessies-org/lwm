#include "sizelimits.h"
#include "test.h"

TEST(DimensionLimiter, SnapsToIncrementWhenRightEdgeMoved) {
  // base=4, increment=10: conforming sizes are 4, 14, 24, ...
  const DimensionLimiter limiter(20, 500, 4, 10);
  int newMin = 0;
  int newMax = 130;  // size 130 -> (130-4)%10 == 6, not conforming.
  limiter.Limit(/*old_min=*/0, /*old_max=*/104, newMin, newMax);
  // new_min unchanged from old_min, so the max end is the one adjusted.
  EXPECT_EQ(newMin, 0);
  EXPECT_EQ(newMax, 124);  // size 124: (124-4)/10 == 12 exactly.
}

TEST(DimensionLimiter, SnapsToIncrementWhenLeftEdgeMoved) {
  const DimensionLimiter limiter(20, 500, 4, 10);
  int newMin = -126;  // size 126 -> (126-4)%10 == 2, not conforming.
  int newMax = 0;
  limiter.Limit(/*old_min=*/0, /*old_max=*/0, newMin, newMax);
  // new_min changed from old_min, so the min end is the one adjusted.
  EXPECT_EQ(newMax, 0);
  EXPECT_EQ(newMin, -124);  // size 124 again.
}

TEST(DimensionLimiter, ClampsToMinimumGrowingFromRightEdge) {
  const DimensionLimiter limiter(20, 500, 4, 10);
  int newMin = 0;
  int newMax = 15;  // size 15 < min 20.
  limiter.Limit(/*old_min=*/0, /*old_max=*/15, newMin, newMax);
  EXPECT_EQ(newMin, 0);
  EXPECT_EQ(newMax, 20);
}

TEST(DimensionLimiter, ClampsToMaximumGrowingFromLeftEdge) {
  const DimensionLimiter limiter(20, 500, 4, 10);
  int newMin = -550;
  int newMax = 50;  // size 600 > max 500.
  limiter.Limit(/*old_min=*/0, /*old_max=*/50, newMin, newMax);
  EXPECT_EQ(newMax, 50);
  EXPECT_EQ(newMin, -450);  // size clamped to 500.
}

TEST(DimensionLimiter, NoChangeWhenAlreadyConforming) {
  const DimensionLimiter limiter(20, 500, 4, 10);
  int newMin = 0;
  int newMax = 124;  // already a conforming size.
  limiter.Limit(/*old_min=*/0, /*old_max=*/104, newMin, newMax);
  EXPECT_EQ(newMin, 0);
  EXPECT_EQ(newMax, 124);
}

TEST(DimensionLimiter, MinGetsClampedToMinMin) {
  // Constructor clamps a silly min up to 10.
  const DimensionLimiter limiter(0, 500, 0, 10);
  int newMin = 0;
  int newMax = 5;  // size 5, below the effective min of 10.
  limiter.Limit(/*old_min=*/0, /*old_max=*/5, newMin, newMax);
  EXPECT_EQ(newMax, 10);
}

TEST(DimensionLimiter, DisplayableSize) {
  const DimensionLimiter limiter(20, 500, 4, 10);
  EXPECT_EQ(limiter.DisplayableSize(124), 12);
  EXPECT_EQ(limiter.DisplayableSize(4), 0);
  EXPECT_EQ(limiter.DisplayableSize(24), 2);
}
