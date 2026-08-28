// speckeysd is a hot key daemon.
//
// It reads a file of "key<tab>command" lines, grabs each of those keys on the
// root window of every screen, and runs the command when the key is pressed.
// SIGHUP makes it exec itself, which is how the file is reloaded.
//
// The grabs have to be on the root: a passive grab fires only when the grab
// window is the focus window, an ancestor of it, or a descendant of it
// containing the pointer, and for a key typed at somebody else's window the
// root is the only window we could own that qualifies. That makes the grab
// exclusive - two clients can't hold the same key on the same window - so a
// key can be unavailable when we start and free later, which is what
// retry_busy_grabs is for.
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
#include <time.h>
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

/* One per hot key in the file: what we want grabbed, and how that went.
 *
 * The keycode is the reason this list exists. The file names a keysym, but
 * GrabKey takes a keycode, and which keycode carries a keysym is up to the
 * keyboard map - which changes, at any moment, for reasons that have nothing
 * to do with us. Remembering the keycode we grabbed is what lets us ungrab it
 * again when the map moves out from under it.
 *
 * The two ways a grab can fail are both temporary, so neither throws the
 * entry away:
 *  - busy: another client holds the key. That client may be a copy of us that
 *    hasn't finished exiting, or a window manager still starting up.
 *  - keycode == 0: the keysym isn't anywhere on the keyboard. A different
 *    keyboard layout, or a keyboard being plugged in, can put it there.
 * Either way the key is unusable now and might not be later, so we keep
 * asking - retry_busy_grabs for the first, regrab_after_mapping_change for
 * the second. */
typedef struct Grab Grab;
struct Grab {
  char* keyname;         /* Ours, for the messages; strdup'd. */
  xcb_keysym_t keysym;   /* What the file asked for. */
  uint16_t modifiers;    /* As in the file: the lock bits are added on. */
  xcb_keycode_t keycode; /* What we last grabbed; 0 if we hold nothing. */
  bool busy;             /* Another client has it; ask again. */
};

std::vector<Grab> grabs;

/* How often we go back for the keys another client holds, and when we last
 * did. The interval is a compromise: a key is unusable until the retry that
 * gets it, but every attempt is a round trip to the server, and nothing
 * forces the client squatting on the key to ever let go. */
static const time_t kRetrySeconds = 5;
time_t last_retry_time;

char* argv0;
char* hot_key_file;

