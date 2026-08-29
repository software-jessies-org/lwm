// Tests for which monitor a new window opens on: LScr::PlacementAreaFor, and
// the full-screen detection behind it.
//
// The case these are about is a game: it fills a monitor, and a window some
// other program opens while it's up has no business landing underneath it.

#include "client.h"
#include "ewmh.h"
#include "lwm.h"
#include "manage.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

namespace {

// A wide primary monitor with a smaller one beside it. PrimaryArea picks the
// larger, so kBig is the primary and kSmall is where a displaced window goes.
const Rect kBig = Rect::FromXYWH(0, 0, 1200, 1000);
const Rect kSmall = Rect::FromXYWH(1200, 0, 800, 600);

void twoMonitors() {
  LScr::I->SetVisibleAreas({kBig, kSmall});
}

// _MOTIF_WM_HINTS asking for no decorations, which is how a client says it
// wants no frame from lwm, and what a game in borderless-fullscreen mode
// sets. 2 is MWM_HINTS_DECORATIONS, and the third word names no decorations
// at all; both constants live in manage.cc, which is where they're read.
void setUndecorated(wmtest::World* world, Window w) {
  world->server().SetProperty32(w, motif_wm_hints, motif_wm_hints,
                                {2, 0, 0, 0, 0});
}

// A window sized exactly to `rect` and asking for no decorations: what a
// Steam game running borderless-fullscreen looks like to lwm. Nothing here
// mentions full screen, which is the point - the geometry is the only sign.
Client* addBorderlessFullScreen(wmtest::World* world, const Rect& rect) {
  const Window w = world->AddClientWindow(rect);
  setUndecorated(world, w);
  return world->MapWindow(w);
}

// An ordinary window with no position of its own, so lwm places it.
Client* addPlacedWindow(wmtest::World* world) {
  return world->MapClientWindow(Rect::FromXYWH(0, 0, 400, 300));
}

void setPid(wmtest::World* world, Window w, uint32_t pid) {
  world->server().SetProperty32(w, ewmh_atom[_NET_WM_PID], XCB_ATOM_CARDINAL,
                                {pid});
}

}  // namespace

TEST(PlacementArea, IsThePrimaryMonitorWhenNothingIsFullScreen) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  Client* c = addPlacedWindow(&world);
  EXPECT_EQ(LScr::I->PlacementAreaFor(c), kBig);
}

TEST(PlacementArea, AvoidsAMonitorFilledByAnotherProgramsWindow) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  Client* game = addBorderlessFullScreen(&world, kBig);
  // lwm left the window covering the monitor, which is what makes it look
  // full screen at all.
  EXPECT_EQ(game->ContentRect(), kBig);
  Client* other = addPlacedWindow(&world);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kSmall);
}

TEST(PlacementArea, NewWindowIsActuallyPlacedOnTheOtherMonitor) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  addBorderlessFullScreen(&world, kBig);
  Client* other = addPlacedWindow(&world);
  const Rect r = other->FrameRect();
  EXPECT_TRUE(r.xMin >= kSmall.xMin);
  EXPECT_TRUE(r.xMax <= kSmall.xMax);
}

TEST(PlacementArea, NoticesAWindowWhichAskedForFullScreenState) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  // A framed window, only a quarter of the monitor's size, which says it is
  // full screen. lwm grows it to the primary monitor on the way in, and it
  // counts because it said so rather than because of its geometry.
  const Window w = world.AddClientWindow(Rect::FromXYWH(0, 0, 600, 500));
  world.server().SetProperty32(w, ewmh_atom[_NET_WM_STATE], XCB_ATOM_ATOM,
                               {ewmh_atom[_NET_WM_STATE_FULLSCREEN]});
  Client* game = world.MapWindow(w);
  EXPECT_TRUE(game->wstate.fullscreen);
  Client* other = addPlacedWindow(&world);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kSmall);
}

TEST(PlacementArea, LeavesTheFullScreenProgramsOwnWindowsWithIt) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  const Window gw = world.AddClientWindow(kBig);
  setUndecorated(&world, gw);
  setPid(&world, gw, 4242);
  world.MapWindow(gw);

  const Window dw = world.AddClientWindow(Rect::FromXYWH(0, 0, 400, 300));
  setPid(&world, dw, 4242);
  Client* dialog = world.MapWindow(dw);
  EXPECT_EQ(LScr::I->PlacementAreaFor(dialog), kBig);
}

