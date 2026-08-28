#include "menulayout.h"
#include "test.h"

namespace {
const MenuStyle kStyle{/*text_height=*/16};
}  // namespace

TEST(MenuStyle, ItemHeightIsTextHeightPlusPadding) {
  EXPECT_EQ(kStyle.ItemHeight(), 16 + MenuStyle::kYPadding);
}

TEST(MenuLayout, IconFitsWithinItemHeight) {
  EXPECT_TRUE(MenuIconSize(kStyle) < kStyle.ItemHeight());
  EXPECT_TRUE(MenuIconSize(kStyle) > 0);
}

TEST(MenuLayout, MarginsAreSumOfLeftAndRight) {
  EXPECT_EQ(MenuMargins(kStyle), MenuLMargin(kStyle) + MenuRMargin(kStyle));
  EXPECT_EQ(MenuHighlightMargins(kStyle),
           MenuLHighlight(kStyle) + MenuRHighlight(kStyle));
}
