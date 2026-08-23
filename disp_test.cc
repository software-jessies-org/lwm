// Tests for EvConfigureRequest - the frame-relative offset arithmetic that
// docs/concepts.md and the comment in disp.cc both label "the Nautilus hack".
//
// This is the highest-risk code in the tree. A client that moves or resizes
// itself (Nautilus does, by dragging its own internal top bar) sends a
// ConfigureRequest whose x and y are, after reparenting, the origin of *our*
// frame rather than of its own window. Get the conversion wrong and every drag
// inside the client area makes the window jump up and to the left, which is
// exactly the bug the offset exists to stop. Until now the only way to check
// it was to drag a Nautilus window around by hand.

#include "client.h"
#include "disp.h"
#include "ewmh.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

namespace {

// The client window's own geometry, as it would arrive from the application.
constexpr int kX = 100;
constexpr int kY = 100;
constexpr int kW = 300;
constexpr int kH = 200;

// Window ids appear in the call log in hex, as WinID prints them.
std::string winStr(Window w) {
  std::ostringstream os;
  os << "0x" << std::hex << w;
  return os.str();
}

// The index of the first recorded call starting with prefix, or -1.
int indexOfCall(const std::vector<std::string>& calls,
                const std::string& prefix) {
  for (size_t i = 0; i < calls.size(); i++) {
    if (calls[i].compare(0, prefix.size(), prefix) == 0) {
      return int(i);
    }
  }
  return -1;
}

xcb_configure_request_event_t configureRequest(Window parent,
                                               Window window,
                                               uint16_t value_mask) {
  xcb_configure_request_event_t e{};
  e.response_type = XCB_CONFIGURE_REQUEST;
  e.parent = parent;
  e.window = window;
  e.value_mask = value_mask;
  return e;
}

}  // namespace

TEST(EvConfigureRequest, RequestedOriginBecomesTheFrameOrigin) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);
  ASSERT_TRUE(c->framed);

  // The offset between the frame's origin and the content's. This is what the
  // client's coordinates have to be corrected by.
  const Point offset = c->ContentRectRelative().origin();
  ASSERT_TRUE(offset.x > 0 && offset.y > 0);

  xcb_configure_request_event_t e = configureRequest(
      c->parent, c->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y);
  e.x = 500;
  e.y = 400;
  world.server().ClearCalls();
  world.server().PushEvent(e);
  ProcessPendingEvents();

  // The client asked to be at 500,400. Interpreted as the frame's origin -
  // which is what the ICCCM means once we've reparented - that puts the
  // content at 500+offset. Anything else and the window has jumped.
  EXPECT_EQ(c->FrameRect().origin(), (Point{500, 400}));
  EXPECT_EQ(c->ContentRect().origin(), (Point{500 + offset.x, 400 + offset.y}));
  EXPECT_EQ(c->ContentRect().area(), (Area{kW, kH}))
      << "a move must not change the size";
}

TEST(EvConfigureRequest, ResizeKeepsTheOriginAndTakesTheNewSize) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  xcb_configure_request_event_t e = configureRequest(
      c->parent, c->window,
      XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT);
  e.width = 500;
  e.height = 400;
  world.server().ClearCalls();
  world.server().PushEvent(e);
  ProcessPendingEvents();

  EXPECT_EQ(c->ContentRect().origin(), before.origin())
      << "a size-only request must not move the window";
  EXPECT_EQ(c->ContentRect().area(), (Area{500, 400}));
  // The frame is still the content plus the furniture: a resize must keep the
  // two in step, or the border ends up the wrong width on one side.
  EXPECT_EQ(Client::ContentFromFrameRect(c->FrameRect()), c->ContentRect());
}

TEST(EvConfigureRequest, ConfiguresTheFrameThenTheContentWindow) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);

  xcb_configure_request_event_t e = configureRequest(
      c->parent, c->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y);
  e.x = 500;
  e.y = 400;
  world.server().ClearCalls();
  world.server().PushEvent(e);
  ProcessPendingEvents();

  const std::vector<std::string>& calls = world.server().Calls();
  const int frame_at = indexOfCall(calls, "ConfigureWindow(" +
                                              winStr(c->parent) + ")");
  const int notify_at =
      indexOfCall(calls, "SendEvent(" + winStr(c->window) + ") type=" +
                             std::to_string(XCB_CONFIGURE_NOTIFY));
  const int content_at = indexOfCall(calls, "ConfigureWindow(" +
                                                winStr(c->window) + ")");
  ASSERT_TRUE(frame_at >= 0 && notify_at >= 0 && content_at >= 0)
      << "frame=" << frame_at << " notify=" << notify_at
      << " content=" << content_at;
  // Frame first, then the ConfigureNotify that tells the client where it now
  // is, then the content window itself. The client is told before its own
  // window moves, which is the order lwm has always used.
  EXPECT_TRUE(frame_at < notify_at && notify_at < content_at)
      << "frame=" << frame_at << " notify=" << notify_at
      << " content=" << content_at;

  // The content window is repositioned within the frame, so its coordinates
  // are frame-relative and equal to the offset - never the root-relative ones
  // the client asked for.
  const Point offset = c->ContentRectRelative().origin();
  std::ostringstream want;
  want << " x=" << offset.x << " y=" << offset.y;
  EXPECT_TRUE(calls[content_at].find(want.str()) != std::string::npos)
      << "got " << calls[content_at] << ", wanted it to contain "
      << want.str();
}

