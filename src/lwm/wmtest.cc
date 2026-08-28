#include "wmtest.h"

#include "client.h"
#include "disp.h"
#include "ewmh.h"
#include "lwm.h"
#include "resource.h"
#include "screen.h"
#include "server.h"
#include "xfont.h"

namespace wmtest {

World::World(int width, int height)
    : server_(new xlib::FakeServer), previous_server_(nullptr) {
  server_->SetScreenSize(width, height);
  previous_server_ = xlib::SetServer(server_);
  // The new FakeServer hands out window ids from the beginning again, so any
  // id the last test's lwm windows claimed would make this test's client
  // windows look like lwm's own - which lwm declines to manage. How far the
  // ids get depends on how many windows a test creates, so without this the
  // tests are order-dependent in a way that only shows up when someone adds
  // one.
  xlib::ForgetLWMWindows();

  // From here on the order matches main()'s, because the dependencies are the
  // same ones: Resources before anything that reads a colour, the atoms before
  // anything that names a property, the font before LScr computes a title bar
  // height, and LScr last.
  xlib::OpenDisplay();

  delete Resources::I;
  Resources::I = nullptr;
  Resources::Init();

  const std::vector<Atom> atoms = xlib::XInternAtoms({
      "WM_STATE",
      "WM_CHANGE_STATE",
      "WM_PROTOCOLS",
      "WM_DELETE_WINDOW",
      "WM_TAKE_FOCUS",
      "COMPOUND_TEXT",
      "_MOZILLA_URL",
      "_MOTIF_WM_HINTS",
  });
  wm_state = atoms[0];
  wm_change_state = atoms[1];
  wm_protocols = atoms[2];
  wm_delete = atoms[3];
  wm_take_focus = atoms[4];
  compound_text = atoms[5];
  _mozilla_url = atoms[6];
  motif_wm_hints = atoms[7];

  ewmh_init();
  xfont::InitForTest(kTextHeight, kTextAscent, kCharWidth);

  // The Shape extension is present but every window is rectangular unless the
  // test says otherwise, which is what lets framing decisions be tested.
  shape = true;
  shape_event = xlib::XShapeQueryExtension();

  // Errors are not fatal: is_initialising is what makes them so, and a test is
  // never in start-up.
  is_initialising = false;

  LScr::I = new LScr();
  LScr::I->Init();
  server_->ClearCalls();
}

World::~World() {
  // LScr owns its clients but has no destructor - lwm never tears one down,
  // because the process exits instead. Leaking them for the length of a test
  // binary is cheaper than inventing a shutdown path that nothing else uses.
  LScr::I = nullptr;
  xlib::SetServer(previous_server_);
  delete server_;
}

Client* World::MapClientWindow(const Rect& rect) {
  const Window w = server_->AddClientWindow(rect);
  xcb_map_request_event_t ev{};
  ev.response_type = XCB_MAP_REQUEST;
  ev.parent = server_->Root();
  ev.window = w;
  server_->PushEvent(ev);
  ProcessPendingEvents();
  return LScr::I->GetClient(w, false);
}

}  // namespace wmtest
