// xlib.cc is the composition and logging layer over the X server.
//
// Every function here does at most three things: log what lwm is about to do,
// turn a convenient call into the primitive request it really is (an
// XMoveWindow is a ConfigureWindow with two fields set), and hand it to the
// installed xlib::Server. The primitives themselves live in realserver.cc,
// behind the interface in server.h, so that a test can install a FakeServer
// and see exactly what would have gone on the wire.
//
// Also here: the things that are neither requests nor policy - the value-list
// builders, the client-side colour-specification parser, and ImageIcon's
// scaling and compositing arithmetic.

#include "debug.h"
#include "error.h"
#include "hider.h"
#include "resource.h"
#include "screen.h"
#include "server.h"
#include "xlib.h"

#include <map>
#include <set>

MousePos getMousePosition() {
  return xlib::server->QueryPointer();
}

namespace xlib {

Server* server;

Server* SetServer(Server* s) {
  Server* old = server;
  server = s;
  return old;
}

void FreeReplyData(void* data) {
  // XCB's replies and errors are plain malloc'd blocks.
  free(data);
}

void* XftDisplay() {
  return server->XftDisplay();
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

bool OpenDisplay() {
  // A test installs its own server before getting here; anything else gets
  // the real one.
  if (!server) {
    SetServer(NewRealServer());
  }
  return server->Open();
}

void CloseDisplay() {
  server->Close();
}

Window Root() {
  return server->Root();
}

int ScreenCount() {
  return server->ScreenCount();
}

int ScreenWidth() {
  return server->ScreenWidth();
}

int ScreenHeight() {
  return server->ScreenHeight();
}

unsigned long Black() {
  return server->Black();
}

unsigned long White() {
  return server->White();
}

Colormap DefaultColourmap() {
  return server->DefaultColourmap();
}

xcb_visualid_t DefaultVisual() {
  return server->DefaultVisual();
}

uint8_t DefaultDepth() {
  return server->DefaultDepth();
}

std::string DisplayName() {
  return server->DisplayName();
}

void Sync() {
  server->Sync();
}

void Flush() {
  server->Flush();
}

int ConnectionFD() {
  return server->ConnectionFD();
}

bool ConnectionIsBroken() {
  return server->ConnectionIsBroken();
}

xcb_generic_event_t* NextEvent() {
  return server->NextEvent();
}

uint32_t NextRequestSequence() {
  return server->NextRequestSequence();
}

bool SelectRootEvents(Window root, uint32_t event_mask) {
  return server->SelectRootEvents(root, event_mask);
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
  server->ReparentWindow(w, new_parent, x, y);
  return 0;
}

int XMapWindow(Window w) {
  LOGD(w) << "XMapWindow(" << WinID(w) << ")";
  server->MapWindow(w);
  return 0;
}

int XMapRaised(Window w) {
  LOGD(w) << "XMapRaised(" << WinID(w) << ")";
  // Xlib's XMapRaised is a raise followed by a map; there's no single request.
  WindowChanges wc;
  wc.StackMode(XCB_STACK_MODE_ABOVE);
  server->ConfigureWindow(w, wc);
  server->MapWindow(w);
  return 0;
}

int XUnmapWindow(Window w) {
  LOGD(w) << "XUnmapWindow(" << WinID(w) << ")";
  server->UnmapWindow(w);
  return 0;
}

int XRaiseWindow(Window w) {
  LOGD(w) << "XRaiseWindow(" << WinID(w) << ")";
  WindowChanges wc;
  wc.StackMode(XCB_STACK_MODE_ABOVE);
  server->ConfigureWindow(w, wc);
  return 0;
}

int XLowerWindow(Window w) {
  LOGD(w) << "XLowerWindow(" << WinID(w) << ")";
  WindowChanges wc;
  wc.StackMode(XCB_STACK_MODE_BELOW);
  server->ConfigureWindow(w, wc);
  return 0;
}

int XAddToSaveSet(Window w) {
  LOGD(w) << "XAddToSaveSet(" << WinID(w) << ")";
  server->ChangeSaveSet(w, true);
  return 0;
}

int XRemoveFromSaveSet(Window w) {
  LOGD(w) << "XRemoveFromSaveSet(" << WinID(w) << ")";
  server->ChangeSaveSet(w, false);
  return 0;
}

int XSetInputFocus(Window focus, int revert_to, Time time) {
  LOGD(focus) << "XSetInputFocus(" << WinID(focus) << ")";
  server->SetInputFocus(focus, revert_to, time);
  return 0;
}

FocusWindow XGetInputFocus() {
  return server->GetInputFocus();
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
  server->ConfigureWindow(w, changes);
  return 0;
}

int XChangeWindowAttributes(Window w, const WindowAttrs& attrs) {
  const ValueList& v = attrs.Values();
  LOGD(w) << "XChangeWindowAttributes(" << WinID(w) << ")"
          << MaskedValues{v, kAttrNames};
  server->ChangeWindowAttributes(w, attrs);
  return 0;
}

void SendEventRaw(Window w,
                  bool propagate,
                  uint32_t event_mask,
                  const char* event32) {
  LOGD(w) << "SendEvent(" << WinID(w) << ")";
  server->SendEventRaw(w, propagate, event_mask, event32);
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
  const WindowAttributes res = server->GetWindowAttributes(w);
  LOGD(w) << "XGetWindowAttributes: " << res.rect;
  return res;
}

WindowGeometry XGetGeometry(Window w) {
  return server->GetGeometry(w);
}

int XDestroyWindow(Window w) {
  LOGD(w) << "XDestroyWindow(" << WinID(w) << ")";
  server->DestroyWindow(w);
  return 0;
}

int XSetWindowBorderWidth(Window w, unsigned int width) {
  LOGD(w) << "XSetWindowBorderWidth(" << WinID(w) << ") -> " << width;
  WindowChanges wc;
  wc.BorderWidth(width);
  server->ConfigureWindow(w, wc);
  return 0;
}

int XSetWindowBackground(Window w, unsigned long pixel) {
  LOGD(w) << "XSetWindowBackground(" << WinID(w) << ") -> " << pixel;
  WindowAttrs attrs;
  attrs.BackPixel(uint32_t(pixel));
  server->ChangeWindowAttributes(w, attrs);
  return 0;
}

int XClearWindow(Window w) {
  LOGD(w) << "XClearWindow(" << WinID(w) << ")";
  // Width and height of zero mean "to the far edge", so this clears the lot.
  server->ClearArea(w, 0, 0, 0, 0, false);
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
  server->ClearArea(w, x, y, width, height, exposures);
  return 0;
}

int XFillRectangle(Window w,
                   GC gc,
                   int x,
                   int y,
                   unsigned int width,
                   unsigned int height) {
  server->FillRectangle(w, gc, x, y, width, height);
  return 0;
}

int XDrawLine(Window w, GC gc, int x1, int y1, int x2, int y2) {
  server->DrawLine(w, gc, x1, y1, x2, y2);
  return 0;
}

int XKillClient(Window w) {
  LOGD(w) << "XKillClient(" << WinID(w) << ")";
  server->KillClient(w);
  return 0;
}

static std::set<Window> lwm_owned_windows;

Window CreateNamedWindow(const std::string& name,
                         const Rect& rect,
                         unsigned int border_width,
                         unsigned long border_colour,
                         unsigned long background_colour) {
  const Window w =
      server->CreateWindow(rect, border_width, border_colour,
                           background_colour);
  lwm_owned_windows.insert(w);

  // Set WM_NAME. This is the modern equivalent of XSetWMName with a
  // STRING-typed text property; the older XStoreName route used to return
  // BadRequest errors despite working.
  server->ChangeProperty(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, name.c_str(),
                         name.size());
  return w;
}

bool IsLWMWindow(Window w) {
  return lwm_owned_windows.count(w);
}

WindowTree WindowTree::Query(Window w) {
  return server->QueryTree(w);
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
  server->GrabButton(button, modifiers, grab_window, owner_events, event_mask,
                     pointer_mode, keyboard_mode, confine_to, cursor);
  return 0;
}

int XUngrabButton(unsigned int button,
                  unsigned int modifiers,
                  Window grab_window) {
  LOGD(grab_window) << "XUngrabButton(" << WinID(grab_window) << ")";
  server->UngrabButton(button, modifiers, grab_window);
  return 0;
}

void XChangeActivePointerGrab(unsigned int event_mask,
                              Cursor cursor,
                              Time time) {
  server->ChangeActivePointerGrab(event_mask, cursor, time);
}

bool XGrabPointer(Window grab_window,
                  bool owner_events,
                  unsigned int event_mask,
                  int pointer_mode,
                  int keyboard_mode,
                  Window confine_to,
                  Cursor cursor,
                  Time time) {
  const int status =
      server->GrabPointer(grab_window, owner_events, event_mask, pointer_mode,
                          keyboard_mode, confine_to, cursor, time);
  LOGD(grab_window) << "XGrabPointer(" << WinID(grab_window)
                    << ") status=" << status;
  return status == XCB_GRAB_STATUS_SUCCESS;
}

void XUngrabPointer(Time time) {
  server->UngrabPointer(time);
}

// ---------------------------------------------------------------------------
// Properties and ICCCM hints.
// ---------------------------------------------------------------------------

const Atom kAnyPropertyType = XCB_GET_PROPERTY_TYPE_ANY;

Atom XInternAtom(const std::string& name) {
  return XInternAtoms({name})[0];
}

std::vector<Atom> XInternAtoms(const std::vector<std::string>& names) {
  return server->InternAtoms(names);
}

int XChangeProperty(Window w,
                    Atom property,
                    Atom type,
                    int format,
                    const void* data,
                    int nelements) {
  LOGD(w) << "XChangeProperty(" << WinID(w) << ") property=" << property;
  server->ChangeProperty(w, property, type, format, data, nelements);
  return 0;
}

void XDeleteProperty(Window w, Atom property) {
  LOGD(w) << "XDeleteProperty(" << WinID(w) << ")";
  server->DeleteProperty(w, property);
}

WindowProperty XGetWindowProperty(Window w,
                                  Atom property,
                                  long length,
                                  Atom req_type) {
  return server->GetWindowProperty(w, property, length, req_type);
}

WMHints XGetWMHints(Window w) {
  return server->GetWMHints(w);
}

NormalHints XGetWMNormalHints(Window w) {
  return server->GetWMNormalHints(w);
}

std::vector<WindowInfo> QueryWindows(const std::vector<Window>& ws) {
  return server->QueryWindows(ws);
}

std::vector<Atom> XGetWMProtocols(Window w) {
  return server->GetWMProtocols(w);
}

Window XGetTransientForHint(Window w) {
  return server->GetTransientForHint(w);
}

// ---------------------------------------------------------------------------
// Graphics contexts, colours and cursors.
// ---------------------------------------------------------------------------

GC XCreateGC(Window w, const GCValues& values) {
  return server->CreateGC(w, values);
}

void XChangeGC(GC gc, const GCValues& values) {
  server->ChangeGC(gc, values);
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
  unsigned long pixel = 0;
  if (!name.empty() && name[0] == '#') {
    uint16_t r = 0, g = 0, b = 0;
    if (parseHexColour(name, &r, &g, &b) &&
        server->AllocColour(r, g, b, &pixel)) {
      return pixel;
    }
    LOGW() << "Couldn't parse colour '" << name << "'; using black";
    return Black();
  }
  if (!server->AllocNamedColour(name, &pixel)) {
    LOGW() << "Couldn't allocate colour '" << name << "'; using black";
    return Black();
  }
  return pixel;
}

Cursor CreateFontCursor(unsigned int shape,
                        unsigned long fg,
                        unsigned long bg) {
  return server->CreateFontCursor(shape, fg, bg);
}

// ---------------------------------------------------------------------------
// Extensions.
// ---------------------------------------------------------------------------

RandRSupport XRRQueryExtension() {
  return server->RandRQueryExtension();
}

void XRRSelectInput(Window w) {
  server->RandRSelectInput(w);
}

std::vector<Rect> XRRGetVisibleAreas(Window root) {
  return server->RandRGetVisibleAreas(root);
}

#ifdef SHAPE
int XShapeQueryExtension() {
  return server->ShapeQueryExtension();
}

void XShapeSelectInput(Window w) {
  server->ShapeSelectInput(w);
}

int XShapeCountRectangles(Window w) {
  return server->ShapeCountRectangles(w);
}

void XShapeCombineShape(Window dest, int x_off, int y_off, Window src) {
  server->ShapeCombineShape(dest, x_off, y_off, src);
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
  const std::vector<uint32_t> pixels =
      server->GetImagePixels(src, width, height, depth);
  for (int i = 0; i < width * height && i < int(pixels.size()); i++) {
    img.Put(i % width, i / width, pixels[i]);
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
  return server->CreatePixmapFromPixels(img.width(), img.height(), img.data());
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
  server->FreePixmap(active_img_);
  server->FreePixmap(inactive_img_);
  server->FreePixmap(menu_img_);
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
  server->CopyArea(pm, w, gc, src_x, src_y, x, y, width, height);
  server->FreeGC(gc);
}

}  // namespace xlib
