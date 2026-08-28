// speckeysd is a hot key daemon.
//
// It reads a file of "key<tab>command" lines, grabs each of those keys on the
// root window of every screen, and runs the command when the key is pressed.
// SIGHUP makes it exec itself, which is how the file is reloaded.
//
// It's on XCB, and unlike lwm and gummiband it is on nothing else: those two
// keep libX11 in the link because Xft needs a Display and nothing draws text
// without it (see docs/xcb-migration-plan.md, "The one real blocker: Xft").
// speckeysd draws nothing, so the only piece of Xlib it ever wanted was
// XStringToKeysym, and xkbcommon's xkb_keysym_from_name takes the same key
// names.
//
// speckeysd_test.sh is the regression test for all of this. Most of what the
// port could get wrong - the order of xcb_grab_key's arguments, which field
// of a key event is the keycode, the root window of the second screen - is
// invisible to the compiler and visible only in whether a key press runs
// anything.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cerrno>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <initializer_list>
#include <vector>

typedef struct HotKey HotKey;
struct HotKey {
  xcb_keysym_t keysym;
  uint16_t modifiers;
  char* command;
  HotKey* next;
};

/* The list of hot keys. */
HotKey* hotkeys = NULL;

/* The connection to the X server. */
xcb_connection_t* conn;

/* The root window of each screen. Xlib had RootWindow(dpy, n); XCB makes you
 * walk the list, so we walk it once here. */
std::vector<xcb_window_t> roots;

/* The keycode/keysym mapping, kept up to date from MappingNotify. */
xcb_key_symbols_t* key_symbols;

char* argv0;
char* hot_key_file;

static void report_key_grab_error(const char* keyname,
                                  const xcb_generic_error_t* e) {
  const char* reason = "unknown reason";
  if (e->error_code == XCB_ACCESS) {
    reason = "the key/button combination is already in use by another client";
  } else if (e->error_code == XCB_VALUE) {
    reason = "the key code was out of range for GrabKey";
  } else if (e->error_code == XCB_WINDOW) {
    reason = "the root window we passed to GrabKey was incorrect";
  }
  fprintf(stderr, "%s: couldn't grab key \"%s\": %s (X error code %i)\n", argv0,
          keyname, reason, e->error_code);
}

static void reap_zombies() {
  int status;
  while (waitpid(-1, &status, WNOHANG) > 0)
    ;
}

static void sigchld_handler(int /*signal_number*/) {
  reap_zombies();
  /* Reinstall this signal handler ready for the next child. */
  signal(SIGCHLD, sigchld_handler);
}

static void shell(const char* command) {
  const char* sh = getenv("SHELL");
  if (sh == 0) {
    sh = "/bin/sh";
  }

  switch (fork()) {
    case 0: /* Child. */
      close(xcb_get_file_descriptor(conn));
      execl(sh, sh, "-c", command, (char*)NULL);
      fprintf(stderr, "%s: can't exec \"%s -c %s\"\n", argv0, sh, command);
      exit(EXIT_FAILURE);
    case -1: /* Error. */
      fprintf(stderr, "%s: couldn't fork\n", argv0);
      break;
  }
}

static void panic(const char* s) {
  fprintf(stderr, "%s: %s\n", argv0, s);
  exit(EXIT_FAILURE);
}

uint16_t parse_modifiers(char* name, const char* full_spec) {
  char* separator = strchr(name, '-');
  uint16_t modifiers = 0;
  if (separator != NULL) {
    *separator = 0;
    modifiers |= parse_modifiers(separator + 1, full_spec);
  }
  // We don't interpret modifies Lock (caps lock), Mod2 (num lock) or Mod3
  // (scroll lock), because they're stateful buttons, not real modifiers in the
  // normal hotkey sense.
  if (!strcmp(name, "Shift")) {
    modifiers |= XCB_MOD_MASK_SHIFT;
  } else if (!strcmp(name, "Control")) {
    modifiers |= XCB_MOD_MASK_CONTROL;
  } else if (!strcmp(name, "Alt") || !strcmp(name, "Mod1")) {
    modifiers |= XCB_MOD_MASK_1;
  } else if (!strcmp(name, "Super") || !strcmp(name, "Mod4")) {
    modifiers |= XCB_MOD_MASK_4;
  } else {
    fprintf(stderr, "%s: ignoring unknown modifier \"%s\" in \"%s\"\n", argv0,
            name, full_spec);
  }
  return modifiers;
}

