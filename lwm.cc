/*
 * lwm, a window manager for X11
 * Copyright (C) 1997-2016 Elliott Hughes, James Carter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>

#include "lwm.h"
#include "xfont.h"
#include "xlib.h"

bool is_initialising;
Display* dpy;  // The connection to the X server.

bool shape;       // Does server have Shape Window extension?
int shape_event;  // ShapeEvent event type.

// Atoms we're interested in. See the ICCCM for more information.
Atom wm_state;
Atom wm_change_state;
Atom wm_protocols;
Atom wm_delete;
Atom wm_take_focus;
Atom compound_text;

// Netscape uses this to give information about the URL it's displaying.
Atom _mozilla_url;

// if we're really short of a clue we might look at motif hints, and
// we're not going to link with motif, so we'll have to do it by hand
Atom motif_wm_hints;

bool forceRestart;
char* argv0;

void rrScreenChangeNotify(XEvent* ev);
void setScreenAreasFromXRandR();

/*ARGSUSED*/
extern int main(int argc, char* argv[]) {
  DebugCLI* debugCLI = nullptr;
  argv0 = argv[0];
  std::vector<std::string> debug_init_commands;
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "-debugcli", 9)) {
      debugCLI = new DebugCLI;
      if (argv[i][9] == '=') {
        // Argument is a sequence of commands, separated by ;.
        debug_init_commands = Split(std::string(argv[i] + 10), ";");
      }
    } else if (!strcmp(argv[i], "-test")) {
      LOGI() << "Run in self-test mode; will run all tests, then exit";
      return RunAllTests() ? 0 : 1;
    }
  }

  is_initialising = true;
  setlocale(LC_ALL, "");

  // Open a connection to the X server.
  dpy = xlib::XOpenDisplay();
  if (dpy == 0) {
    panic("can't open display.");
  }
  if (ScreenCount(dpy) != 1) {
    fprintf(stderr,
            "Sorry, LWM no longer supports multiple screens, and you "
            "have %d set up.\nPlease consider using xrandr.\n",
            ScreenCount(dpy));
  }
  Resources::Init();

  // Set up an error handler.
  xlib::XSetErrorHandler(errorHandler);

  // Set up signal handlers.
  signal(SIGTERM, Terminate);
  signal(SIGINT, Terminate);
  signal(SIGHUP, Terminate);

  // Ignore SIGCHLD.
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
#ifdef SA_NOCLDWAIT
  sa.sa_flags = SA_NOCLDWAIT;
#else
  sa.sa_flags = 0;
#endif
  sigemptyset(&sa.sa_mask);
  sigaction(SIGCHLD, &sa, 0);

  // Internalize useful atoms.
  wm_state = xlib::XInternAtom("WM_STATE");
  wm_change_state = xlib::XInternAtom("WM_CHANGE_STATE");
  wm_protocols = xlib::XInternAtom("WM_PROTOCOLS");
  wm_delete = xlib::XInternAtom("WM_DELETE_WINDOW");
  wm_take_focus = xlib::XInternAtom("WM_TAKE_FOCUS");
  compound_text = xlib::XInternAtom("COMPOUND_TEXT");
  _mozilla_url = xlib::XInternAtom("_MOZILLA_URL");
  motif_wm_hints = xlib::XInternAtom("_MOTIF_WM_HINTS");

  ewmh_init();

  xfont::Init();

  LScr::I = new LScr(dpy);
  LScr::I->Init();
  session_init(argc, argv);

  // Initialisation is finished; from now on, errors are not going to be fatal.
  is_initialising = false;

  // Do we need to support XRandR?
  xlib::RandRSupport rr = xlib::XRRQueryExtension();
  int rr_event_base = rr.event_base;
  bool have_rr = rr.have_rr;
  if (have_rr) {
    xlib::XRRSelectInput(LScr::I->Root(), RRScreenChangeNotifyMask);
    setScreenAreasFromXRandR();
  }

  // See if the server has the Shape Window extension.
  shape = serverSupportsShapes();

  // The main event loop.
  int dpy_fd = ConnectionNumber(dpy);
  int max_fd = dpy_fd + 1;
  int delayed_focus_fd = LScr::I->GetFocuser()->GetTimerFD();
  if (ice_fd >= max_fd) {
    max_fd = ice_fd + 1;
  }
  if (delayed_focus_fd >= max_fd) {
    max_fd = delayed_focus_fd + 1;
  }

  // Just before we start the loop, execute any commands we've been told to
  // run on start-up.
  if (debugCLI) {
    debugCLI->Init(debug_init_commands);
  }

  while (!forceRestart) {
    fd_set readfds;

    FD_ZERO(&readfds);
    FD_SET(dpy_fd, &readfds);
    FD_SET(delayed_focus_fd, &readfds);
    if (ice_fd > 0) {
      FD_SET(ice_fd, &readfds);
    }
    if (debugCLI) {
      FD_SET(STDIN_FILENO, &readfds);
    }
    if (select(max_fd, &readfds, NULL, NULL, NULL) > -1) {
      if (FD_ISSET(dpy_fd, &readfds)) {
        while (xlib::XPending()) {
          XEvent ev;
          xlib::XNextEvent(&ev);
          // xrandr notifications have arbitrary numbers, so check for them
          // before trying the static selection.
          if (ev.type == rr_event_base + RRScreenChangeNotify) {
            rrScreenChangeNotify(&ev);
          } else {
            DispatchXEvent(&ev);
          }
        }
      }
      if (ice_fd > 0 && FD_ISSET(ice_fd, &readfds)) {
        session_process();
      }
      if (FD_ISSET(delayed_focus_fd, &readfds)) {
        LScr::I->GetFocuser()->TimerFDTriggered();
        // My best guess as to why we need this is that, because the event we
        // received didn't come off the Xlib connection (but rather our own
        // timer file descriptor), messages to the X server don't get flushed
        // automatically. The effect of this is that the delayed focus granting
        // sort of happens, but doesn't look like it did until the user does
        // something that triggers any event in LWM (like moving the mouse by
        // a pixel).
        // So call XSync so that we're sure all outstanding messages to, for
        // example, tell the client it has input focus, and redraw its frame,
        // get through.
        xlib::XSync(false);
      }
      if (debugCLI && FD_ISSET(STDIN_FILENO, &readfds)) {
        debugCLI->Read();
      }
    }
  }
  // Someone hit us with a SIGHUP: better exec ourselves to force a config
  // reload and cope with changing screen sizes.
  execvp(argv0, argv);
}

