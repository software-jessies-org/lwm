// Tests for the client lifecycle: adopting a window, hiding it, unhiding it,
// and destroying it - checking each time that LScr's two registries and the
// Focuser's history agree with each other and hold no dangling pointers.
//
// This is the sequence that used to be verifiable only by running lwm and
// watching. The interesting failures are the quiet ones: a frame left in
// parents_ after its client has gone, or a destroyed Client still sitting in
// the focus history waiting to be handed focus.

#include "client.h"
#include "disp.h"
#include "ewmh.h"
#include "lwm.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

namespace {

// The set of windows lwm currently believes it manages, read back off the root
// window's _NET_CLIENT_LIST rather than out of LScr, so that the property and
// the registry can be compared.
std::vector<uint32_t> clientListProperty() {
  const xlib::WindowProperty prop = xlib::XGetWindowProperty(
      LScr::I->Root(), ewmh_atom[_NET_CLIENT_LIST], 100, XCB_ATOM_WINDOW);
  return prop.Data32();
}

}  // namespace

TEST(ClientLifecycle, MapRequestFramesAndRegistersTheWindow) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  EXPECT_TRUE(c->framed);
  EXPECT_EQ(c->State(), NormalState);
  EXPECT_TRUE(c->parent != 0) << "a framed client needs a frame window";
  EXPECT_EQ(LScr::I->GetClient(c->window), c);
  EXPECT_EQ(LScr::I->GetClient(c->parent), c)
      << "the frame must resolve to the same client";
  EXPECT_EQ(LScr::I->Clients().size(), size_t(1));

  // The window really was reparented into the frame, and both are mapped.
  EXPECT_EQ(world.server().Get(c->window)->parent, c->parent);
  EXPECT_TRUE(world.server().Get(c->window)->mapped);
  EXPECT_TRUE(world.server().Get(c->parent)->mapped);

  const std::vector<uint32_t> list = clientListProperty();
  ASSERT_EQ(list.size(), size_t(1));
  EXPECT_EQ(list[0], c->window);
}

TEST(ClientLifecycle, ManageSetsWMStateToNormal) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  const xlib::WindowProperty prop =
      xlib::XGetWindowProperty(c->window, wm_state, 2, wm_state);
  ASSERT_TRUE(prop.ok());
  EXPECT_EQ(int(prop.Data32()[0]), NormalState);
}

TEST(ClientLifecycle, HideUnmapsTheFrameAndDropsFocus) {
  wmtest::World world;
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  ASSERT_TRUE(a && b);
  ASSERT_TRUE(b->HasFocus());

  b->Hide();
  EXPECT_TRUE(b->hidden);
  EXPECT_EQ(b->State(), IconicState);
  EXPECT_FALSE(world.server().Get(b->parent)->mapped)
      << "hiding unmaps the frame, which implicitly unmaps the client";
  EXPECT_FALSE(b->HasFocus());
  EXPECT_TRUE(a->HasFocus())
      << "focus should fall back to the other window, not vanish";

  // The client is hidden, not gone: it stays in both registries.
  EXPECT_EQ(LScr::I->GetClient(b->window), b);
  EXPECT_EQ(LScr::I->Clients().size(), size_t(2));
}

TEST(ClientLifecycle, UnhideRemapsRaisesAndRefocuses) {
  wmtest::World world;
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  ASSERT_TRUE(a && b);
  b->Hide();
  ASSERT_TRUE(a->HasFocus());

  b->Unhide();
  EXPECT_FALSE(b->hidden);
  EXPECT_EQ(b->State(), NormalState);
  EXPECT_TRUE(world.server().Get(b->parent)->mapped);
  EXPECT_TRUE(b->HasFocus()) << "unhiding a window gives it focus";

  // Unhide raises, so b's frame is now the topmost child of the root. It's
  // the frame that gets stacked, not the client window: the client is a child
  // of the frame and rides along with it.
  const std::vector<Window> stack = world.server().ChildrenOf(LScr::I->Root());
  ASSERT_TRUE(!stack.empty());
  EXPECT_EQ(stack.back(), b->parent);
}

TEST(ClientLifecycle, DestroyNotifyRemovesTheClientFromEveryRegistry) {
  wmtest::World world;
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  ASSERT_TRUE(a && b);
  const Window b_window = b->window;
  const Window b_parent = b->parent;
  ASSERT_TRUE(b->HasFocus());

  xcb_destroy_notify_event_t e{};
  e.response_type = XCB_DESTROY_NOTIFY;
  e.event = b_window;
  e.window = b_window;
  world.server().ClearCalls();
  world.server().PushEvent(e);
  ProcessPendingEvents();

  EXPECT_EQ(LScr::I->Clients().size(), size_t(1));
  EXPECT_TRUE(LScr::I->GetClient(b_window, false) == nullptr);
  EXPECT_TRUE(LScr::I->GetClient(b_parent, false) == nullptr)
      << "the frame must not be left behind in parents_";
  EXPECT_TRUE(world.server().DidCall("DestroyWindow("))
      << "the frame window is ours, so we have to destroy it ourselves";
  EXPECT_TRUE(a->HasFocus())
      << "focus must move to a live client, not a freed one";

  const std::vector<uint32_t> list = clientListProperty();
  ASSERT_EQ(list.size(), size_t(1));
  EXPECT_EQ(list[0], a->window);
}

