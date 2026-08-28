// g++ -oxdbg -std=c++14 -g -lXft -rdynamic xdbg.cc -lXext -lX11  -lSM \
//    -lXrandr -lICE -L/usr/lib -I/usr/include/freetype2 -lXft
// g++ -std=c++14 -g -lX11 -lXft -I/usr/local/include/freetype2 -oxdbg xdbg.cc
//
// This program opens a window at the bottom of the screen, showing the location
// of the mouse pointer, the window ID the mouse is over, and the coordinates
// within that window.
// If run with a hotkey (eg 'xdbg Super-z'), xdbg will grab that hotkey and,
// when pressed, it will record the current mouse coordinates, and additionally
// display the position of the pointer relative to those coordinates. A second
// press of the hotkey will clear the base point and print the original and
// final positions, along with the distance, to stdout.

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#define NullEvent -1

// User's font.
static std::string g_font_name{"roboto-16"};

// The connection to the X server.
static Display* dpy;
static int display_width;
static int display_height;

// Main window.
static Window window;
// Root window.
static Window root;
// The default GC.
static GC gc;

// Black pixel.
static unsigned long black;
// White pixel.
static unsigned long white;

// Font.
static XftFont* g_font;
static XftDraw* g_font_draw;
static XftColor g_font_color;

static bool forceRestart;

static void getEvent(XEvent*);

static int baseX = -1;
static int baseY = -1;

static int mouseX;
static int mouseY;

static Window child;
static int t1;
static int t2;
static XConfigureEvent configEvent;

#define MARGIN 10

void DrawString(int line, char* txt) {
  XftDrawStringUtf8(g_font_draw, &g_font_color, g_font, MARGIN,
                    line * 1.2 * g_font->ascent,
                    reinterpret_cast<const FcChar8*>(txt), strlen(txt));
}

void DoExpose(XEvent* ev) {
  // Only handle the last in a group of Expose events.
  if (ev && ev->xexpose.count != 0) {
    return;
  }

  // Clear the window.
  XClearWindow(dpy, window);

  // Draw.
  int line = 1;
  char buf[1024];
  sprintf(buf, "Mouse %d x %d, Win 0x%lx, In window %d x %d", mouseX, mouseY,
          child, t1, t2);
  DrawString(line++, buf);
  if (baseX != -1) {
    sprintf(buf, "From base: %d x %d from %d x %d", mouseX - baseX,
            mouseY - baseY, baseX, baseY);
    DrawString(line++, buf);
  }
  const XConfigureEvent& c = configEvent;
  sprintf(buf, "Config: pos %d x %d, size %d x %d; border %d", c.x, c.y,
          c.width, c.height, c.border_width);
  DrawString(line++, buf);
}

void DoNullEvent() {
  Window wign;
  int ign;
  static bool wasShifted = 0;
  unsigned mask = 0;
  XQueryPointer(dpy, root, &wign, &child, &mouseX, &mouseY, &ign, &ign, &mask);
  bool shifted = mask & ShiftMask;
  if (shifted != wasShifted) {
    shifted = wasShifted;
    if (shifted) {
      baseX = mouseX;
      baseY = mouseY;
    } else {
      printf("Moved from %d, %d -> %d, %d (diff %d, %d)\n", baseX, baseY,
             mouseX, mouseY, mouseX - baseX, mouseY - baseY);
      baseX = -1;
      baseY = -1;
    }
  }
  if (child != 0) {
    XQueryPointer(dpy, child, &wign, &wign, &mouseX, &mouseY, &t1, &t2, &mask);
  } else {
    t1 = t2 = 0;
  }
  // Ensure that the clock is redrawn.
  DoExpose(nullptr);
}

void DoConfigureNotify(const XConfigureEvent& xc) {
  configEvent = xc;
  DoExpose(nullptr);
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

void RestartSelf(int) {
  forceRestart = true;
}

int main(int argc, char* const argv[]) {
  struct sigaction sa = {};
  sa.sa_handler = RestartSelf;
  sigaddset(&(sa.sa_mask), SIGHUP);
  if (sigaction(SIGHUP, &sa, NULL)) {
    std::cerr << "SIGHUP sigaction failed: " << errno << "\n";
  }

  // Open a connection to the X server.
  dpy = XOpenDisplay("");
  if (!dpy) {
    std::cerr << "can't open display\n";
    exit(1);
  }

  // Find the screen's dimensions.
  int screen = DefaultScreen(dpy);
  display_width = DisplayWidth(dpy, screen);
  display_height = DisplayHeight(dpy, screen);

  // Set up an error handler.
  XSetErrorHandler(ErrorHandler);

  // Get the pixel values of the only two colours we use.
  black = BlackPixel(dpy, screen);
  white = WhitePixel(dpy, screen);

  // Get font.
  g_font = XftFontOpenName(dpy, screen, g_font_name.c_str());
  if (g_font == nullptr) {
    std::cerr << "couldn't find font " << g_font_name << "; trying default\n";
    g_font = XftFontOpenName(dpy, 0, "fixed");
    if (g_font == nullptr) {
      std::cerr << "can't find a font\n";
      exit(1);
    }
  }

  // Create the window.
  int window_height = 5 * 1.2 * (g_font->ascent + g_font->descent);
  int window_width = 700;
  root = DefaultRootWindow(dpy);
  XSetWindowAttributes attr;
  attr.override_redirect = False;
  attr.background_pixel = white;
  attr.border_pixel = black;
  attr.event_mask = ExposureMask | VisibilityChangeMask | ButtonMotionMask |
                    PointerMotionHintMask | PointerMotionMask |
                    ButtonPressMask | ButtonReleaseMask | StructureNotifyMask |
                    EnterWindowMask | LeaveWindowMask;
  window = XCreateWindow(
      dpy, root, 0, display_height - window_height, window_width, window_height,
      0, CopyFromParent, InputOutput, CopyFromParent,
      CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &attr);

  // Create the objects needed to render text in the window.
  g_font_draw = XftDrawCreate(dpy, window, DefaultVisual(dpy, screen),
                              DefaultColormap(dpy, screen));
  XRenderColor xrc = {0, 0, 0, 0xffff};
  XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
                     DefaultColormap(dpy, screen), &xrc, &g_font_color);

  // Create GC.
  XGCValues gv;
  gv.foreground = black;
  gv.background = white;
  gc = XCreateGC(dpy, window, GCForeground | GCBackground, &gv);

  // Bring up the window.
  XMapRaised(dpy, window);

  // Make sure all our communication to the server got through.
  XSync(dpy, False);

  std::cout << "Set up; entering loop\n";

  // The main event loop.
  while (!forceRestart) {
    XEvent ev;
    getEvent(&ev);
    switch (ev.type) {
      case NullEvent:
        DoNullEvent();
        break;
      case ConfigureNotify:
        DoConfigureNotify(ev.xconfigure);
        break;
      case Expose:
        DoExpose(&ev);
        break;
    }
  }

  // Someone hit us with a SIGHUP: better exec ourselves to force a config
  // reload and cope with changing screen sizes.
  execvp(argv[0], argv);
}

void getEvent(XEvent* ev) {
  // Is there a message waiting?
  if (QLength(dpy) > 0) {
    XNextEvent(dpy, ev);
    return;
  }

  // Beg...
  XFlush(dpy);

  // Wait one second to see if a message arrives.
  int fd = ConnectionNumber(dpy);
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(fd, &readfds);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000;
  if (select(fd + 1, &readfds, 0, 0, &tv) == 1) {
    XNextEvent(dpy, ev);
    return;
  }

  // No message, so we have a null event.
  ev->type = NullEvent;
}
