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

#include <xcb/randr.h>

#include "lwm.h"
#include "xfont.h"
#include "xlib.h"

bool is_initialising;
xcb_connection_t* conn;  // The connection to the X server.

// The event number RandR's ScreenChangeNotify arrives as. Extensions get
// their event numbers allocated at run time, so this can't be a constant.
static int rr_event_base;
static bool have_rr;

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

void rrScreenChangeNotify(xcb_generic_event_t* ev);
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
  if (!xlib::OpenDisplay()) {
    panic("can't open display.");
  }
  if (xlib::ScreenCount() != 1) {
    fprintf(stderr,
            "Sorry, LWM no longer supports multiple screens, and you "
            "have %d set up.\nPlease consider using xrandr.\n",
            xlib::ScreenCount());
  }
  Resources::Init();

  // There is no error handler to install: XCB delivers errors on the event
  // queue, in sequence with everything else, and DispatchXEvent routes them
  // to HandleXError.

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

  // Internalize useful atoms, in one batch rather than eight round trips.
  const std::vector<Atom> atoms = xlib::XInternAtoms({
      "WM_STATE",
      "WM_CHANGE_STATE",
      "WM_PROTOCOLS",
      "WM_DELETE_WINDOW",
      "WM_TAKE_FOCUS",
      "COMPOUND_TEXT",
      "_MOZILLA_URL",
      "_MOTIF_WM_HINTS",
  });
  wm_state = atoms[0];
  wm_change_state = atoms[1];
  wm_protocols = atoms[2];
  wm_delete = atoms[3];
  wm_take_focus = atoms[4];
  compound_text = atoms[5];
  _mozilla_url = atoms[6];
  motif_wm_hints = atoms[7];

  ewmh_init();

  xfont::Init();

  LScr::I = new LScr();
  LScr::I->Init();
  // Needs the root window, so it has to follow LScr::Init. Re-run whenever a
  // MappingNotify says the keyboard has been remapped; see disp.cc.
  GrabNavigationKeys();
  session_init(argc, argv);

  // Do we need to support XRandR?
  xlib::RandRSupport rr = xlib::XRRQueryExtension();
  rr_event_base = rr.event_base;
  have_rr = rr.have_rr;
  if (have_rr) {
    xlib::XRRSelectInput(LScr::I->Root());
    setScreenAreasFromXRandR();
  }

  // See if the server has the Shape Window extension.
  shape = serverSupportsShapes();

  // Errors from start-up requests are fatal, but under XCB they arrive on the
  // event queue rather than in a callback, so we have to go and collect them
  // before deciding that start-up went well. Sync() waits for the server to
  // have processed everything issued so far, which guarantees any errors are
  // already queued by the time we drain.
  xlib::Sync();
  ProcessPendingEvents();

  // Initialisation is finished; from now on, errors are not going to be fatal.
  is_initialising = false;

  // The main event loop.
  int dpy_fd = xlib::ConnectionFD();
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

    // Drain whatever's queued before blocking. xcb_poll_for_event will read
    // from the socket if it has to, so this covers both events already
    // buffered and events that arrived while we were busy.
    ProcessPendingEvents();
    if (xlib::ConnectionIsBroken()) {
      // Xlib had an IO error handler for this, which lwm never installed; the
      // process would just die inside a library call. Say what happened.
      panic("lost the connection to the X server.");
    }

    // The one flush, in the one place. XCB never pushes requests to the server
    // on its own, so anything the handlers above queued would otherwise sit in
    // the output buffer while we block in select() - which looks like lwm
    // randomly stopping until you jiggle the mouse. That failure mode used to
    // have exactly one instance, the delayed focus below, whose old comment
    // explained it well; under XCB it isn't a special case any more, it's the
    // rule, so it's handled once here instead.
    xlib::Flush();

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
      if (ice_fd > 0 && FD_ISSET(ice_fd, &readfds)) {
        session_process();
      }
      if (FD_ISSET(delayed_focus_fd, &readfds)) {
        LScr::I->GetFocuser()->TimerFDTriggered();
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

bool randrEvent(xcb_generic_event_t* ev) {
  // RandR's events, like Shape's, are numbered from a base the server hands
  // out at run time, so this can't be part of the main switch.
  if (!have_rr ||
      (ev->response_type & 0x7f) !=
          rr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
    return false;
  }
  rrScreenChangeNotify(ev);
  return true;
}

void rrScreenChangeNotify(xcb_generic_event_t* ev) {
  const xcb_randr_screen_change_notify_event_t* rrev =
      (const xcb_randr_screen_change_notify_event_t*)ev;
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

  // We get lots of these - the server sends one notification per output
  // affected by a single reconfiguration - so drop the duplicates. This used
  // to key on Xlib's per-event serial number, which XCB's event doesn't
  // carry; config_timestamp is the better key anyway, being the server's own
  // "when was this screen configuration set" stamp, so every notification
  // arising from one reconfiguration shares it.
  static xcb_timestamp_t lastConfigTimestamp;
  if (rrev->config_timestamp == lastConfigTimestamp) {
    LOGI() << "Dropping duplicate event for screen config timestamp "
           << lastConfigTimestamp;
    return;
  }
  lastConfigTimestamp = rrev->config_timestamp;
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
  const std::string display_str = xlib::DisplayName();

  switch (fork()) {
    case 0:  // Child.
      close(xlib::ConnectionFD());
      if (!display_str.empty()) {
        const std::string env = "DISPLAY=" + display_str;
        // putenv keeps the pointer it's given, so this has to outlive the
        // call; strdup is the least surprising way to say that. We're about
        // to exec anyway.
        putenv(strdup(env.c_str()));
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
  if (button == XCB_BUTTON_INDEX_1) {
    command = Resources::I->Get(Resources::BUTTON1_COMMAND);
  } else if (button == XCB_BUTTON_INDEX_2) {
    command = Resources::I->Get(Resources::BUTTON2_COMMAND);
  }
  if (command.empty()) {
    return;
  }
  RunCommand(command);
}
