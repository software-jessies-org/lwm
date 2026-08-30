#include <xcb/randr.h>

#include "lwm.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

// Tests for the RandR screen-change handling in lwm.cc.
//
// These go at rrScreenChangeNotify() rather than randrEvent(), because
// randrEvent() first checks the run-time event number RandR was given by the
// real server, which the fake one doesn't hand out. Everything below that
// check is what's interesting here anyway.

namespace {

// A ScreenChangeNotify as the server sends it. The size in the event is only
// read by the "is this a nonsense size" guard, so it's the same in every case
// here and comfortably above the threshold; what the layout has actually
// become comes from the server, which is what the fake is standing in for.
xcb_randr_screen_change_notify_event_t ScreenChange(xcb_timestamp_t config) {
  xcb_randr_screen_change_notify_event_t ev = {};
  ev.width = 1280;
  ev.height = 1024;
  ev.timestamp = config;
  ev.config_timestamp = config;
  return ev;
}

void Deliver(const xcb_randr_screen_change_notify_event_t& ev) {
  rrScreenChangeNotify((xcb_generic_event_t*)&ev);
}

const std::vector<Rect> kOneMonitor = {Rect::FromXYWH(0, 0, 1280, 1024)};
const std::vector<Rect> kTwoMonitors = {Rect::FromXYWH(0, 0, 1280, 1024),
                                        Rect::FromXYWH(1280, 0, 1920, 1080)};

}  // namespace

// The regression test for the bug that made plugging a monitor into a laptop
// do nothing until lwm was sent a SIGHUP.
//
// A single reconfiguration produces several notifications, and lwm used to
// drop any whose config_timestamp matched the previous one's, on the grounds
// that they were repeats. They aren't: config_timestamp is the server's
// lastConfigTime, which moves when the set of available outputs and modes
// changes and not when a CRTC is reconfigured. Connecting the monitor moved
// it, and lwm acted on that event - at which point nothing had changed yet -
// and then the change which mattered arrived under the same stamp and was
// thrown away.
TEST(RandR, ActsOnAScreenChangeWhoseConfigTimestampRepeats) {
  wmtest::World world;
  world.server().SetVisibleAreas(kOneMonitor);
  Deliver(ScreenChange(0x1234));
  ASSERT_EQ(1, (int)LScr::I->VisibleAreas(false).size());

  // The monitor comes on, under the config timestamp we've already seen.
  world.server().SetVisibleAreas(kTwoMonitors);
  Deliver(ScreenChange(0x1234));

  const std::vector<Rect> got = LScr::I->VisibleAreas(false);
  ASSERT_EQ(2, (int)got.size()) << "the second notification was ignored";
  EXPECT_EQ(kTwoMonitors[0], got[0]);
  EXPECT_EQ(kTwoMonitors[1], got[1]);
}

// And the same on the way out: unplugging is a screen change like any other.
TEST(RandR, ActsOnAMonitorGoingAway) {
  wmtest::World world;
  world.server().SetVisibleAreas(kTwoMonitors);
  Deliver(ScreenChange(0x1234));
  ASSERT_EQ(2, (int)LScr::I->VisibleAreas(false).size());

  world.server().SetVisibleAreas(kOneMonitor);
  Deliver(ScreenChange(0x1234));

  const std::vector<Rect> got = LScr::I->VisibleAreas(false);
  ASSERT_EQ(1, (int)got.size()) << "the unplug was ignored";
  EXPECT_EQ(kOneMonitor[0], got[0]);
}

// What the timestamp check was there for in the first place: one
// reconfiguration arrives as a burst of notifications, and rearranging every
// window on the display once per notification is work worth not doing. The
// duplicates are now recognised by the layout being the one we already have,
// which drops all of them and can't drop a real change.
TEST(RandR, RepeatedNotificationsMoveNoWindows) {
  wmtest::World world;
  world.server().SetVisibleAreas(kTwoMonitors);
  Deliver(ScreenChange(0x1234));
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);

  world.server().ClearCalls();
  for (int i = 0; i < 4; i++) {
    Deliver(ScreenChange(0x2000 + i));
  }
  EXPECT_EQ(0, (int)world.server().CallsMatching("ConfigureWindow(").size())
      << "an unchanged layout still moved windows about";
}

// A screen size the server can't mean. lwm sees 320x200 when the laptop is
// told to switch to an external monitor which isn't there, and taking it at
// its word crushes every window on the display into the corner.
TEST(RandR, IgnoresNonsensicalScreenSizes) {
  wmtest::World world;
  world.server().SetVisibleAreas(kOneMonitor);
  Deliver(ScreenChange(0x1234));

  world.server().SetVisibleAreas(kTwoMonitors);
  xcb_randr_screen_change_notify_event_t tiny = ScreenChange(0x5678);
  tiny.width = 320;
  tiny.height = 200;
  Deliver(tiny);

  EXPECT_EQ(1, (int)LScr::I->VisibleAreas(false).size())
      << "a 320x200 screen change was believed";
}