static void report_key_grab_error(const char* keyname,
                                  const xcb_generic_error_t* e,
                                  const char* note) {
  const char* reason = "unknown reason";
  if (e->error_code == XCB_ACCESS) {
    reason = "the key/button combination is already in use by another client";
  } else if (e->error_code == XCB_VALUE) {
    reason = "the key code was out of range for GrabKey";
  } else if (e->error_code == XCB_WINDOW) {
    reason = "the root window we passed to GrabKey was incorrect";
  }
  fprintf(stderr, "%s: couldn't grab key \"%s\": %s (X error code %i)%s\n",
          argv0, keyname, reason, e->error_code, note);
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
//
// So one hot key is eight grabs on each screen, and eight entries in the
// hotkeys list - keypress compares the event's modifier state for equality,
// lock bits and all, so the list needs every combination too.
static std::vector<uint16_t> lock_combinations(uint16_t modifiers) {
  std::vector<uint16_t> masks;
  masks.reserve(8);
  for (uint16_t capsMask : {uint16_t(0), uint16_t(XCB_MOD_MASK_LOCK)}) {
    for (uint16_t numLockMask : {uint16_t(0), uint16_t(XCB_MOD_MASK_2)}) {
      for (uint16_t scrLockMask : {uint16_t(0), uint16_t(XCB_MOD_MASK_3)}) {
        masks.push_back(
            uint16_t(modifiers | capsMask | numLockMask | scrLockMask));
      }
    }
  }
  return masks;
}

static void add_hot_key_binding(xcb_keysym_t keysym,
                                uint16_t modifiers,
                                const char* command) {
  HotKey* new_key = new HotKey;
  new_key->keysym = keysym;
  new_key->modifiers = modifiers;
  new_key->command = strdup(command);
  new_key->next = hotkeys;
  hotkeys = new_key;
}

enum GrabOutcome {
  GRAB_OK,     // Every grab this hot key needs is ours.
  GRAB_BUSY,   // Someone else holds it; worth asking again later.
  GRAB_FAILED  // Something no amount of waiting will fix.
};

// grab_hot_key takes the whole of one hot key: every lock combination, on
// every screen. Report says whether a key another client holds is worth a
// message, which it is the first time and not on every retry afterwards.
//
// Beware the argument order: XGrabKey led with the keycode and the modifiers
// and had owner_events buried in the middle, while xcb_grab_key leads with
// owner_events and puts the modifiers before the key. Every one of those is
// an integer, so getting it wrong compiles perfectly happily and grabs
// something else.
static GrabOutcome grab_hot_key(const char* keyname,
                                xcb_keycode_t keycode,
                                uint16_t modifiers,
                                bool report) {
  // The requests all go out first, and only then do we ask what became of
  // them: xcb_request_check waits for the server to catch up with that one
  // request, so anything sent before it has been answered by the time we
  // look. Xlib's equivalent was XSynchronize(True) around the grabs, which
  // cost a round trip each; this way the whole hot key costs one.
  const std::vector<uint16_t> masks = lock_combinations(modifiers);
  std::vector<xcb_void_cookie_t> cookies;
  cookies.reserve(masks.size() * roots.size());
  for (uint16_t mask : masks) {
    for (xcb_window_t root : roots) {
      cookies.push_back(xcb_grab_key_checked(conn, 0 /* owner_events */, root,
                                             mask, keycode,
                                             XCB_GRAB_MODE_ASYNC,
                                             XCB_GRAB_MODE_ASYNC));
    }
  }

  bool busy = false;
  bool failed = false;
  for (xcb_void_cookie_t cookie : cookies) {
    xcb_generic_error_t* err = xcb_request_check(conn, cookie);
    if (err == NULL) {
      continue;
    }
    // A key another client holds is refused in every one of these requests,
    // so say it once rather than once per lock combination per screen.
    if (err->error_code == XCB_ACCESS) {
      if (!busy && report) {
        report_key_grab_error(keyname, err, "; will keep trying");
      }
      busy = true;
    } else {
      if (!failed) {
        report_key_grab_error(keyname, err, "");
      }
      failed = true;
    }
    free(err);
  }
  return failed ? GRAB_FAILED : (busy ? GRAB_BUSY : GRAB_OK);
}

// The counterpart of grab_hot_key: every lock combination, on every screen.
// Unchecked, because UngrabKey on a grab we don't hold is a no-op rather than
// an error, and there'd be nothing useful to do about a failure anyway.
static void ungrab_hot_key(xcb_keycode_t keycode, uint16_t modifiers) {
  for (uint16_t mask : lock_combinations(modifiers)) {
    for (xcb_window_t root : roots) {
      xcb_ungrab_key(conn, keycode, root, mask);
    }
  }
}

// Which keycode currently carries a keysym, or 0 for none.
//
// The reply is a malloc'd list of every keycode carrying the keysym,
// terminated by XCB_NO_SYMBOL; the first is the one Xlib's XKeysymToKeycode
// would have returned.
static xcb_keycode_t keycode_for(xcb_keysym_t keysym) {
  xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(key_symbols, keysym);
  const xcb_keycode_t keycode = keycodes ? keycodes[0] : 0;
  free(keycodes);
  return keycode;
}

// retry_busy_grabs goes back for the keys another client held last time we
// asked. Called from the main loop, so the throttling lives here.
static void retry_busy_grabs() {
  const time_t now = time(NULL);
  if (now - last_retry_time < kRetrySeconds) {
    return;
  }
  last_retry_time = now;

  for (Grab& g : grabs) {
    if (!g.busy) {
      continue;
    }
    // Every lock combination goes in again, including any that got through
    // last time. GrabKey only refuses a key some *other* client holds, so
    // re-grabbing one of our own is not an error, and that saves remembering
    // which of the requests were the ones that failed.
    if (grab_hot_key(g.keyname, g.keycode, g.modifiers, false /* report */) ==
        GRAB_BUSY) {
      continue;
    }
    g.busy = false;
    fprintf(stderr, "%s: grabbed key \"%s\" at last\n", argv0, g.keyname);
  }
}

// regrab_after_mapping_change moves the grabs when the keyboard map moves.
//
// The grabs are on keycodes; the file names keysyms; the map decides which is
// which, and setxkbmap, xmodmap or a keyboard being plugged in can change it
// at any time. Keeping the old grab through that is wrong twice over: the hot
// key stops working, and whatever keysym inherited the old keycode starts
// firing it instead. Refreshing our copy of the map (which is what the
// MappingNotify handler used to do, and all it used to do) fixes only the
// second of those, and only by making the key do nothing at all.
static void regrab_after_mapping_change() {
  for (Grab& g : grabs) {
    const xcb_keycode_t keycode = keycode_for(g.keysym);
    const bool moved = keycode != g.keycode;
    if (moved && g.keycode != 0) {
      // The keysym has gone somewhere else. Let go of the keycode it used to
      // be on, or we go on firing this command for whatever inherited it.
      ungrab_hot_key(g.keycode, g.modifiers);
    }
    g.keycode = keycode;
    g.busy = false;
    if (keycode == 0) {
      // Only worth saying when it's news. A hot key for a key this keyboard
      // simply doesn't have would otherwise say so after every remap.
      if (moved) {
        fprintf(stderr, "%s: key \"%s\" is no longer on this keyboard\n", argv0,
                g.keyname);
      }
      continue;
    }
    // Grabbed again even when the keycode hasn't changed. That looks like
    // wasted work and isn't: a build of this that only re-grabbed keys whose
    // keycode had moved was seen to lose a grab it had never ungrabbed, on a
    // keycode that ended up exactly where it started, after the map had been
    // rewritten a few times in a row. A single unrelated remap doesn't do it,
    // so the rule is something subtler than that - and the exact rule is not
    // worth betting a hot key on. Re-grabbing a key we already hold costs one
    // request in a batch we're already sending and does nothing else. Note
    // there's no ungrab on this path, so the key is never even briefly
    // unheld.
    g.busy =
        grab_hot_key(g.keyname, keycode, g.modifiers, moved /* report */) ==
        GRAB_BUSY;
  }
}

static void add_hot_key(const char* keyname, const char* command) {
  xcb_keysym_t keysym = XCB_NO_SYMBOL;
  uint16_t modifiers = 0;
  if (!parse_hot_key(keyname, &keysym, &modifiers)) {
    // The one failure that is permanent: a name that isn't a keysym at all
    // won't become one because the keyboard changed. Nothing to remember.
    return;
  }

  for (uint16_t mask : lock_combinations(modifiers)) {
    add_hot_key_binding(keysym, mask, command);
  }

  Grab g;
  g.keyname = strdup(keyname);
  g.keysym = keysym;
  g.modifiers = modifiers;
  g.busy = false;
  // A keysym which isn't on the keyboard has no keycode, and a keycode of 0
  // is AnyKey to GrabKey: grabbing it would quietly claim every key with
  // these modifiers. A NoSymbol hot key would be no better, matching any key
  // event the server can't name. Neither is what a typo in the file meant.
  g.keycode = keycode_for(keysym);
  if (g.keycode == 0) {
    fprintf(stderr, "%s: key \"%s\" isn't on this keyboard yet\n", argv0,
            keyname);
  } else {
    g.busy = grab_hot_key(keyname, g.keycode, modifiers, true /* report */) ==
             GRAB_BUSY;
  }
  grabs.push_back(g);
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
   * lot of them and told us about any it refused. Any it refused because
   * another client had the key are marked busy now; start the clock so that
   * the first retry is one interval away rather than immediate. */
  last_retry_time = time(NULL);
  read_hot_key_file(hot_key_file);

  /* The main event loop. */
  while (!forceRestart) {
    /* getEvent gives up after a second whether or not anything arrived, which
     * is what makes this a poll rather than something needing its own timer. */
    retry_busy_grabs();
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
      case XCB_MAPPING_NOTIFY: {
        // The keyboard map has changed under us, and our idea of which
        // keycode carries which keysym is now stale. We grabbed keycodes, but
        // we match on keysyms, so without this the wrong keys run commands.
        xcb_mapping_notify_event_t* map = (xcb_mapping_notify_event_t*)ev;
        xcb_refresh_keyboard_mapping(key_symbols, map);
        // Refreshing the map only stops the wrong key firing. The grabs are
        // on the old keycodes and have to be moved as well, or the hot key
        // itself is gone. Only a keyboard remap moves keysyms about: the
        // modifier and pointer mappings leave every keycode where it was.
        if (map->request == XCB_MAPPING_KEYBOARD) {
          regrab_after_mapping_change();
        }
        break;
      }
      default:
        /* Do I look like I care? */
        break;
    }
    free(ev);
  }
  execvp(argv[0], argv);
}