TEST(EvConfigureRequest, UnmanagedWindowGetsItsRequestPassedStraightThrough) {
  wmtest::World world;
  // A window lwm has never adopted: no MapRequest, so no Client.
  const Window w = world.server().AddClientWindow(Rect::FromXYWH(0, 0, 10, 10));
  ASSERT_TRUE(LScr::I->GetClient(w, false) == nullptr);

  xcb_configure_request_event_t e = configureRequest(
      world.server().Root(), w,
      XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
          XCB_CONFIG_WINDOW_HEIGHT);
  e.x = 42;
  e.y = 43;
  e.width = 200;
  e.height = 150;
  world.server().ClearCalls();
  world.server().PushEvent(e);
  ProcessPendingEvents();

  const std::vector<std::string> calls =
      world.server().CallsMatching("ConfigureWindow(");
  ASSERT_EQ(calls.size(), size_t(1));
  EXPECT_TRUE(calls[0].find("x=42 y=43 width=200 height=150") !=
              std::string::npos)
      << "got " << calls[0];
  EXPECT_EQ(world.server().Get(w)->rect, Rect::FromXYWH(42, 43, 200, 150));
}

TEST(EvConfigureRequest, StackOnlyRequestOnAFramedClientChangesNoGeometry) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);
  const Rect before = c->ContentRect();

  xcb_configure_request_event_t e =
      configureRequest(c->parent, c->window, XCB_CONFIG_WINDOW_STACK_MODE);
  e.stack_mode = XCB_STACK_MODE_ABOVE;
  world.server().ClearCalls();
  world.server().PushEvent(e);
  ProcessPendingEvents();

  EXPECT_EQ(c->ContentRect(), before);
  EXPECT_TRUE(world.server().CallsMatching("ConfigureWindow(").empty())
      << "nothing about the geometry changed, so nothing should be sent";
}

// --- EvUnmapNotify ----------------------------------------------------------
//
// A client that unmaps its own window has withdrawn it, and lwm has to notice:
// withdraw() is what takes the window back out of our save-set. Miss it, and
// the X server maps the window again when lwm disconnects, because a
// disconnecting client's save-set is mapped in its entirety.
//
// The catch is that reparenting a framed client into its frame *also* produces
// an UnmapNotify, reported against the root, which must not be mistaken for a
// withdrawal. Unframed clients never get reparented, so for them the root is
// the only place the real unmap can come from.

namespace {

// An unframed client: one with _NET_WM_WINDOW_TYPE_MENU, as gummiband's
// drop-down menu window has. Driven through the same MapRequest path as
// World::MapClientWindow, but with the window type set first, since it's the
// type manage() reads to decide against framing.
Client* mapMenuWindow(wmtest::World& world, const Rect& rect) {
  const Window w = world.server().AddClientWindow(rect);
  world.server().SetProperty32(w, ewmh_atom[_NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM,
                               {ewmh_atom[_NET_WM_WINDOW_TYPE_MENU]});
  xcb_map_request_event_t ev{};
  ev.response_type = XCB_MAP_REQUEST;
  ev.parent = world.server().Root();
  ev.window = w;
  world.server().PushEvent(ev);
  ProcessPendingEvents();
  return LScr::I->GetClient(w, false);
}

xcb_unmap_notify_event_t unmapNotify(Window event_window, Window window) {
  xcb_unmap_notify_event_t e{};
  e.response_type = XCB_UNMAP_NOTIFY;
  e.event = event_window;
  e.window = window;
  return e;
}

}  // namespace

TEST(EvUnmapNotify, UnframedClientUnmappingItselfIsWithdrawn) {
  wmtest::World world;
  Client* c = mapMenuWindow(world, Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);
  ASSERT_FALSE(c->framed);
  ASSERT_EQ(c->parent, LScr::I->Root());
  ASSERT_EQ(c->State(), NormalState);

  world.server().ClearCalls();
  world.server().PushEvent(unmapNotify(LScr::I->Root(), c->window));
  ProcessPendingEvents();

  EXPECT_EQ(c->State(), WithdrawnState);
  EXPECT_EQ(world.server().CallsMatching("RemoveFromSaveSet(").size(),
            size_t(1))
      << "an unmapped window left in the save-set is mapped again when lwm "
         "disconnects";
}

TEST(EvUnmapNotify, FramedClientUnmappingItselfIsWithdrawn) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);
  ASSERT_TRUE(c->framed);

  // Reported against the frame, which is where a framed client's unmaps come
  // from once it has been reparented.
  world.server().ClearCalls();
  world.server().PushEvent(unmapNotify(c->parent, c->window));
  ProcessPendingEvents();

  EXPECT_EQ(c->State(), WithdrawnState);
  EXPECT_EQ(world.server().CallsMatching("RemoveFromSaveSet(").size(),
            size_t(1));
}

TEST(EvUnmapNotify, ReparentingAFramedClientIsNotAWithdrawal) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);
  ASSERT_TRUE(c->framed);
  ASSERT_EQ(c->State(), NormalState);

  // The unmap the server generates when we reparent the window into the frame
  // is reported against the root. The window is still very much on screen.
  world.server().ClearCalls();
  world.server().PushEvent(unmapNotify(LScr::I->Root(), c->window));
  ProcessPendingEvents();

  EXPECT_EQ(c->State(), NormalState);
  EXPECT_TRUE(world.server().CallsMatching("RemoveFromSaveSet(").empty());
}

TEST(EvUnmapNotify, UnmapOfSomethingOtherThanTheClientWindowIsIgnored) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(kX, kY, kW, kH));
  ASSERT_TRUE(c != nullptr);

  // The frame itself unmapping is lwm's own doing, not a withdrawal.
  world.server().ClearCalls();
  world.server().PushEvent(unmapNotify(LScr::I->Root(), c->parent));
  ProcessPendingEvents();

  EXPECT_EQ(c->State(), NormalState);
  EXPECT_TRUE(world.server().CallsMatching("RemoveFromSaveSet(").empty());
}
