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
