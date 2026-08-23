#ifndef LWM_XLIB_H_included
#define LWM_XLIB_H_included

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <type_traits>
#include <vector>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "geometry.h"
#include "log.h"

// The connection to the X server. Opened by xlib::OpenDisplay(), and declared
// here because nearly every file that touches X needs it.
extern xcb_connection_t* conn;

// X11's resource IDs. These are all 32-bit server-side handles; the distinct
// names are documentation, since the compiler can't tell them apart.
typedef xcb_window_t Window;
typedef xcb_atom_t Atom;
typedef xcb_cursor_t Cursor;
typedef xcb_pixmap_t Pixmap;
typedef xcb_gcontext_t GC;
typedef xcb_colormap_t Colormap;
typedef xcb_timestamp_t Time;

// The ICCCM WM_STATE values (section 4.1.3.1). Spelled out rather than used
// via xcb-icccm's much longer names, because client state is compared against
// these all over the place and the long form makes it unreadable.
constexpr int WithdrawnState = XCB_ICCCM_WM_STATE_WITHDRAWN;
constexpr int NormalState = XCB_ICCCM_WM_STATE_NORMAL;
constexpr int IconicState = XCB_ICCCM_WM_STATE_ICONIC;

struct MousePos {
  int x;
  int y;
  // For mask values, see:
  // https://tronche.com/gui/x/xlib/events/keyboard-pointer/keyboard-pointer.html
  unsigned int modMask;
};

// Queries the server directly for the current pointer location, rather than
// relying on the coordinates in the most recent event (which may be stale by
// the time a drag handler acts on it).
extern MousePos getMousePosition();

namespace xlib {

// FreeReplyData releases a block of memory handed to us by the X client
// library. Which deallocator that means is the library's business, not the
// caller's. Defined once, in xlib.cc.
extern void FreeReplyData(void* data);

// Reply owns a block of memory returned by the X client library and releases
// it when it goes out of scope, so that functions with several early returns
// don't need a matching chain of frees down every path.
// It is safe to construct one holding nullptr.
template <typename T>
class Reply {
 public:
  Reply() = default;
  explicit Reply(T* data) : data_(data) {}
  Reply(Reply&& o) noexcept : data_(o.data_) { o.data_ = nullptr; }
  Reply& operator=(Reply&& o) noexcept {
    if (this != &o) {
      release();
      data_ = o.data_;
      o.data_ = nullptr;
    }
    return *this;
  }
  ~Reply() { release(); }

  T* get() const { return data_; }
  T* operator->() const { return data_; }
  T& operator*() const { return *data_; }
  explicit operator bool() const { return data_ != nullptr; }

 private:
  void release() {
    if (data_) {
      FreeReplyData(data_);
      data_ = nullptr;
    }
  }

  T* data_ = nullptr;

  Reply(const Reply&) = delete;
  Reply& operator=(const Reply&) = delete;
};

// ValueList is the fix for one of XCB's sharper edges. Where Xlib took a
// struct plus a mask and picked out the fields the mask named, XCB takes a
// bare uint32_t array whose entries must appear in increasing order of their
// mask bit - and checks nothing. Get the order wrong and you set the wrong
// attribute to the wrong value, silently.
//
// So no caller ever builds one of those arrays by hand. They add (bit, value)
// pairs in whatever order suits them, and this sorts.
class ValueList {
 public:
  void Add(uint32_t mask_bit, uint32_t value);

  uint32_t Mask() const { return mask_; }
  // The values, ordered by mask bit, as xcb wants them.
  const uint32_t* Values() const { return values_; }
  bool Empty() const { return mask_ == 0; }

 private:
  // 32 is the widest any of the X11 value lists gets (one entry per mask bit).
  uint32_t values_[32] = {};
  uint32_t mask_ = 0;
};

// The window attributes lwm sets. Wraps a ValueList so that call sites read
// as prose and can't get the ordering wrong.
class WindowAttrs {
 public:
  WindowAttrs& EventMask(uint32_t mask);
  WindowAttrs& Cursor(::Cursor c);
  WindowAttrs& WinGravity(uint32_t gravity);
  WindowAttrs& DontPropagate(uint32_t mask);
  WindowAttrs& BackPixel(uint32_t pixel);

  const ValueList& Values() const { return values_; }

 private:
  ValueList values_;
};

// The window geometry/stacking changes lwm makes. Same deal.
class WindowChanges {
 public:
  WindowChanges& X(int x);
  WindowChanges& Y(int y);
  WindowChanges& Width(int w);
  WindowChanges& Height(int h);
  WindowChanges& BorderWidth(int bw);
  WindowChanges& Sibling(Window w);
  WindowChanges& StackMode(uint32_t mode);

