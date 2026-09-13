#ifndef LWM_SERVER_H_included
#define LWM_SERVER_H_included

#include <stdint.h>

#include <string>
#include <vector>

#include "geometry.h"
#include "xlib.h"

// The X server, as an interface.
//
// Everything in lwm that talks to X does so through the free functions in
// xlib.h. Those functions do two things: they log, and they compose (an
// XMoveWindow is a ConfigureWindow with two fields set). Underneath them sits
// exactly one primitive operation per X request, and that set of primitives is
// this interface.
//
// There are two implementations. RealServer issues XCB requests, and is what
// runs. FakeServer (fakeserver.h) keeps a scripted window tree in memory,
// answers queries from it, and records every call - which is what makes the
// event handlers, Focuser and the client lifecycle testable without a display.
//
// The split is deliberately drawn *below* the composition in xlib.cc, so what
// the fake records is what would have gone on the wire: XMoveResizeWindow
// shows up as one ConfigureWindow with x, y, width and height set, exactly as
// the server would see it.
//
// See docs/refactoring-plan.md, phase E.
namespace xlib {

// The keyboard mapping, exactly as the X request hands it over: keysyms for
// every keycode from min_keycode to max_keycode inclusive, keysyms_per_keycode
// of them for each, in one flat run. Turning that into "which keycodes carry
// this keysym?" is xlib.cc's job, so the fake has nothing to reimplement.
struct KeyboardMapping {
  uint8_t min_keycode = 0;
  int keysyms_per_keycode = 0;
  std::vector<uint32_t> keysyms;
};

class Server {
 public:
  virtual ~Server() = default;

  // -------------------------------------------------------------------------
  // Connection, screen and event queue.
  // -------------------------------------------------------------------------

  // Opens the connection. Returns false on failure.
  virtual bool Open() = 0;
  virtual void Close() = 0;

  // The Xlib Display backing this connection, as a void*, or null if there
  // isn't one. Only xfont.cc has any business calling this; see xlib.h.
  virtual void* XftDisplay() = 0;

  virtual Window Root() = 0;
  virtual int ScreenCount() = 0;
  virtual int ScreenWidth() = 0;
  virtual int ScreenHeight() = 0;
  virtual unsigned long Black() = 0;
  virtual unsigned long White() = 0;
  virtual Colormap DefaultColourmap() = 0;
  virtual xcb_visualid_t DefaultVisual() = 0;
  virtual uint8_t DefaultDepth() = 0;
  virtual std::string DisplayName() = 0;

  virtual void Sync() = 0;
  virtual void Flush() = 0;
  virtual int ConnectionFD() = 0;
  virtual bool ConnectionIsBroken() = 0;

  // The next queued event, or null if there are none. The caller owns the
  // result and must free() it.
  virtual xcb_generic_event_t* NextEvent() = 0;

  virtual uint32_t NextRequestSequence() = 0;

  // Selects an event mask on the root window. False means another window
  // manager already holds it.
  virtual bool SelectRootEvents(Window root, uint32_t event_mask) = 0;

  // -------------------------------------------------------------------------
  // Windows.
  // -------------------------------------------------------------------------

  // Creates an InputOutput window whose parent is the root window.
  virtual Window CreateWindow(const Rect& rect,
                              unsigned int border_width,
                              unsigned long border_colour,
                              unsigned long background_colour) = 0;
  virtual void DestroyWindow(Window w) = 0;

  virtual void ConfigureWindow(Window w, const WindowChanges& changes) = 0;
  virtual void ChangeWindowAttributes(Window w, const WindowAttrs& attrs) = 0;
  virtual void ReparentWindow(Window w, Window parent, int x, int y) = 0;

  virtual void MapWindow(Window w) = 0;
  virtual void UnmapWindow(Window w) = 0;

  // insert == true adds to the save set, false removes.
  virtual void ChangeSaveSet(Window w, bool insert) = 0;

  virtual void SetInputFocus(Window w, int revert_to, Time t) = 0;
  virtual FocusWindow GetInputFocus() = 0;

  virtual WindowAttributes GetWindowAttributes(Window w) = 0;
  virtual WindowGeometry GetGeometry(Window w) = 0;
  virtual WindowTree QueryTree(Window w) = 0;

