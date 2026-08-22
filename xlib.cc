#include "debug.h"
#include "error.h"
#include "hider.h"
#include "resource.h"
#include "screen.h"
#include "xbridge.h"
#include "xlib.h"

#include <xcb/randr.h>
#include <xcb/xcb_icccm.h>
#ifdef SHAPE
#include <xcb/shape.h>
#endif

#include <map>
#include <set>

// The setup information for our one screen, cached at connect time.
static xcb_screen_t* screen;

MousePos getMousePosition() {
  MousePos res;
  memset(&res, 0, sizeof(res));
  xlib::Reply<xcb_query_pointer_reply_t> r(xcb_query_pointer_reply(
      conn, xcb_query_pointer(conn, xlib::Root()), nullptr));
  if (!r) {
    return res;
  }
  res.x = r->root_x;
  res.y = r->root_y;
  res.modMask = r->mask;
  return res;
}

namespace xlib {

void FreeReplyData(void* data) {
  // XCB's replies and errors are plain malloc'd blocks.
  free(data);
}

void* XftDisplay() {
  return xbridge::Display();
}

// ---------------------------------------------------------------------------
// Value lists.
// ---------------------------------------------------------------------------

void ValueList::Add(uint32_t mask_bit, uint32_t value) {
  if (mask_ & mask_bit) {
    LOGF() << "Value list already has a value for mask bit " << mask_bit;
  }
  mask_ |= mask_bit;
  // Insert in mask-bit order: count how many set bits are below this one, and
  // shuffle everything from that position up. The arrays are at most a
  // handful of entries, so the shuffle costs nothing and the alternative -
  // trusting call sites to add in the right order - is the silent-corruption
  // hazard this class exists to remove.
  int index = 0;
  for (uint32_t bit = 1; bit < mask_bit; bit <<= 1) {
    if (mask_ & bit) {
      index++;
    }
  }
  int count = 0;
  for (uint32_t m = mask_; m; m >>= 1) {
    count += m & 1;
  }
  for (int i = count - 1; i > index; i--) {
    values_[i] = values_[i - 1];
  }
  values_[index] = value;
}

WindowAttrs& WindowAttrs::EventMask(uint32_t mask) {
  values_.Add(XCB_CW_EVENT_MASK, mask);
  return *this;
}

WindowAttrs& WindowAttrs::Cursor(::Cursor c) {
  values_.Add(XCB_CW_CURSOR, c);
  return *this;
}

WindowAttrs& WindowAttrs::WinGravity(uint32_t gravity) {
  values_.Add(XCB_CW_WIN_GRAVITY, gravity);
  return *this;
}

WindowAttrs& WindowAttrs::DontPropagate(uint32_t mask) {
  values_.Add(XCB_CW_DONT_PROPAGATE, mask);
  return *this;
}

WindowAttrs& WindowAttrs::BackPixel(uint32_t pixel) {
  values_.Add(XCB_CW_BACK_PIXEL, pixel);
  return *this;
}

WindowChanges& WindowChanges::X(int x) {
  values_.Add(XCB_CONFIG_WINDOW_X, uint32_t(x));
  return *this;
}

WindowChanges& WindowChanges::Y(int y) {
  values_.Add(XCB_CONFIG_WINDOW_Y, uint32_t(y));
  return *this;
}

WindowChanges& WindowChanges::Width(int w) {
  values_.Add(XCB_CONFIG_WINDOW_WIDTH, uint32_t(w));
  return *this;
}

WindowChanges& WindowChanges::Height(int h) {
  values_.Add(XCB_CONFIG_WINDOW_HEIGHT, uint32_t(h));
  return *this;
}

WindowChanges& WindowChanges::BorderWidth(int bw) {
  values_.Add(XCB_CONFIG_WINDOW_BORDER_WIDTH, uint32_t(bw));
  return *this;
}

WindowChanges& WindowChanges::Sibling(Window w) {
  values_.Add(XCB_CONFIG_WINDOW_SIBLING, w);
  return *this;
}

WindowChanges& WindowChanges::StackMode(uint32_t mode) {
  values_.Add(XCB_CONFIG_WINDOW_STACK_MODE, mode);
  return *this;
}

WindowChanges& WindowChanges::Rectangle(const Rect& r) {
  return X(r.xMin).Y(r.yMin).Width(r.width()).Height(r.height());
}

WindowChanges& WindowChanges::FromRequestMask(uint16_t value_mask,
                                              int x,
                                              int y,
                                              int width,
                                              int height,
                                              int border_width,
                                              Window sibling,
                                              uint32_t stack_mode) {
  if (value_mask & XCB_CONFIG_WINDOW_X) {
    X(x);
  }
  if (value_mask & XCB_CONFIG_WINDOW_Y) {
    Y(y);
  }
  if (value_mask & XCB_CONFIG_WINDOW_WIDTH) {
    Width(width);
  }
  if (value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
    Height(height);
  }
  if (value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
    BorderWidth(border_width);
  }
  if (value_mask & XCB_CONFIG_WINDOW_SIBLING) {
    Sibling(sibling);
  }
  if (value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
    StackMode(stack_mode);
  }
  return *this;
}

GCValues& GCValues::Function(uint32_t fn) {
  values_.Add(XCB_GC_FUNCTION, fn);
  return *this;
}

GCValues& GCValues::Foreground(uint32_t pixel) {
  values_.Add(XCB_GC_FOREGROUND, pixel);
  return *this;
}

GCValues& GCValues::Background(uint32_t pixel) {
  values_.Add(XCB_GC_BACKGROUND, pixel);
  return *this;
}

GCValues& GCValues::LineWidth(uint32_t width) {
  values_.Add(XCB_GC_LINE_WIDTH, width);
  return *this;
}

GCValues& GCValues::LineStyle(uint32_t style) {
  values_.Add(XCB_GC_LINE_STYLE, style);
  return *this;
}

GCValues& GCValues::CapStyle(uint32_t style) {
  values_.Add(XCB_GC_CAP_STYLE, style);
  return *this;
}

GCValues& GCValues::JoinStyle(uint32_t style) {
  values_.Add(XCB_GC_JOIN_STYLE, style);
  return *this;
}

GCValues& GCValues::SubwindowMode(uint32_t mode) {
  values_.Add(XCB_GC_SUBWINDOW_MODE, mode);
  return *this;
}

// ---------------------------------------------------------------------------
// Connection, screen and event queue.
// ---------------------------------------------------------------------------

// Errors from requests Xlib made come back through here, so they get reported
// the same way the XCB ones do rather than through Xlib's default handler,
// which exits the process.
static void handleXlibError(uint8_t error_code,
                            uint32_t resource_id,
                            uint8_t major_code,
                            uint16_t minor_code,
                            uint32_t sequence) {
  xcb_generic_error_t err{};
  err.error_code = error_code;
  err.resource_id = resource_id;
  err.major_code = major_code;
  err.minor_code = minor_code;
  err.sequence = uint16_t(sequence);
  err.full_sequence = sequence;
  HandleXError(&err);
}

bool OpenDisplay() {
  conn = xbridge::Open(handleXlibError);
  if (!conn) {
    return false;
  }
  screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
  return screen != nullptr;
}

void CloseDisplay() {
  xbridge::Close();
}

Window Root() {
  return screen->root;
}

int ScreenCount() {
  return xcb_setup_roots_length(xcb_get_setup(conn));
}

int ScreenWidth() {
  return screen->width_in_pixels;
}

int ScreenHeight() {
  return screen->height_in_pixels;
}

unsigned long Black() {
  return screen->black_pixel;
}

unsigned long White() {
  return screen->white_pixel;
}

Colormap DefaultColourmap() {
  return screen->default_colormap;
}

xcb_visualid_t DefaultVisual() {
  return screen->root_visual;
}

uint8_t DefaultDepth() {
  return screen->root_depth;
}

std::string DisplayName() {
  return xbridge::DisplayName();
}

void Sync() {
  // GetInputFocus is the traditional cheap round trip: it takes no arguments
  // and can't fail, so waiting for its reply means everything issued before
  // it has been processed, errors included.
  Flush();
  free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
}

void Flush() {
  // Sharing one socket between Xlib and XCB does *not* mean sharing one
  // output buffer. Requests made through Xlib - which now means only Xft's -
  // queue up in Xlib's buffer, and xcb_flush() knows nothing about it. Flush
  // one and not the other and the requests you didn't flush sit there until
  // something else happens to force them out. This goes away with libX11 in
  // phase 4.
  xbridge::Flush();
  xcb_flush(conn);
}

int ConnectionFD() {
  return xcb_get_file_descriptor(conn);
}

bool ConnectionIsBroken() {
  return xcb_connection_has_error(conn) != 0;
}

xcb_generic_event_t* NextEvent() {
  return xcb_poll_for_event(conn);
}

uint32_t NextRequestSequence() {
  // XCB has no "what sequence number is next" accessor, so ask for one the
  // only way available: issue a request that does nothing and take its
  // sequence. A NoOperation is four bytes on the wire and provokes no reply.
  return xcb_no_operation(conn).sequence;
}

bool SelectRootEvents(Window root, uint32_t event_mask) {
  // Only one client may hold SubstructureRedirect on the root window, so this
  // request failing with BadAccess *is* the "another window manager is
  // already running" test. Using the _checked form plus request_check turns
  // that into an immediate answer, instead of Xlib's arrangement where the
  // error turns up in a global handler at some unpredictable later point and
  // has to be recognised by opcode.
  const xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
      conn, root, XCB_CW_EVENT_MASK, &event_mask);
  Reply<xcb_generic_error_t> err(xcb_request_check(conn, cookie));
  return !err;
}

