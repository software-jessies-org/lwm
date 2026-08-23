// Tests for Focuser: the focus history, the hand-over when a focused client
// goes away, and the deferral that avoids the focus-follows-mouse race.
//
// The race is the interesting one. Moving the pointer quickly from window A to
// window C, across B on the way, used to be able to leave B focused, because B
// reacted to its "please take focus" after C reacted to its own. The whole
// point of the timer fd is that the second enter event inside the delay window
// doesn't get acted on immediately, so a third can supersede it - and that is
// only observable if you can control the clock. See focus.h.

#include <sstream>

#include "client.h"
#include "focus.h"
#include "lwm.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"

namespace {

// The clock the tests install in place of CLOCK_MONOTONIC.
uint64_t fake_now_millis;
uint64_t FakeClock() {
  return fake_now_millis;
}

// Installs FakeClock for as long as it's in scope.
class ScopedFakeClock {
 public:
  explicit ScopedFakeClock(uint64_t start) : previous_(focus::NowMillis) {
    fake_now_millis = start;
    focus::NowMillis = FakeClock;
  }
  ~ScopedFakeClock() { focus::NowMillis = previous_; }

 private:
  focus::ClockFn previous_;
};

// The window of whichever client currently has focus, or 0. Comparing window
// ids rather than Client pointers is purely so that a failure prints
// something a human can act on.
Window FocusedClientWindow() {
  Client* c = LScr::I->GetFocuser()->GetFocusedClient();
  return c ? c->window : 0;
}

}  // namespace

TEST(Focuser, MostRecentlyFocusedClientIsTheFocusedOne) {
  wmtest::World world;
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  ASSERT_TRUE(a && b);

  // Mapping a window focuses it, so b holds focus at this point.
  EXPECT_EQ(FocusedClientWindow(), b->window);
  EXPECT_TRUE(b->HasFocus());
  EXPECT_FALSE(a->HasFocus());
  EXPECT_EQ(world.server().FocusedWindow(), b->window)
      << "the client window itself takes focus, not the frame";

  LScr::I->GetFocuser()->FocusClient(a);
  EXPECT_EQ(FocusedClientWindow(), a->window);
  EXPECT_EQ(world.server().FocusedWindow(), a->window);
}

TEST(Focuser, UnfocusHandsOverToThePreviouslyFocusedClient) {
  wmtest::World world;
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  Client* c = world.MapClientWindow(Rect::FromXYWH(400, 10, 100, 100));
  ASSERT_TRUE(a && b && c);
  // History is now c, b, a - most recent first.
  EXPECT_EQ(FocusedClientWindow(), c->window);

  LScr::I->GetFocuser()->UnfocusClient(c);
  EXPECT_EQ(FocusedClientWindow(), b->window)
      << "focus should fall back to the next window in the history";

  LScr::I->GetFocuser()->UnfocusClient(b);
  EXPECT_EQ(FocusedClientWindow(), a->window);

  // Unfocusing a client that never had focus takes it out of the history but
  // must not move focus.
  LScr::I->GetFocuser()->FocusClient(a);
  Client* d = world.MapClientWindow(Rect::FromXYWH(600, 10, 100, 100));
  ASSERT_TRUE(d != nullptr);
  LScr::I->GetFocuser()->UnfocusClient(a);
  EXPECT_EQ(FocusedClientWindow(), d->window);
}

TEST(Focuser, FirstEnterWindowIsActedOnImmediately) {
  wmtest::World world;
  ScopedFakeClock clock(100000);
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  ASSERT_TRUE(a && b);

  // Well clear of the 50ms default delay, so there's nothing to wait for.
  fake_now_millis += 1000;
  LScr::I->GetFocuser()->EnterWindow(a->window);
  EXPECT_EQ(FocusedClientWindow(), a->window);
}