  virtual void ClearArea(Window w,
                         int x,
                         int y,
                         unsigned int width,
                         unsigned int height,
                         bool exposures) = 0;
  virtual void FillRectangle(Window w,
                             GC gc,
                             int x,
                             int y,
                             unsigned int width,
                             unsigned int height) = 0;
  virtual void DrawLine(Window w, GC gc, int x1, int y1, int x2, int y2) = 0;
  virtual void CopyArea(Pixmap src,
                        Window dest,
                        GC gc,
                        int src_x,
                        int src_y,
                        int x,
                        int y,
                        unsigned int width,
                        unsigned int height) = 0;

  virtual void KillClient(Window w) = 0;

  // Sends a synthetic event. event32 is exactly 32 bytes, that being the size
  // of every core X11 event on the wire.
  virtual void SendEventRaw(Window w,
                            bool propagate,
                            uint32_t event_mask,
                            const char* event32) = 0;

  // -------------------------------------------------------------------------
  // Input.
  // -------------------------------------------------------------------------

  virtual void GrabButton(unsigned int button,
                          unsigned int modifiers,
                          Window grab_window,
                          bool owner_events,
                          unsigned int event_mask,
                          int pointer_mode,
                          int keyboard_mode,
                          Window confine_to,
                          Cursor cursor) = 0;
  virtual void UngrabButton(unsigned int button,
                            unsigned int modifiers,
                            Window grab_window) = 0;

  // Keyboard grabs take no event mask: while a passive key grab is active,
  // the key events go to grab_window whatever it has asked for.
  virtual void GrabKey(uint8_t keycode,
                       unsigned int modifiers,
                       Window grab_window,
                       bool owner_events,
                       int pointer_mode,
                       int keyboard_mode) = 0;
  virtual void UngrabKey(uint8_t keycode,
                         unsigned int modifiers,
                         Window grab_window) = 0;

  // Reads the whole keyboard mapping. Waits for its reply, but is only called
  // at start-up and on MappingNotify, so the round trip is rare.
  virtual KeyboardMapping GetKeyboardMapping() = 0;
  virtual void ChangeActivePointerGrab(unsigned int event_mask,
                                       Cursor cursor,
                                       Time t) = 0;

  // Takes an active pointer grab, returning XCB_GRAB_STATUS_SUCCESS or one of
  // the other xcb_grab_status_t values.
  //
  // Unlike almost everything else here, this one waits for its reply. A drag
  // started by _NET_WM_MOVERESIZE has no button press of lwm's own behind it,
  // so this grab is the only thing that will deliver the motion and release
  // events which move the window and end the drag. If it fails and we don't
  // notice, lwm installs a DragHandler that can never receive the events that
  // would retire it, and every later mouse gesture is refused with "already
  // doing something". One round trip on a rare, user-initiated action is a
  // cheap price for not wedging the window manager.
  virtual int GrabPointer(Window grab_window,
                          bool owner_events,
                          unsigned int event_mask,
                          int pointer_mode,
                          int keyboard_mode,
                          Window confine_to,
                          Cursor cursor,
                          Time time) = 0;
  virtual void UngrabPointer(Time time) = 0;

  // Takes an active keyboard grab, returning XCB_GRAB_STATUS_SUCCESS or one of
  // the other xcb_grab_status_t values. Waits for its reply for the same
  // reason GrabPointer does: the keyboard-driven unhide menu has no other way
  // to hear the keys which drive it, so there is no point putting it up at all
  // if the grab was refused.
  virtual int GrabKeyboard(Window grab_window,
                           bool owner_events,
                           int pointer_mode,
                           int keyboard_mode,
                           Time time) = 0;
  virtual void UngrabKeyboard(Time time) = 0;

  // The pointer's position right now, straight from the server rather than
  // from the coordinates in the most recent event.
  virtual MousePos QueryPointer() = 0;

  // Moves the pointer to a position in root coordinates. Only the
  // destination-only form of XCB's WarpPointer is exposed: lwm has no use for
  // the source rectangle the protocol allows, which makes the warp
  // conditional on where the pointer already is.
  virtual void WarpPointer(Point to) = 0;

  // -------------------------------------------------------------------------
  // Properties and ICCCM hints.
  // -------------------------------------------------------------------------