// parse_hot_key splits a specification like "Control-Alt-x" into the keysym
// and the modifier mask it describes. Returns false, having said why, if the
// key part names no keysym we can grab.
static bool parse_hot_key(const char* keyname,
                          xcb_keysym_t* keysym,
                          uint16_t* modifiers) {
  char* copy = strdup(keyname);
  char* unmodified = strrchr(copy, '-');
  *modifiers = 0;
  if (unmodified == NULL) {
    unmodified = copy;
  } else {
    *unmodified = 0;
    ++unmodified;
    *modifiers = parse_modifiers(copy, keyname);
  }
  // xkb_keysym_from_name is XStringToKeysym under a different name: same key
  // names, same case sensitivity, and no libX11 behind it.
  *keysym = xkb_keysym_from_name(unmodified, XKB_KEYSYM_NO_FLAGS);
  const bool ok = *keysym != XKB_KEY_NoSymbol;
  if (!ok) {
    fprintf(stderr, "%s: unknown key \"%s\" in \"%s\"\n", argv0, unmodified,
            keyname);
  }
  free(copy);
  return ok;
}

static void add_hot_key_modified(const char* keyname,
                                 xcb_keysym_t keysym,
                                 xcb_keycode_t keycode,
                                 const char* command,
                                 uint16_t modifiers) {
  HotKey* new_key = new HotKey;
  new_key->keysym = keysym;
  new_key->modifiers = modifiers;
  new_key->command = strdup(command);
  new_key->next = hotkeys;
  hotkeys = new_key;

  // Beware the argument order: XGrabKey led with the keycode and the
  // modifiers and had owner_events buried in the middle, while xcb_grab_key
  // leads with owner_events and puts the modifiers before the key. Every one
  // of those is an integer, so getting it wrong compiles perfectly happily
  // and grabs something else.
  //
  // The requests go out for every screen first, and only then do we ask what
  // became of them: xcb_request_check waits for the server to catch up with
  // that one request, so anything sent before it has been answered by the
  // time we look. Xlib's equivalent was XSynchronize(True) around the grabs,
  // which cost a round trip each.
  std::vector<xcb_void_cookie_t> cookies;
  cookies.reserve(roots.size());
  for (xcb_window_t root : roots) {
    cookies.push_back(xcb_grab_key_checked(conn, 0 /* owner_events */, root,
                                           modifiers, keycode,
                                           XCB_GRAB_MODE_ASYNC,
                                           XCB_GRAB_MODE_ASYNC));
  }
  for (xcb_void_cookie_t cookie : cookies) {
    if (xcb_generic_error_t* err = xcb_request_check(conn, cookie)) {
      report_key_grab_error(keyname, err);
      free(err);
    }
  }
}

static void add_hot_key(const char* keyname, const char* command) {
  xcb_keysym_t keysym = XCB_NO_SYMBOL;
  uint16_t modifiers = 0;
  if (!parse_hot_key(keyname, &keysym, &modifiers)) {
    return;
  }
  // A keysym which isn't on the keyboard has no keycode, and a keycode of 0
  // is AnyKey to GrabKey: grabbing it would quietly claim every key with
  // these modifiers. A NoSymbol hot key would be no better, matching any key
  // event the server can't name. Neither is what a typo in the file meant.
  //
  // The reply is a malloc'd list of every keycode carrying the keysym,
  // terminated by XCB_NO_SYMBOL; the first is the one Xlib's
  // XKeysymToKeycode would have returned.
  xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(key_symbols, keysym);
  const xcb_keycode_t keycode = keycodes ? keycodes[0] : 0;
  free(keycodes);
  if (keycode == 0) {
    fprintf(stderr, "%s: key \"%s\" isn't on this keyboard\n", argv0, keyname);
    return;
  }

  // https://stackoverflow.com/questions/4037230/global-hotkey-with-x11-xlib/4037579
  // X is very particular about the mod key mask. Having caps lock, num lock,
  // scroll lock etc enabled or disabled results in a different mod mask, and
  // thus will fail to match if we don't grab that key too.
  // In X11 terms, we want to ignore these:
  // LockMask = caps lock
  // Mod2Mask = num lock
  // Mod3Mask = scroll lock
  // The masks are cast because XCB gives them an enum type, and a list of
  // {0, some_enum} has no type both halves agree on.
  for (uint16_t capsMask : {uint16_t(0), uint16_t(XCB_MOD_MASK_LOCK)}) {
    for (uint16_t numLockMask : {uint16_t(0), uint16_t(XCB_MOD_MASK_2)}) {
      for (uint16_t scrLockMask : {uint16_t(0), uint16_t(XCB_MOD_MASK_3)}) {
        const uint16_t combinedMask = capsMask | numLockMask | scrLockMask;
        add_hot_key_modified(keyname, keysym, keycode, command,
                             uint16_t(modifiers | combinedMask));
      }
    }
  }
}

static void read_hot_key_file(const char* fname) {
  FILE* fp = fopen(fname, "r");
  if (fp == NULL) {
    fprintf(stderr, "%s: couldn't read \"%s\"\n", argv0, fname);
    return;
  }

  char buf[BUFSIZ];
  while (fgets(buf, BUFSIZ, fp) != NULL) {
    char* tab = strchr(buf, '\t');
    if (*buf == '#' || tab == NULL) {
      continue;
    }
    *tab = 0;
    add_hot_key(buf, tab + 1);
  }

  fclose(fp);
}