TEST(Focuser, ThirdWindowEnteredDuringTheDelayWinsTheRace) {
  wmtest::World world;
  ScopedFakeClock clock(100000);
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  Client* c = world.MapClientWindow(Rect::FromXYWH(400, 10, 100, 100));
  ASSERT_TRUE(a && b && c);

  Focuser* focuser = LScr::I->GetFocuser();

  // A: far enough after anything else that it's granted focus at once.
  fake_now_millis += 1000;
  focuser->EnterWindow(a->window);
  ASSERT_EQ(FocusedClientWindow(), a->window);

  // B: 10ms later, inside the 50ms default delay, so it is deferred.
  fake_now_millis += 10;
  focuser->EnterWindow(b->window);
  EXPECT_EQ(FocusedClientWindow(), a->window)
      << "B was entered too soon after A; focus must not have moved yet";

  // C: 10ms after that. B is still pending, so C supersedes it.
  fake_now_millis += 10;
  focuser->EnterWindow(c->window);
  EXPECT_EQ(FocusedClientWindow(), a->window);

  // The timer goes off; the window that gets focus is C, the one the pointer
  // actually ended up in - not B, which it merely passed over.
  focuser->TimerFDTriggered();
  EXPECT_EQ(FocusedClientWindow(), c->window)
      << "the window the pointer crossed must not steal focus from the one "
         "it stopped in";
  EXPECT_EQ(world.server().FocusedWindow(), c->window);
}

TEST(Focuser, EnteringTheSameWindowTwiceIsIgnored) {
  wmtest::World world;
  ScopedFakeClock clock(100000);
  Client* a = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  Client* b = world.MapClientWindow(Rect::FromXYWH(200, 10, 100, 100));
  ASSERT_TRUE(a && b);

  Focuser* focuser = LScr::I->GetFocuser();
  fake_now_millis += 1000;
  focuser->EnterWindow(a->window);
  ASSERT_EQ(FocusedClientWindow(), a->window);

  world.server().ClearCalls();
  fake_now_millis += 1000;
  focuser->EnterWindow(a->window);
  EXPECT_TRUE(world.server().CallsMatching("SetInputFocus(").empty())
      << "re-entering the focused window should not talk to the server";
}

TEST(Focuser, ClientWithoutInputHintButWithTakeFocusGetsItsChildrenFocused) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  ASSERT_TRUE(c != nullptr);

  // A Java app: the top-level window refuses focus, but says it understands
  // WM_TAKE_FOCUS, and hides a focus proxy among its children. lwm has to find
  // the child by its event mask, because nothing names it.
  const Window proxy = world.server().AddChildWindow(
      c->window, Rect::FromXYWH(0, 0, 10, 10));
  const Window content = world.server().AddChildWindow(
      c->window, Rect::FromXYWH(0, 0, 100, 100));
  world.server().Get(proxy)->all_event_masks = XCB_EVENT_MASK_FOCUS_CHANGE;
  world.server().Get(content)->all_event_masks = XCB_EVENT_MASK_EXPOSURE;
  c->accepts_focus = false;
  c->proto |= Ptakefocus;

  world.server().ClearCalls();
  LScr::I->GetFocuser()->UnfocusClient(c);
  LScr::I->GetFocuser()->FocusClient(c);

  EXPECT_EQ(world.server().FocusedWindow(), proxy)
      << "only the child that selected focus events should be focused";
  EXPECT_EQ(world.server().CallsMatching("SetInputFocus(").size(), size_t(1))
      << "the content window must be left alone";
}

TEST(Focuser, GloballyActiveClientIsSentWmTakeFocus) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(10, 10, 100, 100));
  ASSERT_TRUE(c != nullptr);

  // ICCCM 4.1.7's "globally active" input model: WM_HINTS says input=False,
  // but WM_TAKE_FOCUS is in WM_PROTOCOLS. Every Wine/Proton window looks like
  // this, and (unlike a Java app) has no child windows for us to fall back on,
  // so if we don't send the message the input focus never moves at all and the
  // application gets no key events.
  c->accepts_focus = false;
  c->proto |= Ptakefocus;

  world.server().ClearCalls();
  LScr::I->GetFocuser()->UnfocusClient(c);
  LScr::I->GetFocuser()->FocusClient(c);

  std::ostringstream want;
  want << "message_type=" << wm_protocols << " data0=" << wm_take_focus;
  bool sent = false;
  for (const std::string& call : world.server().CallsMatching("SendEvent(")) {
    if (call.find(want.str()) != std::string::npos) {
      sent = true;
    }
  }
  EXPECT_TRUE(sent) << "no WM_TAKE_FOCUS client message was sent";
}
