#include "geometry.h"
#include "test.h"

TEST(Rect, ParseValid) {
  EXPECT_EQ(Rect::Parse("100x100+10+10"), (Rect{10, 10, 110, 110}));
  EXPECT_EQ(Rect::Parse("50x50-5+45"), (Rect{-5, 45, 45, 95}));
  EXPECT_EQ(Rect::Parse("50x50+45-5"), (Rect{45, -5, 95, 45}));
  EXPECT_EQ(Rect::Parse("1280x960+23+25"), (Rect{23, 25, 1303, 985}));
}

TEST(Rect, ParseInvalid) {
  // No 'x' separator.
  EXPECT_TRUE(Rect::Parse("garbage").empty());
  // No sign before the offsets.
  EXPECT_TRUE(Rect::Parse("100x100").empty());
  // Zero width or height is refused, since Rect::Parse uses the empty rect
  // to signal failure and a genuine zero-size rect would be indistinguishable.
  EXPECT_TRUE(Rect::Parse("0x100+0+0").empty());
  EXPECT_TRUE(Rect::Parse("100x0+0+0").empty());
  EXPECT_TRUE(Rect::Parse("").empty());
}
