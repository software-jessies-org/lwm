// Tests for the X error machinery in error.cc: which errors lwm reports,
// which it swallows, and which of them are bad enough to give up on during
// start-up.
//
// Two failures from a real session are pinned here.
//
// The first killed lwm as it started. The laptop it was on had just had a
// second monitor plugged in, so an XRandR ScreenChangeNotify was waiting on
// the queue when lwm drained it looking for the errors start-up had provoked.
// Dispatching that sent lwm off to move every window it managed, one of which
// had gone away, and the BadWindow that came back was treated as "start-up
// failed" - lwm printed a backtrace and exited. Starting it again by hand
// worked, because by then the race was over.
//
// The second was a screenful of errors every time a window closed: four of
// them, from the requests withdraw() makes about a window the application has
// already destroyed. Those were meant to be suppressed all along, and the
// scope that was supposed to suppress them was opened *underneath* them.
//
// Errors are addressed by sequence number - an X error names the request that
// failed, and the suppression ranges are made of the same numbers - so the
// tests here ask the FakeServer which sequence number a particular recorded
// call was given, and hand the error back against that.

#include <fcntl.h>
#include <unistd.h>

#include "client.h"
#include "disp.h"
#include "error.h"
#include "lwm.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"
#include "xlib.h"

namespace {

// Window ids appear in the call log in hex, as WinID prints them.
std::string winStr(Window w) {
  std::ostringstream os;
  os << "0x" << std::hex << w;
  return os.str();
}

xcb_generic_error_t badWindow(uint32_t sequence, Window w) {
  xcb_generic_error_t err{};
  err.error_code = XCB_WINDOW;
  err.sequence = uint16_t(sequence);
  err.full_sequence = sequence;
  err.resource_id = w;
  return err;
}

// Sends stderr to /dev/null while alive. An error lwm reports is printed, with
// a backtrace, and the tests below deliberately provoke one: without this the
// output of a passing run looks like a crash.
class QuietStderr {
 public:
  QuietStderr() : saved_(dup(STDERR_FILENO)) {
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
  }
  ~QuietStderr() {
    if (saved_ >= 0) {
      dup2(saved_, STDERR_FILENO);
      close(saved_);
    }
  }

 private:
  const int saved_;
};

// The FakeServer's log line for a property lwm wrote on a window, or "" if it
// wrote no such property. Found by name rather than by prefix: the call text
// carries the atom's number as well as its name, and the number depends on
// what else has been interned.
std::string propertyCall(wmtest::World& world,
                         Window w,
                         const std::string& name) {
  const std::string prefix = "ChangeProperty(" + winStr(w) + ") property=";
  for (const std::string& call : world.server().Calls()) {
    if (call.compare(0, prefix.size(), prefix) == 0 &&
        call.find("(" + name + ")") != std::string::npos) {
      return call;
    }
  }
  return "";
}

// Whether lwm reports this error or keeps quiet about it. Reporting means
// printing it, with a backtrace, so the tests below - several of which expect
// exactly that - do it with stderr silenced, or a passing run reads as a pile
// of crashes.
bool reports(const xcb_generic_error_t& err) {
  const uint64_t before = ReportedErrorCount();
  {
    QuietStderr quiet;
    HandleXError(&err);
  }
  return ReportedErrorCount() != before;
}

// The same question about one recorded request, asked by handing its error
// back the way the server would have if the request had named a window which
// no longer existed. `prefix` names the call as it appears in the FakeServer's
// log: "RemoveFromSaveSet(0x102)".
//
// This one goes in through the event queue, because that is where XCB delivers
// errors and where lwm picks them up.
bool reportsBadWindowFor(wmtest::World& world, const std::string& prefix) {
  const uint32_t seq = world.server().SequenceOfCall(prefix);
  EXPECT_TRUE(seq != 0) << "lwm never made the request " << prefix
                        << ", so this test is asking about nothing";
  if (!seq) {
    return false;
  }
  const uint64_t before = ReportedErrorCount();
  {
    QuietStderr quiet;
    world.server().PushError(XCB_WINDOW, seq, 0);
    ProcessPendingEvents();
  }
  return ReportedErrorCount() != before;
}

// Catches lwm giving up, and puts is_initialising back on the way out so that
// a test which returns early through an ASSERT can't leave the rest of the
// suite thinking lwm is still starting.
class StartupPanicCatcher {
 public:
  StartupPanicCatcher() {
    panicked_ = false;
    SetPanicHandlerForTest(&Record);
    is_initialising = true;
  }
  ~StartupPanicCatcher() {
    is_initialising = false;
    SetPanicHandlerForTest(nullptr);
  }

  bool Panicked() const { return panicked_; }

 private:
  static void Record(const char*) { panicked_ = true; }

  static bool panicked_;
};

bool StartupPanicCatcher::panicked_ = false;

}  // namespace

TEST(IgnoredErrors, RequestsInsideTheScopeAreSuppressed) {
  wmtest::World world;
  ScopedIgnoreBadWindow ignorer;
  const uint32_t seq = xlib::NextRequestSequence();
  EXPECT_FALSE(reports(badWindow(seq, 0x123)));
}

TEST(IgnoredErrors, RequestsFromBeforeTheScopeAreStillReported) {
  wmtest::World world;
  const uint32_t seq = xlib::NextRequestSequence();
  ScopedIgnoreBadWindow ignorer;
  EXPECT_TRUE(reports(badWindow(seq, 0x123)))
      << "a scope suppresses the requests made inside it, and no others";
}

