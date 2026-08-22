#ifndef LWM_ERROR_H_included
#define LWM_ERROR_H_included

#include <stdint.h>

#include "xlib.h"

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
// request inside a ScopedIgnore* scope.
extern void HandleXError(const xcb_generic_error_t* err);

// Retires suppression ranges that can no longer match anything. Every event
// carries the sequence number of the last request the server had processed
// when it was generated, so once we see one past the end of a range, that
// range is finished with. Called from the event loop for each event.
extern void RetireIgnoredSequences(uint32_t sequence);

extern void panic(const char*);

#endif  // LWM_ERROR_H_included
