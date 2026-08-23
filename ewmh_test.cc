// Tests for the EWMH state and stacking policy: new_state's add/remove/toggle,
// fix_stack's ordering rules, and the recursion guard that stops the two
// calling each other for ever.

#include <algorithm>

#include "client.h"
#include "disp.h"
#include "ewmh.h"
#include "manage.h"
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

TEST(EwmhChangeState, FullScreenPlacesTheClientAtItsFrameOrigin) {
  wmtest::World world(3000, 1500);
  // Two monitors, the larger (and so the one full-screen picks) starting at
  // x=1000. The bug this catches is invisible on a layout whose primary
  // screen starts at the root origin, which is why the areas are set up first.
  LScr::I->SetVisibleAreas({Rect::FromXYWH(0, 0, 1000, 800),
                            Rect::FromXYWH(1000, 0, 2000, 1500)});
  Client* c = world.MapClientWindow(Rect::FromXYWH(1100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  ASSERT_TRUE(c->wstate.fullscreen);

  const Rect screen = LScr::I->GetPrimaryVisibleArea(false);
  ASSERT_EQ(screen.xMin, 1000);
  EXPECT_EQ(c->ContentRect(), screen);
  EXPECT_EQ(c->FrameRect(), screen)
      << "a full-screen window has no furniture to make room for";
  EXPECT_EQ(world.server().Get(c->parent)->rect, screen);
  // The client window is reparented, so the fake reports its geometry relative
  // to the frame. It has to cover the frame exactly; giving X the root
  // coordinates here pushed it off to the right by the frame's own x origin.
  EXPECT_EQ(world.server().Get(c->window)->rect,
            Rect::FromXYWH(0, 0, screen.width(), screen.height()));
}

TEST(EwmhChangeState, MovingAFullScreenWindowKeepsTheClientOnItsFrame) {
  wmtest::World world(3000, 1500);
  LScr::I->SetVisibleAreas({Rect::FromXYWH(0, 0, 1000, 800),
                            Rect::FromXYWH(1000, 0, 2000, 1500)});
  Client* c = world.MapClientWindow(Rect::FromXYWH(1100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  ASSERT_TRUE(c->wstate.fullscreen);

  const Rect moved = Rect::Translate(c->ContentRect(), Point{-40, 25});
  c->MoveTo(moved);

  EXPECT_EQ(world.server().Get(c->parent)->rect, moved)
      << "the frame follows the content exactly while full screen";
  EXPECT_EQ(world.server().Get(c->window)->rect,
            Rect::FromXYWH(0, 0, moved.width(), moved.height()))
      << "moving must not reintroduce the furniture offset";
}

namespace {

// The visible-area layout the maximisation tests use: one screen, offset from
// the root origin so that a maximised window landing at 0,0 by accident is a
// visible failure rather than a coincidence.
const Rect kScreen = Rect::FromXYWH(200, 100, 1000, 800);

void setOneOffsetScreen() {
  LScr::I->SetVisibleAreas({kScreen});
}

// The content rect a window maximised on both axes should end up with: the
// screen, less the space the frame's furniture takes.
Rect maximizedContent(const Rect& area) {
  return Client::ContentFromFrameRect(area);
}

}  // namespace

TEST(Maximize, BothAxesFillTheScreenAndRestoreOnRemove) {
  wmtest::World world(1400, 1000);
  setOneOffsetScreen();
  Client* c = world.MapClientWindow(Rect::FromXYWH(300, 200, 400, 300));
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);
  EXPECT_TRUE(c->wstate.maximized_vert);
  EXPECT_TRUE(c->wstate.maximized_horz);
  EXPECT_EQ(c->FrameRect(), kScreen)
      << "it's the frame that fills the screen, furniture and all";
  EXPECT_EQ(c->ContentRect(), maximizedContent(kScreen));

  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);
  EXPECT_FALSE(c->IsMaximized());
  EXPECT_EQ(c->ContentRect(), before)
      << "un-maximising must put the window back where it started";
}

TEST(Maximize, VerticalOnlyKeepsTheWindowsOwnWidth) {
  wmtest::World world(1400, 1000);
  setOneOffsetScreen();
  Client* c = world.MapClientWindow(Rect::FromXYWH(300, 200, 400, 300));
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  EXPECT_TRUE(c->wstate.maximized_vert);
  EXPECT_FALSE(c->wstate.maximized_horz);
  EXPECT_EQ(c->FrameRect().yMin, kScreen.yMin);
  EXPECT_EQ(c->FrameRect().yMax, kScreen.yMax);
  EXPECT_EQ(c->ContentRect().xMin, before.xMin)
      << "the horizontal axis wasn't asked for and must not move";
  EXPECT_EQ(c->ContentRect().width(), before.width());
}

TEST(Maximize, SecondAxisDoesNotForgetTheOriginalGeometry) {
  wmtest::World world(1400, 1000);
  setOneOffsetScreen();
  Client* c = world.MapClientWindow(Rect::FromXYWH(300, 200, 400, 300));
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  // Maximise one axis, then the other, then let both go. The rect to restore
  // is recorded on the first transition only; take it on the second and the
  // window comes back screen-height.
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);
  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);
  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);

  EXPECT_EQ(c->ContentRect(), before);
}

