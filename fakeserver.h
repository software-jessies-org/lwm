#ifndef LWM_FAKESERVER_H_included
#define LWM_FAKESERVER_H_included

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "server.h"

// FakeServer is an xlib::Server that keeps a window tree in memory instead of
// talking to a display.
//
// It exists so that the parts of lwm which are *not* pure geometry can be
// tested: event dispatch, the Focuser's history and its A->B->C race, the
// client lifecycle, and fix_stack()'s ordering. All of those were previously
// reachable only by driving a real window manager on a real display by hand.
//
// It does two things:
//
//  * Answers queries. GetGeometry, GetWindowAttributes, QueryTree, the
//    property and hint getters and so on all read a scripted tree that the
//    test sets up, and which mutating calls keep up to date - so a
//    ConfigureWindow really does move the window that a later GetGeometry
//    reports on, and MapWindow really does make it viewable.
//
//  * Records calls. Every call that would have changed something on the server
//    is appended to a log of readable one-line strings, e.g.
//        ConfigureWindow(0x102) x=10 y=20 width=100 height=50
//    Tests assert on that log. Queries are not recorded: they are answers,
//    not effects, and logging them would bury the effects in noise.
//
// It is not an X server. It implements the parts of the protocol lwm uses,
// with the semantics lwm relies on, and no more. Where the real thing would
// report an error, this generally does nothing quietly - a test that depends
// on error behaviour is testing this file rather than lwm.
namespace xlib {

// A property as stored on a fake window.
struct FakeProperty {
  Atom type = 0;
  int format = 0;
  std::vector<uint32_t> data32;  // format 32
  std::string data8;             // formats 8 and 16
};

// A window in the fake tree.
struct FakeWindow {
  Window id = 0;
  Window parent = 0;
  // Children, bottom-most first. This is the stacking order, which
  // ConfigureWindow's stack_mode maintains: fix_stack()'s whole job is to get
  // this right, so the fake has to model it rather than merely record it.
  std::vector<Window> children;

  // Geometry, relative to the parent.
  Rect rect;
  int border_width = 0;
  int depth = 24;

  bool override_redirect = false;
  bool mapped = false;
  bool input_only = false;
  bool destroyed = false;

  // The event mask lwm has selected, and the union over all clients - which
  // is what GetWindowAttributes reports as all_event_masks, and what
  // Focuser::focusChildrenOf tests to find a Java app's focus proxy.
  uint32_t event_mask = 0;
  uint32_t all_event_masks = 0;

  // ICCCM hints, held as the shim's own structs rather than as encoded
  // properties: the encoding is xcb-icccm's business, not lwm's, and a test
  // that had to build a WM_SIZE_HINTS blob by hand would be testing the wrong
  // thing.
  WMHints wm_hints;
  NormalHints normal_hints;
  Window transient_for = 0;
  std::vector<Atom> wm_protocols;

  // How many rectangles make up the bounding shape. Anything other than 1
  // means non-rectangular, which is what makes lwm refuse to frame it.
  int shape_rectangles = 1;

  std::map<Atom, FakeProperty> properties;
};

class FakeServer : public Server {
 public:
  FakeServer();
  ~FakeServer() override;

  // ---------------------------------------------------------------------
  // Scripting: setting up the world before the code under test runs.
  // ---------------------------------------------------------------------

  // Creates a top-level window owned by a notional client application (as
  // opposed to the windows lwm creates for itself). rect is root-relative.
  Window AddClientWindow(const Rect& rect);

  // Creates a child of an existing window. rect is parent-relative.
  Window AddChildWindow(Window parent, const Rect& rect);

  // The window, or null if there isn't one. Tests use this both to script
  // hints before the call and to check the resulting state after it.
  FakeWindow* Get(Window w);

  void SetProperty32(Window w,
                     Atom property,
                     Atom type,
                     const std::vector<uint32_t>& data);
  void SetProperty8(Window w,
                    Atom property,
                    Atom type,
                    const std::string& data);

  // Queues an event for NextEvent() to hand out. Takes the event struct by
  // reference and pads it out to a full xcb_generic_event_t, so that the
  // dispatcher can cast it to anything and read its full_sequence.
  template <typename T>
  void PushEvent(const T& event) {
    static_assert(sizeof(T) <= 32, "X11 events are 32 bytes on the wire");
    PushEventRaw(&event, sizeof(T));
  }

