#ifndef LWM_XLIB_H_included
#define LWM_XLIB_H_included

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

struct WindowProperty {
  Atom actual_type;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char* data;  // Caller must XFree() this if non-null.
  int status;           // Return code from XGetWindowProperty (Success, ...).
};
// Wraps XGetWindowProperty with offset=0 and delete=false, which is what
// every call site in this codebase wants.
extern WindowProperty XGetWindowProperty(Window w,
                                         Atom property,
                                         long length,
                                         Atom req_type);

// Caller must XFree() the result if non-null.
extern XWMHints* XGetWMHints(Window w);

struct WMProtocols {
  Atom* protocols;  // Caller must XFree() this if count > 0.
  int count;
};
extern WMProtocols XGetWMProtocols(Window w);

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

// Caller must XFree() the result if non-null.
extern XRRScreenResources* XRRGetScreenResourcesCurrent(Window w);
// Caller must XFree() the result if non-null.
extern XRRCrtcInfo* XRRGetCrtcInfo(XRRScreenResources* res, RRCrtc crtc);

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
// Caller must XFree() the result.
extern XRectangle* XShapeGetRectangles(Window w,
                                       int kind,
                                       int* count,
                                       int* ordering);
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

  // Create an ImageIcon from an array of unsigned longs.
  // The format is as used for _NET_WM_ICON, so the first two values are the
  // width and height, and then there's one value per pixel. There may be more
  // than one icon, which appears after the first.
  // Again, returns null if there was a problem.
  // The data is not freed - that's the caller's job.
  static ImageIcon* CreateFromPixels(unsigned long* data, unsigned long len);

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

// XFreer calls XFree on the data pointer it's constructed with when its
// destructor is called. This is useful to avoid a massive chain of XFree calls
// on every possible return path of a function.
// It is safe to create one of these with a null pointer.
class XFreer {
 public:
  explicit XFreer(void* data) : data_(data) {}
  ~XFreer() {
    if (data_) {
      XFree(data_);
    }
  }

 private:
  void* data_;
};

}  // namespace xlib

/*
 * This should really have been in X.h --- if you select both ButtonPress
 * and ButtonRelease events, the server makes an automatic grab on the
 * pressed button for you. This is almost always exactly what you want.
 */
#define ButtonMask (ButtonPressMask | ButtonReleaseMask)

#endif  // LWM_XLIB_H_included