  // Interns several atoms at once, returning them in the order given. The
  // batch form is the primitive because that's how start-up uses it: every
  // request goes out before any reply is waited for.
  virtual std::vector<Atom> InternAtoms(
      const std::vector<std::string>& names) = 0;

  virtual void ChangeProperty(Window w,
                              Atom property,
                              Atom type,
                              int format,
                              const void* data,
                              int nelements) = 0;
  virtual void DeleteProperty(Window w, Atom property) = 0;
  virtual WindowProperty GetWindowProperty(Window w,
                                           Atom property,
                                           long length,
                                           Atom req_type) = 0;

  virtual WMHints GetWMHints(Window w) = 0;
  virtual NormalHints GetWMNormalHints(Window w) = 0;

  // Attributes, geometry and normal hints for several windows in one batch.
  virtual std::vector<WindowInfo> QueryWindows(
      const std::vector<Window>& ws) = 0;

  virtual std::vector<Atom> GetWMProtocols(Window w) = 0;
  virtual Window GetTransientForHint(Window w) = 0;

  // -------------------------------------------------------------------------
  // Graphics contexts, colours and cursors.
  // -------------------------------------------------------------------------

  virtual GC CreateGC(Window w, const GCValues& values) = 0;
  virtual void ChangeGC(GC gc, const GCValues& values) = 0;
  virtual void FreeGC(GC gc) = 0;

  // Colour allocation, in the two forms the protocol offers. Both return
  // false if the colour couldn't be allocated; note that AllocNamedColour
  // only knows the server's rgb.txt names and not hex specifications, which
  // is why ColourByName() in xlib.cc parses those itself.
  virtual bool AllocColour(uint16_t r,
                           uint16_t g,
                           uint16_t b,
                           unsigned long* pixel) = 0;
  virtual bool AllocNamedColour(const std::string& name,
                                unsigned long* pixel) = 0;

  // Loads a cursor by its theme name ("left_ptr", "fleur", ...), honouring
  // the user's configured cursor theme and size. Falls back to the core
  // cursor font if the name isn't in the theme, or if there's no theme at all.
  virtual Cursor CreateNamedCursor(const std::string& name) = 0;

  // -------------------------------------------------------------------------
  // Pixmaps and images.
  // -------------------------------------------------------------------------

  // Reads a rectangle of pixels back off the server, one 32-bit word per
  // pixel, row-major, width*height of them. A depth-1 drawable comes back as
  // one word per pixel holding 0 or 1: unpacking the server's bit order and
  // scanline padding is this interface's job, not its callers'.
  virtual std::vector<uint32_t> GetImagePixels(Pixmap src,
                                               int width,
                                               int height,
                                               int depth) = 0;

  // Creates a 24-bit pixmap holding the given width*height pixels.
  virtual Pixmap CreatePixmapFromPixels(int width,
                                        int height,
                                        const uint32_t* data) = 0;
  virtual void FreePixmap(Pixmap p) = 0;

  // -------------------------------------------------------------------------
  // Extensions.
  // -------------------------------------------------------------------------

  virtual RandRSupport RandRQueryExtension() = 0;
  virtual void RandRSelectInput(Window w) = 0;
  virtual std::vector<Rect> RandRGetVisibleAreas(Window root) = 0;

#ifdef SHAPE
  virtual int ShapeQueryExtension() = 0;
  virtual void ShapeSelectInput(Window w) = 0;
  virtual int ShapeCountRectangles(Window w) = 0;
  virtual void ShapeCombineShape(Window dest,
                                 int x_off,
                                 int y_off,
                                 Window src) = 0;
#endif
};

// The server every xlib:: function talks to. Null until SetServer() is called;
// OpenDisplay() installs a RealServer if nothing else has been installed
// first, which is what lets a test install a FakeServer instead simply by
// getting in early.
extern Server* server;

// Installs a server, returning the one previously installed (which the caller
// owns, and which a test will want to put back afterwards). Does not delete
// anything.
extern Server* SetServer(Server* s);

// Builds a server that speaks XCB. Caller owns the result.
extern Server* NewRealServer();

}  // namespace xlib

#endif  // LWM_SERVER_H_included