TEST(IgnoredErrors, RequestsFromAfterTheScopeAreStillReported) {
  wmtest::World world;
  uint32_t seq = 0;
  {
    ScopedIgnoreBadWindow ignorer;
    seq = xlib::NextRequestSequence();
  }
  const uint32_t later = xlib::NextRequestSequence();
  EXPECT_TRUE(later > seq);
  EXPECT_TRUE(reports(badWindow(later, 0x123)));
}

TEST(IgnoredErrors, OnlyTheNamedErrorCodeIsSuppressed) {
  wmtest::World world;
  ScopedIgnoreBadWindow ignorer;
  xcb_generic_error_t err = badWindow(xlib::NextRequestSequence(), 0x123);
  err.error_code = XCB_MATCH;
  EXPECT_TRUE(reports(err))
      << "ScopedIgnoreBadWindow covers BadWindow and BadColor, not BadMatch";
}

TEST(WithdrawingAWindow, ErrorsFromAWindowThatHasGoneAreNotReported) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  const Window w = c->window;

  // The application destroys its window and exits. X sends the UnmapNotify
  // before the DestroyNotify, so lwm withdraws a window that is already gone,
  // and every request withdraw() makes about it fails.
  world.server().ClearCalls();
  xcb_unmap_notify_event_t e{};
  e.response_type = XCB_UNMAP_NOTIFY;
  e.event = c->parent;
  e.window = w;
  world.server().PushEvent(e);
  ProcessPendingEvents();

  // The four requests from the bug report: the save-set removal, then the
  // three properties SetState writes.
  const std::string save_set = "RemoveFromSaveSet(" + winStr(w) + ")";
  EXPECT_FALSE(reportsBadWindowFor(world, save_set))
      << "withdrawing a window that has been destroyed is expected, not news";
  for (const char* name : {"WM_STATE", "_NET_WM_STATE", "_NET_FRAME_EXTENTS"}) {
    testing::Context ctx(name);
    const std::string call = propertyCall(world, w, name);
    ASSERT_TRUE(!call.empty()) << "withdraw() should have written " << name;
    EXPECT_FALSE(reportsBadWindowFor(world, call));
  }
}

TEST(AdoptingAWindow, ErrorsFromAWindowThatDiesDuringManageAreNotReported) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  const Window w = c->window;

  // A window can be destroyed at any point while lwm is taking it on: the
  // application only has to exit between the MapRequest and any of the
  // requests manage() makes. Reparenting it into the frame is the one which
  // matters most, because at start-up an error from it used to be fatal.
  EXPECT_FALSE(reportsBadWindowFor(world, "ReparentWindow(" + winStr(w) + ")"));
  EXPECT_FALSE(reportsBadWindowFor(world, "AddToSaveSet(" + winStr(w) + ")"));
}

TEST(StartupErrors, EverythingBeforeTheMarkIsAStartupError) {
  wmtest::World world;
  const uint32_t seq = xlib::NextRequestSequence();
  MarkEndOfStartupRequests();
  const xcb_generic_error_t err = badWindow(seq, 0x123);
  EXPECT_TRUE(IsStartupError(&err))
      << "a request lwm made while starting up did fail";
}

TEST(StartupErrors, AnErrorFromTheDrainIsNotAStartupError) {
  wmtest::World world;
  MarkEndOfStartupRequests();
  // Everything from here on is a request an event handler made while lwm was
  // draining the queue, which is not the same thing as start-up failing.
  const xcb_generic_error_t err = badWindow(xlib::NextRequestSequence(), 0x123);
  EXPECT_FALSE(IsStartupError(&err));
}

TEST(StartupErrors, AClientDyingDuringAScreenChangeDoesNotStopLwmStarting) {
  // The crash from the bug report, in full: lwm has just started, a client
  // window has gone away, and an XRandR notification that arrived during
  // start-up makes lwm move every window it manages. The ConfigureWindow for
  // the dead one fails, and the error turns up in the same drain of the queue
  // that start-up's own errors come in on - whereupon lwm printed a backtrace
  // and exited, and the user had to start it again by hand.
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  const Window w = c->window;

  MarkEndOfStartupRequests();
  world.server().DestroyWindow(w);
  world.server().ClearCalls();
  LScr::I->SetVisibleAreas({Rect::FromXYWH(0, 0, 1024, 768)});

  const uint32_t seq =
      world.server().SequenceOfCall("ConfigureWindow(" + winStr(w) + ")");
  ASSERT_TRUE(seq != 0) << "the screen change should have moved the client";

  StartupPanicCatcher catcher;
  {
    QuietStderr quiet;
    world.server().PushError(XCB_WINDOW, seq, w);
    ProcessPendingEvents();
  }
  EXPECT_FALSE(catcher.Panicked())
      << "a client window vanishing is not lwm failing to start";
}

TEST(StartupErrors, AnErrorFromStartupItselfIsStillFatal) {
  // The other half of the same rule, so that the fix for the crash above can't
  // quietly turn into "nothing at start-up is ever fatal". A request lwm made
  // while setting itself up failing still means lwm has no business carrying
  // on.
  wmtest::World world;
  const uint32_t seq = xlib::NextRequestSequence();
  MarkEndOfStartupRequests();
  const xcb_generic_error_t err = badWindow(seq, 0x123);

  StartupPanicCatcher catcher;
  {
    QuietStderr quiet;
    HandleXError(&err);
  }
  EXPECT_TRUE(catcher.Panicked());
}