  // Sets x, y, width and height from a rectangle.
  WindowChanges& Rectangle(const Rect& r);

  // Sets only the fields named by a client's ConfigureRequest mask, from that
  // request's values. Used where lwm passes a client's request through.
  WindowChanges& FromRequestMask(uint16_t value_mask,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 int border_width,
                                 Window sibling,
                                 uint32_t stack_mode);

  const ValueList& Values() const { return values_; }

 private:
  ValueList values_;
};

// The graphics context settings lwm uses.
class GCValues {
 public:
  GCValues& Function(uint32_t fn);
  GCValues& Foreground(uint32_t pixel);
  GCValues& Background(uint32_t pixel);
  GCValues& LineWidth(uint32_t width);
  GCValues& LineStyle(uint32_t style);
  GCValues& CapStyle(uint32_t style);
  GCValues& JoinStyle(uint32_t style);
  GCValues& SubwindowMode(uint32_t mode);

  const ValueList& Values() const { return values_; }

 private:
  ValueList values_;
};

// ---------------------------------------------------------------------------
// Connection, screen and event queue.
// ---------------------------------------------------------------------------

// Opens the connection to the X server. Returns false on failure.
extern bool OpenDisplay();
extern void CloseDisplay();

// Returns the Xlib Display that backs our connection, as a void* so that this
// header doesn't have to name an Xlib type. xfont.cc is the only legitimate
// caller: Xft has no XCB port, so text rendering is the one thing that still
// needs a real Display. Everything else goes over XCB.
extern void* XftDisplay();

// Facts about the screen, from the connection setup. lwm supports exactly one
// screen; ScreenCount() exists only so start-up can complain about the rest.
extern Window Root();
extern int ScreenCount();
extern int ScreenWidth();
extern int ScreenHeight();
extern unsigned long Black();
extern unsigned long White();
extern Colormap DefaultColourmap();
extern xcb_visualid_t DefaultVisual();
extern uint8_t DefaultDepth();

// The DISPLAY value to hand to child processes.
extern std::string DisplayName();

// Waits for every request issued so far to be processed by the server, so
// that any errors they provoke have arrived before we carry on. This is a
// real round trip, so use it sparingly.
extern void Sync();

// Pushes everything queued to the server. XCB never does this behind your
// back, so exactly one place calls it: the top of the event loop, just before
// select(). Individual shim functions must not.
extern void Flush();

// The file descriptor to select() on for incoming events.
extern int ConnectionFD();

// True if the connection has been shut down (server exit, protocol error).
extern bool ConnectionIsBroken();

// Returns the next queued event, or null if there are none. The caller owns
// the result and must free() it.
extern xcb_generic_event_t* NextEvent();

// The sequence number the next request issued will get. Used to bracket a
// range of requests whose errors we intend to ignore; see error.h.
extern uint32_t NextRequestSequence();

// Selects the given event mask on the root window, which is how a window
// manager claims the display. Returns false if another window manager
// already holds it - unlike Xlib, XCB can answer that question immediately
// rather than via a deferred BadAccess in the error handler.
extern bool SelectRootEvents(Window root, uint32_t event_mask);

// ---------------------------------------------------------------------------
// Windows.
// ---------------------------------------------------------------------------

extern int XMoveResizeWindow(Window w, const Rect& r);
extern int XMoveResizeWindow(Window w,
                             int x,
                             int y,
                             unsigned width,
                             unsigned height);

extern int XMoveWindow(Window w, const Point& origin);
extern int XMoveWindow(Window w, int x, int y);
extern int XResizeWindow(Window w, const Area& area);
extern int XResizeWindow(Window w, unsigned width, unsigned height);

extern int XReparentWindow(Window w, Window new_parent, int x, int y);

extern int XMapWindow(Window w);
extern int XMapRaised(Window w);
extern int XUnmapWindow(Window w);
extern int XRaiseWindow(Window w);
extern int XLowerWindow(Window w);

extern int XAddToSaveSet(Window w);
extern int XRemoveFromSaveSet(Window w);

extern int XSetInputFocus(Window focus, int revert_to, Time time);

struct FocusWindow {
  Window window;
  int revert_to;
};
extern FocusWindow XGetInputFocus();

extern int XConfigureWindow(Window w, const WindowChanges& changes);
extern int XChangeWindowAttributes(Window w, const WindowAttrs& attrs);

extern void SendClientMessage(Window w, Atom a, long data0, long data1);

// What lwm wants to know about a window. Note that under XCB this needs two
// requests, since GetWindowAttributes carries no geometry - Xlib was hiding
// a second round trip here all along.
struct WindowAttributes {
  Rect rect;
  int border_width = 0;
  bool override_redirect = false;
  // True if the window is mapped and all its ancestors are mapped.
  bool viewable = false;
  // InputOnly windows have no border width to set, among other things.
  bool input_only = false;
  // The union of every client's event selections on this window, used to spot
  // which of a Java app's child windows will accept focus.
  uint32_t all_event_masks = 0;
  bool ok = false;
};

extern WindowAttributes XGetWindowAttributes(Window w);

struct WindowGeometry {
  // The root window of the screen the window is on, which is what the
  // GetGeometry reply carries - not the window's parent. Xlib's XGetGeometry
  // returned the same thing.
  Window root;
  Rect rect;
  int border_width;
  int bpp;
  // ok == true if the window geometry was fetched correctly.
  bool ok;
};

extern WindowGeometry XGetGeometry(Window w);

extern int XDestroyWindow(Window w);
extern int XSetWindowBorderWidth(Window w, unsigned int width);
extern int XSetWindowBackground(Window w, unsigned long pixel);
extern int XClearWindow(Window w);
extern int XClearArea(Window w,
                      int x,
                      int y,
                      unsigned int width,
                      unsigned int height,
                      bool exposures);
extern int XFillRectangle(Window w,
                          GC gc,
                          int x,
                          int y,
                          unsigned int width,
                          unsigned int height);
extern int XDrawLine(Window w, GC gc, int x1, int y1, int x2, int y2);

extern int XKillClient(Window w);

// Sends a synthetic event to a window. Takes exactly 32 bytes off the wire,
// that being the size of every core X11 event.
extern void SendEventRaw(Window w,
                         bool propagate,
                         uint32_t event_mask,
                         const char* event32);

// Sends a synthetic event, given the appropriate XCB event struct.
// The padding matters: xcb_send_event always copies 32 bytes from the pointer
// it's given, whatever the struct's own size, and several event structs are
// smaller than that (a ConfigureNotify is 28 bytes). Passing one straight in
// therefore reads off the end of it and puts whatever was next in memory on
// the wire, so everything goes through this padded buffer instead.
template <typename T>
void SendEvent(Window w, bool propagate, uint32_t event_mask, const T& event) {
  // Pass the event struct, not a pointer to it. Handing this a pointer used
  // to compile perfectly happily and then memcpy the pointer's own bytes onto
  // the wire, which the server rejects with a BadValue naming a byte of the
  // caller's stack address as the "event type".
  static_assert(!std::is_pointer<T>::value,
                "pass the event struct by reference, not its address");
  static_assert(sizeof(T) <= 32, "X11 events are 32 bytes on the wire");
  char buf[32] = {};
  memcpy(buf, &event, sizeof(T));
  SendEventRaw(w, propagate, event_mask, buf);
}

// Creates a window with the given properties, whose parent is the root window.
extern Window CreateNamedWindow(const std::string& name,
                                const Rect& rect,
                                unsigned int border_width,
                                unsigned long border_colour,
                                unsigned long background_colour);

extern bool IsLWMWindow(Window w);

struct WindowTree {
  Window self;
  Window parent;
  Window root;
  std::vector<Window> children;

