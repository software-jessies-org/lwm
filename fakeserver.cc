#include "fakeserver.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <sstream>

namespace xlib {
namespace {

std::string hex(uint32_t v) {
  std::ostringstream os;
  os << "0x" << std::hex << v;
  return os.str();
}

// The value a ValueList holds for one mask bit, if it holds one at all. XCB's
// values arrive as a bare array ordered by increasing mask bit, so finding one
// means counting the set bits below it.
bool valueFor(const ValueList& v, uint32_t bit, uint32_t* out) {
  if (!(v.Mask() & bit)) {
    return false;
  }
  int index = 0;
  for (int i = 0; (1u << i) < bit; i++) {
    if (v.Mask() & (1u << i)) {
      index++;
    }
  }
  *out = v.Values()[index];
  return true;
}

// Renders a value list as "name=value" pairs, in mask-bit order, for the call
// log. names is indexed by bit number; a null entry means lwm never sets that
// bit and the test would rather see the number than a wrong name.
std::string describe(const ValueList& v, const char* const* names) {
  std::ostringstream os;
  int index = 0;
  for (int bit = 0; bit < 32; bit++) {
    if (!(v.Mask() & (1u << bit))) {
      continue;
    }
    os << " ";
    if (names[bit]) {
      os << names[bit];
    } else {
      os << "bit" << bit;
    }
    os << "=" << v.Values()[index];
    index++;
  }
  return os.str();
}

const char* const kConfigNames[32] = {"x",       "y",       "width",
                                      "height",  "border_width",
                                      "sibling", "stack_mode"};

const char* const kAttrNames[32] = {
    "background_pixmap",     "background_pixel", "border_pixmap",
    "border_pixel",          "bit_gravity",      "win_gravity",
    "backing_store",         "backing_planes",   "backing_pixel",
    "override_redirect",     "save_under",       "event_mask",
    "do_not_propagate_mask", "colormap",         "cursor"};

}  // namespace

FakeServer::FakeServer() {
  // The root window is a window like any other, so that QueryTree and the
  // viewability walk don't need a special case for it.
  root_ = next_id_++;
  FakeWindow& root = windows_[root_];
  root.id = root_;
  root.parent = 0;
  root.rect = Rect::FromXYWH(0, 0, width_, height_);
  root.mapped = true;
}

FakeServer::~FakeServer() {
  for (xcb_generic_event_t* ev : events_) {
    free(ev);
  }
}

// ---------------------------------------------------------------------------
// Scripting and inspection.
// ---------------------------------------------------------------------------

Window FakeServer::NewWindow(Window parent, const Rect& rect, bool lwm_owned) {
  const Window id = next_id_++;
  FakeWindow& w = windows_[id];
  w.id = id;
  w.parent = parent;
  w.rect = rect;
  (void)lwm_owned;  // xlib.cc keeps its own record of which windows are ours.
  FakeWindow* p = Get(parent);
  if (p) {
    p->children.push_back(id);
  }
  return id;
}

Window FakeServer::AddClientWindow(const Rect& rect) {
  return NewWindow(root_, rect, false);
}

Window FakeServer::AddChildWindow(Window parent, const Rect& rect) {
  return NewWindow(parent, rect, false);
}

FakeWindow* FakeServer::Get(Window w) {
  auto it = windows_.find(w);
  return it == windows_.end() ? nullptr : &it->second;
}

void FakeServer::SetProperty32(Window w,
                               Atom property,
                               Atom type,
                               const std::vector<uint32_t>& data) {
  FakeWindow* win = Get(w);
  if (!win) {
    return;
  }
  FakeProperty& p = win->properties[property];
  p.type = type;
  p.format = 32;
  p.data32 = data;
  p.data8.clear();
}

void FakeServer::SetProperty8(Window w,
                              Atom property,
                              Atom type,
                              const std::string& data) {
  FakeWindow* win = Get(w);
  if (!win) {
    return;
  }
  FakeProperty& p = win->properties[property];
  p.type = type;
  p.format = 8;
  p.data8 = data;
  p.data32.clear();
}

void FakeServer::PushEventRaw(const void* event, size_t len) {
  // sizeof(xcb_generic_event_t), not 32. An X11 event is 32 bytes on the wire,
  // but libxcb hands out a slightly larger block with a `full_sequence` field
  // tacked on the end - the 32-bit sequence number, which the 16-bit one in
  // the event itself has wrapped away. ProcessPendingEvents() reads exactly
  // that field on every event, so a 32-byte block reads off the end of itself.
  // Allocating the whole struct also covers the dispatcher casting a generic
  // event to whichever struct its response type calls for, several of which
  // are bigger than what the test handed us.
  xcb_generic_event_t* ev = static_cast<xcb_generic_event_t*>(
      calloc(1, sizeof(xcb_generic_event_t)));
  memcpy(ev, event, len);
  events_.push_back(ev);
}

void FakeServer::SetScreenSize(int width, int height) {
  width_ = width;
  height_ = height;
  windows_[root_].rect = Rect::FromXYWH(0, 0, width, height);
}

void FakeServer::SetMousePosition(int x, int y, unsigned int mod_mask) {
  mouse_.x = x;
  mouse_.y = y;
  mouse_.modMask = mod_mask;
}

std::vector<std::string> FakeServer::CallsMatching(
    const std::string& prefix) const {
  std::vector<std::string> res;
  for (const std::string& call : calls_) {
    if (call.compare(0, prefix.size(), prefix) == 0) {
      res.push_back(call);
    }
  }
  return res;
}

bool FakeServer::DidCall(const std::string& prefix) const {
  return !CallsMatching(prefix).empty();
}

std::vector<Window> FakeServer::ChildrenOf(Window w) {
  FakeWindow* win = Get(w);
  return win ? win->children : std::vector<Window>();
}

Atom FakeServer::Intern(const std::string& name) {
  auto it = atoms_.find(name);
  if (it != atoms_.end()) {
    return it->second;
  }
  const Atom a = next_atom_++;
  atoms_[name] = a;
  atom_names_[a] = name;
  return a;
}

std::string FakeServer::NameOfAtom(Atom a) const {
  auto it = atom_names_.find(a);
  return it == atom_names_.end() ? std::string() : it->second;
}

// ---------------------------------------------------------------------------
// Connection, screen and event queue.
// ---------------------------------------------------------------------------

bool FakeServer::Open() {
  return true;
}

void FakeServer::Close() {}

void* FakeServer::XftDisplay() {
  return nullptr;
}

Window FakeServer::Root() {
  return root_;
}

int FakeServer::ScreenCount() {
  return 1;
}

int FakeServer::ScreenWidth() {
  return width_;
}

int FakeServer::ScreenHeight() {
  return height_;
}

unsigned long FakeServer::Black() {
  return 0;
}

unsigned long FakeServer::White() {
  return 0xffffff;
}

Colormap FakeServer::DefaultColourmap() {
  return 1;
}

xcb_visualid_t FakeServer::DefaultVisual() {
  return 1;
}

uint8_t FakeServer::DefaultDepth() {
  return 24;
}

std::string FakeServer::DisplayName() {
  return ":fake";
}

// Sync and Flush have nothing to push and nothing to wait for. They are not
// recorded: they say nothing about what lwm decided to do, and lwm calls
// Flush once round every pass of the event loop.
void FakeServer::Sync() {}
void FakeServer::Flush() {}

int FakeServer::ConnectionFD() {
  return -1;
}

bool FakeServer::ConnectionIsBroken() {
  return false;
}

xcb_generic_event_t* FakeServer::NextEvent() {
  if (events_.empty()) {
    return nullptr;
  }
  xcb_generic_event_t* ev = events_.front();
  events_.pop_front();
  return ev;  // Caller frees, as with the real thing.
}

uint32_t FakeServer::NextRequestSequence() {
  return sequence_++;
}

bool FakeServer::SelectRootEvents(Window root, uint32_t event_mask) {
  Record("SelectRootEvents(" + hex(root) + ") event_mask=" +
         std::to_string(event_mask));
  FakeWindow* w = Get(root);
  if (w) {
    w->event_mask = event_mask;
    w->all_event_masks |= event_mask;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Windows.
// ---------------------------------------------------------------------------

Window FakeServer::CreateWindow(const Rect& rect,
                                unsigned int border_width,
                                unsigned long border_colour,
                                unsigned long background_colour) {
  const Window w = NewWindow(root_, rect, true);
  Get(w)->border_width = border_width;
  std::ostringstream os;
  os << "CreateWindow(" << hex(w) << ") " << rect
     << " border_width=" << border_width << " border=" << border_colour
     << " background=" << background_colour;
  Record(os.str());
  return w;
}

void FakeServer::Unlink(FakeWindow* win) {
  FakeWindow* parent = Get(win->parent);
  if (!parent) {
    return;
  }
  std::vector<Window>& kids = parent->children;
  kids.erase(std::remove(kids.begin(), kids.end(), win->id), kids.end());
}

void FakeServer::DestroyWindow(Window w) {
  Record("DestroyWindow(" + hex(w) + ")");
  FakeWindow* win = Get(w);
  if (!win) {
    return;
  }
  // X destroys the subtree with the window, and a window id that has been
  // destroyed stops resolving - which is exactly the BadWindow that lwm's
  // ScopedIgnoreBadWindow exists to swallow, so the fake models it by
  // forgetting the window rather than flagging it.
  const std::vector<Window> children = win->children;
  for (const Window child : children) {
    DestroyWindow(child);
  }
  win = Get(w);
  if (!win) {
    return;
  }
  Unlink(win);
  windows_.erase(w);
  if (focused_ == w) {
    focused_ = 0;
  }
}

void FakeServer::Restack(FakeWindow* win, uint32_t stack_mode, Window sibling) {
  FakeWindow* parent = Get(win->parent);
  if (!parent) {
    return;
  }
  std::vector<Window>& kids = parent->children;
  kids.erase(std::remove(kids.begin(), kids.end(), win->id), kids.end());
  // Only the two stack modes lwm uses are modelled; Opposite, TopIf and
  // BottomIf never appear in a request lwm makes.
  if (stack_mode == XCB_STACK_MODE_BELOW) {
    if (sibling) {
      auto it = std::find(kids.begin(), kids.end(), sibling);
      kids.insert(it, win->id);
    } else {
      kids.insert(kids.begin(), win->id);
    }
    return;
  }
  if (sibling) {
    auto it = std::find(kids.begin(), kids.end(), sibling);
    if (it != kids.end()) {
      kids.insert(it + 1, win->id);
      return;
    }
  }
  kids.push_back(win->id);
}

void FakeServer::ConfigureWindow(Window w, const WindowChanges& changes) {
  const ValueList& v = changes.Values();
  if (v.Empty()) {
    return;
  }
  Record("ConfigureWindow(" + hex(w) + ")" + describe(v, kConfigNames));
  FakeWindow* win = Get(w);
  if (!win) {
    return;
  }
  uint32_t val = 0;
  Rect r = win->rect;
  if (valueFor(v, XCB_CONFIG_WINDOW_X, &val)) {
    r = Rect::FromXYWH(int32_t(val), r.yMin, r.width(), r.height());
  }
  if (valueFor(v, XCB_CONFIG_WINDOW_Y, &val)) {
    r = Rect::FromXYWH(r.xMin, int32_t(val), r.width(), r.height());
  }
  if (valueFor(v, XCB_CONFIG_WINDOW_WIDTH, &val)) {
    r = Rect::FromXYWH(r.xMin, r.yMin, val, r.height());
  }
  if (valueFor(v, XCB_CONFIG_WINDOW_HEIGHT, &val)) {
    r = Rect::FromXYWH(r.xMin, r.yMin, r.width(), val);
  }
  win->rect = r;
  if (valueFor(v, XCB_CONFIG_WINDOW_BORDER_WIDTH, &val)) {
    win->border_width = val;
  }
  uint32_t sibling = 0;
  valueFor(v, XCB_CONFIG_WINDOW_SIBLING, &sibling);
  if (valueFor(v, XCB_CONFIG_WINDOW_STACK_MODE, &val)) {
    Restack(win, val, sibling);
  }
}

void FakeServer::ChangeWindowAttributes(Window w, const WindowAttrs& attrs) {
  const ValueList& v = attrs.Values();
  if (v.Empty()) {
    return;
  }
  Record("ChangeWindowAttributes(" + hex(w) + ")" + describe(v, kAttrNames));
  FakeWindow* win = Get(w);
  if (!win) {
    return;
  }
  uint32_t val = 0;
  if (valueFor(v, XCB_CW_EVENT_MASK, &val)) {
    win->event_mask = val;
    win->all_event_masks |= val;
  }
}

void FakeServer::ReparentWindow(Window w, Window parent, int x, int y) {
  Record("ReparentWindow(" + hex(w) + ") parent=" + hex(parent) + " x=" +
         std::to_string(x) + " y=" + std::to_string(y));
  FakeWindow* win = Get(w);
  FakeWindow* p = Get(parent);
  if (!win || !p) {
    return;
  }
  Unlink(win);
  win->parent = parent;
  win->rect = Rect::FromXYWH(x, y, win->rect.width(), win->rect.height());
  p->children.push_back(w);
}

void FakeServer::MapWindow(Window w) {
  Record("MapWindow(" + hex(w) + ")");
  FakeWindow* win = Get(w);
  if (win) {
    win->mapped = true;
  }
}

void FakeServer::UnmapWindow(Window w) {
  Record("UnmapWindow(" + hex(w) + ")");
  FakeWindow* win = Get(w);
  if (win) {
    win->mapped = false;
  }
}

void FakeServer::ChangeSaveSet(Window w, bool insert) {
  Record(std::string(insert ? "AddToSaveSet(" : "RemoveFromSaveSet(") +
         hex(w) + ")");
}

void FakeServer::SetInputFocus(Window w, int revert_to, Time t) {
  (void)t;
  Record("SetInputFocus(" + hex(w) + ") revert_to=" +
         std::to_string(revert_to));
  focused_ = w;
}

FocusWindow FakeServer::GetInputFocus() {
  FocusWindow res{};
  res.window = focused_;
  res.revert_to = XCB_INPUT_FOCUS_POINTER_ROOT;
  return res;
}

WindowAttributes FakeServer::GetWindowAttributes(Window w) {
  WindowAttributes res{};
  FakeWindow* win = Get(w);
  if (!win) {
    return res;
  }
  res.ok = true;
  res.rect = win->rect;
  res.border_width = win->border_width;
  res.override_redirect = win->override_redirect;
  res.input_only = win->input_only;
  res.all_event_masks = win->all_event_masks;
  // Viewable means mapped, and every ancestor mapped too.
  res.viewable = true;
  for (FakeWindow* p = win; p; p = Get(p->parent)) {
    if (!p->mapped) {
      res.viewable = false;
      break;
    }
  }
  return res;
}

WindowGeometry FakeServer::GetGeometry(Window w) {
  WindowGeometry res{};
  FakeWindow* win = Get(w);
  if (!win) {
    return res;
  }
  res.ok = true;
  res.root = root_;
  res.rect = win->rect;
  res.border_width = win->border_width;
  res.bpp = win->depth;
  return res;
}

WindowTree FakeServer::QueryTree(Window w) {
  WindowTree res{};
  FakeWindow* win = Get(w);
  if (!win) {
    return res;
  }
  res.root = root_;
  res.parent = win->parent;
  if (res.parent) {
    res.self = w;
  }
  res.children = win->children;
  return res;
}

void FakeServer::ClearArea(Window w,
                           int x,
                           int y,
                           unsigned int width,
                           unsigned int height,
                           bool exposures) {
  std::ostringstream os;
  os << "ClearArea(" << hex(w) << ") x=" << x << " y=" << y
     << " width=" << width << " height=" << height
     << " exposures=" << (exposures ? 1 : 0);
  Record(os.str());
}

void FakeServer::FillRectangle(Window w,
                               GC gc,
                               int x,
                               int y,
                               unsigned int width,
                               unsigned int height) {
  std::ostringstream os;
  os << "FillRectangle(" << hex(w) << ") gc=" << hex(gc) << " x=" << x
     << " y=" << y << " width=" << width << " height=" << height;
  Record(os.str());
}

void FakeServer::DrawLine(Window w, GC gc, int x1, int y1, int x2, int y2) {
  std::ostringstream os;
  os << "DrawLine(" << hex(w) << ") gc=" << hex(gc) << " " << x1 << "," << y1
     << " -> " << x2 << "," << y2;
  Record(os.str());
}

void FakeServer::CopyArea(Pixmap src,
                          Window dest,
                          GC gc,
                          int src_x,
                          int src_y,
                          int x,
                          int y,
                          unsigned int width,
                          unsigned int height) {
  std::ostringstream os;
  os << "CopyArea(" << hex(src) << " -> " << hex(dest) << ") gc=" << hex(gc)
     << " src=" << src_x << "," << src_y << " dest=" << x << "," << y
     << " " << width << "x" << height;
  Record(os.str());
}

void FakeServer::KillClient(Window w) {
  Record("KillClient(" + hex(w) + ")");
}

void FakeServer::SendEventRaw(Window w,
                              bool propagate,
                              uint32_t event_mask,
                              const char* event32) {
  // Recorded, not delivered: everything lwm sends is aimed at a client, and
  // there are no clients here to react. The response type is the useful part,
  // since it's what distinguishes a ConfigureNotify from a WM_DELETE_WINDOW
  // client message.
  std::ostringstream os;
  os << "SendEvent(" << hex(w) << ") type=" << int(event32[0] & 0x7f)
     << " propagate=" << (propagate ? 1 : 0) << " event_mask=" << event_mask;
  if ((event32[0] & 0x7f) == XCB_CLIENT_MESSAGE) {
    xcb_client_message_event_t msg;
    memcpy(&msg, event32, sizeof(msg));
    os << " message_type=" << msg.type << " data0=" << msg.data.data32[0];
  }
  Record(os.str());
}

// ---------------------------------------------------------------------------
// Input.
// ---------------------------------------------------------------------------

void FakeServer::GrabButton(unsigned int button,
                            unsigned int modifiers,
                            Window grab_window,
                            bool owner_events,
                            unsigned int event_mask,
                            int pointer_mode,
                            int keyboard_mode,
                            Window confine_to,
                            Cursor cursor) {
  (void)owner_events;
  (void)event_mask;
  (void)pointer_mode;
  (void)keyboard_mode;
  (void)confine_to;
  (void)cursor;
  Record("GrabButton(" + hex(grab_window) + ") button=" +
         std::to_string(button) + " modifiers=" + std::to_string(modifiers));
}

void FakeServer::UngrabButton(unsigned int button,
                              unsigned int modifiers,
                              Window grab_window) {
  Record("UngrabButton(" + hex(grab_window) + ") button=" +
         std::to_string(button) + " modifiers=" + std::to_string(modifiers));
}

void FakeServer::GrabKey(uint8_t keycode,
                         unsigned int modifiers,
                         Window grab_window,
                         bool owner_events,
                         int pointer_mode,
                         int keyboard_mode) {
  (void)owner_events;
  (void)pointer_mode;
  (void)keyboard_mode;
  Record("GrabKey(" + hex(grab_window) + ") keycode=" +
         std::to_string(keycode) + " modifiers=" + std::to_string(modifiers));
}

void FakeServer::UngrabKey(uint8_t keycode,
                           unsigned int modifiers,
                           Window grab_window) {
  Record("UngrabKey(" + hex(grab_window) + ") keycode=" +
         std::to_string(keycode) + " modifiers=" + std::to_string(modifiers));
}

KeyboardMapping FakeServer::GetKeyboardMapping() {
  // A keyboard with nothing on it but the four arrow keys, at the keycodes a
  // PC keyboard really uses for them. Two keysyms per keycode, because that's
  // the commonest shape of a real mapping and it means the flattening
  // arithmetic in xlib.cc is exercised rather than sidestepped.
  KeyboardMapping res;
  res.min_keycode = 8;
  res.keysyms_per_keycode = 2;
  const uint8_t max_keycode = 255;
  res.keysyms.assign((max_keycode - res.min_keycode + 1) * 2, 0);
  const struct {
    uint8_t keycode;
    uint32_t keysym;
  } arrows[] = {
      {113, kKeysymLeft},
      {111, kKeysymUp},
      {114, kKeysymRight},
      {116, kKeysymDown},
  };
  for (const auto& a : arrows) {
    res.keysyms[(a.keycode - res.min_keycode) * res.keysyms_per_keycode] =
        a.keysym;
  }
  return res;
}

void FakeServer::ChangeActivePointerGrab(unsigned int event_mask,
                                         Cursor cursor,
                                         Time t) {
  (void)event_mask;
  (void)t;
  Record("ChangeActivePointerGrab() cursor=" + hex(cursor));
}

int FakeServer::GrabPointer(Window grab_window,
                            bool owner_events,
                            unsigned int event_mask,
                            int pointer_mode,
                            int keyboard_mode,
                            Window confine_to,
                            Cursor cursor,
                            Time time) {
  (void)owner_events;
  (void)event_mask;
  (void)pointer_mode;
  (void)keyboard_mode;
  (void)confine_to;
  (void)time;
  Record("GrabPointer(" + hex(grab_window) + ") cursor=" + hex(cursor));
  return pointer_grab_status_;
}

void FakeServer::UngrabPointer(Time time) {
  (void)time;
  Record("UngrabPointer()");
}

MousePos FakeServer::QueryPointer() {
  return mouse_;
}

// ---------------------------------------------------------------------------
// Properties and ICCCM hints.
// ---------------------------------------------------------------------------

std::vector<Atom> FakeServer::InternAtoms(
    const std::vector<std::string>& names) {
  std::vector<Atom> res;
  res.reserve(names.size());
  for (const std::string& name : names) {
    res.push_back(Intern(name));
  }
  return res;
}

void FakeServer::ChangeProperty(Window w,
                                Atom property,
                                Atom type,
                                int format,
                                const void* data,
                                int nelements) {
  std::ostringstream os;
  os << "ChangeProperty(" << hex(w) << ") property=" << property;
  const std::string name = NameOfAtom(property);
  if (!name.empty()) {
    os << " (" << name << ")";
  }
  os << " type=" << type << " format=" << format << " nelements=" << nelements;
  Record(os.str());

  FakeWindow* win = Get(w);
  if (!win) {
    return;
  }
  FakeProperty& p = win->properties[property];
  p.type = type;
  p.format = format;
  p.data32.clear();
  p.data8.clear();
  if (format == 32) {
    const uint32_t* words = static_cast<const uint32_t*>(data);
    p.data32.assign(words, words + nelements);
  } else {
    const char* bytes = static_cast<const char*>(data);
    p.data8.assign(bytes, nelements * (format == 16 ? 2 : 1));
  }
}

void FakeServer::DeleteProperty(Window w, Atom property) {
  Record("DeleteProperty(" + hex(w) + ") property=" +
         std::to_string(property));
  FakeWindow* win = Get(w);
  if (win) {
    win->properties.erase(property);
  }
}

WindowProperty FakeServer::GetWindowProperty(Window w,
                                             Atom property,
                                             long length,
                                             Atom req_type) {
  WindowProperty res{};
  FakeWindow* win = Get(w);
  if (!win) {
    return res;  // success stays false: this is the BadWindow case.
  }
  // A GetProperty for a property that isn't there still succeeds; it just
  // comes back empty. That distinction matters, because lwm's ok() means
  // "there was something there", not "the request worked".
  res.success = true;
  auto it = win->properties.find(property);
  if (it == win->properties.end()) {
    return res;
  }
  const FakeProperty& p = it->second;
  res.actual_type = p.type;
  res.actual_format = p.format;
  if (req_type != kAnyPropertyType && req_type != p.type) {
    return res;
  }
  if (p.format == 32) {
    const size_t available = p.data32.size();
    const size_t take = std::min<size_t>(available, size_t(std::max(0L, length)));
    res.data32.assign(p.data32.begin(), p.data32.begin() + take);
    res.nitems = take;
    res.bytes_after = (available - take) * 4;
  } else {
    const size_t available = p.data8.size();
    const size_t take =
        std::min<size_t>(available, size_t(std::max(0L, length)) * 4);
    res.data8 = p.data8.substr(0, take);
    res.nitems = take;
    res.bytes_after = available - take;
  }
  return res;
}

WMHints FakeServer::GetWMHints(Window w) {
  FakeWindow* win = Get(w);
  return win ? win->wm_hints : WMHints{};
}

NormalHints FakeServer::GetWMNormalHints(Window w) {
  FakeWindow* win = Get(w);
  return win ? win->normal_hints : NormalHints{};
}

std::vector<WindowInfo> FakeServer::QueryWindows(
    const std::vector<Window>& ws) {
  std::vector<WindowInfo> res(ws.size());
  for (size_t i = 0; i < ws.size(); i++) {
    res[i].attributes = GetWindowAttributes(ws[i]);
    res[i].normal_hints = GetWMNormalHints(ws[i]);
  }
  return res;
}

std::vector<Atom> FakeServer::GetWMProtocols(Window w) {
  FakeWindow* win = Get(w);
  return win ? win->wm_protocols : std::vector<Atom>();
}

Window FakeServer::GetTransientForHint(Window w) {
  FakeWindow* win = Get(w);
  return win ? win->transient_for : 0;
}

// ---------------------------------------------------------------------------
// Graphics contexts, colours and cursors.
// ---------------------------------------------------------------------------

GC FakeServer::CreateGC(Window w, const GCValues& values) {
  (void)w;
  (void)values;
  return next_id_++;
}

void FakeServer::ChangeGC(GC gc, const GCValues& values) {
  (void)gc;
  (void)values;
}

void FakeServer::FreeGC(GC gc) {
  (void)gc;
}

bool FakeServer::AllocColour(uint16_t r,
                             uint16_t g,
                             uint16_t b,
                             unsigned long* pixel) {
  // A 24-bit TrueColor visual, which is what every display lwm runs on has
  // been for twenty years: the top 8 bits of each component, packed.
  *pixel = (unsigned long)(r >> 8) << 16 | (unsigned long)(g >> 8) << 8 |
           (unsigned long)(b >> 8);
  return true;
}

bool FakeServer::AllocNamedColour(const std::string& name,
                                  unsigned long* pixel) {
  // Only the two names in lwm's own defaults. Everything else it asks for is
  // a hex specification, which ColourByName parses before it gets here.
  // Matching is case-insensitive, as the server's own rgb.txt lookup is:
  // lwm spells these "Black" and "White".
  std::string lower;
  for (const char ch : name) {
    lower.push_back(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
  }
  if (lower == "black") {
    *pixel = 0;
    return true;
  }
  if (lower == "white") {
    *pixel = 0xffffff;
    return true;
  }
  return false;
}

Cursor FakeServer::CreateFontCursor(unsigned int shape,
                                    unsigned long fg,
                                    unsigned long bg) {
  (void)shape;
  (void)fg;
  (void)bg;
  return next_id_++;
}

// ---------------------------------------------------------------------------
// Pixmaps and images.
// ---------------------------------------------------------------------------

std::vector<uint32_t> FakeServer::GetImagePixels(Pixmap src,
                                                 int width,
                                                 int height,
                                                 int depth) {
  (void)src;
  (void)depth;
  return std::vector<uint32_t>(size_t(width) * height, 0);
}

Pixmap FakeServer::CreatePixmapFromPixels(int width,
                                          int height,
                                          const uint32_t* data) {
  (void)width;
  (void)height;
  (void)data;
  return next_id_++;
}

void FakeServer::FreePixmap(Pixmap p) {
  Record("FreePixmap(" + hex(p) + ")");
}

// ---------------------------------------------------------------------------
// Extensions.
// ---------------------------------------------------------------------------

RandRSupport FakeServer::RandRQueryExtension() {
  // No RandR: the multi-monitor maths is tier-0 and tested directly in
  // screenlayout_test.cc, so there's nothing here for it to add.
  return RandRSupport{};
}

void FakeServer::RandRSelectInput(Window w) {
  (void)w;
}

std::vector<Rect> FakeServer::RandRGetVisibleAreas(Window root) {
  (void)root;
  return {Rect::FromXYWH(0, 0, width_, height_)};
}

#ifdef SHAPE
int FakeServer::ShapeQueryExtension() {
  // An event base well clear of the core events, as a real server's would be.
  return 64;
}

void FakeServer::ShapeSelectInput(Window w) {
  (void)w;
}

int FakeServer::ShapeCountRectangles(Window w) {
  FakeWindow* win = Get(w);
  return win ? win->shape_rectangles : 1;
}

void FakeServer::ShapeCombineShape(Window dest,
                                   int x_off,
                                   int y_off,
                                   Window src) {
  Record("ShapeCombineShape(" + hex(dest) + ") src=" + hex(src) + " x_off=" +
         std::to_string(x_off) + " y_off=" + std::to_string(y_off));
}
#endif

}  // namespace xlib
