#include "screenlayout.h"

#include "strings.h"
#include "test.h"

namespace {

struct MapToNewAreasCase {
  const char* name;
  const char* before;
  const char* oldVis;
  const char* newVis;
  const char* want;
};

const MapToNewAreasCase kMapToNewAreasCases[] = {
    {"identity", "10x10+10+10", "100x100+0+0", "100x100+0+0", "10x10+10+10"},
    {"wider", "10x10+45+45", "100x100+0+0", "200x100+0+0", "10x10+95+45"},
    {"taller", "10x10+45+45", "100x100+0+0", "100x200+0+0", "10x10+45+95"},
    {"narrower", "10x10+45+45", "100x100+0+0", "50x100+0+0", "10x10+20+45"},
    {"shorter", "10x10+45+45", "100x100+0+0", "100x50+0+0", "10x10+45+20"},
    {"off left", "50x50-5+45", "500x500+0+0", "900x500+0+0", "50x50-5+45"},
    {"off right", "50x50+480+45", "500x500+0+0", "900x500+0+0",
     "50x50+880+45"},
    {"off top", "50x50+45-5", "500x500+0+0", "500x900+0+0", "50x50+45-5"},
    {"off bottom", "50x50+45+480", "500x500+0+0", "500x900+0+0",
     "50x50+45+880"},
    {"left narrow", "400x50-5+45", "500x500+0+0", "300x700+0+0",
     "300x50-5+65"},
    {"shrink", "400x500+5+5", "500x700+0+0", "300x230+0+0", "300x230+0+0"},
    {"shrink struts", "400x500+5+5", "500x700+0+0", "300x230+0+30",
     "300x230+0+30"},
    {"left multiscreen", "200x200+0+100", "500x700+0+0",
     "500x700+0+300 800x1000+500+0", "200x200+0+400"},
    {"right multiscreen", "200x200+500+100", "700x500+0+0",
     "700x500+0+300 800x1000+700+0", "200x200+1300+266"},
    {"keep left", "100x400+120+50", "700x500+0+0",
     "700x500+0+500 800x1000+500+0", "100x400+240+550"},
    {"keep right", "100x300+540+50", "700x500+0+0",
     "700x500+0+500 800x1000+500+0", "100x300+1080+175"},
    {"no straddle l", "200x300+220+50", "700x500+0+0",
     "700x500+0+500 700x700+700+0", "200x300+500+550"},
    {"no straddle r", "200x300+280+50", "700x500+0+0",
     "700x500+0+500 700x700+700+0", "200x300+700+100"},
    {"keep left ymax", "100x500+120+0", "700x500+0+0",
     "700x500+0+500 800x1000+700+0", "100x500+280+500"},
    {"keep right ymax", "100x500+540+0", "700x500+0+0",
     "700x500+0+500 800x1000+700+0", "100x1000+1260+0"},
    {"to small l", "200x500+100+500", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "200x500+38+0"},
    {"to small l wide", "800x500+100+500", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "700x500+0+0"},
    {"to small r", "200x1000+1000+0", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "200x500+384+0"},
    {"to small r wide", "800x1000+1000+0", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "700x500+0+0"},
    {"full size no crash", "500x500+0+0", "500x500+0+0", "900x500+0+0",
     "500x500+0+0"},
    {"full width no crash", "500x300+0+50", "500x500+0+0", "900x500+0+0",
     "500x300+0+50"},
};

std::vector<Rect> parseRects(const std::string& rects) {
  std::vector<Rect> res;
  for (const std::string& r : Split(rects, " ")) {
    const Rect rect = Rect::Parse(r);
    if (rect.empty()) {
      return std::vector<Rect>();
    }
    res.push_back(rect);
  }
  return res;
}

}  // namespace