  // Screen size. Must be set before LScr is constructed to have any effect.
  void SetScreenSize(int width, int height);

  // Where QueryPointer() says the mouse is.
  void SetMousePosition(int x, int y, unsigned int mod_mask);

  // What GrabPointer() answers. Defaults to success; set it to one of the
  // other xcb_grab_status_t values to test what lwm does when the pointer is
  // already grabbed by somebody else.
  void SetPointerGrabStatus(int status) { pointer_grab_status_ = status; }

  // ---------------------------------------------------------------------
  // Inspection: what the code under test did.
  // ---------------------------------------------------------------------

  const std::vector<std::string>& Calls() const { return calls_; }
  void ClearCalls() { calls_.clear(); }

  // The recorded calls whose text starts with the given prefix, which is
  // usually a method name - "ConfigureWindow(" to get every reconfiguration,
  // or "ConfigureWindow(0x102)" to get one window's.
  std::vector<std::string> CallsMatching(const std::string& prefix) const;

  // True if any recorded call starts with the given prefix.
  bool DidCall(const std::string& prefix) const;

  // The window last given input focus, and the stacking order of a window's
  // children (bottom-most first).
  Window FocusedWindow() const { return focused_; }
  std::vector<Window> ChildrenOf(Window w);

  // ---------------------------------------------------------------------
  // Pixmaps.
  // ---------------------------------------------------------------------

  // Creates a pixmap holding the given pixels, as an application does for the
  // icon it hands us in WM_HINTS. Pass a non-zero id to make the pixmap take
  // that particular ID: that's what a real server does once an ID has been
  // freed and its range handed out again to a later client, and it's how the
  // icon cache's key handling gets tested.
  Pixmap CreatePixmapWithPixels(int width,
                                int height,
                                int depth,
                                const std::vector<uint32_t>& pixels,
                                Pixmap id = 0);

  // The atom an interned name was given, interning it if need be, and the
  // name an atom was interned under. Tests need both: the code under test
  // names properties by atom, and the log names them by number.
  Atom Intern(const std::string& name);
  std::string NameOfAtom(Atom a) const;

  // ---------------------------------------------------------------------
  // xlib::Server.
  // ---------------------------------------------------------------------

  bool Open() override;
  void Close() override;
  void* XftDisplay() override;
  Window Root() override;
  int ScreenCount() override;
  int ScreenWidth() override;
  int ScreenHeight() override;
  unsigned long Black() override;
  unsigned long White() override;
  Colormap DefaultColourmap() override;
  xcb_visualid_t DefaultVisual() override;
  uint8_t DefaultDepth() override;
  std::string DisplayName() override;
  void Sync() override;
  void Flush() override;
  int ConnectionFD() override;
  bool ConnectionIsBroken() override;
  xcb_generic_event_t* NextEvent() override;
  uint32_t NextRequestSequence() override;
  bool SelectRootEvents(Window root, uint32_t event_mask) override;

  Window CreateWindow(const Rect& rect,
                      unsigned int border_width,
                      unsigned long border_colour,
                      unsigned long background_colour) override;
  void DestroyWindow(Window w) override;
  void ConfigureWindow(Window w, const WindowChanges& changes) override;
  void ChangeWindowAttributes(Window w, const WindowAttrs& attrs) override;
  void ReparentWindow(Window w, Window parent, int x, int y) override;
  void MapWindow(Window w) override;
  void UnmapWindow(Window w) override;
  void ChangeSaveSet(Window w, bool insert) override;
  void SetInputFocus(Window w, int revert_to, Time t) override;
  FocusWindow GetInputFocus() override;
  WindowAttributes GetWindowAttributes(Window w) override;
  WindowGeometry GetGeometry(Window w) override;
  WindowTree QueryTree(Window w) override;
  void ClearArea(Window w,
                 int x,
                 int y,
                 unsigned int width,
                 unsigned int height,
                 bool exposures) override;
  void FillRectangle(Window w,
                     GC gc,
                     int x,
                     int y,
                     unsigned int width,
                     unsigned int height) override;
  void DrawLine(Window w, GC gc, int x1, int y1, int x2, int y2) override;
  void CopyArea(Pixmap src,
                Window dest,
                GC gc,
                int src_x,
                int src_y,
                int x,
                int y,
                unsigned int width,
                unsigned int height) override;
  void KillClient(Window w) override;
  void SendEventRaw(Window w,
                    bool propagate,
                    uint32_t event_mask,
                    const char* event32) override;