TEST(ClientLifecycle, DestroyNotifyForAnUnknownWindowIsIgnored) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  ASSERT_TRUE(c != nullptr);

  xcb_destroy_notify_event_t e{};
  e.response_type = XCB_DESTROY_NOTIFY;
  e.event = 0x9999;
  e.window = 0x9999;
  world.server().PushEvent(e);
  ProcessPendingEvents();

  EXPECT_EQ(LScr::I->Clients().size(), size_t(1));
  EXPECT_EQ(LScr::I->GetClient(c->window), c);
}

TEST(ClientLifecycle, MapRequestForAWindowThatHasAlreadyGoneIsIgnored) {
  wmtest::World world;
  const Window w = world.server().AddClientWindow(
      Rect::FromXYWH(10, 10, 100, 100));

  xcb_map_request_event_t e{};
  e.response_type = XCB_MAP_REQUEST;
  e.parent = world.server().Root();
  e.window = w;
  world.server().PushEvent(e);
  // The application exits before we get round to asking about its window, so
  // the attributes come back empty and there is no client to be had. lwm used
  // to walk straight into a null pointer here.
  world.server().DestroyWindow(w);
  ProcessPendingEvents();

  EXPECT_EQ(LScr::I->Clients().size(), size_t(0));
}

TEST(ClientLifecycle, OverrideRedirectWindowsAreNotAdopted) {
  wmtest::World world;
  const Window w = world.server().AddClientWindow(
      Rect::FromXYWH(10, 10, 100, 100));
  world.server().Get(w)->override_redirect = true;

  xcb_map_request_event_t e{};
  e.response_type = XCB_MAP_REQUEST;
  e.parent = world.server().Root();
  e.window = w;
  world.server().PushEvent(e);
  ProcessPendingEvents();

  EXPECT_TRUE(LScr::I->GetClient(w, false) == nullptr)
      << "an override-redirect window has told us to keep our hands off it";
  EXPECT_EQ(LScr::I->Clients().size(), size_t(0));
}

TEST(ClientLifecycle, NonRectangularWindowsAreLeftUnframed) {
  wmtest::World world;
  const Window w = world.server().AddClientWindow(
      Rect::FromXYWH(10, 10, 100, 100));
  // More than one rectangle in the bounding shape means the window isn't a
  // rectangle, and a frame drawn around it would look wrong (xeyes).
  world.server().Get(w)->shape_rectangles = 4;

  xcb_map_request_event_t e{};
  e.response_type = XCB_MAP_REQUEST;
  e.parent = world.server().Root();
  e.window = w;
  world.server().PushEvent(e);
  ProcessPendingEvents();

  Client* c = LScr::I->GetClient(w, false);
  ASSERT_TRUE(c != nullptr);
  EXPECT_FALSE(c->framed);
}

// Maps a window carrying the given _MOTIF_WM_HINTS words and returns the
// Client, so the two Motif cases below differ only in the hints.
namespace {

Client* mapWithMotifHints(wmtest::World* world,
                          const std::vector<uint32_t>& hints) {
  const Window w =
      world->server().AddClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  world->server().SetProperty32(w, motif_wm_hints, motif_wm_hints, hints);

  xcb_map_request_event_t e{};
  e.response_type = XCB_MAP_REQUEST;
  e.parent = world->server().Root();
  e.window = w;
  world->server().PushEvent(e);
  ProcessPendingEvents();
  return LScr::I->GetClient(w, false);
}

// _MOTIF_WM_HINTS words 0 and 2: the flags, and the decorations bitmap.
constexpr uint32_t kMwmHintsDecorations = 1 << 1;
constexpr uint32_t kMwmDecorAll = 1 << 0;
constexpr uint32_t kMwmDecorBorder = 1 << 1;

}  // namespace

TEST(ClientLifecycle, MotifHintsAskingForNoDecorationsLeaveTheWindowUnframed) {
  wmtest::World world;
  // What a Java JFrame with setUndecorated(true) sets, and what every
  // borderless-fullscreen game under Wine sets: decorations are named, and the
  // set named is empty.
  Client* c = mapWithMotifHints(&world, {kMwmHintsDecorations, 0, 0, 0, 0});
  ASSERT_TRUE(c != nullptr);
  EXPECT_FALSE(c->framed)
      << "the window asked for no decorations and must not be reparented";
  EXPECT_EQ(c->parent, LScr::I->Root());
}

TEST(ClientLifecycle, MotifHintsAskingForABorderStillGetAFrame) {
  wmtest::World world;
  Client* c = mapWithMotifHints(
      &world, {kMwmHintsDecorations, 0, kMwmDecorAll | kMwmDecorBorder, 0, 0});
  ASSERT_TRUE(c != nullptr);
  EXPECT_TRUE(c->framed);
}