  // Query returns the set of children of the given window.
  static WindowTree Query(Window w);

  // Parent returns the parent window of w, or 0 if the parent is the root
  // window.
  static Window ParentOf(Window w);
};

// ---------------------------------------------------------------------------
// Input.
// ---------------------------------------------------------------------------

extern int XGrabButton(unsigned int button,
                       unsigned int modifiers,
                       Window grab_window,
                       bool owner_events,
                       unsigned int event_mask,
                       int pointer_mode,
                       int keyboard_mode,
                       Window confine_to,
                       Cursor cursor);
extern int XUngrabButton(unsigned int button,
                         unsigned int modifiers,
                         Window grab_window);

extern void XChangeActivePointerGrab(unsigned int event_mask,
                                     Cursor cursor,
                                     Time time);

// Takes an active pointer grab. Returns true if we got it. See
// Server::GrabPointer for why this one waits for its reply.
extern bool XGrabPointer(Window grab_window,
                         bool owner_events,
                         unsigned int event_mask,
                         int pointer_mode,
                         int keyboard_mode,
                         Window confine_to,
                         Cursor cursor,
                         Time time);
extern void XUngrabPointer(Time time);

// ---------------------------------------------------------------------------
// Properties and ICCCM hints.
// ---------------------------------------------------------------------------

extern Atom XInternAtom(const std::string& name);

// Interns several atoms at once, returning them in the order given.
// Under Xlib each intern was a separate blocking round trip; here every
// request goes out before any reply is waited for, so start-up's ~70 interns
// cost one round trip between them rather than seventy.
extern std::vector<Atom> XInternAtoms(const std::vector<std::string>& names);

extern int XChangeProperty(Window w,
                           Atom property,
                           Atom type,
                           int format,
                           const void* data,
                           int nelements);

extern void XDeleteProperty(Window w, Atom property);

// WindowProperty is the result of reading a property off a window. It owns
// its data, so there is nothing for the caller to free.
//
// Note the deliberate absence of a raw `unsigned char* data`. Xlib expanded a
// format-32 property into an array of `long`, which is 64 bits on LP64, while
// XCB hands back the actual 32-bit wire data. Call sites that cast the raw
// bytes to `unsigned long*` were therefore correct under one library and
// silently wrong under the other, and the symptom is garbage (corrupt icons,
// nonsense struts) rather than a crash. So the raw pointer is not on offer:
// read 32-bit properties through Data32(), strings through Data8(), and let
// this one place own the difference. See docs/xcb-migration-plan.md, hazard 1.
struct WindowProperty {
  Atom actual_type = 0;
  int actual_format = 0;  // Bits per item: 8, 16 or 32.
  unsigned long nitems = 0;
  unsigned long bytes_after = 0;
  bool success = false;