// ---------------------------------------------------------------------------
// Windows.
// ---------------------------------------------------------------------------

int XMoveResizeWindow(Window w, const Rect& r) {
  return XMoveResizeWindow(w, r.xMin, r.yMin, r.width(), r.height());
}

int XMoveResizeWindow(Window w, int x, int y, unsigned width, unsigned height) {
  LOGD(w) << "XMoveResizeWindow(" << WinID(w) << ") -> " << x << "," << y << " "
          << width << "x" << height;
  WindowChanges wc;
  wc.X(x).Y(y).Width(width).Height(height);
  return XConfigureWindow(w, wc);
  // Possible errors: BadValue, BadWindow.
}

int XMoveWindow(Window w, const Point& origin) {
  return XMoveWindow(w, origin.x, origin.y);
}

int XMoveWindow(Window w, int x, int y) {
  LOGD(w) << "XMoveWindow(" << WinID(w) << ") -> " << x << "," << y;
  WindowChanges wc;
  wc.X(x).Y(y);
  return XConfigureWindow(w, wc);
  // Possible errors: BadWindow.
}

int XResizeWindow(Window w, const Area& area) {
  return XResizeWindow(w, area.width, area.height);
}

int XResizeWindow(Window w, unsigned width, unsigned height) {
  LOGD(w) << "XResizeWindow(" << WinID(w) << ") -> " << width << "x" << height;
  WindowChanges wc;
  wc.Width(width).Height(height);
  return XConfigureWindow(w, wc);
  // Possible errors: BadValue, BadWindow.
}

int XReparentWindow(Window w, Window new_parent, int x, int y) {
  LOGD(w) << "XReparentWindow(" << WinID(w)
          << ") -> new parent = " << WinID(new_parent) << " @ " << x << ","
          << y;
  xcb_reparent_window(conn, w, new_parent, x, y);
  // Possible errors: BadMatch, BadWindow.
  return 0;
}

int XMapWindow(Window w) {
  LOGD(w) << "XMapWindow(" << WinID(w) << ")";
  xcb_map_window(conn, w);
  // Possible errors: BadWindow.
  return 0;
}

int XMapRaised(Window w) {
  LOGD(w) << "XMapRaised(" << WinID(w) << ")";
  // Xlib's XMapRaised is a raise followed by a map; there's no single request.
  const uint32_t above = XCB_STACK_MODE_ABOVE;
  xcb_configure_window(conn, w, XCB_CONFIG_WINDOW_STACK_MODE, &above);
  xcb_map_window(conn, w);
  // Possible errors: BadWindow.
  return 0;
}

int XUnmapWindow(Window w) {
  LOGD(w) << "XUnmapWindow(" << WinID(w) << ")";
  xcb_unmap_window(conn, w);
  // Possible errors: BadWindow.
  return 0;
}

int XRaiseWindow(Window w) {
  LOGD(w) << "XRaiseWindow(" << WinID(w) << ")";
  const uint32_t above = XCB_STACK_MODE_ABOVE;
  xcb_configure_window(conn, w, XCB_CONFIG_WINDOW_STACK_MODE, &above);
  // Possible errors: BadWindow.
  return 0;
}

int XLowerWindow(Window w) {
  LOGD(w) << "XLowerWindow(" << WinID(w) << ")";
  const uint32_t below = XCB_STACK_MODE_BELOW;
  xcb_configure_window(conn, w, XCB_CONFIG_WINDOW_STACK_MODE, &below);
  // Possible errors: BadWindow.
  return 0;
}

int XAddToSaveSet(Window w) {
  LOGD(w) << "XAddToSaveSet(" << WinID(w) << ")";
  xcb_change_save_set(conn, XCB_SET_MODE_INSERT, w);
  // Possible errors: BadAccess, BadWindow.
  return 0;
}

int XRemoveFromSaveSet(Window w) {
  LOGD(w) << "XRemoveFromSaveSet(" << WinID(w) << ")";
  xcb_change_save_set(conn, XCB_SET_MODE_DELETE, w);
  // Possible errors: BadAccess, BadWindow.
  return 0;
}

int XSetInputFocus(Window focus, int revert_to, Time time) {
  LOGD(focus) << "XSetInputFocus(" << WinID(focus) << ")";
  xcb_set_input_focus(conn, revert_to, focus, time);
  // Possible errors: BadMatch, BadValue, BadWindow.
  return 0;
}