TEST(ScreenLayout, MapToNewAreas) {
  for (const MapToNewAreasCase& tc : kMapToNewAreasCases) {
    testing::Context ctx(tc.name);
    const Rect in = Rect::Parse(tc.before);
    ASSERT_FALSE(in.empty()) << "tc.before parse failed (" << tc.before
                              << ")";
    const Rect want = Rect::Parse(tc.want);
    ASSERT_FALSE(want.empty()) << "tc.want parse failed (" << tc.want << ")";
    const std::vector<Rect> oldVis = parseRects(tc.oldVis);
    ASSERT_FALSE(oldVis.empty()) << "oldVis parse failed (" << tc.oldVis
                                  << ")";
    const std::vector<Rect> newVis = parseRects(tc.newVis);
    ASSERT_FALSE(newVis.empty()) << "newVis parse failed (" << tc.newVis
                                  << ")";
    const Rect got = MapToNewAreas(in, oldVis, newVis);
    EXPECT_EQ(got, want);
  }
}

TEST(ScreenLayout, AreasMinusStruts) {
  const std::vector<Rect> in = {Rect::FromXYWH(0, 0, 1000, 800)};
  const EWMHStrut strut{10, 20, 30, 40};
  const std::vector<Rect> got = areasMinusStruts(in, strut);
  const std::vector<Rect> want = {Rect{10, 30, 980, 760}};
  EXPECT_EQ(got, want);
}

TEST(ScreenLayout, AreasMinusStrutsNoStrut) {
  const std::vector<Rect> in = {Rect::FromXYWH(0, 0, 1000, 800)};
  const std::vector<Rect> got = areasMinusStruts(in, EWMHStrut{0, 0, 0, 0});
  EXPECT_EQ(got, in);
}

TEST(PrimaryArea, PicksLargest) {
  const std::vector<Rect> areas = {
      Rect::FromXYWH(0, 0, 800, 600),
      Rect::FromXYWH(800, 0, 1920, 1080),
  };
  EXPECT_EQ(PrimaryArea(areas), areas[1]);
}

TEST(PrimaryArea, TiesBrokenByTopmostThenLeftmost) {
  const std::vector<Rect> sameSize = {
      Rect::FromXYWH(100, 50, 800, 600),
      Rect::FromXYWH(0, 50, 800, 600),
      Rect::FromXYWH(0, 0, 800, 600),
  };
  EXPECT_EQ(PrimaryArea(sameSize), Rect::FromXYWH(0, 0, 800, 600));
}

TEST(FindBestScreenFor, OverlapWins) {
  const std::vector<Rect> areas = {
      Rect::FromXYWH(0, 0, 700, 500),
      Rect::FromXYWH(700, 0, 800, 1000),
  };
  // Mostly inside the second screen.
  EXPECT_EQ(findBestScreenFor(Rect::FromXYWH(650, 10, 200, 200), areas),
            areas[1]);
}

TEST(FindBestScreenFor, NoOverlapPicksNearest) {
  // Regression case: ImageMagick's `display` places its window off every
  // screen, and lwm has to pick something sensible rather than crash.
  const std::vector<Rect> areas = {
      Rect::FromXYWH(0, 0, 700, 500),
      Rect::FromXYWH(700, 0, 800, 1000),
  };
  // Well off to the left of both screens: nearer to the first.
  EXPECT_EQ(findBestScreenFor(Rect::FromXYWH(-500, 0, 50, 50), areas),
            areas[0]);
}

TEST(MakeVisible, AlreadyVisibleIsUnchanged) {
  const std::vector<Rect> areas = {Rect::FromXYWH(0, 0, 1000, 800)};
  const Rect r = Rect::FromXYWH(100, 100, 200, 200);
  EXPECT_EQ(makeVisible(r, areas), r);
}

TEST(MakeVisible, OffscreenGetsTranslatedBackOn) {
  const std::vector<Rect> areas = {Rect::FromXYWH(0, 0, 1000, 800)};
  const Rect r = Rect::FromXYWH(-50, -50, 200, 200);
  EXPECT_EQ(makeVisible(r, areas), Rect::FromXYWH(0, 0, 200, 200));
}

TEST(MakeVisible, TooWideGetsClampedToScreenWidth) {
  const std::vector<Rect> areas = {Rect::FromXYWH(0, 0, 1000, 800)};
  const Rect r = Rect::FromXYWH(-100, 100, 1200, 200);
  const Rect got = makeVisible(r, areas);
  EXPECT_EQ(got.xMin, 0);
  EXPECT_EQ(got.xMax, 1000);
}