  // ok() is true if the property was read and had some content.
  bool ok() const { return success && nitems > 0; }

  // The property contents as 32-bit words. Empty unless actual_format == 32.
  const std::vector<uint32_t>& Data32() const { return data32; }

  // The property contents as bytes. Used for string properties (format 8).
  const std::string& Data8() const { return data8; }

  std::vector<uint32_t> data32;
  std::string data8;
};

// Reads a property, from offset 0 and without deleting it, which is what
// every call site in this codebase wants. length is in 32-bit multiples, as
// the underlying request defines it. Pass type = kAnyPropertyType to accept
// whatever type the property happens to have.
extern const Atom kAnyPropertyType;
extern WindowProperty XGetWindowProperty(Window w,
                                         Atom property,
                                         long length,
                                         Atom req_type);

// The parts of WM_HINTS lwm cares about (ICCCM section 4.1.2.4).
struct WMHints {
  bool ok = false;
  bool has_input = false;
  bool input = false;
  bool has_initial_state = false;
  int initial_state = 0;
  Pixmap icon_pixmap = 0;
  Pixmap icon_mask = 0;
};
extern WMHints XGetWMHints(Window w);

// The parts of WM_NORMAL_HINTS lwm cares about (ICCCM section 4.1.2.3).
// A missing field is reported as absent rather than defaulted, because the
// DimensionLimiter treats "no minimum" differently from "minimum of zero".
struct NormalHints {
  bool ok = false;
  bool has_min_size = false;
  bool has_max_size = false;
  bool has_base_size = false;
  bool has_resize_inc = false;
  int min_width = 0, min_height = 0;
  int max_width = 0, max_height = 0;
  int base_width = 0, base_height = 0;
  int width_inc = 1, height_inc = 1;
};
extern NormalHints XGetWMNormalHints(Window w);

// Everything lwm needs before it can decide whether to adopt a window.
struct WindowInfo {
  WindowAttributes attributes;
  NormalHints normal_hints;
};

// Queries several windows at once. Every request goes out before any reply is
// waited for, so scanning the window tree at start-up costs one round trip
// rather than three per window already on screen.
extern std::vector<WindowInfo> QueryWindows(const std::vector<Window>& ws);

// Returns the atoms listed in the window's WM_PROTOCOLS property, or an empty
// vector if it has none.
extern std::vector<Atom> XGetWMProtocols(Window w);

// Returns 0 if the window has no WM_TRANSIENT_FOR hint.
extern Window XGetTransientForHint(Window w);

// ---------------------------------------------------------------------------
// Graphics contexts, colours and cursors.
// ---------------------------------------------------------------------------

extern GC XCreateGC(Window w, const GCValues& values);

// Changes settings on an existing GC. Used for the dotted separator line in
// the unhide menu, which shares the menu's GC.
extern void XChangeGC(GC gc, const GCValues& values);

// Looks up a colour by name ("white", "#A0522D", ...) and returns the pixel
// value to draw with. Returns black if the name can't be resolved.
extern unsigned long ColourByName(const std::string& name);

// Creates a cursor from the standard cursor font. Unlike Xlib, the colours
// are given at creation time, so there is no separate recolour step.
extern Cursor CreateFontCursor(unsigned int shape,
                               unsigned long fg,
                               unsigned long bg);

// ---------------------------------------------------------------------------
// Extensions.
// ---------------------------------------------------------------------------

struct RandRSupport {
  bool have_rr;
  int event_base;
};
// Initialises RandR and returns the event number its ScreenChangeNotify will
// arrive as. Must be called before any other RandR request.
extern RandRSupport XRRQueryExtension();
extern void XRRSelectInput(Window w);

// The visible area of each enabled monitor. setScreenAreasFromXRandR() wants
// nothing else out of RandR, so the shim reduces the whole CRTC walk to this.
// Disabled CRTCs (mode == 0) are left out.
extern std::vector<Rect> XRRGetVisibleAreas(Window root);

#ifdef SHAPE
// Initialises the Shape extension and returns the event number its
// ShapeNotify will arrive as, or -1 if the server doesn't support it.
extern int XShapeQueryExtension();
extern void XShapeSelectInput(Window w);
// Returns how many rectangles make up the window's bounding shape. lwm only
// ever asks "is this more than one?" (i.e. is the window non-rectangular), so
// the shim returns the count rather than a list the caller then has to free.
extern int XShapeCountRectangles(Window w);
// Applies src's bounding shape to dest, offset by (x_off, y_off).
extern void XShapeCombineShape(Window dest, int x_off, int y_off, Window src);
#endif

// ---------------------------------------------------------------------------
// Icons.
// ---------------------------------------------------------------------------

// ImageIcon holds and image, and optionally a mask, for painting an icon on
// the screen. It is used to draw application icons in the unhide menu, and in
// the title bar of windows that have them.
// Given a specific box to draw into, this will draw the image in the middle
// of the box, or if the icon is larger than the box it clips the image so that
// the image's middle is visible inside the given box.
// Images are not scaled.
class ImageIcon {
 public:
  ~ImageIcon();