FocusWindow XGetInputFocus() {
  FocusWindow res{};
  Reply<xcb_get_input_focus_reply_t> r(
      xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
  if (r) {
    res.window = r->focus;
    res.revert_to = r->revert_to;
  }
  return res;
}

// Prints a value list as the mask bits it sets, for the debug log. Both the
// window-attribute and window-change lists are covered, since the bit names
// don't overlap in any way that matters for reading a log.
struct MaskedValues {
  const ValueList& v;
  const char* const* names;
};

std::ostream& operator<<(std::ostream& os, const MaskedValues& mv) {
  int index = 0;
  for (int bit = 0; bit < 32; bit++) {
    if (!(mv.v.Mask() & (1u << bit))) {
      continue;
    }
    os << " " << mv.names[bit] << "->" << mv.v.Values()[index];
    index++;
  }
  return os;
}

static const char* const kConfigNames[32] = {
    "x",       "y",       "width", "height", "border_width",
    "sibling", "stack_mode"};

static const char* const kAttrNames[32] = {
    "background_pixmap", "background_pixel",     "border_pixmap",
    "border_pixel",      "bit_gravity",          "win_gravity",
    "backing_store",     "backing_planes",       "backing_pixel",
    "override_redirect", "save_under",           "event_mask",
    "do_not_propagate_mask", "colormap",         "cursor"};

int XConfigureWindow(Window w, const WindowChanges& changes) {
  const ValueList& v = changes.Values();
  LOGD(w) << "XConfigureWindow(" << WinID(w) << ")"
          << MaskedValues{v, kConfigNames};
  if (!v.Empty()) {
    xcb_configure_window(conn, w, v.Mask(), v.Values());
  }
  // Possible errors: BadMatch, BadValue, BadWindow.
  return 0;
}

int XChangeWindowAttributes(Window w, const WindowAttrs& attrs) {
  const ValueList& v = attrs.Values();
  LOGD(w) << "XChangeWindowAttributes(" << WinID(w) << ")"
          << MaskedValues{v, kAttrNames};
  if (!v.Empty()) {
    xcb_change_window_attributes(conn, w, v.Mask(), v.Values());
  }
  // Possible errors: BadMatch, BadValue, BadWindow.
  return 0;
}

void SendEventRaw(Window w,
                  bool propagate,
                  uint32_t event_mask,
                  const char* event32) {
  LOGD(w) << "SendEvent(" << WinID(w) << ")";
  xcb_send_event(conn, propagate, w, event_mask, event32);
  // Possible errors: BadValue, BadWindow.
}

void SendClientMessage(Window w, Atom a, long data0, long data1) {
  LOGD(w) << "SendClientMessage, atom " << a << ": " << data0 << ", " << data1;
  xcb_client_message_event_t ev{};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.window = w;
  ev.type = a;
  ev.format = 32;
  ev.data.data32[0] = uint32_t(data0);
  ev.data.data32[1] = uint32_t(data1);
  const uint32_t mask =
      (w == Root()) ? XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT : 0;
  SendEvent(w, false, mask, ev);
}

WindowAttributes XGetWindowAttributes(Window w) {
  WindowAttributes res{};
  // Two requests, because XCB's GetWindowAttributes carries no geometry.
  // Xlib's XGetWindowAttributes did the same thing; it just didn't say so.
  // Fired together so this costs one round trip rather than two.
  const xcb_get_window_attributes_cookie_t attr_cookie =
      xcb_get_window_attributes(conn, w);
  const xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, w);
  Reply<xcb_get_window_attributes_reply_t> attr(
      xcb_get_window_attributes_reply(conn, attr_cookie, nullptr));
  Reply<xcb_get_geometry_reply_t> geom(
      xcb_get_geometry_reply(conn, geom_cookie, nullptr));
  if (!attr || !geom) {
    return res;
  }
  res.ok = true;
  res.override_redirect = attr->override_redirect;
  res.viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
  res.input_only = attr->_class == XCB_WINDOW_CLASS_INPUT_ONLY;
  res.all_event_masks = attr->all_event_masks;
  res.rect = Rect::FromXYWH(geom->x, geom->y, geom->width, geom->height);
  res.border_width = geom->border_width;
  LOGD(w) << "XGetWindowAttributes: " << res.rect;
  return res;
}

WindowGeometry XGetGeometry(Window w) {
  WindowGeometry res{};
  Reply<xcb_get_geometry_reply_t> geom(
      xcb_get_geometry_reply(conn, xcb_get_geometry(conn, w), nullptr));
  if (!geom) {
    return res;  // ok = false on creation.
  }
  res.ok = true;
  res.parent = geom->root;
  res.rect = Rect::FromXYWH(geom->x, geom->y, geom->width, geom->height);
  res.border_width = geom->border_width;
  res.bpp = geom->depth;
  return res;
}

int XDestroyWindow(Window w) {
  LOGD(w) << "XDestroyWindow(" << WinID(w) << ")";
  xcb_destroy_window(conn, w);
  // Possible errors: BadWindow.
  return 0;
}

int XSetWindowBorderWidth(Window w, unsigned int width) {
  LOGD(w) << "XSetWindowBorderWidth(" << WinID(w) << ") -> " << width;
  const uint32_t bw = width;
  xcb_configure_window(conn, w, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
  // Possible errors: BadValue, BadWindow.
  return 0;
}

int XSetWindowBackground(Window w, unsigned long pixel) {
  LOGD(w) << "XSetWindowBackground(" << WinID(w) << ") -> " << pixel;
  const uint32_t p = uint32_t(pixel);
  xcb_change_window_attributes(conn, w, XCB_CW_BACK_PIXEL, &p);
  // Possible errors: BadWindow.
  return 0;
}

int XClearWindow(Window w) {
  LOGD(w) << "XClearWindow(" << WinID(w) << ")";
  // Width and height of zero mean "to the far edge", so this clears the lot.
  xcb_clear_area(conn, 0, w, 0, 0, 0, 0);
  // Possible errors: BadMatch, BadWindow.
  return 0;
}

int XClearArea(Window w,
               int x,
               int y,
               unsigned int width,
               unsigned int height,
               bool exposures) {
  LOGD(w) << "XClearArea(" << WinID(w) << ") -> " << x << "," << y << " "
          << width << "x" << height;
  xcb_clear_area(conn, exposures, w, x, y, width, height);
  // Possible errors: BadMatch, BadValue, BadWindow.
  return 0;
}

int XFillRectangle(Window w,
                   GC gc,
                   int x,
                   int y,
                   unsigned int width,
                   unsigned int height) {
  const xcb_rectangle_t rect = {int16_t(x), int16_t(y), uint16_t(width),
                                uint16_t(height)};
  xcb_poly_fill_rectangle(conn, w, gc, 1, &rect);
  // Possible errors: BadDrawable, BadGC.
  return 0;
}

int XDrawLine(Window w, GC gc, int x1, int y1, int x2, int y2) {
  const xcb_point_t points[2] = {{int16_t(x1), int16_t(y1)},
                                 {int16_t(x2), int16_t(y2)}};
  xcb_poly_line(conn, XCB_COORD_MODE_ORIGIN, w, gc, 2, points);
  // Possible errors: BadDrawable, BadGC.
  return 0;
}

int XKillClient(Window w) {
  LOGD(w) << "XKillClient(" << WinID(w) << ")";
  xcb_kill_client(conn, w);
  // Possible errors: BadValue.
  return 0;
}

static std::set<Window> lwm_owned_windows;

Window CreateNamedWindow(const std::string& name,
                         const Rect& rect,
                         unsigned int border_width,
                         unsigned long border_colour,
                         unsigned long background_colour) {
  const Window w = xcb_generate_id(conn);
  // XCB has no CreateSimpleWindow, so spell out the two colours and inherit
  // everything else from the parent.
  ValueList values;
  values.Add(XCB_CW_BACK_PIXEL, uint32_t(background_colour));
  values.Add(XCB_CW_BORDER_PIXEL, uint32_t(border_colour));
  xcb_create_window(conn, XCB_COPY_FROM_PARENT, w, Root(), rect.xMin, rect.yMin,
                    rect.width(), rect.height(), border_width,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
                    values.Mask(), values.Values());
  lwm_owned_windows.insert(w);

  // Set WM_NAME. This is the modern equivalent of XSetWMName with a
  // STRING-typed text property; the older XStoreName route used to return
  // BadRequest errors despite working.
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
                      XCB_ATOM_STRING, 8, name.size(), name.c_str());
  return w;
}

