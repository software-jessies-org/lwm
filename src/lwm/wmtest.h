#ifndef LWM_WMTEST_H_included
#define LWM_WMTEST_H_included

#include "fakeserver.h"

class Client;

// Test scaffolding for the parts of lwm that need a server.
//
// lwm is built around three singletons - the xlib::Server, Resources::I and
// LScr::I - which main() sets up in a particular order. wmtest::World does the
// same thing against a FakeServer, and puts everything back on the way out, so
// that a test can call into the event handlers, the Focuser or manage() as if
// lwm were running.
//
// One World per test. Construct it on the stack; the singletons are live for
// as long as it is.
//
// See docs/refactoring-plan.md, phase E.
namespace wmtest {

class World {
 public:
  // width and height are the screen size the fake reports.
  explicit World(int width = 1280, int height = 1024);
  ~World();

  xlib::FakeServer& server() { return *server_; }

  // Creates a client window of the given size and drives it through the same
  // MapRequest path a real application's window takes, so the resulting
  // Client has been through manage(): framed, reparented, mapped and focused.
  // Returns null if lwm declined to adopt it.
  Client* MapClientWindow(const Rect& rect);

  // The same, in two halves, for a test which has to script properties or
  // hints on the window before manage() reads them: AddClientWindow creates
  // it, MapWindow sends the MapRequest.
  Window AddClientWindow(const Rect& rect);
  Client* MapWindow(Window w);

  // The text metrics the fake font reports. The frame geometry is derived
  // from these, so tests that assert on positions need them.
  static constexpr int kTextHeight = 16;
  static constexpr int kTextAscent = 12;
  static constexpr int kCharWidth = 8;

 private:
  xlib::FakeServer* server_;
  xlib::Server* previous_server_;

  World(const World&) = delete;
  World& operator=(const World&) = delete;
};

}  // namespace wmtest

#endif  // LWM_WMTEST_H_included