  void GrabButton(unsigned int button,
                  unsigned int modifiers,
                  Window grab_window,
                  bool owner_events,
                  unsigned int event_mask,
                  int pointer_mode,
                  int keyboard_mode,
                  Window confine_to,
                  Cursor cursor) override;
  void UngrabButton(unsigned int button,
                    unsigned int modifiers,
                    Window grab_window) override;
  void GrabKey(uint8_t keycode,
               unsigned int modifiers,
               Window grab_window,
               bool owner_events,
               int pointer_mode,
               int keyboard_mode) override;
  void UngrabKey(uint8_t keycode,
                 unsigned int modifiers,
                 Window grab_window) override;
  KeyboardMapping GetKeyboardMapping() override;
  void ChangeActivePointerGrab(unsigned int event_mask,
                               Cursor cursor,
                               Time t) override;
  int GrabPointer(Window grab_window,
                  bool owner_events,
                  unsigned int event_mask,
                  int pointer_mode,
                  int keyboard_mode,
                  Window confine_to,
                  Cursor cursor,
                  Time time) override;
  void UngrabPointer(Time time) override;
  MousePos QueryPointer() override;
  void WarpPointer(Point to) override;

  std::vector<Atom> InternAtoms(const std::vector<std::string>& names) override;
  void ChangeProperty(Window w,
                      Atom property,
                      Atom type,
                      int format,
                      const void* data,
                      int nelements) override;
  void DeleteProperty(Window w, Atom property) override;
  WindowProperty GetWindowProperty(Window w,
                                   Atom property,
                                   long length,
                                   Atom req_type) override;
  WMHints GetWMHints(Window w) override;
  NormalHints GetWMNormalHints(Window w) override;
  std::vector<WindowInfo> QueryWindows(const std::vector<Window>& ws) override;
  std::vector<Atom> GetWMProtocols(Window w) override;
  Window GetTransientForHint(Window w) override;

  GC CreateGC(Window w, const GCValues& values) override;
  void ChangeGC(GC gc, const GCValues& values) override;
  void FreeGC(GC gc) override;
  bool AllocColour(uint16_t r,
                   uint16_t g,
                   uint16_t b,
                   unsigned long* pixel) override;
  bool AllocNamedColour(const std::string& name,
                        unsigned long* pixel) override;
  Cursor CreateNamedCursor(const std::string& name) override;

  std::vector<uint32_t> GetImagePixels(Pixmap src,
                                       int width,
                                       int height,
                                       int depth) override;
  Pixmap CreatePixmapFromPixels(int width,
                                int height,
                                const uint32_t* data) override;
  void FreePixmap(Pixmap p) override;

  RandRSupport RandRQueryExtension() override;
  void RandRSelectInput(Window w) override;
  std::vector<Rect> RandRGetVisibleAreas(Window root) override;

#ifdef SHAPE
  int ShapeQueryExtension() override;
  void ShapeSelectInput(Window w) override;
  int ShapeCountRectangles(Window w) override;
  void ShapeCombineShape(Window dest, int x_off, int y_off, Window src) override;
#endif

 private:
  void PushEventRaw(const void* event, size_t len);
  void Record(const std::string& call) { calls_.push_back(call); }
  Window NewWindow(Window parent, const Rect& rect, bool lwm_owned);
  void Restack(FakeWindow* win, uint32_t stack_mode, Window sibling);
  void Unlink(FakeWindow* win);

  // A pixmap's contents, so that icon rendering can be driven from a test.
  struct FakePixmap {
    int width = 0;
    int height = 0;
    int depth = 0;
    std::vector<uint32_t> pixels;
  };

  std::map<Window, FakeWindow> windows_;
  std::map<Pixmap, FakePixmap> pixmaps_;
  std::map<std::string, Atom> atoms_;
  std::map<Atom, std::string> atom_names_;
  std::deque<xcb_generic_event_t*> events_;

  Window root_ = 0;
  int width_ = 1280;
  int height_ = 1024;
  Window focused_ = 0;
  MousePos mouse_{};
  int pointer_grab_status_ = XCB_GRAB_STATUS_SUCCESS;
  uint32_t next_id_ = 0x100;
  Atom next_atom_ = 1000;
  uint32_t sequence_ = 1;

  std::vector<std::string> calls_;
};

}  // namespace xlib

#endif  // LWM_FAKESERVER_H_included