bool IsLWMWindow(Window w) {
  return lwm_owned_windows.count(w);
}

WindowTree WindowTree::Query(Window w) {
  WindowTree res = {};
  Reply<xcb_query_tree_reply_t> tree(
      xcb_query_tree_reply(conn, xcb_query_tree(conn, w), nullptr));
  if (!tree) {
    return res;
  }
  res.root = tree->root;
  res.parent = tree->parent;
  if (res.parent) {
    res.self = w;
  }
  const xcb_window_t* children = xcb_query_tree_children(tree.get());
  const int n = xcb_query_tree_children_length(tree.get());
  res.children.assign(children, children + n);
  return res;
}

// Returns the parent window of w, or 0 if we hit the root or on error.
Window WindowTree::ParentOf(Window w) {
  WindowTree wt = WindowTree::Query(w);
  return (wt.parent == wt.root) ? 0 : wt.parent;
}

// ---------------------------------------------------------------------------
// Input.
// ---------------------------------------------------------------------------

int XGrabButton(unsigned int button,
                unsigned int modifiers,
                Window grab_window,
                bool owner_events,
                unsigned int event_mask,
                int pointer_mode,
                int keyboard_mode,
                Window confine_to,
                Cursor cursor) {
  LOGD(grab_window) << "XGrabButton(" << WinID(grab_window) << ")";
  xcb_grab_button(conn, owner_events, grab_window, event_mask, pointer_mode,
                  keyboard_mode, confine_to, cursor, button, modifiers);
  // Possible errors: BadCursor, BadValue, BadWindow.
  return 0;
}

int XUngrabButton(unsigned int button,
                  unsigned int modifiers,
                  Window grab_window) {
  LOGD(grab_window) << "XUngrabButton(" << WinID(grab_window) << ")";
  xcb_ungrab_button(conn, button, grab_window, modifiers);
  // Possible errors: BadValue, BadWindow.
  return 0;
}

void XChangeActivePointerGrab(unsigned int event_mask,
                              Cursor cursor,
                              Time time) {
  xcb_change_active_pointer_grab(conn, cursor, time, event_mask);
}

// ---------------------------------------------------------------------------
// Properties and ICCCM hints.
// ---------------------------------------------------------------------------

const Atom kAnyPropertyType = XCB_GET_PROPERTY_TYPE_ANY;

Atom XInternAtom(const std::string& name) {
  Reply<xcb_intern_atom_reply_t> r(xcb_intern_atom_reply(
      conn, xcb_intern_atom(conn, 0, name.size(), name.c_str()), nullptr));
  return r ? r->atom : 0;
}

int XChangeProperty(Window w,
                    Atom property,
                    Atom type,
                    int format,
                    const void* data,
                    int nelements) {
  LOGD(w) << "XChangeProperty(" << WinID(w) << ") property=" << property;
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, property, type, format,
                      nelements, data);
  // Possible errors: BadAlloc, BadAtom, BadMatch, BadValue, BadWindow.
  return 0;
}

void XDeleteProperty(Window w, Atom property) {
  LOGD(w) << "XDeleteProperty(" << WinID(w) << ")";
  xcb_delete_property(conn, w, property);
}

WindowProperty XGetWindowProperty(Window w,
                                  Atom property,
                                  long length,
                                  Atom req_type) {
  WindowProperty res{};
  Reply<xcb_get_property_reply_t> r(xcb_get_property_reply(
      conn, xcb_get_property(conn, 0, w, property, req_type, 0, length),
      nullptr));
  if (!r) {
    return res;
  }
  res.success = true;
  res.actual_type = r->type;
  res.actual_format = r->format;
  res.bytes_after = r->bytes_after;
  res.nitems = xcb_get_property_value_length(r.get());
  const void* value = xcb_get_property_value(r.get());
  if (res.actual_format == 32) {
    // value_length is in bytes; a format-32 property has one item per four of
    // them. These are the real 32 bits that were on the wire - the thing Xlib
    // used to widen to 64-bit longs behind everyone's back.
    res.nitems /= 4;
    const uint32_t* words = static_cast<const uint32_t*>(value);
    res.data32.assign(words, words + res.nitems);
  } else if (res.actual_format == 16) {
    res.nitems /= 2;
    res.data8.assign(static_cast<const char*>(value), res.nitems * 2);
  } else {
    res.data8.assign(static_cast<const char*>(value), res.nitems);
  }
  return res;
}

WMHints XGetWMHints(Window w) {
  WMHints res{};
  xcb_icccm_wm_hints_t hints;
  if (!xcb_icccm_get_wm_hints_reply(conn, xcb_icccm_get_wm_hints(conn, w),
                                    &hints, nullptr)) {
    return res;
  }
  res.ok = true;
  res.has_input = (hints.flags & XCB_ICCCM_WM_HINT_INPUT) != 0;
  res.input = hints.input != 0;
  res.has_initial_state = (hints.flags & XCB_ICCCM_WM_HINT_STATE) != 0;
  res.initial_state = hints.initial_state;
  if (hints.flags & XCB_ICCCM_WM_HINT_ICON_PIXMAP) {
    res.icon_pixmap = hints.icon_pixmap;
  }
  if (hints.flags & XCB_ICCCM_WM_HINT_ICON_MASK) {
    res.icon_mask = hints.icon_mask;
  }
  return res;
}

