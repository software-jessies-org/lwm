#ifndef LWM_ERROR_H_included
#define LWM_ERROR_H_included

#include <stdint.h>

// Deliberately <xcb/xcb.h> rather than "xlib.h": xfont.cc and xbridge.cc need
// to report errors too, and they're the translation units that include Xlib,
// which can't coexist with xlib.h's typedefs.
#include <xcb/xcb.h>

// Some events arrive for windows that have already been destroyed, so a
// handful of requests are expected to fail some of the time. Create one of
// these in a scope to say so.
//
// Under Xlib this had to be a global "please ignore BadWindow" flag, switched
// on and off around the requests in question and hoping the error turned up
// while it was on - which only really worked for requests that happened to
// round-trip. XCB gives every error the sequence number of the request that
// caused it, so this instead records the range of sequence numbers issued
// inside the scope and suppresses only errors belonging to those requests.
// Errors from anything else, before or after, still get reported. Nesting is
// fine; ranges accumulate.
class ScopedIgnoreErrors {
 public:
  // Suppresses errors with the given error code, from requests issued during
  // this object's lifetime.
  explicit ScopedIgnoreErrors(uint8_t error_code);
  ~ScopedIgnoreErrors();

 private:
  uint8_t error_code_;
  uint32_t first_sequence_;

  ScopedIgnoreErrors(const ScopedIgnoreErrors&) = delete;
  ScopedIgnoreErrors& operator=(const ScopedIgnoreErrors&) = delete;
};

// BadWindow and BadColor go together: both mean "the resource I was told
// about has since been destroyed".
class ScopedIgnoreBadWindow {
 public:
  ScopedIgnoreBadWindow();

 private:
  ScopedIgnoreErrors window_;
  ScopedIgnoreErrors colour_;
};

class ScopedIgnoreBadMatch {
 public:
  ScopedIgnoreBadMatch() : match_(XCB_MATCH) {}

 private:
  ScopedIgnoreErrors match_;
};

// Reports an error that arrived on the event queue, unless it came from a
// request inside a ScopedIgnore* scope. An error from start-up's own requests
// is fatal while is_initialising is set; see IsStartupError.
extern void HandleXError(const xcb_generic_error_t* err);

// Says that start-up has issued its last request. main() calls this just
// before it syncs and drains the queue looking for the errors start-up
// provoked.
//
// The drain is the reason this exists. It dispatches whatever else has turned
// up as well - a client mapping a window, or, on a machine whose monitors are
// still being sorted out as the session starts, an XRandR ScreenChangeNotify,
// which sends lwm off to move every window it manages. Those handlers issue
// requests of their own, and a request about a client window can always fail
// with BadWindow, because the client can always have closed the window. Errors
// from those are ordinary errors; only the ones from requests issued before
// this mark say that lwm's own start-up went wrong.
extern void MarkEndOfStartupRequests();

// True if this error came from a request issued during start-up, rather than
// from one an event handler made while the queue was being drained. Everything
// is a start-up error until MarkEndOfStartupRequests is called.
extern bool IsStartupError(const xcb_generic_error_t* err);

// How many errors HandleXError has reported, as opposed to suppressed. Tests
// use it to check that an error which is expected - and which lwm is expected
// to keep quiet about - really was swallowed.
extern uint64_t ReportedErrorCount();

// Drops every suppression range, the start-up mark and the reported count.
// Only wmtest::World calls this, for the same reason it calls
// xlib::ForgetLWMWindows(): all of that is keyed on request sequence numbers,
// and each test starts a fresh server whose sequence numbers begin again, so
// a range left over from the last test would suppress this one's errors.
extern void ForgetErrorState();

// Retires suppression ranges that can no longer match anything. Every event
// carries the sequence number of the last request the server had processed
// when it was generated, so once we see one past the end of a range, that
// range is finished with. Called from the event loop for each event.
extern void RetireIgnoredSequences(uint32_t sequence);

extern void panic(const char*);

// Where panic() goes instead of killing the process. Only tests set this, and
// only so that "would lwm have given up here?" can be a question with an
// answer rather than the end of the test binary; pass nullptr to put the real
// behaviour back. HandleXError does nothing after its panic, so a handler
// which returns leaves lwm exactly where a real one would have; the other
// callers of panic() carry on into code that assumes it never returned, so
// don't drive those with a handler installed.
extern void SetPanicHandlerForTest(void (*handler)(const char*));

#endif  // LWM_ERROR_H_included
