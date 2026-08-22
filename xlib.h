#ifndef LWM_XLIB_H_included
#define LWM_XLIB_H_included

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include <X11/SM/SMlib.h>
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xproto.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif

#include "geometry.h"
#include "log.h"

// The connection to the X server. Defined and opened in lwm.cc's main(),
// but declared here since nearly every file that touches X needs it.
extern Display* dpy;

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
// caller's: Xlib wants XFree, XCB's replies are plain malloc'd. Defined once,
// in xlib.cc, so that switching libraries is a one-line change.
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

// Returns the Xlib Display that backs our connection, as a void* so that this
// header doesn't have to name an Xlib type. xfont.cc is the only legitimate
// caller: Xft has no XCB port, so text rendering is the one thing that still
// needs a real Display. Everything else goes over XCB.
extern void* XftDisplay();

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

extern int XConfigureWindow(Window w, unsigned int val_mask, XWindowChanges* v);
extern int XChangeWindowAttributes(Window w,
                                   unsigned int val_mask,
                                   XSetWindowAttributes* v);

extern void SendClientMessage(Window w, Atom a, long data0, long data1);

extern XWindowAttributes XGetWindowAttributes(Window w);

struct WindowGeometry {
  Window parent;
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
extern int XSendEvent(Window w, bool propagate, long event_mask, XEvent* event);

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

extern Atom XInternAtom(const std::string& name);

extern int XChangeProperty(Window w,
                           Atom property,
                           Atom type,
                           int format,
                           int mode,
                           const unsigned char* data,
                           int nelements);

// WindowProperty is the result of reading a property off a window. It owns
// its data, so there is nothing for the caller to free.
//
// Note the deliberate absence of a raw `unsigned char* data`. Xlib expands a
// format-32 property into an array of `long`, which is 64 bits on LP64, while
// XCB hands back the actual 32-bit wire data. Call sites that cast the raw
// bytes to `unsigned long*` are therefore correct under one library and
// silently wrong under the other, and the symptom is garbage (corrupt icons,
// nonsense struts) rather than a crash. So the raw pointer is not on offer:
// read 32-bit properties through Data32(), strings through Data8(), and let
// this one place own the difference. See docs/xcb-migration-plan.md, hazard 1.
struct WindowProperty {
  Atom actual_type = 0;
  int actual_format = 0;  // Bits per item: 8, 16 or 32.
  unsigned long nitems = 0;
  unsigned long bytes_after = 0;
  int status = 0;  // Success, or an X error code.

  // ok() is true if the property was read and had some content.
  bool ok() const { return status == Success && nitems > 0; }

  // The property contents as 32-bit words. Empty unless actual_format == 32.
  const std::vector<uint32_t>& Data32() const { return data32; }

  // The property contents as bytes. Used for string properties (format 8).
  const std::string& Data8() const { return data8; }

  std::vector<uint32_t> data32;
  std::string data8;
};

// Wraps XGetWindowProperty with offset=0 and delete=false, which is what
// every call site in this codebase wants. length is in 32-bit multiples, as
// the underlying call defines it.
extern WindowProperty XGetWindowProperty(Window w,
                                         Atom property,
                                         long length,
                                         Atom req_type);

extern Reply<XWMHints> XGetWMHints(Window w);

// Returns the atoms listed in the window's WM_PROTOCOLS property, or an empty
// vector if it has none.
extern std::vector<Atom> XGetWMProtocols(Window w);

// Returns None if the window has no WM_TRANSIENT_FOR hint.
extern Window XGetTransientForHint(Window w);

extern bool XGetWMNormalHints(Window w,
                              XSizeHints* hints,
                              long* supplied_return);

extern GC XCreateGC(Window w, unsigned long value_mask, XGCValues* values);
extern int XSetLineAttributes(GC gc,
                              unsigned int line_width,
                              int line_style,
                              int cap_style,
                              int join_style);

extern int XSync(bool discard);

// Returns null on failure, same as the underlying XOpenDisplay.
extern Display* XOpenDisplay();
extern void XCloseDisplay();

extern int XPending();
extern void XNextEvent(XEvent* event);
extern XErrorHandler XSetErrorHandler(XErrorHandler handler);

struct RandRSupport {
  bool have_rr;
  int event_base;
  int error_base;
};
extern RandRSupport XRRQueryExtension();
extern void XRRSelectInput(Window w, int mask);

// The visible area of one enabled monitor. setScreenAreasFromXRandR() wants
// nothing else out of RandR, so the shim reduces the whole CRTC walk to this.
// Disabled CRTCs (mode == 0) are left out.
extern std::vector<Rect> XRRGetVisibleAreas(Window root);

extern Cursor XCreateFontCursor(unsigned int shape);
extern void XRecolorCursor(Cursor c, XColor* fg, XColor* bg);
// Returns true if the colour was successfully allocated.
extern bool XAllocNamedColor(Colormap cmap,
                             const std::string& name,
                             XColor* screen_def,
                             XColor* exact_def);

// Returns null if there's no resource manager string set.
extern char* XResourceManagerString();

extern void XDeleteProperty(Window w, Atom property);

extern void XChangeActivePointerGrab(unsigned int event_mask,
                                     Cursor cursor,
                                     Time time);

#ifdef SHAPE
extern void XShapeSelectInput(Window w, unsigned long mask);
// Returns how many rectangles make up the window's shape. lwm only ever asks
// "is this more than one?" (i.e. is the window non-rectangular), so the shim
// returns the count rather than a list the caller then has to free.
extern int XShapeCountRectangles(Window w, int kind);
extern void XShapeCombineShape(Window dest,
                               int dest_kind,
                               int x_off,
                               int y_off,
                               Window src,
                               int src_kind,
                               int op);
extern int XShapeQueryExtension(int* event_base, int* error_base);
#endif

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
  unsigned int num_children;

  // Query returns the set of children of the given window.
  static WindowTree Query(Display* dpy, Window w);

  // Parent returns the parent window of w, or 0 if the parent is the root
  // window.
  static Window ParentOf(Window w);
};

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
 * This should really have been in X.h --- if you select both ButtonPress
 * and ButtonRelease events, the server makes an automatic grab on the
 * pressed button for you. This is almost always exactly what you want.
 */
#define ButtonMask (ButtonPressMask | ButtonReleaseMask)

#endif  // LWM_XLIB_H_included