NormalHints XGetWMNormalHints(Window w) {
  NormalHints res{};
  xcb_size_hints_t hints;
  if (!xcb_icccm_get_wm_normal_hints_reply(
          conn, xcb_icccm_get_wm_normal_hints(conn, w), &hints, nullptr)) {
    return res;
  }
  res.ok = true;
  res.has_min_size = (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) != 0;
  res.has_max_size = (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) != 0;
  res.has_base_size = (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) != 0;
  res.has_resize_inc = (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) != 0;
  res.min_width = hints.min_width;
  res.min_height = hints.min_height;
  res.max_width = hints.max_width;
  res.max_height = hints.max_height;
  res.base_width = hints.base_width;
  res.base_height = hints.base_height;
  res.width_inc = hints.width_inc;
  res.height_inc = hints.height_inc;
  return res;
}

std::vector<Atom> XGetWMProtocols(Window w) {
  // Interned here rather than taken from lwm.cc's global, so that the shim
  // doesn't depend on start-up ordering elsewhere.
  static Atom wm_protocols_atom = XInternAtom("WM_PROTOCOLS");
  xcb_icccm_get_wm_protocols_reply_t protocols;
  if (!xcb_icccm_get_wm_protocols_reply(
          conn,
          xcb_icccm_get_wm_protocols_unchecked(conn, w, wm_protocols_atom),
          &protocols, nullptr)) {
    return {};
  }
  std::vector<Atom> res(protocols.atoms,
                        protocols.atoms + protocols.atoms_len);
  xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
  return res;
}

Window XGetTransientForHint(Window w) {
  xcb_window_t trans = 0;
  if (!xcb_icccm_get_wm_transient_for_reply(
          conn, xcb_icccm_get_wm_transient_for(conn, w), &trans, nullptr)) {
    return 0;
  }
  return trans;
}

// ---------------------------------------------------------------------------
// Graphics contexts, colours and cursors.
// ---------------------------------------------------------------------------

GC XCreateGC(Window w, const GCValues& values) {
  const GC gc = xcb_generate_id(conn);
  const ValueList& v = values.Values();
  xcb_create_gc(conn, gc, w, v.Mask(), v.Values());
  return gc;
}

void XChangeGC(GC gc, const GCValues& values) {
  const ValueList& v = values.Values();
  if (!v.Empty()) {
    xcb_change_gc(conn, gc, v.Mask(), v.Values());
  }
}

// Parses an X11 hexadecimal colour specification into 16-bit components.
// The forms are #RGB, #RRGGBB, #RRRGGGBBB and #RRRRGGGGBBBB, each digit group
// being left-justified into 16 bits (so #f00 is full red, not 0x000f).
static bool parseHexColour(const std::string& spec,
                           uint16_t* r,
                           uint16_t* g,
                           uint16_t* b) {
  const size_t digits = spec.size() - 1;
  if (digits % 3) {
    return false;
  }
  const size_t per = digits / 3;
  if (per < 1 || per > 4) {
    return false;
  }
  uint16_t* out[3] = {r, g, b};
  for (int i = 0; i < 3; i++) {
    uint32_t v = 0;
    for (size_t j = 0; j < per; j++) {
      const char c = spec[1 + i * per + j];
      int d;
      if (c >= '0' && c <= '9') {
        d = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        d = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        d = c - 'A' + 10;
      } else {
        return false;
      }
      v = (v << 4) | d;
    }
    // Left-justify into 16 bits, replicating the top digits into the low ones
    // so that #fff comes out as 0xffff rather than 0xf000.
    *out[i] = uint16_t(v << (16 - per * 4));
    for (size_t shift = per * 4; shift < 16; shift += per * 4) {
      *out[i] |= uint16_t(v << (16 - per * 4 - shift));
    }
  }
  return true;
}

unsigned long ColourByName(const std::string& name) {
  // Beware: the AllocNamedColor *request* only knows the server's colour
  // database - the names in rgb.txt. It does not understand "#B87058". Xlib's
  // XAllocNamedColor hid that, parsing hex specifications client-side before
  // sending a plain AllocColor. So must we; every colour in lwm's default
  // configuration is a hex specification, and handing them all to
  // AllocNamedColor gets you a window manager painted entirely black.
  if (!name.empty() && name[0] == '#') {
    uint16_t r = 0, g = 0, b = 0;
    if (parseHexColour(name, &r, &g, &b)) {
      Reply<xcb_alloc_color_reply_t> col(xcb_alloc_color_reply(
          conn, xcb_alloc_color(conn, DefaultColourmap(), r, g, b), nullptr));
      if (col) {
        return col->pixel;
      }
    }
    LOGW() << "Couldn't parse colour '" << name << "'; using black";
    return Black();
  }
  Reply<xcb_alloc_named_color_reply_t> r(xcb_alloc_named_color_reply(
      conn,
      xcb_alloc_named_color(conn, DefaultColourmap(), name.size(),
                            name.c_str()),
      nullptr));
  if (!r) {
    LOGW() << "Couldn't allocate colour '" << name << "'; using black";
    return Black();
  }
  return r->pixel;
}

// Converts an 8-bit colour component to the 16-bit one the protocol wants.
static uint16_t extend8To16(unsigned long c) {
  const uint16_t v = c & 0xff;
  return v | (v << 8);
}

Cursor CreateFontCursor(unsigned int shape,
                        unsigned long fg,
                        unsigned long bg) {
  // The standard cursor font holds each cursor as a glyph plus the mask glyph
  // immediately after it, which is why the source and mask characters differ
  // by one. Unlike Xlib's XCreateFontCursor the colours are given here rather
  // than in a follow-up XRecolorCursor, so there is no separate recolour step.
  static xcb_font_t cursor_font = 0;
  if (!cursor_font) {
    cursor_font = xcb_generate_id(conn);
    static const char kCursorFontName[] = "cursor";
    xcb_open_font(conn, cursor_font, sizeof(kCursorFontName) - 1,
                  kCursorFontName);
  }
  const Cursor c = xcb_generate_id(conn);
  xcb_create_glyph_cursor(conn, c, cursor_font, cursor_font, shape, shape + 1,
                          extend8To16(fg >> 16), extend8To16(fg >> 8),
                          extend8To16(fg), extend8To16(bg >> 16),
                          extend8To16(bg >> 8), extend8To16(bg));
  return c;
}

// ---------------------------------------------------------------------------
// Extensions.
// ---------------------------------------------------------------------------

RandRSupport XRRQueryExtension() {
  RandRSupport res{};
  const xcb_query_extension_reply_t* ext =
      xcb_get_extension_data(conn, &xcb_randr_id);
  if (!ext || !ext->present) {
    return res;
  }
  // The version handshake isn't optional: RandR rejects every other request
  // until it has happened.
  Reply<xcb_randr_query_version_reply_t> version(xcb_randr_query_version_reply(
      conn, xcb_randr_query_version(conn, 1, 5), nullptr));
  if (!version) {
    return res;
  }
  res.have_rr = true;
  res.event_base = ext->first_event;
  return res;
}