TEST(Maximize, StrutsAreRespected) {
  wmtest::World world(1400, 1000);
  setOneOffsetScreen();
  // A panel across the top of the screen. Unlike a full-screen window, a
  // maximised one has to leave it alone.
  EWMHStrut strut{};
  strut.top = kScreen.yMin + 40;
  ASSERT_TRUE(LScr::I->ChangeStrut(strut));

  Client* c = world.MapClientWindow(Rect::FromXYWH(300, 200, 400, 300));
  ASSERT_TRUE(c != nullptr);
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);

  EXPECT_EQ(c->FrameRect().yMin, kScreen.yMin + 40)
      << "a maximised window stops at the strut";
  EXPECT_EQ(c->FrameRect().yMax, kScreen.yMax);
}

TEST(Maximize, StateIsPublishedOnTheWindow) {
  wmtest::World world(1400, 1000);
  setOneOffsetScreen();
  Client* c = world.MapClientWindow(Rect::FromXYWH(300, 200, 400, 300));
  ASSERT_TRUE(c != nullptr);

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  const xlib::WindowProperty prop = xlib::XGetWindowProperty(
      c->window, ewmh_atom[_NET_WM_STATE], 100, XCB_ATOM_ATOM);
  const std::vector<uint32_t>& state = prop.Data32();
  EXPECT_TRUE(std::find(state.begin(), state.end(),
                        ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]) != state.end())
      << "a client that asked to be maximised has to be able to read it back";
  EXPECT_TRUE(std::find(state.begin(), state.end(),
                        ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]) == state.end());
}

TEST(Maximize, InitialStateSetBeforeMappingIsHonoured) {
  wmtest::World world(1400, 1000);
  setOneOffsetScreen();
  const Rect original = Rect::FromXYWH(300, 200, 400, 300);
  const Window w = world.server().AddClientWindow(original);
  // EWMH says a client sets _NET_WM_STATE before mapping to say how it wants
  // to start. This one wants to start maximised.
  world.server().SetProperty32(w, ewmh_atom[_NET_WM_STATE], XCB_ATOM_ATOM,
                               {ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT],
                                ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]});
  xcb_map_request_event_t e{};
  e.response_type = XCB_MAP_REQUEST;
  e.parent = world.server().Root();
  e.window = w;
  world.server().PushEvent(e);
  ProcessPendingEvents();

  Client* c = LScr::I->GetClient(w, false);
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->FrameRect(), kScreen);

  // And it still knows where to go back to.
  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);
  EXPECT_EQ(c->ContentRect(), original);
}

TEST(Maximize, FullScreenWinsWhileItLasts) {
  wmtest::World world(1400, 1000);
  setOneOffsetScreen();
  Client* c = world.MapClientWindow(Rect::FromXYWH(300, 200, 400, 300));
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  const Rect full = c->ContentRect();
  // Maximising underneath full screen changes the state but not the geometry.
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);
  EXPECT_EQ(c->ContentRect(), full)
      << "a full-screen window stays full screen";

  // Leaving full screen lands on the maximised geometry, not the original.
  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  EXPECT_EQ(c->FrameRect(), kScreen);
  EXPECT_NE(c->ContentRect(), before);

  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_VERT]);
  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_MAXIMIZED_HORZ]);
  EXPECT_EQ(c->ContentRect(), before);
}

