// RealServer: the xlib::Server implementation that speaks XCB.
//
// Everything here is a single X request (or the smallest group that has to
// travel together, such as the attributes-plus-geometry pair that XCB needs
// where Xlib pretended one request would do). Composition, logging and
// anything resembling policy live one level up, in xlib.cc.

#include <string.h>

#include "error.h"
#include "server.h"
#include "xbridge.h"
#include "xlib.h"

#include <xcb/randr.h>
#include <xcb/xcb_icccm.h>
#ifdef SHAPE
#include <xcb/shape.h>
#endif

namespace xlib {
namespace {

// Errors from requests Xlib made come back through here, so they get reported
// the same way the XCB ones do rather than through Xlib's default handler,
// which exits the process.
void handleXlibError(uint8_t error_code,
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

NormalHints normalHintsFrom(const xcb_size_hints_t& hints) {
  NormalHints res{};
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

// Converts an 8-bit colour component to the 16-bit one the protocol wants.
uint16_t extend8To16(unsigned long c) {
  const uint16_t v = c & 0xff;
  return v | (v << 8);
}

class RealServer : public Server {
 public:
  // -------------------------------------------------------------------------
  // Connection, screen and event queue.
  // -------------------------------------------------------------------------

  bool Open() override {
    conn = xbridge::Open(handleXlibError);
    if (!conn) {
      return false;
    }
    screen_ = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    return screen_ != nullptr;
  }

  void Close() override { xbridge::Close(); }

  void* XftDisplay() override { return xbridge::Display(); }

  Window Root() override { return screen_->root; }

  int ScreenCount() override {
    return xcb_setup_roots_length(xcb_get_setup(conn));
  }

  int ScreenWidth() override { return screen_->width_in_pixels; }
  int ScreenHeight() override { return screen_->height_in_pixels; }
  unsigned long Black() override { return screen_->black_pixel; }
  unsigned long White() override { return screen_->white_pixel; }
  Colormap DefaultColourmap() override { return screen_->default_colormap; }
  xcb_visualid_t DefaultVisual() override { return screen_->root_visual; }
  uint8_t DefaultDepth() override { return screen_->root_depth; }

  std::string DisplayName() override { return xbridge::DisplayName(); }

  void Sync() override {
    // GetInputFocus is the traditional cheap round trip: it takes no arguments
    // and can't fail, so waiting for its reply means everything issued before
    // it has been processed, errors included.
    Flush();
    free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
  }

  void Flush() override {
    // Sharing one socket between Xlib and XCB does *not* mean sharing one
    // output buffer. Requests made through Xlib - which now means only Xft's -
    // queue up in Xlib's buffer, and xcb_flush() knows nothing about it. Flush
    // one and not the other and the requests you didn't flush sit there until
    // something else happens to force them out. This goes away with libX11 in
    // phase 4.
    xbridge::Flush();
    xcb_flush(conn);
  }

  int ConnectionFD() override { return xcb_get_file_descriptor(conn); }

  bool ConnectionIsBroken() override {
    return xcb_connection_has_error(conn) != 0;
  }

  xcb_generic_event_t* NextEvent() override { return xcb_poll_for_event(conn); }

  uint32_t NextRequestSequence() override {
    // XCB has no "what sequence number is next" accessor, so ask for one the
    // only way available: issue a request that does nothing and take its
    // sequence. A NoOperation is four bytes on the wire and provokes no reply.
    return xcb_no_operation(conn).sequence;
  }

  bool SelectRootEvents(Window root, uint32_t event_mask) override {
    // Only one client may hold SubstructureRedirect on the root window, so
    // this request failing with BadAccess *is* the "another window manager is
    // already running" test. Using the _checked form plus request_check turns
    // that into an immediate answer, instead of Xlib's arrangement where the
    // error turns up in a global handler at some unpredictable later point and
    // has to be recognised by opcode.
    const xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
        conn, root, XCB_CW_EVENT_MASK, &event_mask);
    Reply<xcb_generic_error_t> err(xcb_request_check(conn, cookie));
    return !err;
  }

  // -------------------------------------------------------------------------
  // Windows.
  // -------------------------------------------------------------------------

  Window CreateWindow(const Rect& rect,
                      unsigned int border_width,
                      unsigned long border_colour,
                      unsigned long background_colour) override {
    const Window w = xcb_generate_id(conn);
    // XCB has no CreateSimpleWindow, so spell out the two colours and inherit
    // everything else from the parent.
    ValueList values;
    values.Add(XCB_CW_BACK_PIXEL, uint32_t(background_colour));
    values.Add(XCB_CW_BORDER_PIXEL, uint32_t(border_colour));
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, w, Root(), rect.xMin,
                      rect.yMin, rect.width(), rect.height(), border_width,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
                      values.Mask(), values.Values());
    return w;
  }

  void DestroyWindow(Window w) override {
    xcb_destroy_window(conn, w);
    // Possible errors: BadWindow.
  }

  void ConfigureWindow(Window w, const WindowChanges& changes) override {
    const ValueList& v = changes.Values();
    if (!v.Empty()) {
      xcb_configure_window(conn, w, v.Mask(), v.Values());
    }
    // Possible errors: BadMatch, BadValue, BadWindow.
  }

  void ChangeWindowAttributes(Window w, const WindowAttrs& attrs) override {
    const ValueList& v = attrs.Values();
    if (!v.Empty()) {
      xcb_change_window_attributes(conn, w, v.Mask(), v.Values());
    }
    // Possible errors: BadMatch, BadValue, BadWindow.
  }

  void ReparentWindow(Window w, Window parent, int x, int y) override {
    xcb_reparent_window(conn, w, parent, x, y);
    // Possible errors: BadMatch, BadWindow.
  }

  void MapWindow(Window w) override {
    xcb_map_window(conn, w);
    // Possible errors: BadWindow.
  }

  void UnmapWindow(Window w) override {
    xcb_unmap_window(conn, w);
    // Possible errors: BadWindow.
  }

  void ChangeSaveSet(Window w, bool insert) override {
    xcb_change_save_set(conn, insert ? XCB_SET_MODE_INSERT : XCB_SET_MODE_DELETE,
                        w);
    // Possible errors: BadAccess, BadWindow.
  }

  void SetInputFocus(Window w, int revert_to, Time t) override {
    xcb_set_input_focus(conn, revert_to, w, t);
    // Possible errors: BadMatch, BadValue, BadWindow.
  }

  FocusWindow GetInputFocus() override {
    FocusWindow res{};
    Reply<xcb_get_input_focus_reply_t> r(
        xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
    if (r) {
      res.window = r->focus;
      res.revert_to = r->revert_to;
    }
    return res;
  }

  WindowAttributes GetWindowAttributes(Window w) override {
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
    return res;
  }

  WindowGeometry GetGeometry(Window w) override {
    WindowGeometry res{};
    Reply<xcb_get_geometry_reply_t> geom(
        xcb_get_geometry_reply(conn, xcb_get_geometry(conn, w), nullptr));
    if (!geom) {
      return res;  // ok = false on creation.
    }
    res.ok = true;
    res.root = geom->root;
    res.rect = Rect::FromXYWH(geom->x, geom->y, geom->width, geom->height);
    res.border_width = geom->border_width;
    res.bpp = geom->depth;
    return res;
  }

  WindowTree QueryTree(Window w) override {
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

  void ClearArea(Window w,
                 int x,
                 int y,
                 unsigned int width,
                 unsigned int height,
                 bool exposures) override {
    xcb_clear_area(conn, exposures, w, x, y, width, height);
    // Possible errors: BadMatch, BadValue, BadWindow.
  }

  void FillRectangle(Window w,
                     GC gc,
                     int x,
                     int y,
                     unsigned int width,
                     unsigned int height) override {
    const xcb_rectangle_t rect = {int16_t(x), int16_t(y), uint16_t(width),
                                  uint16_t(height)};
    xcb_poly_fill_rectangle(conn, w, gc, 1, &rect);
    // Possible errors: BadDrawable, BadGC.
  }

  void DrawLine(Window w, GC gc, int x1, int y1, int x2, int y2) override {
    const xcb_point_t points[2] = {{int16_t(x1), int16_t(y1)},
                                   {int16_t(x2), int16_t(y2)}};
    xcb_poly_line(conn, XCB_COORD_MODE_ORIGIN, w, gc, 2, points);
    // Possible errors: BadDrawable, BadGC.
  }

  void CopyArea(Pixmap src,
                Window dest,
                GC gc,
                int src_x,
                int src_y,
                int x,
                int y,
                unsigned int width,
                unsigned int height) override {
    xcb_copy_area(conn, src, dest, gc, src_x, src_y, x, y, width, height);
  }

  void KillClient(Window w) override {
    xcb_kill_client(conn, w);
    // Possible errors: BadValue.
  }

  void SendEventRaw(Window w,
                    bool propagate,
                    uint32_t event_mask,
                    const char* event32) override {
    xcb_send_event(conn, propagate, w, event_mask, event32);
    // Possible errors: BadValue, BadWindow.
  }

  // -------------------------------------------------------------------------
  // Input.
  // -------------------------------------------------------------------------

  void GrabButton(unsigned int button,
                  unsigned int modifiers,
                  Window grab_window,
                  bool owner_events,
                  unsigned int event_mask,
                  int pointer_mode,
                  int keyboard_mode,
                  Window confine_to,
                  Cursor cursor) override {
    xcb_grab_button(conn, owner_events, grab_window, event_mask, pointer_mode,
                    keyboard_mode, confine_to, cursor, button, modifiers);
    // Possible errors: BadCursor, BadValue, BadWindow.
  }

  void UngrabButton(unsigned int button,
                    unsigned int modifiers,
                    Window grab_window) override {
    xcb_ungrab_button(conn, button, grab_window, modifiers);
    // Possible errors: BadValue, BadWindow.
  }

  void GrabKey(uint8_t keycode,
               unsigned int modifiers,
               Window grab_window,
               bool owner_events,
               int pointer_mode,
               int keyboard_mode) override {
    xcb_grab_key(conn, owner_events, grab_window, modifiers, keycode,
                 pointer_mode, keyboard_mode);
    // Possible errors: BadAccess (another client holds this combination),
    // BadValue, BadWindow.
  }

  void UngrabKey(uint8_t keycode,
                 unsigned int modifiers,
                 Window grab_window) override {
    xcb_ungrab_key(conn, keycode, grab_window, modifiers);
    // Possible errors: BadValue, BadWindow.
  }

  KeyboardMapping GetKeyboardMapping() override {
    const xcb_setup_t* setup = xcb_get_setup(conn);
    KeyboardMapping res;
    res.min_keycode = setup->min_keycode;
    const int count = setup->max_keycode - setup->min_keycode + 1;
    if (count <= 0) {
      return res;
    }
    Reply<xcb_get_keyboard_mapping_reply_t> r(xcb_get_keyboard_mapping_reply(
        conn,
        xcb_get_keyboard_mapping(conn, setup->min_keycode, count),
        nullptr));
    if (!r) {
      return res;
    }
    res.keysyms_per_keycode = r->keysyms_per_keycode;
    const xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(r.get());
    const int len = xcb_get_keyboard_mapping_keysyms_length(r.get());
    res.keysyms.assign(syms, syms + len);
    return res;
  }

  void ChangeActivePointerGrab(unsigned int event_mask,
                               Cursor cursor,
                               Time t) override {
    xcb_change_active_pointer_grab(conn, cursor, t, event_mask);
  }

  int GrabPointer(Window grab_window,
                  bool owner_events,
                  unsigned int event_mask,
                  int pointer_mode,
                  int keyboard_mode,
                  Window confine_to,
                  Cursor cursor,
                  Time time) override {
    Reply<xcb_grab_pointer_reply_t> r(xcb_grab_pointer_reply(
        conn,
        xcb_grab_pointer(conn, owner_events, grab_window, event_mask,
                         pointer_mode, keyboard_mode, confine_to, cursor, time),
        nullptr));
    // No reply means the request died rather than the grab being refused, but
    // either way we did not get the pointer, so report it as a failure. Any
    // non-success value will do; the caller only compares against success.
    return r ? int(r->status) : int(XCB_GRAB_STATUS_NOT_VIEWABLE);
  }

  void UngrabPointer(Time time) override { xcb_ungrab_pointer(conn, time); }

  MousePos QueryPointer() override {
    MousePos res;
    memset(&res, 0, sizeof(res));
    Reply<xcb_query_pointer_reply_t> r(
        xcb_query_pointer_reply(conn, xcb_query_pointer(conn, Root()), nullptr));
    if (!r) {
      return res;
    }
    res.x = r->root_x;
    res.y = r->root_y;
    res.modMask = r->mask;
    return res;
  }

  // -------------------------------------------------------------------------
  // Properties and ICCCM hints.
  // -------------------------------------------------------------------------

  std::vector<Atom> InternAtoms(
      const std::vector<std::string>& names) override {
    std::vector<xcb_intern_atom_cookie_t> cookies;
    cookies.reserve(names.size());
    for (const std::string& name : names) {
      cookies.push_back(xcb_intern_atom(conn, 0, name.size(), name.c_str()));
    }
    // Only now do we start waiting: by this point every request is already on
    // its way, so the whole batch costs one round trip.
    std::vector<Atom> res;
    res.reserve(names.size());
    for (const xcb_intern_atom_cookie_t cookie : cookies) {
      Reply<xcb_intern_atom_reply_t> r(
          xcb_intern_atom_reply(conn, cookie, nullptr));
      res.push_back(r ? r->atom : 0);
    }
    return res;
  }

  void ChangeProperty(Window w,
                      Atom property,
                      Atom type,
                      int format,
                      const void* data,
                      int nelements) override {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, property, type, format,
                        nelements, data);
    // Possible errors: BadAlloc, BadAtom, BadMatch, BadValue, BadWindow.
  }

  void DeleteProperty(Window w, Atom property) override {
    xcb_delete_property(conn, w, property);
  }

  WindowProperty GetWindowProperty(Window w,
                                   Atom property,
                                   long length,
                                   Atom req_type) override {
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
      // value_length is in bytes; a format-32 property has one item per four
      // of them. These are the real 32 bits that were on the wire - the thing
      // Xlib used to widen to 64-bit longs behind everyone's back.
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

  WMHints GetWMHints(Window w) override {
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

  NormalHints GetWMNormalHints(Window w) override {
    xcb_size_hints_t hints;
    if (!xcb_icccm_get_wm_normal_hints_reply(
            conn, xcb_icccm_get_wm_normal_hints(conn, w), &hints, nullptr)) {
      return NormalHints{};
    }
    return normalHintsFrom(hints);
  }

  std::vector<WindowInfo> QueryWindows(
      const std::vector<Window>& ws) override {
    struct Cookies {
      xcb_get_window_attributes_cookie_t attrs;
      xcb_get_geometry_cookie_t geom;
      xcb_get_property_cookie_t hints;
    };
    std::vector<Cookies> cookies;
    cookies.reserve(ws.size());
    for (const Window w : ws) {
      cookies.push_back(Cookies{xcb_get_window_attributes(conn, w),
                                xcb_get_geometry(conn, w),
                                xcb_icccm_get_wm_normal_hints(conn, w)});
    }
    // Everything is in flight; now collect.
    std::vector<WindowInfo> res(ws.size());
    for (size_t i = 0; i < ws.size(); i++) {
      Reply<xcb_get_window_attributes_reply_t> attr(
          xcb_get_window_attributes_reply(conn, cookies[i].attrs, nullptr));
      Reply<xcb_get_geometry_reply_t> geom(
          xcb_get_geometry_reply(conn, cookies[i].geom, nullptr));
      if (attr && geom) {
        WindowAttributes& a = res[i].attributes;
        a.ok = true;
        a.override_redirect = attr->override_redirect;
        a.viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
        a.input_only = attr->_class == XCB_WINDOW_CLASS_INPUT_ONLY;
        a.all_event_masks = attr->all_event_masks;
        a.rect = Rect::FromXYWH(geom->x, geom->y, geom->width, geom->height);
        a.border_width = geom->border_width;
      }
      xcb_size_hints_t hints;
      if (xcb_icccm_get_wm_normal_hints_reply(conn, cookies[i].hints, &hints,
                                              nullptr)) {
        res[i].normal_hints = normalHintsFrom(hints);
      }
    }
    return res;
  }

  std::vector<Atom> GetWMProtocols(Window w) override {
    // Interned here rather than taken from lwm.cc's global, so that the shim
    // doesn't depend on start-up ordering elsewhere.
    if (!wm_protocols_atom_) {
      wm_protocols_atom_ = InternAtoms({"WM_PROTOCOLS"})[0];
    }
    xcb_icccm_get_wm_protocols_reply_t protocols;
    if (!xcb_icccm_get_wm_protocols_reply(
            conn,
            xcb_icccm_get_wm_protocols_unchecked(conn, w, wm_protocols_atom_),
            &protocols, nullptr)) {
      return {};
    }
    std::vector<Atom> res(protocols.atoms,
                          protocols.atoms + protocols.atoms_len);
    xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
    return res;
  }

  Window GetTransientForHint(Window w) override {
    xcb_window_t trans = 0;
    if (!xcb_icccm_get_wm_transient_for_reply(
            conn, xcb_icccm_get_wm_transient_for(conn, w), &trans, nullptr)) {
      return 0;
    }
    return trans;
  }

  // -------------------------------------------------------------------------
  // Graphics contexts, colours and cursors.
  // -------------------------------------------------------------------------

  GC CreateGC(Window w, const GCValues& values) override {
    const GC gc = xcb_generate_id(conn);
    const ValueList& v = values.Values();
    xcb_create_gc(conn, gc, w, v.Mask(), v.Values());
    return gc;
  }

  void ChangeGC(GC gc, const GCValues& values) override {
    const ValueList& v = values.Values();
    if (!v.Empty()) {
      xcb_change_gc(conn, gc, v.Mask(), v.Values());
    }
  }

  void FreeGC(GC gc) override { xcb_free_gc(conn, gc); }

  bool AllocColour(uint16_t r,
                   uint16_t g,
                   uint16_t b,
                   unsigned long* pixel) override {
    Reply<xcb_alloc_color_reply_t> col(xcb_alloc_color_reply(
        conn, xcb_alloc_color(conn, DefaultColourmap(), r, g, b), nullptr));
    if (!col) {
      return false;
    }
    *pixel = col->pixel;
    return true;
  }

  bool AllocNamedColour(const std::string& name,
                        unsigned long* pixel) override {
    Reply<xcb_alloc_named_color_reply_t> r(xcb_alloc_named_color_reply(
        conn,
        xcb_alloc_named_color(conn, DefaultColourmap(), name.size(),
                              name.c_str()),
        nullptr));
    if (!r) {
      return false;
    }
    *pixel = r->pixel;
    return true;
  }

  Cursor CreateFontCursor(unsigned int shape,
                          unsigned long fg,
                          unsigned long bg) override {
    // The standard cursor font holds each cursor as a glyph plus the mask
    // glyph immediately after it, which is why the source and mask characters
    // differ by one. Unlike Xlib's XCreateFontCursor the colours are given
    // here rather than in a follow-up XRecolorCursor, so there is no separate
    // recolour step.
    if (!cursor_font_) {
      cursor_font_ = xcb_generate_id(conn);
      static const char kCursorFontName[] = "cursor";
      xcb_open_font(conn, cursor_font_, sizeof(kCursorFontName) - 1,
                    kCursorFontName);
    }
    const Cursor c = xcb_generate_id(conn);
    xcb_create_glyph_cursor(conn, c, cursor_font_, cursor_font_, shape,
                            shape + 1, extend8To16(fg >> 16),
                            extend8To16(fg >> 8), extend8To16(fg),
                            extend8To16(bg >> 16), extend8To16(bg >> 8),
                            extend8To16(bg));
    return c;
  }

  // -------------------------------------------------------------------------
  // Pixmaps and images.
  // -------------------------------------------------------------------------

  std::vector<uint32_t> GetImagePixels(Pixmap src,
                                       int width,
                                       int height,
                                       int depth) override {
    std::vector<uint32_t> pixels(size_t(width) * height, 0);
    Reply<xcb_get_image_reply_t> r(xcb_get_image_reply(
        conn,
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, src, 0, 0, width, height,
                      ~0),
        nullptr));
    if (!r) {
      return pixels;
    }
    const uint8_t* data = xcb_get_image_data(r.get());
    const int len = xcb_get_image_data_length(r.get());
    if (depth == 1) {
      // A bitmap comes back one bit per pixel. Both the padding of each
      // scanline and which end of a byte the first pixel sits in are
      // properties of the *server*, declared in the connection setup - Xlib
      // read them from the same place. Assuming LSB-first, 32-bit-padded
      // happens to be right on ordinary Linux servers and wrong elsewhere, and
      // the failure mode is a mirrored or sheared icon mask rather than
      // anything noisy, so ask.
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
          pixels[size_t(y) * width + x] = (data[byte] >> bit) & 1;
        }
      }
      return pixels;
    }
    // Anything else is 32 bits per pixel: lwm only ever asks for 24-bit
    // drawables, which the server pads out to a word each.
    const int words = len / 4;
    const uint32_t* src_words = reinterpret_cast<const uint32_t*>(data);
    for (int i = 0; i < words && i < width * height; i++) {
      pixels[i] = src_words[i];
    }
    return pixels;
  }

  Pixmap CreatePixmapFromPixels(int width,
                                int height,
                                const uint32_t* data) override {
    const Pixmap pm = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 24, pm, Root(), width, height);
    const GC gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, pm, 0, nullptr);
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, gc, width, height, 0, 0,
                  0, 24, size_t(width) * height * sizeof(uint32_t),
                  reinterpret_cast<const uint8_t*>(data));
    xcb_free_gc(conn, gc);
    return pm;
  }

  void FreePixmap(Pixmap p) override { xcb_free_pixmap(conn, p); }

  // -------------------------------------------------------------------------
  // Extensions.
  // -------------------------------------------------------------------------

  RandRSupport RandRQueryExtension() override {
    RandRSupport res{};
    const xcb_query_extension_reply_t* ext =
        xcb_get_extension_data(conn, &xcb_randr_id);
    if (!ext || !ext->present) {
      return res;
    }
    // The version handshake isn't optional: RandR rejects every other request
    // until it has happened.
    Reply<xcb_randr_query_version_reply_t> version(
        xcb_randr_query_version_reply(
            conn, xcb_randr_query_version(conn, 1, 5), nullptr));
    if (!version) {
      return res;
    }
    res.have_rr = true;
    res.event_base = ext->first_event;
    return res;
  }

  void RandRSelectInput(Window w) override {
    xcb_randr_select_input(conn, w, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
  }

  std::vector<Rect> RandRGetVisibleAreas(Window root) override {
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
    // Fire all the per-CRTC queries before collecting any of them, so the
    // whole walk costs one round trip rather than one per monitor.
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
  int ShapeQueryExtension() override {
    const xcb_query_extension_reply_t* ext =
        xcb_get_extension_data(conn, &xcb_shape_id);
    if (!ext || !ext->present) {
      return -1;
    }
    return ext->first_event;
  }

  void ShapeSelectInput(Window w) override {
    xcb_shape_select_input(conn, w, 1);
  }

  int ShapeCountRectangles(Window w) override {
    Reply<xcb_shape_get_rectangles_reply_t> r(xcb_shape_get_rectangles_reply(
        conn, xcb_shape_get_rectangles(conn, w, XCB_SHAPE_SK_BOUNDING),
        nullptr));
    if (!r) {
      return 0;
    }
    return xcb_shape_get_rectangles_rectangles_length(r.get());
  }

  void ShapeCombineShape(Window dest,
                         int x_off,
                         int y_off,
                         Window src) override {
    xcb_shape_combine(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                      XCB_SHAPE_SK_BOUNDING, dest, x_off, y_off, src);
  }
#endif

 private:
  // The setup information for our one screen, cached at connect time.
  xcb_screen_t* screen_ = nullptr;
  xcb_font_t cursor_font_ = 0;
  Atom wm_protocols_atom_ = 0;
};

}  // namespace

Server* NewRealServer() {
  return new RealServer;
}

}  // namespace xlib