void XRRSelectInput(Window w) {
  xcb_randr_select_input(conn, w,
                         XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
}

std::vector<Rect> XRRGetVisibleAreas(Window root) {
  std::vector<Rect> visible;
  Reply<xcb_randr_get_screen_resources_current_reply_t> res(
      xcb_randr_get_screen_resources_current_reply(
          conn, xcb_randr_get_screen_resources_current(conn, root), nullptr));
  if (!res) {
    LOGE() << "Failed to get RandR screen resources";
    return visible;
  }
  const int ncrtc =
      xcb_randr_get_screen_resources_current_crtcs_length(res.get());
  if (!ncrtc) {
    LOGE() << "Empty list of CRTs";
    return visible;
  }
  const xcb_randr_crtc_t* crtcs =
      xcb_randr_get_screen_resources_current_crtcs(res.get());
  // Fire all the per-CRTC queries before collecting any of them, so the whole
  // walk costs one round trip rather than one per monitor.
  std::vector<xcb_randr_get_crtc_info_cookie_t> cookies;
  cookies.reserve(ncrtc);
  for (int i = 0; i < ncrtc; i++) {
    cookies.push_back(
        xcb_randr_get_crtc_info(conn, crtcs[i], res->config_timestamp));
  }
  for (int i = 0; i < ncrtc; i++) {
    Reply<xcb_randr_get_crtc_info_reply_t> info(
        xcb_randr_get_crtc_info_reply(conn, cookies[i], nullptr));
    if (!info) {
      continue;
    }
    LOGI() << "CRT " << i << " (" << crtcs[i] << "): " << info->width << "x"
           << info->height << ", offset " << info->x << "," << info->y
           << " (mode=" << info->mode << ")";
    // Ignore any CRT with mode==0; that's a disabled output.
    if (!info->mode) {
      continue;
    }
    visible.push_back(Rect{info->x, info->y, info->x + int(info->width),
                           info->y + int(info->height)});
  }
  return visible;
}

#ifdef SHAPE
int XShapeQueryExtension() {
  const xcb_query_extension_reply_t* ext =
      xcb_get_extension_data(conn, &xcb_shape_id);
  if (!ext || !ext->present) {
    return -1;
  }
  return ext->first_event;
}

void XShapeSelectInput(Window w) {
  xcb_shape_select_input(conn, w, 1);
}

int XShapeCountRectangles(Window w) {
  Reply<xcb_shape_get_rectangles_reply_t> r(xcb_shape_get_rectangles_reply(
      conn, xcb_shape_get_rectangles(conn, w, XCB_SHAPE_SK_BOUNDING), nullptr));
  if (!r) {
    return 0;
  }
  return xcb_shape_get_rectangles_rectangles_length(r.get());
}

void XShapeCombineShape(Window dest, int x_off, int y_off, Window src) {
  xcb_shape_combine(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                    XCB_SHAPE_SK_BOUNDING, dest, x_off, y_off, src);
}
#endif

// ---------------------------------------------------------------------------
// Icons.
// ---------------------------------------------------------------------------

// targetImageIconSize returns the max size we want to use for window icons.
// This is determined by the minimum of the space available in the two places
// we display icons, which are the title bar of the window, and the 'unhide'
// menu.
static int targetImageIconSize() {
  const int mh = menuItemHeight();
  const int th = titleBarHeight();
  return (mh < th) ? mh : th;
}

// static
void ImageIcon::ConfigureIconSizes() {
  // Sets the WM_ICON_SIZE property, which applications read to decide what
  // size icon to hand us.
  // By requesting anything up to 1024 pixels on a side, we allow the app
  // to provide the largest icon size it's likely to have.
  // This means the app doesn't do any down-scaling for us. For Java apps this
  // is a good move, as while Java is perfectly capable of scaling down images
  // smoothly, it handily forgets this ability and uses the butt-ugly jagged
  // down-sampling method.
  const uint32_t min_size = targetImageIconSize();
  const uint32_t max_size = 1024;
  // WM_ICON_SIZE is CARDINAL[6]/32: min w/h, max w/h, then the increments.
  const uint32_t sizes[6] = {min_size, min_size, max_size, max_size, 1, 1};
  XChangeProperty(Root(), XCB_ATOM_WM_ICON_SIZE, XCB_ATOM_WM_ICON_SIZE, 32,
                  sizes, 6);
}

static std::map<unsigned long, ImageIcon*>* image_icon_cache;
// The refcounts are used to keep track of how many clones of the same image
// have been returned. The internally-kept ImageIcon doesn't count, only clones
// cause the refcount to be increased.
// When an image is cloned, an internal variable is set (gc_hash_), which causes
// the removeCacheRef function to be called when that ImageIcon is destroyed.
// Note (particularly for testing) that windows which specify a pixmap directly
// typically don't trigger the caching behaviour, as each window has its own
// copy of the pixmap. To definitely test the reference counting, use Chrome
// or Firefox, both of which have their icons as a bunch of pixels embedded in
// the _NET_WM_ICON property. As we calculate the hash based on the pixel data,
// this triggers reuse of scaled images.
static std::map<unsigned long, int>* image_cache_refcounts;

static ImageIcon* fromCache(unsigned long hash) {
  if (!image_icon_cache) {
    return nullptr;
  }
  auto it = image_icon_cache->find(hash);
  if (it == image_icon_cache->end()) {
    return nullptr;
  }
  return it->second;
}

static void removeCacheRef(unsigned long hash) {
  const int remainingRefs = --((*image_cache_refcounts)[hash]);
  if (remainingRefs == 0) {
    ImageIcon* icon = (*image_icon_cache)[hash];
    image_icon_cache->erase(hash);
    image_cache_refcounts->erase(hash);
    icon->destroyResources();
    delete icon;
  }
}

static void toCache(unsigned long hash, ImageIcon* icon) {
  if (!image_icon_cache) {
    image_icon_cache = new std::map<unsigned long, ImageIcon*>;
    image_cache_refcounts = new std::map<unsigned long, int>;
  }
  (*image_icon_cache)[hash] = icon;
  // Don't add a refcount here; instead we increment refcounts only on clone.
}

static unsigned long hashData(const uint32_t* data, size_t len) {
  // For simplicity, coerce the data into a string, and then hash it.
  std::string s((const char*)data, len * sizeof(uint32_t));
  std::hash<std::string> h;
  return h(s);
}

static unsigned long hashPixmaps(Pixmap img, Pixmap mask) {
  // Assuming the same image and mask are used together (which is probably a
  // safe assumption), we can just use the img as the hash.
  mask = mask;
  return (unsigned long)img;
}

// Image is a plain 32-bit-per-pixel buffer, which is all lwm's icon handling
// ever needed. Under Xlib this was an XImage, chiefly for XGetPixel and
// XPutPixel; those cost a function call per pixel and bought nothing here,
// because every image we deal with is the display's own 24-bit-in-32 ZPixmap
// format anyway.
class Image {
 public:
  Image(int width, int height)
      : width_(width), height_(height), pixels_(size_t(width) * height, 0) {}

  int width() const { return width_; }
  int height() const { return height_; }

  uint32_t Get(int x, int y) const { return pixels_[size_t(y) * width_ + x]; }
  void Put(int x, int y, uint32_t v) { pixels_[size_t(y) * width_ + x] = v; }

  const uint32_t* data() const { return pixels_.data(); }
  size_t byte_size() const { return pixels_.size() * sizeof(uint32_t); }

 private:
  int width_;
  int height_;
  std::vector<uint32_t> pixels_;
};

// Reads a rectangle of pixels back off the server.
static Image getImage(Pixmap src, int width, int height, int depth) {
  Image img(width, height);
  Reply<xcb_get_image_reply_t> r(xcb_get_image_reply(
      conn,
      xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, src, 0, 0, width, height,
                    ~0),
      nullptr));
  if (!r) {
    return img;
  }
  const uint8_t* data = xcb_get_image_data(r.get());
  const int len = xcb_get_image_data_length(r.get());
  if (depth == 1) {
    // A bitmap comes back one bit per pixel. Both the padding of each
    // scanline and which end of a byte the first pixel sits in are properties
    // of the *server*, declared in the connection setup - Xlib read them from
    // the same place. Assuming LSB-first, 32-bit-padded happens to be right
    // on ordinary Linux servers and wrong elsewhere, and the failure mode is
    // a mirrored or sheared icon mask rather than anything noisy, so ask.
    const xcb_setup_t* setup = xcb_get_setup(conn);
    const int pad_bits = setup->bitmap_format_scanline_pad;
    const bool msb_first =
        setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_MSB_FIRST;
    const int stride = ((width + pad_bits - 1) / pad_bits) * (pad_bits / 8);
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        const int byte = y * stride + (x / 8);
        if (byte >= len) {
          continue;
        }
        const int bit = msb_first ? (7 - (x % 8)) : (x % 8);
        img.Put(x, y, (data[byte] >> bit) & 1);
      }
    }
    return img;
  }
  // Anything else is 32 bits per pixel: lwm only ever asks for 24-bit
  // drawables, which the server pads out to a word each.
  const int words = len / 4;
  const uint32_t* src_words = reinterpret_cast<const uint32_t*>(data);
  for (int i = 0; i < words && i < width * height; i++) {
    img.Put(i % width, i / width, src_words[i]);
  }
  return img;
}

