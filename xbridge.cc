#include "xbridge.h"

// One of only two translation units allowed to include Xlib; see xbridge.h.
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

namespace xbridge {
namespace {

// ::Display, not xbridge::Display: the function below shadows the type name
// inside this namespace, which is the one wart of having an X11 type and an
// accessor with the same name.
::Display* dpy;
ErrorFunc error_func;

int handleError(::Display*, XErrorEvent* e) {
  if (error_func) {
    error_func(e->error_code, uint32_t(e->resourceid), e->request_code,
               e->minor_code, uint32_t(e->serial));
  }
  return 0;
}

}  // namespace

xcb_connection_t* Open(ErrorFunc on_error) {
  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    return nullptr;
  }
  error_func = on_error;
  XSetErrorHandler(handleError);
  xcb_connection_t* conn = XGetXCBConnection(dpy);
  if (!conn) {
    return nullptr;
  }
  // After this, Xlib's XNextEvent and friends must not be called - which is
  // the point, since we want a single event queue and it should be the XCB
  // one. Xft only makes requests, never reads events, so it is unaffected.
  XSetEventQueueOwner(dpy, XCBOwnsEventQueue);
  return conn;
}

void Close() {
  XCloseDisplay(dpy);
}

void Flush() {
  XFlush(dpy);
}

void* Display() {
  return dpy;
}

std::string DisplayName() {
  // DisplayString is a macro, so it doesn't mind the shadowed type name.
  const char* s = DisplayString(dpy);
  return s ? std::string(s) : std::string();
}

}  // namespace xbridge