TEST(PlacementArea, ADifferentProcessIsADifferentProgram) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  const Window gw = world.AddClientWindow(kBig);
  setUndecorated(&world, gw);
  setPid(&world, gw, 4242);
  world.MapWindow(gw);

  const Window dw = world.AddClientWindow(Rect::FromXYWH(0, 0, 400, 300));
  setPid(&world, dw, 4243);
  Client* other = world.MapWindow(dw);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kSmall);
}

TEST(PlacementArea, TheSamePidOnADifferentMachineIsADifferentProgram) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  const Window gw = world.AddClientWindow(kBig);
  setUndecorated(&world, gw);
  setPid(&world, gw, 4242);
  world.server().SetProperty8(gw, XCB_ATOM_WM_CLIENT_MACHINE, XCB_ATOM_STRING,
                              "here");
  world.MapWindow(gw);

  const Window dw = world.AddClientWindow(Rect::FromXYWH(0, 0, 400, 300));
  setPid(&world, dw, 4242);
  world.server().SetProperty8(dw, XCB_ATOM_WM_CLIENT_MACHINE, XCB_ATOM_STRING,
                              "elsewhere");
  Client* other = world.MapWindow(dw);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kSmall);
}

TEST(PlacementArea, AClientLeaderKeepsAWindowWithItsProgram) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  const Window gw = world.AddClientWindow(kBig);
  setUndecorated(&world, gw);
  world.server().SetProperty32(gw, wm_client_leader, XCB_ATOM_WINDOW, {gw});
  world.MapWindow(gw);

  const Window dw = world.AddClientWindow(Rect::FromXYWH(0, 0, 400, 300));
  world.server().SetProperty32(dw, wm_client_leader, XCB_ATOM_WINDOW, {gw});
  Client* dialog = world.MapWindow(dw);
  EXPECT_EQ(LScr::I->PlacementAreaFor(dialog), kBig);
}

TEST(PlacementArea, AWindowGroupKeepsAWindowWithItsProgram) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  const Window gw = world.AddClientWindow(kBig);
  setUndecorated(&world, gw);
  world.server().Get(gw)->wm_hints.ok = true;
  world.server().Get(gw)->wm_hints.window_group = gw;
  world.MapWindow(gw);

  const Window dw = world.AddClientWindow(Rect::FromXYWH(0, 0, 400, 300));
  world.server().Get(dw)->wm_hints.ok = true;
  world.server().Get(dw)->wm_hints.window_group = gw;
  Client* dialog = world.MapWindow(dw);
  EXPECT_EQ(LScr::I->PlacementAreaFor(dialog), kBig);
}

TEST(PlacementArea, ATransientOfTheFullScreenWindowStaysWithIt) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  const Window gw = world.AddClientWindow(kBig);
  setUndecorated(&world, gw);
  world.MapWindow(gw);

  const Window dw = world.AddClientWindow(Rect::FromXYWH(0, 0, 400, 300));
  world.server().Get(dw)->transient_for = gw;
  Client* dialog = world.MapWindow(dw);
  EXPECT_EQ(LScr::I->PlacementAreaFor(dialog), kBig);
}

TEST(PlacementArea, AMaximisedWindowIsNotFullScreen) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  Client* big = world.MapClientWindow(Rect::FromXYWH(0, 0, 400, 300));
  big->SetMaximized(true, true);
  Client* other = addPlacedWindow(&world);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kBig);
}

TEST(PlacementArea, AHiddenFullScreenWindowBlocksNothing) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  Client* game = addBorderlessFullScreen(&world, kBig);
  Client* other = addPlacedWindow(&world);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kSmall);
  game->Hide();
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kBig);
}

TEST(PlacementArea, StaysPutWhenThereIsNowhereElseToGo) {
  wmtest::World world(1200, 1000);
  LScr::I->SetVisibleAreas({kBig});
  addBorderlessFullScreen(&world, kBig);
  Client* other = addPlacedWindow(&world);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kBig);
}

TEST(PlacementArea, AFullScreenWindowOnAnotherMonitorChangesNothing) {
  wmtest::World world(2000, 1000);
  twoMonitors();
  addBorderlessFullScreen(&world, kSmall);
  Client* other = addPlacedWindow(&world);
  EXPECT_EQ(LScr::I->PlacementAreaFor(other), kBig);
}
