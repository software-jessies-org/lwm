// g++ -oruler -std=c++14 -g -lXft -rdynamic ruler.cc -lXext -lX11 -lSM \
//    -lXrandr -lICE -L/usr/lib -I/usr/include/freetype2 -lXft
//
// Ruler takes over painting of the root window, and draws a pattern of vertical
// and horizontal lines on it, with regular spacing.
// This can be useful for debugging window positioning while working on LWM.

#include <iostream>

#include <X11/Xlib.h>

#define BIG_BOX 100
#define SMALL_BOX 10

static GC gc_bg1;
static GC gc_bg2;
static GC gc_black;
static GC gc_grey;

unsigned long GetColour(Display* dpy, const char* name) {
  XColor colour, exact;
  XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), name, &colour,
                   &exact);
  return colour.pixel;
}

void SetupGCs(Display* dpy, Window w) {
  XGCValues gv = {};
  gv.function = GXcopy;
  gv.line_width = 1;
  const unsigned long gv_mask = GCForeground | GCFunction | GCLineWidth;
  gv.foreground = GetColour(dpy, "#eeeeee");
  gc_bg1 = XCreateGC(dpy, w, gv_mask, &gv);
  gv.foreground = GetColour(dpy, "#cccccc");
  gc_bg2 = XCreateGC(dpy, w, gv_mask, &gv);
  gv.foreground = GetColour(dpy, "#111111");
  gc_black = XCreateGC(dpy, w, gv_mask, &gv);
  gv.foreground = GetColour(dpy, "#888888");
  gc_grey = XCreateGC(dpy, w, gv_mask, &gv);
}

GC GCForLinePos(int pos) {
  return (pos % BIG_BOX) ? gc_grey : gc_black;
}

int NextSmallPos(int pos) {
  int remainder = pos % SMALL_BOX;
  return remainder ? (pos + SMALL_BOX - remainder) : pos;
}

int NextBigBox(int pos) {
  return BIG_BOX * ((pos / BIG_BOX) + 1);
}

void DrawRootWindow(const XExposeEvent& ev) {
  const int max_x = ev.x + ev.width;
  const int max_y = ev.y + ev.height;
  for (int x = ev.x; x < max_x; x = NextBigBox(x)) {
    const int r_width = std::min(NextBigBox(x), max_x) - x;
    for (int y = ev.y; y < max_y; y = NextBigBox(y)) {
      const int r_height = std::min(NextBigBox(y), max_y) - y;
      GC gc = ((x / BIG_BOX + y / BIG_BOX) % 2) ? gc_bg1 : gc_bg2;
      XFillRectangle(ev.display, ev.window, gc, x, y, r_width, r_height);
    }
  }
  for (int x = NextSmallPos(ev.x); x <= max_x; x += SMALL_BOX) {
    XDrawLine(ev.display, ev.window, GCForLinePos(x), x, ev.y, x, max_y);
  }
  for (int y = NextSmallPos(ev.y); y <= max_y; y += SMALL_BOX) {
    XDrawLine(ev.display, ev.window, GCForLinePos(y), ev.x, y, max_x, y);
  }
}

int ErrorHandler(Display* d, XErrorEvent* e) {
  char msg[80];
  XGetErrorText(d, e->error_code, msg, sizeof(msg));

  char number[80];
  snprintf(number, sizeof(number), "%d", e->request_code);

  char req[80];
  XGetErrorDatabaseText(d, "XRequest", number, number, req, sizeof(req));

  std::cerr << "protocol request " << req << " on resource " << std::hex
            << e->resourceid << " failed: " << msg << "\n";
  return 0;
}

int main(int argc, char* const argv[]) {
  // Open a connection to the X server.
  Display* dpy = XOpenDisplay("");
  if (!dpy) {
    std::cerr << "can't open display\n";
    exit(1);
  }

  Window root = DefaultRootWindow(dpy);
  SetupGCs(dpy, root);
  XSelectInput(dpy, root, ExposureMask);

  {
    XExposeEvent ev = {};
    ev.display = dpy;
    ev.window = root;
    ev.width = DisplayWidth(dpy, DefaultScreen(dpy));
    ev.height = DisplayHeight(dpy, DefaultScreen(dpy));
    DrawRootWindow(ev);
  }

  XSetErrorHandler(ErrorHandler);

  // Make sure all our communication to the server got through.
  XSync(dpy, False);

  // The main event loop.
  while (true) {
    XEvent ev;
    XNextEvent(dpy, &ev);
    if (ev.type == Expose) {
      DrawRootWindow(ev.xexpose);
    }
  }
}