// copyWithScaling scales down the contents of src into dest.
// The src image *must* be at least as large as the dest image.
// This function applies some very simple anti-aliasing. It could be improved
// by using sub-pixel accuracy, and weighted averages, but this doesn't seem to
// be necessary, given the results we get from this much simpler approach.
static void copyWithScaling(const Image& src, Image* dest) {
  for (int y = 0; y < dest->height(); y++) {
    const int src_min_y = y * src.height() / dest->height();
    const int src_max_y = (y + 1) * src.height() / dest->height();  // exclusive
    for (int x = 0; x < dest->width(); x++) {
      const int src_min_x = x * src.width() / dest->width();
      const int src_max_x = (x + 1) * src.width() / dest->width();  // exclusive
      // An unsigned long has plenty of space to the left of even the red
      // component, so we don't bother shifting the components down and up
      // again. However, we must treat each component separately for the
      // averaging, otherwise we'll get bleed between the components due to
      // integer rounding when we divide by the number of pixels.
      unsigned long r = 0;
      unsigned long g = 0;
      unsigned long b = 0;
      for (int sy = src_min_y; sy < src_max_y; sy++) {
        for (int sx = src_min_x; sx < src_max_x; sx++) {
          const unsigned long val = src.Get(sx, sy);
          r += val & 0xff0000;
          g += val & 0xff00;
          b += val & 0xff;
        }
      }
      // We're never called in a case where dest is larger than src, so the
      // following calculation cannot divide by zero.
      const int div = (src_max_y - src_min_y) * (src_max_x - src_min_x);
      r = (r / div) & 0xff0000;
      g = (g / div) & 0xff00;
      b = (b / div) & 0xff;
      dest->Put(x, y, uint32_t(r | g | b));
    }
  }
}

static Pixmap pixmapFromImage(const Image& img) {
  const Pixmap pm = xcb_generate_id(conn);
  xcb_create_pixmap(conn, 24, pm, Root(), img.width(), img.height());
  const GC gc = xcb_generate_id(conn);
  xcb_create_gc(conn, gc, pm, 0, nullptr);
  xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, gc, img.width(),
                img.height(), 0, 0, 0, 24, img.byte_size(),
                reinterpret_cast<const uint8_t*>(img.data()));
  xcb_free_gc(conn, gc);
  return pm;
}

// Background is a little helper, used to provide a suitable background colour
// to the imageDataToImage function.
// The case this is used in is when the user has configured a top border width,
// which means the top edge of the icon used for an active window needs the
// active border colour for its background, rather than the title background.
// For this reason, we only really need two colours, and a vertical index from
// which to use the second colour (so we ignore the X coordinate entirely).
// It should be noted that as imageDataToImage works on the original image,
// baking in the expected background colour before scaling the image down,
// calls to .At(x, y) provide coordinates in the original image's coordinate
// system, not in the (usually more restricted) space of the destination image.
class Background {
 public:
  explicit Background(unsigned long colour)
      : top_(colour), boundary_(0), bottom_(colour) {}
  Background(unsigned long top, int boundary, unsigned long bottom)
      : top_(top), boundary_(boundary), bottom_(bottom) {}

  unsigned long At(int x, int y) const {
    x = x;  // Unused, but the API looks a bit weird without X.
    return (y > boundary_ ? bottom_ : top_) | 0xff000000;
  }

 private:
  unsigned long top_;
  int boundary_;
  unsigned long bottom_;
};

static void imageDataToImage(Image* dest,
                             const Image& orig,
                             const Image* mask,
                             const Background& background) {
  for (int y = 0; y < orig.height(); y++) {
    for (int x = 0; x < orig.width(); x++) {
      const unsigned long rgb = orig.Get(x, y) | 0xff000000;
      const bool opaque = mask ? mask->Get(x, y) != 0 : true;
      dest->Put(x, y, uint32_t(opaque ? rgb : background.At(x, y)));
    }
  }
}

static void pixelDataToImage(Image* img,
                             const uint32_t* data,
                             int width,
                             int height,
                             unsigned long background) {
  const unsigned long bgr = background & 0xff0000;
  const unsigned long bgg = background & 0xff00;
  const unsigned long bgb = background & 0xff;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const unsigned long argb = (unsigned long)*data++;
      const unsigned long a = (argb >> 24) & 0xff;  // alpha for foreground.
      const unsigned long bga = 0xff - a;           // alpha for background.
      // Treat the 3 channels separately, to avoid cross-channel bleed (which
      // makes the icons of rhythmbox and xfce4-mixer look like CGA vomit).
      unsigned long r = (((argb & 0xff0000) * a + bgr * bga) / 0xff) & 0xff0000;
      unsigned long g = (((argb & 0xff00) * a + bgg * bga) / 0xff) & 0xff00;
      unsigned long b = (((argb & 0xff) * a + bgb * bga) / 0xff) & 0xff;
      img->Put(x, y, uint32_t(r | g | b));
    }
  }
}