  // Create either creates an ImageIcon capable of drawing the icon on a 24bit
  // display, or returns null.
  static ImageIcon* Create(Pixmap img, Pixmap mask);

  // Create an ImageIcon from an array of 32-bit words.
  // The format is as used for _NET_WM_ICON, so the first two values are the
  // width and height, and then there's one value per pixel. There may be more
  // than one icon, which appears after the first.
  // Again, returns null if there was a problem.
  // The data is not freed - that's the caller's job.
  // These really are 32 bits per pixel and not `unsigned long`: see the note
  // on WindowProperty above.
  static ImageIcon* CreateFromPixels(const uint32_t* data, size_t len);

  // Paints the image with the 'inactive' background on the given window,
  // centred within the box given by x, y, w, h.
  void PaintInactive(Window w, int x, int y, int width, int height) {
    paint(w, inactive_img_, x, y, width, height);
  }

  // Paints the image with the 'active' background on the given window,
  // centred within the box given by x, y, w, h.
  void PaintActive(Window w, int x, int y, int width, int height) {
    paint(w, active_img_, x, y, width, height);
  }

  // Paints the image with the menu's white background on the given window,
  // centred within the box given by x, y, w, h.
  void PaintMenu(Window w, int x, int y, int width, int height) {
    paint(w, menu_img_, x, y, width, height);
  }

  // ConfigureIconSizes tells X11 what sizes we desire for window icons.
  // This is used by applications to scale their icons to desirable sizes.
  // Otherwise, the defaults they use are rather random (Chrome, Firefox,
  // FreeBSD use quite large sizes, but Java scales everything to 16x16, which
  // looks ugly).
  static void ConfigureIconSizes();

  void destroyResources();

 private:
  ImageIcon(Pixmap active_img,
            Pixmap inactive_img,
            Pixmap menu_img,
            unsigned int img_w,
            unsigned int img_h,
            unsigned int depth);

  void paint(Window w, Pixmap img, int x, int y, int width, int height);

  ImageIcon* clone(unsigned long hash);

  Pixmap active_img_;
  Pixmap inactive_img_;
  Pixmap menu_img_;
  unsigned int img_w_ = 0;
  unsigned int img_h_ = 0;
  unsigned int depth_ = 0;
  unsigned long gc_hash_ = 0;
};

}  // namespace xlib

/*
 * This should really have been in the protocol headers --- if you select both
 * ButtonPress and ButtonRelease events, the server makes an automatic grab on
 * the pressed button for you. This is almost always exactly what you want.
 */
#define ButtonMask \
  (XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE)

#endif  // LWM_XLIB_H_included
