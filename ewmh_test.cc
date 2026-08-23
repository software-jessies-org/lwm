// Tests for the EWMH state and stacking policy: new_state's add/remove/toggle,
// fix_stack's ordering rules, and the recursion guard that stops the two
// calling each other for ever.

#include <algorithm>

#include "client.h"
#include "disp.h"
#include "ewmh.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

namespace {

enum { kRemove = 0, kAdd = 1, kToggle = 2 };

// Where the client's frame sits in the root window's stacking order, counting
// from the bottom, or -1 if it isn't there. The root has lwm's own popup and
// menu windows among its children too, so only the relative order of the
// frames means anything.
int stackIndex(wmtest::World* world, Client* c) {
  const std::vector<Window> stack =
      world->server().ChildrenOf(LScr::I->Root());
  const auto it = std::find(stack.begin(), stack.end(), c->parent);
  return it == stack.end() ? -1 : int(it - stack.begin());
}

// A _NET_WM_STATE client message, as a pager or the application itself would
// send it.
void sendStateMessage(wmtest::World* world,
                      Client* c,
                      uint32_t action,
                      Atom state) {
  xcb_client_message_event_t e{};
  e.response_type = XCB_CLIENT_MESSAGE;
  e.format = 32;
  e.window = c->window;
  e.type = ewmh_atom[_NET_WM_STATE];
  e.data.data32[0] = action;
  e.data.data32[1] = state;
  world->server().PushEvent(e);
  ProcessPendingEvents();
}

}  // namespace

TEST(NewState, AddRemoveToggle) {
  EXPECT_FALSE(new_state(kRemove, true));
  EXPECT_FALSE(new_state(kRemove, false));
  EXPECT_TRUE(new_state(kAdd, false));
  EXPECT_TRUE(new_state(kAdd, true));
  EXPECT_TRUE(new_state(kToggle, false));
  EXPECT_FALSE(new_state(kToggle, true));
  // Anything else leaves the state alone rather than guessing. The two
  // "bad action in _NET_WM_STATE (99)" lines this puts on stderr are the
  // point of the case, not a test failure.
  EXPECT_TRUE(new_state(99, true));
  EXPECT_FALSE(new_state(99, false));
}

TEST(FixStack, OrdersBelowDesktopNormalAbove) {
  wmtest::World world;
  Client* normal = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* below = world.MapClientWindow(Rect::FromXYWH(20, 20, 100, 100));
  Client* desktop = world.MapClientWindow(Rect::FromXYWH(30, 30, 100, 100));
  Client* above = world.MapClientWindow(Rect::FromXYWH(40, 40, 100, 100));
  ASSERT_TRUE(normal && below && desktop && above);

  below->wstate.below = true;
  desktop->wtype = WTypeDesktop;
  above->wstate.above = true;
  fix_stack();

  // Bottom to top: the desktop underneath everything, then anything asking to
  // be below, then ordinary windows, then anything asking to be above.
  EXPECT_TRUE(stackIndex(&world, desktop) < stackIndex(&world, below))
      << "desktop=" << stackIndex(&world, desktop)
      << " below=" << stackIndex(&world, below);
  EXPECT_TRUE(stackIndex(&world, below) < stackIndex(&world, normal))
      << "below=" << stackIndex(&world, below)
      << " normal=" << stackIndex(&world, normal);
  EXPECT_TRUE(stackIndex(&world, normal) < stackIndex(&world, above))
      << "normal=" << stackIndex(&world, normal)
      << " above=" << stackIndex(&world, above);
}

TEST(FixStack, FullScreenGoesAboveDocks) {
  wmtest::World world;
  Client* dock = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* full = world.MapClientWindow(Rect::FromXYWH(20, 20, 100, 100));
  ASSERT_TRUE(dock && full);

  dock->wtype = WTypeDock;
  full->wstate.fullscreen = true;
  fix_stack();

  // A dock is raised, but a full-screen window is raised after it. The comment
  // in fix_stack() is explicit that this is deliberate: without it the panel
  // ends up over the top of a full-screen window.
  EXPECT_TRUE(stackIndex(&world, dock) < stackIndex(&world, full))
      << "dock=" << stackIndex(&world, dock)
      << " full=" << stackIndex(&world, full);
}

TEST(FixStack, DockMarkedBelowIsNotRaised) {
  wmtest::World world;
  Client* normal = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* dock = world.MapClientWindow(Rect::FromXYWH(20, 20, 100, 100));
  ASSERT_TRUE(normal && dock);

  dock->wtype = WTypeDock;
  dock->wstate.below = true;
  fix_stack();

  EXPECT_TRUE(stackIndex(&world, dock) < stackIndex(&world, normal))
      << "a dock that asked to be below must stay below";
}

TEST(EwmhSetClientList, RecursionGuardStopsTheRestackFeedingBackIn) {
  wmtest::World world;
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(20, 20, 100, 100));
  Client* c = world.MapClientWindow(Rect::FromXYWH(30, 30, 100, 100));
  ASSERT_TRUE(a && b && c);
  a->wstate.below = true;
  c->wstate.above = true;

  world.server().ClearCalls();
  ewmh_set_client_list();

  // fix_stack() raises and lowers, and every Raise() and Lower() ends by
  // asking for the client list to be updated again. The guard means the
  // property is written exactly once per outermost call, not once per restack.
  int writes = 0;
  for (const std::string& call : world.server().Calls()) {
    if (call.find("(_NET_CLIENT_LIST)") != std::string::npos) {
      writes++;
    }
  }
  EXPECT_EQ(writes, 1);
}

TEST(EwmhChangeState, ClientMessageSetsAndClearsAboveState) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->wstate.above);

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_ABOVE]);
  EXPECT_TRUE(c->wstate.above);

  sendStateMessage(&world, c, kToggle, ewmh_atom[_NET_WM_STATE_ABOVE]);
  EXPECT_FALSE(c->wstate.above);

  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_ABOVE]);
  EXPECT_FALSE(c->wstate.above);
}

TEST(EwmhChangeState, FullScreenFillsTheScreenAndRestoresOnExit) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  EXPECT_TRUE(c->wstate.fullscreen);
  EXPECT_EQ(c->ContentRect(), LScr::I->GetPrimaryVisibleArea(false));

  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  EXPECT_FALSE(c->wstate.fullscreen);
  EXPECT_EQ(c->ContentRect(), before)
      << "leaving full screen must put the window back where it was";
}