ImageIcon::ImageIcon(Pixmap active_img,
                     Pixmap inactive_img,
                     Pixmap menu_img,
                     unsigned int img_w,
                     unsigned int img_h,
                     unsigned int depth)
    : active_img_(active_img),
      inactive_img_(inactive_img),
      menu_img_(menu_img),
      img_w_(img_w),
      img_h_(img_h),
      depth_(depth) {}

ImageIcon::~ImageIcon() {
  if (gc_hash_) {
    removeCacheRef(gc_hash_);
  }
}

ImageIcon* ImageIcon::clone(unsigned long hash) {
  ImageIcon* res = new ImageIcon(active_img_, inactive_img_, menu_img_, img_w_,
                                 img_h_, depth_);
  res->gc_hash_ = hash;
  return res;
}

void ImageIcon::destroyResources() {
  xcb_free_pixmap(conn, active_img_);
  xcb_free_pixmap(conn, inactive_img_);
  xcb_free_pixmap(conn, menu_img_);
}

// static
ImageIcon* ImageIcon::Create(Pixmap img, Pixmap mask) {
  if (!img) {
    return nullptr;
  }
  const unsigned long pm_hash = hashPixmaps(img, mask);
  ImageIcon* result = fromCache(pm_hash);
  if (result) {
    return result->clone(pm_hash);
  }

  const WindowGeometry geom = XGetGeometry(img);
  // Not going to bother trying to paint stuff that's not colourful enough.
  if (!geom.ok || geom.bpp != 24) {
    return nullptr;
  }
  // The image is too large for our needs. Figure out how big to make the
  // width and height dimensions.
  const int targetSize = targetImageIconSize();
  const int width =
      (geom.rect.width() < targetSize) ? geom.rect.width() : targetSize;
  const int height =
      (geom.rect.height() < targetSize) ? geom.rect.height() : targetSize;

  const Image orig_img =
      getImage(img, geom.rect.width(), geom.rect.height(), 24);
  Image mask_img(1, 1);
  if (mask) {
    mask_img = getImage(mask, geom.rect.width(), geom.rect.height(), 1);
  }

  // src_img will be filled in with the data from orig_img, but with the mask
  // (and background) applied.
  Image src_img(geom.rect.width(), geom.rect.height());
  Image dest_img(width, height);

  // For each possible background, generate the RGB values by applying the
  // image values and background value with the alpha channel.
  // If the user has configured a top border width, the 'active' icon has two
  // background colours, one for the top edge and one for the rest. Note that
  // the vertical separation is at topBorderWidth(), but we scale that to the
  // source image's coordinates, as imageDataToImage runs before we scale the
  // image down to our target size.
  const Image* mask_ptr = mask ? &mask_img : nullptr;
  imageDataToImage(
      &src_img, orig_img, mask_ptr,
      Background(LScr::I->ActiveBorder(),
                 topBorderWidth() * geom.rect.height() / targetSize,
                 Resources::I->GetColour(Resources::TITLE_BG_COLOUR)));
  copyWithScaling(src_img, &dest_img);
  const Pixmap active_pm = pixmapFromImage(dest_img);

  imageDataToImage(&src_img, orig_img, mask_ptr,
                   Background(LScr::I->InactiveBorder()));
  copyWithScaling(src_img, &dest_img);
  const Pixmap inactive_pm = pixmapFromImage(dest_img);

  imageDataToImage(
      &src_img, orig_img, mask_ptr,
      Background(Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR)));
  copyWithScaling(src_img, &dest_img);
  const Pixmap menu_pm = pixmapFromImage(dest_img);

  result = new ImageIcon(active_pm, inactive_pm, menu_pm, width, height, 24);
  toCache(pm_hash, result);
  return result->clone(pm_hash);
}

// Use Google Chrome or Chromium to test CreateFromPixels.
// static
ImageIcon* ImageIcon::CreateFromPixels(const uint32_t* data, size_t len) {
  if (data == nullptr || len < 2) {
    return nullptr;
  }
  const unsigned long pm_hash = hashData(data, len);
  ImageIcon* result = fromCache(pm_hash);
  if (result) {
    return result->clone(pm_hash);
  }

  const int src_width = data[0];
  const int src_height = data[1];
  if (src_width <= 0 || src_height <= 0 ||
      len < size_t(2 + src_width * src_height)) {
    fprintf(stderr, "Invalid width (%d) vs height (%d) vs size (%d)\n",
            src_width, src_height, int(len));
    return nullptr;
  }

  // Calculate the destination size of the icon, either the same as source, or
  // the desired size if that's smaller.
  // This code assumes the icon is square (if not, it will be distorted).
  // But it's almost guaranteed to be so.
  const int targetSize = targetImageIconSize();
  const int width = (src_width < targetSize) ? src_width : targetSize;
  const int height = (src_height < targetSize) ? src_height : targetSize;

  Image src_img(src_width, src_height);
  Image dest_img(width, height);

  // For each possible background, generate the RGB values by applying the
  // image values and background value with the alpha channel.
  pixelDataToImage(&src_img, data + 2, src_width, src_height,
                   Resources::I->GetColour(Resources::TITLE_BG_COLOUR));
  copyWithScaling(src_img, &dest_img);
  const Pixmap active_pm = pixmapFromImage(dest_img);

  pixelDataToImage(&src_img, data + 2, src_width, src_height,
                   LScr::I->InactiveBorder());
  copyWithScaling(src_img, &dest_img);
  const Pixmap inactive_pm = pixmapFromImage(dest_img);

  pixelDataToImage(&src_img, data + 2, src_width, src_height,
                   Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR));
  copyWithScaling(src_img, &dest_img);
  const Pixmap menu_pm = pixmapFromImage(dest_img);

  result = new ImageIcon(active_pm, inactive_pm, menu_pm, width, height, 24);
  toCache(pm_hash, result);
  return result->clone(pm_hash);
}

void ImageIcon::paint(Window w,
                      Pixmap pm,
                      int x,
                      int y,
                      int width,
                      int height) {
  if (!pm) {
    return;
  }
  const int xo = (width - (int)img_w_) / 2;
  const int yo = (height - (int)img_h_) / 2;
  GCValues gv;
  gv.Function(XCB_GX_COPY);
  const GC gc = XCreateGC(w, gv);
  // If the pixmap is smaller than what we want to draw, adjust the coordinates
  // so we draw within the bounding box.
  if (xo > 0) {
    x += xo;
    width = img_w_;
  }
  if (yo > 0) {
    y += yo;
    height = img_h_;
  }
  // If the pixmap is bigger than what we want to draw, adjust the src
  // coordinates to draw something in the middle of the source pixmap.
  const int src_x = (xo < 0) ? -xo : 0;
  const int src_y = (yo < 0) ? -yo : 0;
  xcb_copy_area(conn, pm, w, gc, src_x, src_y, x, y, width, height);
  xcb_free_gc(conn, gc);
}

}  // namespace xlib
