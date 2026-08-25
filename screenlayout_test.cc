#include "screenlayout.h"

#include <string>

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

// The two-monitor layout that provokes the bug SnapToMonitor exists for: a
// large primary that doesn't start at the root origin, and a smaller monitor
// to its left.
static const std::vector<Rect> kTwoMonitors = {
    Rect::FromXYWH(0, 400, 1920, 1200),   // secondary, to the left
    Rect::FromXYWH(1920, 0, 3840, 2160),  // primary
};

TEST(SnapToMonitor, MonitorSizedWindowJustAboveItsMonitorIsPulledOn) {
  // Shadow of the Tomb Raider, borderless full screen, with a 32-pixel panel
  // reserving the top of the primary monitor.
  const Rect asked = Rect::FromXYWH(1920, -32, 3840, 2160);
  EXPECT_EQ(SnapToMonitor(asked, kTwoMonitors),
            Rect::FromXYWH(1920, 0, 3840, 2160));
}

TEST(SnapToMonitor, WindowAlreadyOnItsMonitorIsUntouched) {
  const Rect asked = Rect::FromXYWH(1920, 0, 3840, 2160);
  EXPECT_EQ(SnapToMonitor(asked, kTwoMonitors), asked);
}

TEST(SnapToMonitor, SmallerMonitorGetsItsOwnWindowSnapped) {
  const Rect asked = Rect::FromXYWH(-3, 395, 1920, 1200);
  EXPECT_EQ(SnapToMonitor(asked, kTwoMonitors),
            Rect::FromXYWH(0, 400, 1920, 1200));
}

TEST(SnapToMonitor, SizeMustMatchAMonitorExactly) {
  // One pixel short of the primary. A window that isn't trying to cover a
  // monitor is none of our business, however close it gets.
  const Rect asked = Rect::FromXYWH(1920, -32, 3840, 2159);
  EXPECT_EQ(SnapToMonitor(asked, kTwoMonitors), asked);
}

TEST(SnapToMonitor, WindowMostlyOffItsMonitorIsLeftAlone) {
  // Deliberately parked with only a third of it on the primary: that's a
  // position, not a mistake, so don't drag it back.
  const Rect asked = Rect::FromXYWH(1920, 1440, 3840, 2160);
  EXPECT_EQ(SnapToMonitor(asked, kTwoMonitors), asked);
}

TEST(SnapToMonitor, WindowSizedLikeAMonitorButOnAnotherIsLeftAlone) {
  // Same size as the secondary monitor, but sitting on the primary. It doesn't
  // cover a monitor and never did, so there's nothing to correct.
  const Rect asked = Rect::FromXYWH(2500, 500, 1920, 1200);
  EXPECT_EQ(SnapToMonitor(asked, kTwoMonitors), asked);
}

TEST(SnapToMonitor, IdenticalMonitorsSnapToTheOneTheWindowIsOn) {
  const std::vector<Rect> twins = {Rect::FromXYWH(0, 0, 1920, 1080),
                                   Rect::FromXYWH(1920, 0, 1920, 1080)};
  EXPECT_EQ(SnapToMonitor(Rect::FromXYWH(1900, -20, 1920, 1080), twins),
            Rect::FromXYWH(1920, 0, 1920, 1080))
      << "nearly on the right-hand twin, so it belongs to the right-hand twin";
  EXPECT_EQ(SnapToMonitor(Rect::FromXYWH(20, 20, 1920, 1080), twins),
            Rect::FromXYWH(0, 0, 1920, 1080));
}

TEST(SnapToMonitor, NoMonitorsIsNotACrash) {
  const Rect asked = Rect::FromXYWH(10, 10, 100, 100);
  EXPECT_EQ(SnapToMonitor(asked, {}), asked);
}

namespace {

// A big monitor with a small one to the right of it.
const std::vector<Rect> kBigAndSmall = {
    Rect::FromXYWH(0, 0, 1000, 800),
    Rect::FromXYWH(1000, 0, 400, 300),
};

}  // namespace

TEST(ShrinkToFitMonitor, WindowThatFitsIsUntouched) {
  const Rect win = Rect::FromXYWH(100, 100, 200, 150);
  EXPECT_EQ(ShrinkToFitMonitor(win, kBigAndSmall), win);
}

TEST(ShrinkToFitMonitor, WindowHangingOffTheEdgeStaysWhereItIs) {
  // Dragging a window half off the side of the screen is allowed, and this
  // must not undo it: only a window which can't fit is moved.
  const Rect win = Rect::FromXYWH(-50, 700, 200, 150);
  EXPECT_EQ(ShrinkToFitMonitor(win, kBigAndSmall), win);
}

TEST(ShrinkToFitMonitor, TooBigShrinksAndLandsOnTheMonitor) {
  // 600x500 is more than the small monitor holds either way. Mostly on it, so
  // that's the monitor it has to fit.
  const Rect win = Rect::FromXYWH(1050, 20, 600, 500);
  const Rect got = ShrinkToFitMonitor(win, kBigAndSmall);
  EXPECT_EQ(got.area(), (Area{400, 300}));
  EXPECT_EQ(got, Rect::FromXYWH(1000, 0, 400, 300));
}

TEST(ShrinkToFitMonitor, OnlyTheAxisThatCannotFitIsTouched) {
  // Too wide for the small monitor, but short enough for it, and deliberately
  // hanging off the bottom: the height and the y position both stay as they
  // are.
  const Rect win = Rect::FromXYWH(1050, 200, 600, 200);
  const Rect got = ShrinkToFitMonitor(win, kBigAndSmall);
  EXPECT_EQ(got.height(), 200);
  EXPECT_EQ(got.yMin, 200);
  EXPECT_EQ(got.width(), 400);
}

TEST(ShrinkToFitMonitor, AShrunkAxisIsFlushWithTheMonitor) {
  // Too wide either way round: whether the window overhangs the small
  // monitor's left edge or its right, the axis it had to shrink on ends up
  // filling the monitor exactly, because that's the only place it fits.
  for (int x : {900, 1100}) {
    testing::Context ctx(std::to_string(x));
    const Rect got =
        ShrinkToFitMonitor(Rect::FromXYWH(x, 50, 500, 200), kBigAndSmall);
    EXPECT_EQ(got.xMin, 1000);
    EXPECT_EQ(got.xMax, 1400);
  }
}

TEST(ShrinkToFitMonitor, NoMonitorsAtAllIsNotACrash) {
  const Rect win = Rect::FromXYWH(100, 100, 200, 150);
  EXPECT_EQ(ShrinkToFitMonitor(win, {}), win);
}