void rrScreenChangeNotify(XEvent* ev) {
  XRRScreenChangeNotifyEvent* rrev = (XRRScreenChangeNotifyEvent*)ev;
  int nScrWidth = rrev->width;
  int nScrHeight = rrev->height;
  // If my laptop is connected to a screen that is switched off, of I try
  // to switch to an external screen when none is connected, LWM gets this
  // event with a new size of 320x200. This forces all the windows to be
  // crushed into tiny little boxes, which is really annoying to repair
  // once I've got the external screen connected and X sorted out again.
  // A simple solution to this is to ignore any notifications smaller than
  // the smallest vaguely sensible size I can think of (and, TBH, this is
  // really too small to be sensible already).
  if (nScrWidth < 600 || nScrHeight < 400) {
    LOGW() << "Ignoring tiny screen dimensions from xrandr: " << nScrWidth
           << "x" << nScrHeight;
    return;
  }

  static long lastSerial;
  if (rrev->serial == lastSerial) {
    LOGI() << "Dropping duplicate event for serial " << lastSerial;
    return;  // Drop duplicate message (we get lots of these).
  }
  lastSerial = rrev->serial;
  setScreenAreasFromXRandR();
}

void setScreenAreasFromXRandR() {
  // Change the screen dimensions according to the total extent of all visible
  // areas, and don't rely on the size provided in the event itself. This is
  // because when switching from internal+external monitors to internal only,
  // the first couple of notifications claim the old area. However, querying
  // the CRT info already gets the correct sizes and locations (including
  // mode=0 for those that are disabled, which the shim drops).
  const std::vector<Rect> visible =
      xlib::XRRGetVisibleAreas(LScr::I->Root());
  if (visible.empty()) {
    return;  // The shim has already logged why.
  }
  LScr::I->SetVisibleAreas(visible);
}

extern void RunCommand(const std::string& command) {
  const char* sh = getenv("SHELL");
  if (!sh) {
    sh = "/bin/sh";
  }
  const char* display_str = DisplayString(dpy);

  switch (fork()) {
    case 0:  // Child.
      close(ConnectionNumber(dpy));
      if (display_str) {
        const int len = strlen(display_str) + 9;
        char* str = (char*)malloc(len);
        snprintf(str, len, "DISPLAY=%s", display_str);
        putenv(str);
      }
      execl(sh, sh, "-c", command.c_str(), NULL);
      fprintf(stderr, "%s: can't exec \"%s -c %s\"\n", argv0, sh,
              command.c_str());
      execlp("xterm", "xterm", NULL);
      exit(EXIT_FAILURE);
    case -1:  // Error.
      fprintf(stderr, "%s: couldn't fork\n", argv0);
      break;
  }
}

extern void shell(int button) {
  std::string command;
  if (button == Button1) {
    command = Resources::I->Get(Resources::BUTTON1_COMMAND);
  } else if (button == Button2) {
    command = Resources::I->Get(Resources::BUTTON2_COMMAND);
  }
  if (command.empty()) {
    return;
  }
  RunCommand(command);
}
