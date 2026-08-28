// csdclient - a stand-in for an application that draws its own window
// decorations, for ui_test.sh. Not part of lwm; ui_test.sh compiles it into
// its own temporary directory (see the `csdclient` helper there).
//
// Applications like the Steam launcher, or anything using GTK's client-side
// decorations, draw their own title bar and resize grip and ask the window
// manager for none of its own, through _MOTIF_WM_HINTS. Having no furniture
// for the user to drag, they move and resize themselves by asking the window
// manager to take over the drag the user has started on their own widgets,
// with a _NET_WM_MOVERESIZE client message.
//
// This program does both halves of that, so the tests can drive an
// undecorated window the way a real one behaves:
//
//   csdclient window <x> <y> <w> <h>
//       Maps an undecorated window of the given geometry, prints its window
//       id on stdout, and stays alive until killed.
//
//   csdclient moveresize <window-id> <direction> <button>
//       Sends _NET_WM_MOVERESIZE for that window. direction is the EWMH
//       value (8 = move, 4 = size bottom right, 11 = cancel; see
//       EWMHDirection in disp.h), button the one being held.
//
// Build: g++ -o csdclient -std=c++17 csdclient.cc -lxcb

#include <xcb/xcb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

xcb_connection_t* conn;
xcb_screen_t* screen;

xcb_atom_t atom(const char* name) {
  xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(
      conn, xcb_intern_atom(conn, 0, strlen(name), name), nullptr);
  const xcb_atom_t res = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
  free(r);
  return res;
}

int makeWindow(int argc, char** argv) {
  if (argc != 6) {
    fprintf(stderr, "usage: csdclient window <x> <y> <w> <h>\n");
    return 1;
  }
  const int x = atoi(argv[2]);
  const int y = atoi(argv[3]);
  const int w = atoi(argv[4]);
  const int h = atoi(argv[5]);

  const xcb_window_t win = xcb_generate_id(conn);
  const uint32_t vals[] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE};
  xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, x, y, w, h,
                    0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);

  // The bit that makes lwm leave the window bare: MWM_HINTS_DECORATIONS set
  // in the flags, and no decoration bits at all in word 2.
  const xcb_atom_t motif = atom("_MOTIF_WM_HINTS");
  const uint32_t hints[5] = {1 << 1, 0, 0, 0, 0};
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, motif, motif, 32, 5,
                      hints);

  // A normal window, so this is a client that means to be moved around -
  // not a dock or a splash screen, which lwm leaves bare for quite a
  // different reason.
  const xcb_atom_t wtype = atom("_NET_WM_WINDOW_TYPE");
  const xcb_atom_t normal = atom("_NET_WM_WINDOW_TYPE_NORMAL");
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, wtype, XCB_ATOM_ATOM,
                      32, 1, &normal);

  const std::string name = "csdwin";
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                      XCB_ATOM_STRING, 8, name.size(), name.c_str());

  xcb_map_window(conn, win);
  xcb_flush(conn);
  printf("0x%x\n", win);
  fflush(stdout);
  for (;;) {
    pause();
  }
}

int sendMoveResize(int argc, char** argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: csdclient moveresize <window-id> <dir> <button>\n");
    return 1;
  }
  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = strtoul(argv[2], nullptr, 0);
  ev.type = atom("_NET_WM_MOVERESIZE");
  // x_root and y_root. lwm asks the server where the pointer is rather than
  // trusting these, so they're set to something obviously wrong on purpose.
  ev.data.data32[0] = 0;
  ev.data.data32[1] = 0;
  ev.data.data32[2] = atoi(argv[3]);
  ev.data.data32[3] = atoi(argv[4]);
  ev.data.data32[4] = 1;  // Source indication: a normal application.
  // To the root, with the substructure masks: that's how the window manager
  // is listening.
  xcb_send_event(conn, 0, screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                     XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 (const char*)&ev);
  xcb_flush(conn);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  conn = xcb_connect(nullptr, nullptr);
  if (!conn || xcb_connection_has_error(conn)) {
    fprintf(stderr, "csdclient: cannot open display\n");
    return 1;
  }
  screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
  if (argc > 1 && !strcmp(argv[1], "window")) {
    return makeWindow(argc, argv);
  }
  if (argc > 1 && !strcmp(argv[1], "moveresize")) {
    return sendMoveResize(argc, argv);
  }
  fprintf(stderr, "usage: csdclient window|moveresize ...\n");
  return 1;
}
