// Usage:
//   setvisname <window id> <new title>
// For example:
//   setvisname 0xe00003 "Hello world"
//
// This program sets the UTF8-encoded "_NET_WM_VISIBLE_NAME" property of the
// identified window to the given string.
// Normally, it is the job of the window manager to set this property, in case
// it decides to display a title other than that provided by the client. This
// is for cases such as window managers that add a " <1>", " <2>" etc suffix
// to windows which have the same name as each other.
//
// LWM does no such thing, but rather we want to use the visible window name
// property to allow the user to override the title provided by the client.
// The principle of this is "the user is in control". It can be used to identify
// a window as having a special meaning, but one primary use case is to allow
// the user to stomp on annoying web apps which make the window title flip-flop
// between two (or more) states in order to demand attention. Google's
// "Hangouts Chat" app is particularly offensive in this regard.
//
// The expected use case for this is:
// 1: Configure LWM to run a command when the user alt-clicks on a window's
//    title bar (via Xresources).
// 2: Provide a shell script as the given command, which will run zenity to
//    request user input.
// 3: On an OK response from the user, run setvisname with the window id with
//    which the LWM-spawned script was run, and the string entered by the user.
// 4: Upon setting the _NET_WM_VISIBLE_NAME property, LWM will pick up on that
//    and, from that point on, display the provided name in preference to
//    anything the window's owning client is trying to do.
//
// To compile this program, run:
// g++ -osetvisname -std=c++14 -g -rdynamic setvisname.cc -lX11 -L/usr/lib

#include <cstring>
#include <iostream>

#include <X11/Xlib.h>

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

  XSetErrorHandler(ErrorHandler);
  if (argc != 3) {
    std::cerr << "Usage: setvisname <window id> <new visible title>";
    exit(1);
  }
  long window = strtol(argv[1], nullptr, 0);
  Atom vis_atom = XInternAtom(dpy, "_NET_WM_VISIBLE_NAME", false);
  Atom utf8_atom = XInternAtom(dpy, "UTF8_STRING", false);
  XChangeProperty(dpy, window, vis_atom, utf8_atom, 8, PropModeReplace,
                  (const unsigned char*)argv[2], strlen(argv[2]));

  // Make sure all our communication to the server got through.
  XSync(dpy, False);
}
