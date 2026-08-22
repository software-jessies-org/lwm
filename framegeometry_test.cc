#include "framegeometry.h"
#include "test.h"

namespace {
// Representative of the default resources: a few pixels of border, a
// title bar tall enough for a normal font.
const FrameStyle kStyle{/*border_width=*/2, /*top_border_width=*/2,
                        /*text_height=*/16};
}  // namespace

TEST(FrameStyle, TitleBarHeightIsTextHeightPlusBorder) {
  EXPECT_EQ(kStyle.TitleBarHeight(), 18);
}

TEST(CloseBounds, ClickableAreaIsLargerThanTheDrawnCross) {
  // Usability: it should be easy to hit the close icon, so the clickable
  // area (displayBounds=false) is deliberately bigger than what's drawn
  // (displayBounds=true).
  const Rect drawn = CloseBounds(kStyle, /*displayBounds=*/true);
  const Rect clickable = CloseBounds(kStyle, /*displayBounds=*/false);
  EXPECT_EQ(drawn.xMin, clickable.xMin);
  EXPECT_EQ(drawn.yMin, clickable.yMin);
  EXPECT_TRUE(clickable.xMax > drawn.xMax);
  EXPECT_TRUE(clickable.yMax > drawn.yMax);
}

TEST(TitleBarBounds, SpansFromCloseIconToWindowEdge) {
  const Rect r = TitleBarBounds(kStyle, /*windowWidth=*/500);
  EXPECT_EQ(r.xMin, kStyle.TitleBarHeight());
  EXPECT_EQ(r.yMin, kStyle.top_border_width);
  EXPECT_EQ(r.xMax, 500 - kStyle.TitleBarHeight());
  EXPECT_EQ(r.yMax, kStyle.TitleBarHeight());
}

TEST(EdgeBoundsFor, LeftEdgeExtendsOnePixelPastTheFrame) {
  const Rect frame = Rect::FromXYWH(0, 0, 400, 300);
  const Rect r = EdgeBoundsFor(kStyle, frame, ELeft);
  // The -1 fudge: the frame's own 1px X border sits outside its coordinate
  // space, so the hit-test region must reach one pixel further left than
  // (0,0) to cover it.
  EXPECT_EQ(r.xMin, -1);
  EXPECT_EQ(r.xMax, kStyle.TitleBarHeight());
}

TEST(EdgeBoundsFor, RightEdgeExtendsOnePixelPastTheFrame) {
  const Rect frame = Rect::FromXYWH(0, 0, 400, 300);
  const Rect r = EdgeBoundsFor(kStyle, frame, ERight);
  EXPECT_EQ(r.xMin, frame.width() - kStyle.TitleBarHeight());
  // The +1 fudge, mirroring the left edge's -1.
  EXPECT_EQ(r.xMax, frame.width() + 1);
}

TEST(EdgeBoundsFor, NoneIsTheInteriorTitleBarArea) {
  const Rect frame = Rect::FromXYWH(0, 0, 400, 300);
  const Rect r = EdgeBoundsFor(kStyle, frame, ENone);
  const int inset = kStyle.TitleBarHeight();
  EXPECT_EQ(r, (Rect{inset, inset, frame.width() - inset,
                     frame.height() - inset}));
}

TEST(FrameContentConversion, RoundTrips) {
  const Rect content = Rect::FromXYWH(100, 100, 400, 300);
  const Rect frame = FrameFromContentRect(kStyle, content);
  EXPECT_EQ(ContentFromFrameRect(kStyle, frame), content);
}

TEST(FrameContentConversion, FrameIsBiggerThanContent) {
  const Rect content = Rect::FromXYWH(100, 100, 400, 300);
  const Rect frame = FrameFromContentRect(kStyle, content);
  EXPECT_EQ(frame.xMin, content.xMin - kStyle.border_width);
  EXPECT_EQ(frame.yMin, content.yMin - kStyle.TitleBarHeight());
  EXPECT_EQ(frame.xMax, content.xMax + kStyle.border_width);
  EXPECT_EQ(frame.yMax, content.yMax + kStyle.border_width);
}