static void keypress(xcb_key_press_event_t* ev) {
  // The keycode is called 'detail' in every XCB key and button event; there's
  // no xkey sub-struct to name it. Column 1 is the shifted keysym, so that a
  // hot key written "Shift-A" matches, which is what XkbKeycodeToKeysym's
  // level argument was doing here.
  xcb_keysym_t keysym = xcb_key_symbols_get_keysym(
      key_symbols, ev->detail, ev->state & XCB_MOD_MASK_SHIFT ? 1 : 0);

  HotKey* h = hotkeys;
  for (; h != NULL; h = h->next) {
    if (h->keysym == keysym && ev->state == h->modifiers) {
      shell(h->command);
      break;
    }
  }
}

bool forceRestart;

static xcb_generic_event_t* getEvent() {
  // Is there a message waiting? poll_for_queued_event only looks at what has
  // already been read off the socket, which is what QLength used to report.
  if (xcb_generic_event_t* ev = xcb_poll_for_queued_event(conn)) {
    return ev;
  }

  // Beg... XCB never pushes requests to the server on its own, so anything we
  // queued would otherwise sit in the output buffer while we block in
  // select(). This is the one flush, in the one place.
  xcb_flush(conn);
  if (xcb_connection_has_error(conn)) {
    panic("lost the connection to the X server.");
  }

  // Wait one second to see if a message arrives.
  int fd = xcb_get_file_descriptor(conn);
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(fd, &readfds);
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  if (select(fd + 1, &readfds, 0, 0, &tv) == 1) {
    // This can still come back empty: what woke us might have been a reply
    // rather than an event, which is a null event as far as we're concerned.
    return xcb_poll_for_event(conn);
  }

  // No message, so we have a null event.
  return NULL;
}

// If we execvp ourselves in the signal handler itself, it seems to prevent
// future signals of the same type from being delivered. So we need this
// convoluted use of a global variable to detect whether we need to restart,
// along with a more complicated 'getEvent' function to ensure we frequently
// wake up and check for forceRestart being set in the main loop.
void RestartSelf(int /*signum*/) {
  forceRestart = true;
}

int main(int argc, char* argv[]) {
  argv0 = argv[0];
  hot_key_file = argv[1];

  // Set SIGHUP to make us exec ourselves. This provides a nice easy way for the
  // user to make us reload our config file.
  struct sigaction sa = {};
  sa.sa_handler = RestartSelf;
  sigaddset(&(sa.sa_mask), SIGHUP);
  if (sigaction(SIGHUP, &sa, NULL)) {
    fprintf(stderr, "SIGHUP sigaction failed: %d\n", errno);
  }

  /* Open a connection to the X server. */
  conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(conn)) {
    panic("can't open display.");
  }

  /* Set up signal handlers. */
  signal(SIGCHLD, sigchld_handler);

  /* One root window per screen, all of which we grab our keys on: a key press
   * goes to the root of whichever screen the pointer is on. */
  for (xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
       it.rem; xcb_screen_next(&it)) {
    roots.push_back(it.data->root);
  }

  key_symbols = xcb_key_symbols_alloc(conn);
  if (key_symbols == NULL) {
    panic("can't read the keyboard mapping.");
  }

  if (argc != 2) {
    panic("syntax: speckeysd <keys file>");
  }

  /* There's no XSync to follow this any more: every grab it makes is waited
   * on as it's made, so by the time we're here the server has dealt with the
   * lot of them and told us about any it refused. */
  read_hot_key_file(hot_key_file);

  /* The main event loop. */
  while (!forceRestart) {
    xcb_generic_event_t* ev = getEvent();
    if (ev == NULL) {
      continue;
    }
    // Bit 0x80 of response_type marks an event that arrived via SendEvent,
    // and has to be masked off before the type means anything. We don't
    // otherwise care where an event came from.
    switch (ev->response_type & 0x7f) {
      case 0:
        // Errors are events here rather than a callback. The grabs are all
        // checked as they're made, so anything reaching this point is a
        // surprise; say so rather than dropping it.
        fprintf(stderr, "%s: X error code %i\n", argv0,
                ((xcb_generic_error_t*)ev)->error_code);
        break;
      case XCB_KEY_PRESS:
        keypress((xcb_key_press_event_t*)ev);
        break;
      case XCB_MAPPING_NOTIFY:
        // The keyboard map has changed under us, and our idea of which
        // keycode carries which keysym is now stale. We grabbed keycodes, but
        // we match on keysyms, so without this the wrong keys run commands.
        xcb_refresh_keyboard_mapping(key_symbols,
                                     (xcb_mapping_notify_event_t*)ev);
        break;
      default:
        /* Do I look like I care? */
        break;
    }
    free(ev);
  }
  execvp(argv[0], argv);
}