// --- _NET_FRAME_EXTENTS -----------------------------------------------------
//
// Clients that care where their *frame* lands (Wine and Chromium both do) read
// this rather than guessing at the decoration sizes.

namespace {

// The four numbers of _NET_FRAME_EXTENTS - left, right, top, bottom - as the
// client reads them back. Returns an empty vector if the property is missing.
std::vector<uint32_t> frameExtents(Client* c) {
  const xlib::WindowProperty prop = xlib::XGetWindowProperty(
      c->window, ewmh_atom[_NET_FRAME_EXTENTS], 4, XCB_ATOM_CARDINAL);
  return prop.ok() ? prop.Data32() : std::vector<uint32_t>();
}

// What the extents ought to be, read off the client's own geometry. The
// property is worth nothing to a client if it doesn't agree with where lwm
// actually puts the two windows. The frame's X border is added on each side
// because it's drawn outside FrameRect - see ewmh_set_frame_extents.
std::vector<uint32_t> extentsFromGeometry(Client* c) {
  const Rect f = c->FrameRect();
  const Rect r = c->ContentRect();
  const int b = c->framed && !c->wstate.fullscreen ? kFrameBorderWidth : 0;
  return {uint32_t(r.xMin - f.xMin + b), uint32_t(f.xMax - r.xMax + b),
          uint32_t(r.yMin - f.yMin + b), uint32_t(f.yMax - r.yMax + b)};
}

// A window lwm won't frame: _NET_WM_WINDOW_TYPE_DOCK, as a panel would set.
Client* mapDockWindow(wmtest::World& world, const Rect& rect) {
  const Window w = world.server().AddClientWindow(rect);
  world.server().SetProperty32(w, ewmh_atom[_NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM,
                               {ewmh_atom[_NET_WM_WINDOW_TYPE_DOCK]});
  xcb_map_request_event_t ev{};
  ev.response_type = XCB_MAP_REQUEST;
  ev.parent = world.server().Root();
  ev.window = w;
  world.server().PushEvent(ev);
  ProcessPendingEvents();
  return LScr::I->GetClient(w, false);
}

}  // namespace

TEST(FrameExtents, PublishedForAFramedWindow) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 400, 300));
  ASSERT_TRUE(c != nullptr);

  // The default border resource is 6 and the fake font's text height is 16, so
  // the title bar is 22 - and every side gains the frame's own 1px X border.
  const std::vector<uint32_t> expected = {7, 7, 23, 7};
  EXPECT_EQ(frameExtents(c), expected);
  EXPECT_EQ(frameExtents(c), extentsFromGeometry(c))
      << "the published extents have to match where lwm really puts things";
}

TEST(FrameExtents, ZeroForAnUndecoratedWindow) {
  wmtest::World world;
  Client* c = mapDockWindow(world, Rect::FromXYWH(0, 0, 1280, 30));
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->framed);

  const std::vector<uint32_t> expected = {0, 0, 0, 0};
  EXPECT_EQ(frameExtents(c), expected);
}

TEST(FrameExtents, ZeroWhileFullScreenAndBackAgainAfter) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 400, 300));
  ASSERT_TRUE(c != nullptr);
  const std::vector<uint32_t> framed = frameExtents(c);

  sendStateMessage(&world, c, kAdd, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  const std::vector<uint32_t> zeroes = {0, 0, 0, 0};
  EXPECT_EQ(frameExtents(c), zeroes)
      << "a full-screen window has no furniture to make room for";

  sendStateMessage(&world, c, kRemove, ewmh_atom[_NET_WM_STATE_FULLSCREEN]);
  EXPECT_EQ(frameExtents(c), framed) << "and gets it back on the way out";
}

TEST(FrameExtents, ClearedWhenTheWindowIsWithdrawn) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 400, 300));
  ASSERT_TRUE(c != nullptr);

  withdraw(c);
  const std::vector<uint32_t> zeroes = {0, 0, 0, 0};
  EXPECT_EQ(frameExtents(c), zeroes)
      << "a withdrawn window is no longer framed, so it mustn't keep stale "
         "extents";
}
