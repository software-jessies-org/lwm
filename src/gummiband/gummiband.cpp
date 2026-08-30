// gummiband is a launcher.
//
// The config file is called '.gummiband', and consists of a sequence of
// entries, each representing an item on the menu. Each entry is defined by a
// sequence of key=value pairs.
//
// A few examples:
// # A simple item on the left side, named 'XTerm', which runs xterm:
// name=XTerm
// click=exec /usr/local/bin/xterm
//
// # A clock on the right-hand side, which will update every second.
// name=exec date +"%Y-%m-%d %H:%M:%S"
// position=right
// updatesecs=1
//
// # A drop-down menu on the right-hand side, which displays which audio
// # connector is active, and offers a means to switch.
// # Note: the 'audioconn' command is invented; I imagine you might want to
// # write a script and put it in $HOME/bin/.
// # Only 'front' and 'back' are valid values here; whichever is selected will
// # be passed to the 'exec audioconn', being substituted in for '<item>'.
// name=exec audioconn status
// updatesecs=5
// position=right
// menuitems=front,back
// menuclick=exec audioconn <item>
//
// # A drop-down menu on the left-hand side, which displays the set of network
// # interfaces available. We assume to have a 'networkconn' script which
// # when called with the argument 'status' prints out the current network
// # status, when called with 'list' prints out the set of network interfaces
// # (one per line), and when called with any other string, it tries to connect
// # to that network.
// name=exec networkconn status
// updatesecs=10
// position=left
// menuitems=exec networkconn list
// menuclick=exec networkconn <item>
//
// End of examples.
//
// The name protocol.
//
// A command run for a 'name=' may print more than the line the item is drawn
// from. The lines are:
//
//   1. the text to display.
//   2. the colours to display it in: one or two '#rrggbb' specifications, the
//      first the foreground and the second the background. A blank line means
//      the panel's usual colours.
//   3. onwards: the text of a tooltip, shown while the pointer rests on the
//      item.
//
// Any line after the first may be left out. A battery script printing
//
// 17%
// #ffffff #ff0000
// Discharging at 21W
// 48 minutes remaining
//
// draws '17%' in white on red, with the detail there for whoever hovers over
// it. Only 'name=' reads its command's output this way: 'menuitems=' takes
// one drop-down entry per line, as it always has.
//
// In general, if a value begins with 'exec ', it will be treated as a command
// to execute, and anything printed out by that command on stdout will be used
// as the value. For the name, for example, the following entries would look
// the same (although one's more costly than the other):
//
// name=hello
// name=exec echo hello
//
// The keyword 'name=' always begins a new item. Anything following that name
// will be attributed to it.
//
// Entries added to the left will be added left to right (so the first is the
// leftmost); entries added to the right are added right to left (so the first
// is the rightmost).

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <map>
#include <string>
#include <vector>

#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_xrm.h>

// Xft is the last of libX11, and the only reason any of it is still here.
// It has no XCB port, and it's the only way gummiband has of drawing text, so
// the two libraries share one socket: we open it with XOpenDisplay, hand the
// event queue to XCB with XSetEventQueueOwner, and pull the XCB connection
// out with XGetXCBConnection. Everything except text goes through XCB.
//
// See docs/xcb-migration-plan.md ("The one real blocker: Xft"); lwm does the
// same thing, but keeps it quarantined in its own translation unit because
// lwm's headers define Window too. gummiband is one file and includes no such
// header, so Xlib and XCB can sit side by side here.
#include <X11/Xft/Xft.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>

#include "lwm/log.h"

using namespace std;

// Only set this to non-zero if you're running a test gummiband alongside your
// normal instance.
#define Y_OFFSET 0

// The following are the Xresource values we read. They are:
// The font to use (default: roboto-16)
#define XRES_FONT "gummiband.font"
// The normal background colour (default: white)
#define XRES_BG "gummiband.background"
// The normal text foreground colour (default: black)
#define XRES_FG "gummiband.foreground"
// The background colour for the pointed-at item (default: pale blue).
#define XRES_SEL_BG "gummiband.selBackground"
// The text foreground colour for the pointed-at item (default: black).
#define XRES_SEL_FG "gummiband.selForeground"

// The connection to the X server, in both its guises. They are the same
// socket: conn is what everything but Xft uses, dpy is what Xft needs.
static xcb_connection_t* conn;
static Display* dpy;

// The screen we're displayed on, and the Xlib screen number that goes with it
// (needed by the Xft calls, which take one).
static xcb_screen_t* screen;
static int screen_num;

// display_xmin can be non-zero, if we'rd using xrandr and the highest screen
// is offset from the X=0 line. This happens, for example, if I have my laptop
// connected to an external screen, in the following configuration (note - the
// '=' signs shows where gummiband's window is positioned):
//
//   display_xmin
//    |
// <----->
//        +==========+
//        |          |
// +------|          |
// |      |          |
// |______|__________|
static int display_xmin;
static int display_width;

// display_ymin is the top of whichever monitor we're currently on. It is
// normally the top of the display, and is only something else while we've
// stepped aside onto a lower monitor to get out of the way of a full-screen
// window: see "stepping out of the way" below.
static int display_ymin;

static xcb_window_t window;           // Main window.
static xcb_window_t dropdown_window;  // Drop-down window.
static xcb_window_t tooltip_window;   // Tooltip window.
static int window_height;
static xcb_gcontext_t dropdown_gc;
static xcb_gcontext_t dropdown_highlight_gc;
static xcb_gcontext_t tooltip_gc;

// Font stuff.
static XftFont* g_font;
static XftDraw* g_dropdown_font_draw;
static XftDraw* g_tooltip_font_draw;
static XftColor g_font_color;
static XftColor g_selected_font_color;
static int g_font_height;
static int g_font_yoff;

// Colours as pulled out from the Xresources.
static uint32_t colour_bg;
static uint32_t colour_fg;
static uint32_t colour_sel_bg;
static uint32_t colour_sel_fg;

class Menu;
class MenuItem;
class Updaters;
class DropDown;

static Menu* menu;
static MenuItem* selected;
static Updaters* updaters;
static DropDown* dropdown;

static bool forceRestart;

// Reply owns a block of memory handed back by XCB and frees it when it goes
// out of scope, so functions with several early returns don't need a matching
// chain of frees down every path. It is safe to construct one holding null.
template <typename T>
class Reply {
 public:
  explicit Reply(T* data) : data_(data) {}
  Reply(const Reply&) = delete;
  Reply& operator=(const Reply&) = delete;
  ~Reply() { free(data_); }

  T* get() const { return data_; }
  T* operator->() const { return data_; }
  explicit operator bool() const { return data_ != nullptr; }

 private:
  T* data_;
};

// ValueList builds the (mask, values) pair XCB wants for CreateWindow,
// CreateGC, ConfigureWindow and friends.
//
// Xlib took a struct plus a mask and picked out the fields the mask named.
// XCB takes a bare array whose entries must appear in increasing order of
// their mask bits, and does not check: get the order wrong and you set the
// wrong attribute to the wrong value, silently. So nothing here hand-writes
// an array - Add() puts each value in its place, whatever order the call site
// supplies them in.
class ValueList {
 public:
  void Add(uint32_t mask_bit, uint32_t value) {
    if (mask_ & mask_bit) {
      LOGF() << "Value list already has a value for mask bit " << mask_bit;
    }
    mask_ |= mask_bit;
    // Count the set bits below this one to find where the value goes, then
    // shuffle everything from there up. The lists are a handful of entries
    // long, so the shuffle costs nothing.
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

  uint32_t Mask() const { return mask_; }
  const uint32_t* Values() const { return values_; }

 private:
  uint32_t mask_ = 0;
  uint32_t values_[32] = {};
};

// Flush pushes everything we've queued to the server.
//
// Sharing one socket between Xlib and XCB is not sharing one output buffer:
// the requests Xft makes queue up in Xlib's buffer, which xcb_flush() knows
// nothing about. Flush one and not the other and the requests you didn't
// flush sit there until something else happens to force them out.
static void Flush() {
  XFlush(dpy);
  xcb_flush(conn);
}

// Sync waits until the server has processed everything we've sent, so that
// any errors those requests provoked are already on our event queue.
// GetInputFocus is the traditional cheap round trip: no arguments, and it
// can't fail.
static void Sync() {
  Flush();
  free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
}

// InternAtoms interns several atoms in a single round trip: it fires all the
// requests off first, and only then collects the replies. Xlib's XInternAtom
// blocked on each one in turn.
//
// Note the 0 for only_if_exists: we always want the atom created. Asking only
// for atoms that already exist returns None whenever gummiband starts before
// the window manager, because nothing has interned _NET_WM_STRUT yet - and
// we'd then silently skip setting the strut.
static vector<xcb_atom_t> InternAtoms(const vector<string>& names) {
  vector<xcb_intern_atom_cookie_t> cookies;
  cookies.reserve(names.size());
  for (const string& name : names) {
    cookies.push_back(
        xcb_intern_atom(conn, 0, name.size(), name.c_str()));
  }
  vector<xcb_atom_t> res;
  res.reserve(names.size());
  for (xcb_intern_atom_cookie_t cookie : cookies) {
    Reply<xcb_intern_atom_reply_t> reply(
        xcb_intern_atom_reply(conn, cookie, nullptr));
    res.push_back(reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE));
  }
  return res;
}

// Split does the obvious. Eg Split("a; b; c", "; ") -> ["a", "b", "c"].
static vector<string> Split(const string& in, const string& sep) {
  size_t begin = 0;
  vector<string> res;
  while (begin < in.size()) {
    const size_t end = in.find(sep, begin);
    if (end == string::npos) {
      res.push_back(in.substr(begin));
      return res;
    }
    res.push_back(in.substr(begin, end - begin));
    begin = end + sep.size();
  }
  return res;
}

// KV returns the key and value (key=val) as a pair.
static pair<string, string> KV(const string& in) {
  int i = in.find('=');
  if (i == string::npos) {
    return make_pair(in, string());
  }
  return make_pair(in.substr(0, i), in.substr(i + 1));
}

// kDefaultConfig is the default configuration: an xterm on the left side, and
// a clock on the right. See the man page, or the comment at the top of this
// file, for more examples on how to customise this.
const char* kDefaultConfig = R"CFG(
name=XTerm
click=exec xterm

name=exec date +"%Y-%m-%d %H:%M:%S"
position=right
updatesecs=1
)CFG";

// LoadConfigLines tries to load in all lines from a file called '.gummiband',
// or if that's not there, returns kDefaultConfig, split into lines.
static vector<string> LoadConfigLines() {
  FILE* fp = fopen(".gummiband", "r");
  if (!fp) {
    return Split(kDefaultConfig, "\n");
  }
  char line[BUFSIZ];
  vector<string> res;
  while (fgets(line, BUFSIZ, fp) != 0) {
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
      len--;
    }
    res.push_back(string(line, len));
  }
  fclose(fp);
  return res;
}

// StringsSource abstracts away where we're getting strings from - by a static
// list, or by executing some sub-process and reading its stdout.
class StringsSource {
 public:
  virtual ~StringsSource() = default;
  virtual vector<string> Get() = 0;
};

static const char* const kWhitespace = " \t\n\r";

// SplitWhitespace splits on runs of whitespace, keeping none of it.
static vector<string> SplitWhitespace(const string& in) {
  vector<string> res;
  size_t i = 0;
  while (i < in.size()) {
    const size_t begin = in.find_first_not_of(kWhitespace, i);
    if (begin == string::npos) {
      break;
    }
    size_t end = in.find_first_of(kWhitespace, begin);
    if (end == string::npos) {
      end = in.size();
    }
    res.push_back(in.substr(begin, end - begin));
    i = end;
  }
  return res;
}

// ItemDisplay is how a menu item should look right now: the text to draw, the
// colours to draw it in, and what to say if the pointer settles on it.
// Everything but the text is optional; see "the name protocol" at the top of
// this file.
struct ItemDisplay {
  string text;
  string fg;  // Colour specification, or empty for the panel's own.
  string bg;  // Ditto.
  vector<string> tooltip;
};

// ParseItemDisplay reads the lines a name source produced.
static ItemDisplay ParseItemDisplay(const vector<string>& lines) {
  ItemDisplay res;
  if (lines.empty()) {
    return res;
  }
  res.text = lines[0];
  if (lines.size() > 1) {
    // A blank colour line means "the usual colours", and is how a program
    // says it has a tooltip but nothing to say about how it should look.
    const vector<string> colours = SplitWhitespace(lines[1]);
    for (int i = 0; i < colours.size() && i < 2; i++) {
      if (colours[i][0] != '#') {
        LOGE() << "Item '" << res.text << "': colour line '" << lines[1]
               << "' has '" << colours[i] << "' where a #rrggbb colour "
               << "should be";
        break;
      }
      (i == 0 ? res.fg : res.bg) = colours[i];
    }
  }
  for (int i = 2; i < lines.size(); i++) {
    res.tooltip.push_back(lines[i]);
  }
  // Nothing is gained by a tooltip with blank rows along the bottom, which is
  // what a program that pads its output would otherwise get.
  while (!res.tooltip.empty() && res.tooltip.back().empty()) {
    res.tooltip.pop_back();
  }
  return res;
}

// ItemDisplaySource is the name source of a single menu item, parsed. It
// re-parses only when the lines underneath it change: Get() is called on every
// repaint, which on a panel with a clock on it is once a second.
class ItemDisplaySource {
 public:
  explicit ItemDisplaySource(StringsSource* ss) : ss_(ss), parsed_(false) {}

  const ItemDisplay& Get() {
    vector<string> lines = ss_->Get();
    if (!parsed_ || lines != lines_) {
      lines_ = lines;
      display_ = ParseItemDisplay(lines_);
      parsed_ = true;
    }
    return display_;
  }

 private:
  StringsSource* ss_;
  vector<string> lines_;
  ItemDisplay display_;
  bool parsed_;
};

// StaticStringsSource is a StringsSource that has a pre-defined set of
// strings it returns every time.
class StaticStringsSource : public StringsSource {
 public:
  explicit StaticStringsSource(const string& s) : strings_(1, s) {}
  explicit StaticStringsSource(const vector<string>& ss) : strings_(ss) {}
  vector<string> Get() { return strings_; }

 private:
  vector<string> strings_;
};

// TrimTrailingWhitespace removes all newlines, tabs, spaces etc from the right-
// -hand side of the string.
static string TrimTrailingWhitespace(const string& s) {
  const size_t pos = s.find_last_not_of("\t\n\r ");
  if (pos == string::npos) {
    // Nothing but whitespace: the whole string goes. This used to return the
    // string untouched, which meant a program's blank line came back as the
    // "\n" fgets read rather than as the empty string - and a blank line is
    // how the name protocol says "no colours of my own".
    return string();
  }
  return s.substr(0, pos + 1);
}

// ExecStringsSource returns stdout from the run command, one entry per line.
// The Get() function will block until the sub-process completes.
class ExecStringsSource : public StringsSource {
 public:
  explicit ExecStringsSource(const string& command) : command_(command) {}

  vector<string> Get() {
    vector<string> res;
    FILE* fp = popen(command_.c_str(), "r");
    if (fp == NULL) {
      LOGE() << "Failed to run command '" << command_ << "'";
      return res;
    }
    char line[BUFSIZ];
    while (fgets(line, BUFSIZ, fp) != 0) {
      res.push_back(TrimTrailingWhitespace(line));
    }
    pclose(fp);
    return res;
  }

 private:
  const string command_;
};

// UpdatableStringsSource is backed by an ExecStringsSource, but caches the
// result, only refreshing when its update timer expires. Normally this will be
// registered with the 'Updaters' instance to ensure any cached data is updated
// regularly.
class UpdatableStringsSource : public StringsSource {
 public:
  UpdatableStringsSource(ExecStringsSource* source, int update_secs)
      : source_(source), update_secs_(update_secs), last_update_(0) {}

  vector<string> Get() {
    Update();  // Just in case.
    return cached_;
  }

  time_t NextUpdateTime() { return last_update_ + update_secs_; }

  // Returns true if the source was update, and that update yielded a new value.
  bool Update() {
    time_t now = time(nullptr);
    // The check for now >= last_update_ is to cope with the clock going
    // backwards, eg. when the computer's clock was messed up and ntpd wakes up
    // and fixes it.
    if (now >= last_update_ && now < NextUpdateTime()) {
      return false;
    }
    vector<string> new_val = source_->Get();
    last_update_ = now;
    if (new_val == cached_) {
      return false;
    }
    cached_ = new_val;
    return true;
  }

 private:
  ExecStringsSource* source_;
  vector<string> cached_;
  int update_secs_;
  time_t last_update_;
};

// Updaters holds onto all the things that need to be updated on some schedule,
// and calls their Update functions when appropriate.
class Updaters {
 public:
  Updaters() = default;

  void Add(UpdatableStringsSource* uss) { upds_.push_back(uss); }

  // Updates anything that needs it, returning true if any yielded a new value.
  bool Update() {
    bool res = false;
    for (UpdatableStringsSource* uss : upds_) {
      res |= uss->Update();
    }
    return res;
  }

 private:
  // We just keep the updatable things in whatever order. We could turn this
  // into a priority queue, but it's not worth the effort.
  vector<UpdatableStringsSource*> upds_;
};

// Action is a generic 'thing that can happen if you click on a MenuItem'.
class Action {
 public:
  virtual ~Action() = default;
  // x, y and width denote the line spanning the bottom of the clicked menu
  // item. This can be used if a drop-down menu is to be created, to position
  // it correctly.
  virtual void Act(int x, int y, int width) = 0;
};


// Exec runs the given command in the background as a child process, but returns
// immediately without waiting for the child to complete. This function is used
// for all actual launching.
static void Exec(const string& command) {
  static const char* sh;
  if (sh == nullptr) {
    sh = getenv("SHELL");
    if (sh == nullptr) {
      sh = "/bin/sh";
    }
  }

  switch (fork()) {
    case 0:  // Child.
      close(xcb_get_file_descriptor(conn));
      switch (fork()) {
        case 0:
          execl(sh, sh, "-c", command.c_str(), (char*)NULL);
          LOGF() << "exec \"" << sh << " -c " << command.c_str()
                 << "\" failed: " << Log::Errno(errno);
          _exit(EXIT_FAILURE);  // Not reached: LOGF exits.
        case -1:
          LOGF() << "fork failed: " << Log::Errno(errno);
          _exit(EXIT_FAILURE);  // Not reached: LOGF exits.
        default:
          _exit(EXIT_SUCCESS);
      }
    case -1:  // Error.
      LOGE() << "fork failed: " << Log::Errno(errno);
      break;
    default:
      wait(0);
  }
}

// ExecAction is an action attached to a MenuItem whose sole purpose is to run
// a fixed command when clicked. This is the thing that supports your 'Xterm'
// button.
class ExecAction : public Action {
 public:
  explicit ExecAction(const string& command) : command_(command) {}
  void Act(int, int, int) { Exec(command_); }

 private:
  string command_;
};

// TextWidth returns the display width of the given string in pixels.
static int TextWidth(const string& s) {
  XGlyphInfo extents;
  XftTextExtentsUtf8(dpy, g_font, reinterpret_cast<const FcChar8*>(s.data()),
                     s.size(), &extents);
  return extents.xOff;
}

// FontColour returns the foreground font colour for normal or highlighted text.
static XftColor* FontColour(bool selected) {
  return selected ? &g_selected_font_color : &g_font_color;
}

// The colours an item named for itself, looked up when first seen and then
// kept. Allocating a colour is a round trip to the server and the panel
// repaints every second, so asking each time would put a stall in every
// repaint. Both are defined further down, beside the allocation they wrap.
static uint32_t ItemColour(const string& name);
static XftColor* ItemFontColour(const string& name);

// FillRectangle and DrawSegments are thin wrappers over the XCB drawing
// requests, whose parameters arrive as arrays of structs rather than as the
// loose arguments Xlib took.
static void FillRectangle(xcb_drawable_t target,
                          xcb_gcontext_t gc,
                          int x,
                          int y,
                          int width,
                          int height) {
  const xcb_rectangle_t rect = {int16_t(x), int16_t(y), uint16_t(width),
                                uint16_t(height)};
  xcb_poly_fill_rectangle(conn, target, gc, 1, &rect);
}

// ChangeGCForeground repoints a GC at another colour, which is how the one
// spare GC paints as many different item backgrounds as the items ask for.
static void ChangeGCForeground(xcb_gcontext_t gc, uint32_t colour) {
  ValueList values;
  values.Add(XCB_GC_FOREGROUND, colour);
  xcb_change_gc(conn, gc, values.Mask(), values.Values());
}

static void DrawSegments(xcb_drawable_t target,
                         xcb_gcontext_t gc,
                         const vector<xcb_segment_t>& segments) {
  xcb_poly_segment(conn, target, gc, segments.size(), segments.data());
}

// A DropDown instance is created when the drop-down menu is opened from some
// MenuItem. It holds the set of items that were queried when the drop-down
// menu opened, handles drawing the menu, and acts on any click on a drop-down
// item.
class DropDown {
 public:
  DropDown(int width, const vector<string>& items, const string& command)
      : width_(width), items_(items), command_(command), selected_index_(-1) {}

  void Close() {
    xcb_unmap_window(conn, dropdown_window);
    delete this;
    dropdown = nullptr;
  }

  void Paint() {
    // ClearArea with zero width and height means 'to the far corner', which
    // is what XClearWindow was shorthand for. The 0 is exposures: we're the
    // ones painting, so we don't want the server to send us an Expose for the
    // area we just cleared.
    xcb_clear_area(conn, 0, dropdown_window, 0, 0, 0, 0);
    const int y_max = items_.size() * g_font_height - 1;
    const int x_max = width_ - 1;
    // Left edge, bottom edge, right edge: three lines, one request.
    DrawSegments(dropdown_window, dropdown_gc,
                 {{0, 0, 0, int16_t(y_max)},
                  {0, int16_t(y_max), int16_t(x_max), int16_t(y_max)},
                  {int16_t(x_max), 0, int16_t(x_max), int16_t(y_max)}});
    for (int i = 0; i < items_.size(); i++) {
      const int y = i * g_font_height;
      const string& item = items_[i];
      if (i == selected_index_) {
        FillRectangle(dropdown_window, dropdown_highlight_gc, 5, y, width_ - 10,
                      g_font_height);
      }
      XftDrawStringUtf8(g_dropdown_font_draw, FontColour(i == selected_index_),
                        g_font, 10, y + g_font_yoff,
                        reinterpret_cast<const FcChar8*>(item.data()),
                        item.size());
    }
  }

  void MouseMove(int x, int y) {
    const int index = GetIndexAt(x, y);
    if (index == selected_index_) {
      return;
    }
    selected_index_ = index;
    Paint();
  }

  void MouseLeft() {
    const bool was_selected = selected_index_ != -1;
    selected_index_ = -1;
    if (was_selected) {
      Paint();
    }
  }

  void MouseClick(int x, int y) {
    const int index = GetIndexAt(x, y);
    if (index == -1) {
      Close();
      return;
    }
    string command = command_;
    const size_t i = command_.find("<item>");
    if (i != string::npos) {
      command = command_.substr(0, i) + items_[index] + command_.substr(i + 6);
    }
    Exec(command);
    Close();
    return;
  }

 private:
  const int width_;
  const vector<string> items_;
  const string command_;
  int selected_index_;

  int GetIndexAt(int x, int y) {
    if (x < 0 || x >= width_ || y < 0 || y >= g_font_height * items_.size()) {
      return -1;
    }
    return y / g_font_height;
  }
};

// MoveResizeWindow and MapRaised stand in for the Xlib convenience functions
// of (almost) the same names. XCB has only the general ConfigureWindow, whose
// values have to be in mask-bit order; ValueList handles that.
static void MoveResizeWindow(xcb_window_t w,
                             int x,
                             int y,
                             int width,
                             int height) {
  ValueList values;
  values.Add(XCB_CONFIG_WINDOW_X, x);
  values.Add(XCB_CONFIG_WINDOW_Y, y);
  values.Add(XCB_CONFIG_WINDOW_WIDTH, width);
  values.Add(XCB_CONFIG_WINDOW_HEIGHT, height);
  xcb_configure_window(conn, w, values.Mask(), values.Values());
}

static void MapRaised(xcb_window_t w) {
  ValueList values;
  values.Add(XCB_CONFIG_WINDOW_STACK_MODE, XCB_STACK_MODE_ABOVE);
  xcb_configure_window(conn, w, values.Mask(), values.Values());
  xcb_map_window(conn, w);
}

// DropDownMenuAction acts on a click on a MenuItem by opening a drop-down
// menu at the appropriate position. It also creates a 'DropDown' instance to
// handle the menu while it's open.
class DropDownMenuAction : public Action {
 public:
  DropDownMenuAction(StringsSource* items, const string& command)
      : items_(items), command_(command) {}

  void Act(int x, int y, int width) {
    if (dropdown) {
      dropdown->Close();
      return;
    }
    vector<string> items = items_->Get();
    if (items.empty()) {
      items.push_back("<empty>");
    }
    const int height = items.size() * g_font_height;
    for (const string& item : items) {
      const int iw = TextWidth(item) + 20;
      if (iw > width) {
        width = iw;
      }
    }
    if (x + width > display_width) {
      x = display_width - width;
    }
    // So far, x is relative to gummiband's main window coordinates, but those
    // migth be offset, for example if we have multiple screens and the highest
    // (the one with Y=0) as an min X location greater than 0. See the comment
    // on the definition of 'display_xmin' above for a diagram.
    // Opening a window has to happen relative to 0, 0 in the overall display
    // coordinates. So we must apply the display_xmin so it appears at the right
    // location.
    x += display_xmin;
    // Likewise the vertical: y is measured from the top of the panel, which
    // is at the top of the monitor we're on rather than the top of the
    // display whenever we've stepped aside onto a lower one.
    MoveResizeWindow(dropdown_window, x, y + display_ymin + Y_OFFSET, width,
                     height);
    MapRaised(dropdown_window);
    dropdown = new DropDown(width, items, command_);
  }

 private:
  StringsSource* items_;
  const string command_;
};

// MenuItem is a single entity on the gummiband. It might or might not be
// clickable; clicking it may execute something or create a drop-down menu,
// and its text may be fixed or dynamic.
// Note that for dynamic text, we rely on the underlying StringSource to sort
// that out - MenuItem itself has no concept of updating things.
class MenuItem {
 public:
  MenuItem(ItemDisplaySource* source, Action* action)
      : source_(source), action_(action) {}

  const ItemDisplay& Display() { return source_->Get(); }

  bool HasAction() { return action_; }

  void Act() {
    if (action_) {
      action_->Act(x_, window_height, width_);
    }
  }

  // Called by the redraw code to let the menu item know where it's being drawn.
  void SetPosition(int x, int width) {
    x_ = x;
    width_ = width;
  }

  bool ContainsX(int x) { return x >= x_ && x < x_ + width_; }

  // Where the item was last drawn, which is where its tooltip goes.
  int X() const { return x_; }

 private:
  ItemDisplaySource* source_;
  Action* action_;
  int x_;
  int width_;
};


// MenuSet holds a set of menu items, in order. There's one MenuSet for the
// left-hand set, and one for the right-hand set of items.
class MenuSet {
 public:
  explicit MenuSet(bool rtl) : rtl_(rtl){};
  ~MenuSet() = default;

  // Takes ownership of item.
  void Add(MenuItem* item) { items_.push_back(item); }

  void Paint(xcb_drawable_t target,
             xcb_gcontext_t highlight_gc,
             xcb_gcontext_t bg_gc,
             XftDraw* g_font_draw) {
    int x = rtl_ ? (display_width - 5) : 5;
    for (MenuItem* mi : items_) {
      const ItemDisplay& item = mi->Display();
      const int width = 20 + TextWidth(item.text);
      if (rtl_) {
        x -= width;
      }
      mi->SetPosition(x, width);
      // Only an item you can click on is highlighted, so that the highlight
      // goes on meaning "this does something". An item's own background gives
      // way to the highlight, but its own foreground does not: the colour a
      // status item picked is usually saying something (red for a flat
      // battery) that a moment's hovering has no business hiding.
      const bool is_selected = mi == selected && mi->HasAction();
      if (is_selected) {
        FillRectangle(target, highlight_gc, x + 5, 0, width - 10,
                      window_height);
      } else if (!item.bg.empty()) {
        ChangeGCForeground(bg_gc, ItemColour(item.bg));
        FillRectangle(target, bg_gc, x + 5, 0, width - 10, window_height);
      }
      XftColor* colour =
          item.fg.empty() ? FontColour(is_selected) : ItemFontColour(item.fg);
      XftDrawStringUtf8(g_font_draw, colour, g_font, x + 10, g_font_yoff,
                        reinterpret_cast<const FcChar8*>(item.text.data()),
                        item.text.size());
      if (!rtl_) {
        x += width;
      }
    }
  }

  // The item under the pointer, whether or not clicking it would do anything:
  // an item with no action still has a tooltip to show.
  MenuItem* ItemAt(int mouseX) {
    for (MenuItem* mi : items_) {
      if (mi->ContainsX(mouseX)) {
        return mi;
      }
    }
    return nullptr;
  }

 private:
  bool rtl_;
  vector<MenuItem*> items_;
};

// Menu is the thing that has the sets of left and right menu items.
class Menu {
 public:
  Menu()
      : left_(new MenuSet(false)),
        right_(new MenuSet(true)),
        buffer_(XCB_NONE),
        g_font_draw_(nullptr) {}

  void Add(MenuItem* item, bool is_right) {
    (is_right ? right_ : left_)->Add(item);
  }

  void Paint() {
    EnsureDrawBufferExists();
    FillRectangle(buffer_, clear_gc_, 0, 0, display_width, window_height);
    left_->Paint(buffer_, highlight_gc_, item_bg_gc_, g_font_draw_);
    right_->Paint(buffer_, highlight_gc_, item_bg_gc_, g_font_draw_);
    // XCopyArea took the source and destination positions on either side of
    // the size; xcb_copy_area takes both positions first, then the size. This
    // is one of the argument reorderings that compiles perfectly happily and
    // paints nonsense.
    xcb_copy_area(conn, buffer_, window, cp_gc_, 0, 0, 0, 0, display_width,
                  window_height);
  }

  MenuItem* ItemAt(int mouseX) {
    MenuItem* res = left_->ItemAt(mouseX);
    return res ? res : right_->ItemAt(mouseX);
  }

  void SizeChanged() {
    if (buffer_ != XCB_NONE) {
      // The XftDraw holds a Render Picture for the pixmap, so it has to go
      // before the pixmap does, and it has to go at all: this used to be left
      // behind, leaking a Picture on every screen reconfiguration.
      XftDrawDestroy(g_font_draw_);
      g_font_draw_ = nullptr;
      xcb_free_pixmap(conn, buffer_);
      xcb_free_gc(conn, clear_gc_);
      xcb_free_gc(conn, highlight_gc_);
      xcb_free_gc(conn, item_bg_gc_);
      xcb_free_gc(conn, cp_gc_);
      buffer_ = XCB_NONE;
    }
  }

 private:
  void EnsureDrawBufferExists() {
    if (buffer_ != XCB_NONE) {
      return;
    }
    // XCB has the client pick resource IDs itself, so creating anything is a
    // generate_id followed by the create request; none of these round-trip.
    buffer_ = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, buffer_, window, display_width,
                      window_height);
    clear_gc_ = xcb_generate_id(conn);
    {
      ValueList values;
      values.Add(XCB_GC_FOREGROUND, colour_bg);
      xcb_create_gc(conn, clear_gc_, buffer_, values.Mask(), values.Values());
    }
    highlight_gc_ = xcb_generate_id(conn);
    {
      ValueList values;
      values.Add(XCB_GC_FOREGROUND, colour_sel_bg);
      xcb_create_gc(conn, highlight_gc_, buffer_, values.Mask(),
                    values.Values());
    }
    // No colour of its own: ChangeGCForeground points this at whichever one
    // the item being painted asked for.
    item_bg_gc_ = xcb_generate_id(conn);
    {
      ValueList values;
      values.Add(XCB_GC_FOREGROUND, colour_bg);
      xcb_create_gc(conn, item_bg_gc_, buffer_, values.Mask(), values.Values());
    }
    cp_gc_ = xcb_generate_id(conn);
    {
      ValueList values;
      values.Add(XCB_GC_FUNCTION, XCB_GX_COPY);
      xcb_create_gc(conn, cp_gc_, window, values.Mask(), values.Values());
    }
    // Xft needs the pixmap to exist on the server before it draws to it. It
    // will, because libX11 shares libxcb's socket and so its requests keep
    // their place in the sequence; but it's a request Xft makes rather than
    // reads, so it can't be told about our pixmap any other way.
    g_font_draw_ = XftDrawCreate(dpy, buffer_, DefaultVisual(dpy, screen_num),
                                 DefaultColormap(dpy, screen_num));
  }

  MenuSet* left_;
  MenuSet* right_;

  xcb_pixmap_t buffer_;
  xcb_gcontext_t clear_gc_;
  xcb_gcontext_t highlight_gc_;
  xcb_gcontext_t item_bg_gc_;
  xcb_gcontext_t cp_gc_;
  XftDraw* g_font_draw_;
};

// SetDeadline puts a moment `ms` milliseconds from now into *when, and
// MillisUntil says how long that moment still has to wait, or zero if it has
// been and gone. The monotonic clock, because these are all "shortly": a
// wall clock that ntpd has just stepped would otherwise make "in 100ms" mean
// anything at all.
static void SetDeadline(struct timespec* when, int ms) {
  clock_gettime(CLOCK_MONOTONIC, when);
  when->tv_sec += ms / 1000;
  when->tv_nsec += (ms % 1000) * 1000000L;
  if (when->tv_nsec >= 1000000000L) {
    when->tv_nsec -= 1000000000L;
    when->tv_sec++;
  }
}

static int MillisUntil(const struct timespec& when) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const long ms = (when.tv_sec - now.tv_sec) * 1000L +
                  (when.tv_nsec - now.tv_nsec) / 1000000L;
  return ms > 0 ? int(ms) : 0;
}

// ---------------------------------------------------------------- tooltips
//
// An item whose command printed more than a line of text and a line of
// colours has something further to say, and says it in a small window hung
// under the item while the pointer rests there. The pause before it appears
// is deliberate: sweeping the pointer along the panel on the way to a button
// shouldn't leave a trail of tooltips behind it.

static const int kTooltipDelayMs = 500;

// Whether a tooltip is waiting to appear, and when it is due. tooltip_due is
// only meaningful while tooltip_pending.
static bool tooltip_pending;
static struct timespec tooltip_due;

// Whether the tooltip window is up, what is written in it, and how wide
// ShowTooltip made it. The width is remembered rather than measured again
// because the border has to be drawn at the window's edge, and a repaint has
// no other way of knowing where that is.
static bool tooltip_shown;
static vector<string> tooltip_lines;
static int tooltip_width;

static void HideTooltip() {
  tooltip_pending = false;
  if (!tooltip_shown) {
    return;
  }
  tooltip_shown = false;
  tooltip_lines.clear();
  xcb_unmap_window(conn, tooltip_window);
}

static void PaintTooltip() {
  xcb_clear_area(conn, 0, tooltip_window, 0, 0, 0, 0);
  const int y_max = tooltip_lines.size() * g_font_height - 1;
  const int x_max = tooltip_width - 1;
  // Left, bottom and right edges, as the drop-down does: the top edge is
  // where the panel is, so a line there would only thicken the panel's own.
  DrawSegments(tooltip_window, tooltip_gc,
               {{0, 0, 0, int16_t(y_max)},
                {0, int16_t(y_max), int16_t(x_max), int16_t(y_max)},
                {int16_t(x_max), 0, int16_t(x_max), int16_t(y_max)}});
  for (int i = 0; i < tooltip_lines.size(); i++) {
    const string& line = tooltip_lines[i];
    XftDrawStringUtf8(g_tooltip_font_draw, FontColour(false), g_font, 10,
                      i * g_font_height + g_font_yoff,
                      reinterpret_cast<const FcChar8*>(line.data()),
                      line.size());
  }
}

// ShowTooltip puts the tooltip under an item, or takes it away again if that
// item has nothing to say. The panel's own colours, whatever the item chose
// for itself: an item picks its colours to be noticed across the room, which
// is not what a paragraph of text wants.
static void ShowTooltip(MenuItem* item) {
  const vector<string>& lines = item->Display().tooltip;
  if (lines.empty()) {
    HideTooltip();
    return;
  }
  tooltip_lines = lines;
  tooltip_width = 0;
  for (const string& line : tooltip_lines) {
    tooltip_width = max(tooltip_width, TextWidth(line) + 20);
  }
  const int height = tooltip_lines.size() * g_font_height;
  // The item's x is measured across the panel; the window goes at a position
  // on the display, so it needs the same display_xmin and display_ymin
  // corrections the drop-down needs. See DropDownMenuAction::Act.
  int x = item->X() + display_xmin;
  x = min(x, display_xmin + display_width - tooltip_width);
  x = max(x, display_xmin);
  MoveResizeWindow(tooltip_window, x, display_ymin + window_height + Y_OFFSET,
                   tooltip_width, height);
  MapRaised(tooltip_window);
  tooltip_shown = true;
  PaintTooltip();
}

// RequestTooltip starts the clock for whatever the pointer has just moved
// onto, and takes down the tooltip of whatever it moved off. Whether the new
// item has a tooltip at all is a question for kTooltipDelayMs later: asking
// now would mean running the item's command on every sweep of the pointer.
static void RequestTooltip() {
  HideTooltip();
  if (!selected || dropdown) {
    return;
  }
  tooltip_pending = true;
  SetDeadline(&tooltip_due, kTooltipDelayMs);
}

// GetEvent returns the next event, or null if a second went by without one.
static xcb_generic_event_t* GetEvent();

static bool IsExec(const string& s) {
  return (s.size() > 5) && (s.substr(0, 5) == "exec ");
}

struct ItemState {
  explicit ItemState(int line)
      : start_line(line), update_secs(0), is_right(false) {}

  int start_line;
  string name;
  int update_secs;
  bool is_right;
  string click;
  string menu_items;
  string menu_click;

  void CreateItem() {
    if (name.empty()) {
      return;
    }
    StringsSource* name_src;
    if (IsExec(name)) {
      ExecStringsSource* ess = new ExecStringsSource(name);
      if (update_secs > 0) {
        UpdatableStringsSource* uss =
            new UpdatableStringsSource(ess, update_secs);
        updaters->Add(uss);
        name_src = uss;
      } else {
        name_src = ess;
      }
    } else {
      name_src = new StaticStringsSource(name);
    }
    Action* action = nullptr;
    if (!click.empty()) {
      action = new ExecAction(click);
    } else if (!menu_items.empty()) {
      StringsSource* items = nullptr;
      if (IsExec(menu_items)) {
        items = new ExecStringsSource(menu_items);
      } else {
        items = new StaticStringsSource(Split(menu_items, ","));
      }
      if (menu_click.empty()) {
        LOGF() << Context() << "Menu with no 'menuclick' command";
      } else if (menu_click.find("<item>") == string::npos) {
        LOGF() << Context() << "menuclick command '" << menu_click
               << "' does not contain substring '<item>'";
      }
      action = new DropDownMenuAction(items, menu_click);
    }
    MenuItem* item = new MenuItem(new ItemDisplaySource(name_src), action);
    menu->Add(item, is_right);
  }

  string Context() {
    ostringstream str;
    str << "Item at line " << start_line << ": ";
    return str.str();
  }
};

static bool IgnoreConfigLine(const string& line) {
  return line.empty() || line[0] == '#';
}

static void ReadConfig() {
  const vector<string> lines = LoadConfigLines();
  menu = new Menu;
  updaters = new Updaters;

  ItemState state(0);
  for (int i = 0; i < lines.size(); i++) {
    if (IgnoreConfigLine(lines[i])) {
      continue;
    }
    pair<string, string> kv = KV(lines[i]);
    if (kv.first == "name") {
      state.CreateItem();
      state = ItemState(i);
      state.name = kv.second;
    } else if (kv.first == "updatesecs") {
      const int val = (int)strtol(kv.second.c_str(), (char**)0, 0);
      if (errno == EINVAL) {
        LOGE() << "line " << (i + 1) << ": can't parse int '" << kv.second
               << "'";
      } else if (val < 0) {
        LOGE() << "line " << (i + 1) << ": updatesecs must be positive";
      } else {
        state.update_secs = val;
      }
    } else if (kv.first == "position") {
      if (kv.second != "left" && kv.second != "right") {
        LOGE() << "line " << (i + 1) << ": position must be left or right";
      } else {
        state.is_right = kv.second == "right";
      }
    } else if (kv.first == "click") {
      state.click = kv.second;
    } else if (kv.first == "menuitems") {
      state.menu_items = kv.second;
    } else if (kv.first == "menuclick") {
      state.menu_click = kv.second;
    }
  }
  state.CreateItem();
}


static void DoExpose(const xcb_expose_event_t* ev) {
  // Only handle the last in a group of Expose events.
  if (ev && ev->count != 0) {
    return;
  }
  menu->Paint();
  if (dropdown) {
    dropdown->Paint();
  }
  if (tooltip_shown) {
    PaintTooltip();
  }
}

// An event pulled off the XCB queue by DropQueuedMotionEvents but not handled
// yet. XCB has no equivalent of XCheckMaskEvent, which used to let us pick the
// queued motion events out and leave everything else where it was; the only
// way to see what's on the queue is to take it off the front. So we take
// events off until we find one that isn't a motion, and park it here for
// GetEvent to hand back next time round.
static xcb_generic_event_t* pushed_back;

// Discards the run of motion events at the head of the queue. The position we
// actually act on comes from QueryPointer just below, so all the intermediate
// ones would tell us is where the pointer used to be.
static void DropQueuedMotionEvents() {
  while (!pushed_back) {
    xcb_generic_event_t* ev = xcb_poll_for_queued_event(conn);
    if (!ev) {
      return;
    }
    if ((ev->response_type & 0x7f) != XCB_MOTION_NOTIFY) {
      pushed_back = ev;
      return;
    }
    free(ev);
  }
}

static void DoMouseMoved(const xcb_motion_notify_event_t* ev) {
  if (ev->event == dropdown_window && dropdown) {
    dropdown->MouseMove(ev->event_x, ev->event_y);
    return;
  }
  const xcb_window_t event_window = ev->event;
  DropQueuedMotionEvents();

  Reply<xcb_query_pointer_reply_t> pointer(xcb_query_pointer_reply(
      conn, xcb_query_pointer(conn, event_window), nullptr));
  if (!pointer) {
    return;
  }
  MenuItem* old_selected = selected;
  selected = menu->ItemAt(pointer->win_x);
  if (old_selected != selected) {
    DoExpose(nullptr);
    RequestTooltip();
  }
}

static void DoButtonPress(const xcb_button_press_event_t* ev) {
  if (ev->event == dropdown_window && dropdown) {
    dropdown->MouseClick(ev->event_x, ev->event_y);
    return;
  }
  HideTooltip();
  selected = menu->ItemAt(ev->event_x);
  DoExpose(nullptr);
}

static void DoButtonRelease() {
  // An item with no action is still selected, because it may have a tooltip;
  // clicking one is a click on nothing, which is how an open drop-down gets
  // dismissed by clicking beside it.
  if (selected && selected->HasAction()) {
    selected->Act();
  } else if (dropdown) {
    dropdown->Close();
  }
  DoExpose(nullptr);
}

static void DoLeave(const xcb_leave_notify_event_t* ev) {
  if (ev->event == dropdown_window && dropdown) {
    dropdown->MouseLeft();
  } else {
    selected = nullptr;
    HideTooltip();
    DoExpose(nullptr);
  }
}

// HandleError reports an X error. Under Xlib these arrived through a callback
// installed with XSetErrorHandler; under XCB they come back as events, with a
// response_type of zero.
//
// The two string lookups are Xlib's, and are the one place we call into it for
// something other than text drawing. They're worth the dependency while it's
// there anyway: the alternative is a couple of hundred lines of static tables
// naming the error codes and request opcodes.
static void HandleError(const xcb_generic_error_t* e) {
  char msg[80];
  XGetErrorText(dpy, e->error_code, msg, sizeof(msg));

  char number[80];
  snprintf(number, sizeof(number), "%d", e->major_code);

  char req[80];
  XGetErrorDatabaseText(dpy, "XRequest", number, number, req, sizeof(req));

  LOGE() << "protocol request " << req << " on resource " << std::hex
         << e->resource_id << " failed: " << msg;
}

// Errors provoked by the requests Xft makes still go to Xlib's own handler
// rather than onto the XCB queue, so it needs one of these too. Xlib's default
// handler exits the process, which is a poor response to a font problem.
static int XlibErrorHandler(Display*, XErrorEvent* e) {
  xcb_generic_error_t err = {};
  err.error_code = e->error_code;
  err.resource_id = uint32_t(e->resourceid);
  err.major_code = e->request_code;
  err.minor_code = e->minor_code;
  HandleError(&err);
  return 0;
}

static void AddResource(map<string, string>* target,
                        xcb_xrm_database_t* db,
                        const string& name,
                        const string& dflt) {
  (*target)[name] = dflt;
  if (!db) {
    return;
  }
  char* value = nullptr;
  // The class argument must be null. xcb-xrm fails the whole lookup unless the
  // class string has exactly as many dot-separated components as the name, and
  // the classes this used to pass were single words ("Font", "String") against
  // two-component names like "gummiband.font". Xlib's XrmGetResource had the
  // same rule, so those lookups never matched and every resource here silently
  // took its default. Matching by name alone is what was always meant.
  if (xcb_xrm_resource_get_string(db, name.c_str(), nullptr, &value) < 0) {
    return;
  }
  if (!value) {
    return;
  }
  (*target)[name] = value;
  free(value);
}

extern map<string, string> GetResources() {
  map<string, string> res;
  // xcb-xrm reads the RESOURCE_MANAGER property off the root window itself, so
  // there's no separate fetch-the-string step as there was with
  // XResourceManagerString. A null database just means nothing is set, which
  // AddResource copes with.
  xcb_xrm_database_t* db = xcb_xrm_database_from_default(conn);
  AddResource(&res, db, XRES_FONT, "roboto-16");
  AddResource(&res, db, XRES_BG, "white");
  AddResource(&res, db, XRES_FG, "black");
  AddResource(&res, db, XRES_SEL_BG, "#b3e0ff");
  AddResource(&res, db, XRES_SEL_FG, "black");
  if (db) {
    xcb_xrm_database_free(db);
  }
  return res;
}

static void RestartSelf(int) {
  forceRestart = true;
}

// ChangeAtomProperty replaces a property holding a single atom.
static void ChangeAtomProperty(xcb_window_t w,
                               xcb_atom_t property,
                               xcb_atom_t value) {
  // Note the argument order: XChangeProperty took the mode after the format,
  // xcb_change_property takes it first. And the data length is in items of
  // the stated format, as it was before.
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, property, XCB_ATOM_ATOM,
                      32, 1, &value);
}

// The atoms gummiband needs, interned once by InternAllAtoms before anything
// uses them.
static xcb_atom_t atom_net_wm_window_type;
static xcb_atom_t atom_net_wm_window_type_dock;
static xcb_atom_t atom_net_wm_window_type_menu;
static xcb_atom_t atom_net_wm_window_type_tooltip;
static xcb_atom_t atom_net_wm_state;
static xcb_atom_t atom_net_wm_state_below;
static xcb_atom_t atom_net_wm_state_fullscreen;
static xcb_atom_t atom_net_wm_strut;
static xcb_atom_t atom_net_client_list;

static void InternAllAtoms() {
  const vector<xcb_atom_t> atoms = InternAtoms(
      {"_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK",
       "_NET_WM_WINDOW_TYPE_MENU", "_NET_WM_WINDOW_TYPE_TOOLTIP",
       "_NET_WM_STATE", "_NET_WM_STATE_BELOW", "_NET_WM_STATE_FULLSCREEN",
       "_NET_WM_STRUT", "_NET_CLIENT_LIST"});
  atom_net_wm_window_type = atoms[0];
  atom_net_wm_window_type_dock = atoms[1];
  atom_net_wm_window_type_menu = atoms[2];
  atom_net_wm_window_type_tooltip = atoms[3];
  atom_net_wm_state = atoms[4];
  atom_net_wm_state_below = atoms[5];
  atom_net_wm_state_fullscreen = atoms[6];
  atom_net_wm_strut = atoms[7];
  atom_net_client_list = atoms[8];
}

// SetStrut publishes, or withdraws, the space we ask other windows to keep
// clear for us.
//
// _NET_WM_STRUT gives left, right, top and bottom, each reserving a strip
// along the whole of that edge of the display. It has no way of naming a
// monitor - _NET_WM_STRUT_PARTIAL does, and lwm doesn't implement it - so we
// only claim a strut while we're at home along the top. Once we've stepped
// aside onto a lower monitor, a top strut would reserve a strip across the
// top of the monitor we just got out of the way of, which is where the window
// we stepped aside for is.
static void SetStrut(bool reserve) {
  // These are 32 bits each, and must be written as such. Xlib's
  // XChangeProperty took an array of long and narrowed it for the wire; XCB
  // sends what you give it, so an array of long here would put four 64-bit
  // values into a property the server has been told holds four 32-bit ones.
  const uint32_t val[4] = {0, 0, reserve ? uint32_t(window_height) : 0, 0};
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, window, atom_net_wm_strut,
                      XCB_ATOM_CARDINAL, 32, 4, val);
}

static void SetWindowProps(xcb_window_t w) {
  // _NET_WM_WINDOW_TYPE describes that this is a kind of dock or panel window,
  // that should probably be kept on top, but that the window manager certainly
  // shouldn't decorate with frame, title bar etc.
  ChangeAtomProperty(w, atom_net_wm_window_type, atom_net_wm_window_type_dock);

  // We're setting struts to try to keep other windows out of our way, but if
  // the user really wants to move a window over us, we should err on the side
  // of discretion, and not get in their way. After all, we reside at the top
  // of the screen, so forcing ourselves on top (which is the usual default
  // for 'dock' windows) is more likely to get in the way of the user's ability
  // to move a window out of the way.
  ChangeAtomProperty(w, atom_net_wm_state, atom_net_wm_state_below);
}

static void SetDropDownWindowProps(xcb_window_t w) {
  // _NET_WM_WINDOW_TYPE describes that this is a kind of dock or panel window,
  // that should probably be kept on top, but that the window manager certainly
  // shouldn't decorate with frame, title bar etc.
  ChangeAtomProperty(w, atom_net_wm_window_type, atom_net_wm_window_type_menu);
}

// ---------------------------------------------------------------- monitors

// Rect is a rectangle in root-window coordinates: what RandR reports a
// monitor as, and what a window's geometry is turned into so the two can be
// compared.
struct Rect {
  int x;
  int y;
  int width;
  int height;

  int xMax() const { return x + width; }
  int yMax() const { return y + height; }
  long Area() const { return long(width) * long(height); }

  // True if this rectangle covers every pixel of o.
  bool Contains(const Rect& o) const {
    return x <= o.x && y <= o.y && xMax() >= o.xMax() && yMax() >= o.yMax();
  }

  // The number of pixels the two rectangles have in common.
  long Overlap(const Rect& o) const {
    const long w = min(xMax(), o.xMax()) - max(x, o.x);
    const long h = min(yMax(), o.yMax()) - max(y, o.y);
    return (w > 0 && h > 0) ? w * h : 0;
  }
};

// The monitors making up the display. Set up before the first RandR query as
// the whole screen, so that the code below works on a server with no RandR at
// all.
static vector<Rect> monitors;

// Whether the server has RandR at all, and we've done the version handshake
// with it. Set once, in main. Everything which asks the server about monitors
// checks this first: without RandR there is nothing to ask, the display is the
// single monitor set up above, and asking anyway would only log an error.
static bool have_rr;

// MonitorsFromRandR asks RandR 1.5 for the monitor list: the one
// 'xrandr --listmonitors' prints. That's one entry per monitor as the user
// thinks of it, which is not the same as one per CRTC - two mirrored outputs
// share a CRTC, and a tiled 4K panel needs two - and which the user can
// override with 'xrandr --setmonitor'. When nobody has set any, the server
// derives the list from the active outputs, so this is the right question to
// ask whether or not anything has been configured by hand.
//
// On a server older than RandR 1.5 the request is an error, which comes back
// as a null reply here (asking for the reply with a null error pointer
// discards it rather than putting it on the event queue) and sends us to
// MonitorsFromCrtcs instead.
static vector<Rect> MonitorsFromRandR() {
  vector<Rect> res;
  Reply<xcb_randr_get_monitors_reply_t> reply(xcb_randr_get_monitors_reply(
      conn, xcb_randr_get_monitors(conn, screen->root, 1), nullptr));
  if (!reply) {
    return res;
  }
  xcb_randr_monitor_info_iterator_t it =
      xcb_randr_get_monitors_monitors_iterator(reply.get());
  for (; it.rem; xcb_randr_monitor_info_next(&it)) {
    res.push_back(
        Rect{it.data->x, it.data->y, it.data->width, it.data->height});
  }
  return res;
}

// MonitorsFromCrtcs is the fallback for servers without RandR 1.5. A CRTC is
// a scan-out engine rather than a monitor, so this gets mirrored outputs and
// tiled panels wrong, but it is the best such a server can tell us.
static vector<Rect> MonitorsFromCrtcs() {
  vector<Rect> res;
  Reply<xcb_randr_get_screen_resources_current_reply_t> screen_res(
      xcb_randr_get_screen_resources_current_reply(
          conn, xcb_randr_get_screen_resources_current(conn, screen->root),
          nullptr));
  if (!screen_res) {
    return res;
  }
  const int ncrtc =
      xcb_randr_get_screen_resources_current_crtcs_length(screen_res.get());
  const xcb_randr_crtc_t* crtcs =
      xcb_randr_get_screen_resources_current_crtcs(screen_res.get());
  // Fire all the per-CRTC queries off before collecting any of them, so the
  // whole walk costs one round trip rather than one per monitor. Xlib's
  // XRRGetCrtcInfo blocked on each in turn.
  vector<xcb_randr_get_crtc_info_cookie_t> cookies;
  cookies.reserve(ncrtc);
  for (int i = 0; i < ncrtc; i++) {
    cookies.push_back(
        xcb_randr_get_crtc_info(conn, crtcs[i], screen_res->config_timestamp));
  }
  for (int i = 0; i < ncrtc; i++) {
    // A CRTC with no mode isn't driving anything.
    Reply<xcb_randr_get_crtc_info_reply_t> crt(
        xcb_randr_get_crtc_info_reply(conn, cookies[i], nullptr));
    if (crt && crt->mode) {
      res.push_back(Rect{crt->x, crt->y, crt->width, crt->height});
    }
  }
  return res;
}

static void RefreshMonitors() {
  if (!have_rr) {
    return;  // Nothing to ask: the display is one monitor and always was.
  }
  vector<Rect> found = MonitorsFromRandR();
  if (found.empty()) {
    found = MonitorsFromCrtcs();
  }
  if (found.empty()) {
    // Better a stale layout than none at all.
    LOGE() << "RandR reported no monitors";
    return;
  }
  monitors = found;
}

// HomeArea is where the panel belongs when nothing is in its way: spanning
// the run of monitors along the top of the display, stitched together where
// they abut. See the comment on display_xmin above for a picture of why that
// isn't simply the whole width of the display.
static Rect HomeArea() {
  int top = INT_MAX;
  for (const Rect& m : monitors) {
    top = min(top, m.y);
  }
  // Each monitor along that top edge, keyed by its left edge and holding its
  // right one.
  map<int, int> x_ranges;
  for (const Rect& m : monitors) {
    if (m.y == top) {
      x_ranges[m.x] = m.xMax();
    }
  }
  if (x_ranges.empty()) {
    LOGE() << "No screen space along the top of the display";
    return Rect{display_xmin, display_ymin, display_width, window_height};
  }
  // Walk rightwards from the leftmost, following each monitor to whichever
  // one starts exactly where it ends, so that a gap between two monitors
  // stops us.
  const int xmin = x_ranges.begin()->first;
  int xmax = xmin;
  for (auto it = x_ranges.begin(); it != x_ranges.end() && it->second > xmax;
       it = x_ranges.find(xmax)) {
    xmax = it->second;
  }
  return Rect{xmin, top, xmax - xmin, window_height};
}

// ------------------------------------------------- stepping out of the way
//
// A game run full screen on the monitor the panel lives on leaves the panel
// either sitting on top of the game or, since we ask to be stacked below,
// hidden underneath it. Neither is any use, and with more than one monitor
// there's somewhere better to be, so while such a window is up we move to
// another monitor, and come back when it goes.
//
// "Full screen" means two different things here, because games mean two
// different things by it. One is _NET_WM_STATE_FULLSCREEN, which is what a
// toolkit asks for. The other is a window simply sized and placed to cover a
// monitor exactly: that's what 'borderless fullscreen' in a game's display
// settings produces, and what Steam titles under Proton generally do. No
// state is set and nothing is asked for - the window is just that big - so
// the only way to notice is to measure it.

// Whether the panel is currently somewhere other than HomeArea().
static bool displaced;

// WindowListProperty reads a property holding a list of window ids, and says
// whether the property was there at all. That isn't the same as whether it
// held anything, and is how we tell "no window manager is running" from "the
// window manager is running and nothing is open".
static bool WindowListProperty(xcb_window_t w,
                               xcb_atom_t prop,
                               vector<xcb_window_t>* into) {
  Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(
      conn, xcb_get_property(conn, 0, w, prop, XCB_ATOM_WINDOW, 0, 1024),
      nullptr));
  if (!reply || reply->type != XCB_ATOM_WINDOW || reply->format != 32) {
    return false;
  }
  const xcb_window_t* data =
      (const xcb_window_t*)xcb_get_property_value(reply.get());
  into->assign(data, data + xcb_get_property_value_length(reply.get()) /
                                sizeof(xcb_window_t));
  return true;
}

// WindowsToExamine returns the windows which might be covering a monitor.
//
// _NET_CLIENT_LIST is the right answer wherever it exists, because it names
// the clients' own windows rather than the frames the window manager wrapped
// them in. The distinction matters: a merely *maximised* window's frame does
// fill its monitor, while the window inside the frame doesn't, and a
// maximised window is no reason to go anywhere.
//
// With no window manager running there is no such property, and the clients
// are root's own children, so ask for those instead.
static vector<xcb_window_t> WindowsToExamine() {
  vector<xcb_window_t> res;
  if (WindowListProperty(screen->root, atom_net_client_list, &res)) {
    return res;
  }
  Reply<xcb_query_tree_reply_t> tree(
      xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), nullptr));
  if (!tree) {
    return res;
  }
  const xcb_window_t* children = xcb_query_tree_children(tree.get());
  res.assign(children, children + xcb_query_tree_children_length(tree.get()));
  return res;
}

// PropertyHasAtom says whether a property holding a list of atoms contains a
// particular one.
static bool PropertyHasAtom(const xcb_get_property_reply_t* reply,
                            xcb_atom_t atom) {
  if (!reply || reply->type != XCB_ATOM_ATOM || reply->format != 32) {
    return false;
  }
  const xcb_atom_t* data = (const xcb_atom_t*)xcb_get_property_value(reply);
  const int n = xcb_get_property_value_length(reply) / sizeof(xcb_atom_t);
  for (int i = 0; i < n; i++) {
    if (data[i] == atom) {
      return true;
    }
  }
  return false;
}

// CoveredMonitors says, for each entry in `monitors`, whether some window is
// filling it.
//
// Four questions about each window, all fired off before any reply is
// collected, so the scan costs two round trips however many windows there
// are. Errors are discarded rather than reported: a window can be destroyed
// between being listed and being asked about, which is normal rather than a
// fault.
static vector<bool> CoveredMonitors() {
  vector<bool> res(monitors.size(), false);
  const vector<xcb_window_t> windows = WindowsToExamine();
  struct Queries {
    xcb_get_window_attributes_cookie_t attrs;
    xcb_get_geometry_cookie_t geometry;
    xcb_translate_coordinates_cookie_t position;
    xcb_get_property_cookie_t state;
  };
  vector<Queries> queries;
  queries.reserve(windows.size());
  for (xcb_window_t w : windows) {
    queries.push_back(Queries{
        xcb_get_window_attributes(conn, w),
        xcb_get_geometry(conn, w),
        // A window's own geometry is relative to its parent, which under a
        // window manager is the frame rather than the root, so the position
        // has to be asked for separately.
        xcb_translate_coordinates(conn, w, screen->root, 0, 0),
        xcb_get_property(conn, 0, w, atom_net_wm_state, XCB_ATOM_ATOM, 0, 64),
    });
  }
  for (size_t i = 0; i < queries.size(); i++) {
    // Every reply has to be collected, even for a window we've already
    // decided we don't care about: one left uncollected sits in the
    // connection's buffer for ever.
    Reply<xcb_get_window_attributes_reply_t> attrs(
        xcb_get_window_attributes_reply(conn, queries[i].attrs, nullptr));
    Reply<xcb_get_geometry_reply_t> geometry(
        xcb_get_geometry_reply(conn, queries[i].geometry, nullptr));
    Reply<xcb_translate_coordinates_reply_t> position(
        xcb_translate_coordinates_reply(conn, queries[i].position, nullptr));
    Reply<xcb_get_property_reply_t> state(
        xcb_get_property_reply(conn, queries[i].state, nullptr));
    // Our own windows are never in our way, and an unmapped window - which is
    // what a hidden or iconified one is - isn't covering anything.
    if (windows[i] == window || windows[i] == dropdown_window ||
        windows[i] == tooltip_window) {
      continue;
    }
    if (!attrs || attrs->map_state != XCB_MAP_STATE_VIEWABLE) {
      continue;
    }
    if (!geometry || !position) {
      continue;
    }
    // The window's footprint, border included: the border is painted, so it
    // covers what's under it. TranslateCoordinates reports where the window's
    // origin is, which is *inside* the border, so the border has to be added
    // back on both sides - without which a window drawn exactly over a
    // monitor looks a border-width short of covering it.
    const int bw = geometry->border_width;
    const Rect r{position->dst_x - bw, position->dst_y - bw,
                 geometry->width + 2 * bw, geometry->height + 2 * bw};
    const bool asked_for_it =
        PropertyHasAtom(state.get(), atom_net_wm_state_fullscreen);
    for (size_t m = 0; m < monitors.size(); m++) {
      // A window which says it is full screen is taken at its word as long as
      // it covers most of the monitor; one which says nothing has to cover
      // the monitor exactly, or better.
      if (r.Contains(monitors[m]) ||
          (asked_for_it && r.Overlap(monitors[m]) * 2 >= monitors[m].Area())) {
        res[m] = true;
      }
    }
  }
  return res;
}

// PlacePanel puts the panel where it's told, and says whether that was
// anywhere new.
static bool PlacePanel(const Rect& target) {
  if (target.x == display_xmin && target.y == display_ymin &&
      target.width == display_width) {
    return false;
  }
  // Whatever the tooltip was pointing at is about to be somewhere else.
  HideTooltip();
  const bool width_changed = target.width != display_width;
  display_xmin = target.x;
  display_ymin = target.y;
  display_width = target.width;
  MoveResizeWindow(window, target.x, target.y + Y_OFFSET, target.width,
                   window_height);
  if (width_changed) {
    // The pixmap the panel is painted into is as wide as the panel, so it has
    // to be thrown away and made again at the new width.
    menu->SizeChanged();
  }
  return true;
}

// ReconsiderPlacement works out which monitor the panel should be on, and
// moves it there if that isn't where it already is.
static void ReconsiderPlacement() {
  const Rect home = HomeArea();
  const vector<bool> covered = CoveredMonitors();

  // Is anything filling a monitor we're at home on? Home can span several
  // monitors, and any one of them being covered is enough: half a panel under
  // a game is no better than all of it.
  bool must_move = false;
  for (size_t i = 0; i < monitors.size(); i++) {
    if (covered[i] && monitors[i].Overlap(home) > 0) {
      must_move = true;
    }
  }

  // Somewhere to go: the monitor nearest the top of the display that nothing
  // is filling, ties broken by the leftmost. If everything is covered then
  // there is nowhere better than home.
  const Rect* refuge = nullptr;
  if (must_move) {
    for (size_t i = 0; i < monitors.size(); i++) {
      if (covered[i]) {
        continue;
      }
      const Rect& m = monitors[i];
      if (!refuge || m.y < refuge->y || (m.y == refuge->y && m.x < refuge->x)) {
        refuge = &m;
      }
    }
  }

  const bool away = refuge != nullptr;
  const Rect target =
      away ? Rect{refuge->x, refuge->y, refuge->width, window_height} : home;
  const bool moved = PlacePanel(target);
  if (away != displaced) {
    displaced = away;
    SetStrut(!away);
  }
  if (moved && away) {
    // Come to the top of the stack on arrival: we ask to be stacked below
    // other windows, which on the monitor we've just fled to would leave us
    // underneath whatever was already there.
    MapRaised(window);
  }
}

// Rescans are deferred rather than done as the events which prompt them
// arrive: dragging a window produces a ConfigureNotify per pointer motion,
// and there is no point asking about every window on the display sixty times
// a second to be told that nothing has gone full screen. The first event of a
// burst starts the clock, and one scan happens kRescanDelayMs later, however
// many more events arrive in the meantime.
static const int kRescanDelayMs = 100;
static bool rescan_pending;
static struct timespec rescan_due;

// Whether that scan should re-read the monitor layout before deciding where
// the panel goes, rather than reusing the list it already has.
static bool monitors_stale;

static void RequestRescan() {
  if (rescan_pending) {
    return;  // The clock is already running; don't restart it.
  }
  rescan_pending = true;
  SetDeadline(&rescan_due, kRescanDelayMs);
}

// RequestLayoutRefresh asks for a rescan which re-reads the monitor layout
// first: a monitor has been plugged in or unplugged, or one has changed size
// or moved, so the answer to "where does the panel belong" has changed for
// reasons no amount of looking at windows would reveal.
static void RequestLayoutRefresh() {
  // Set this before the deferral check: a burst may already have started the
  // clock for a plain window rescan, and that scan has to be upgraded rather
  // than left to run on a monitor list we now know to be out of date.
  monitors_stale = true;
  RequestRescan();
}

// DoRescan is the deferred scan itself, run from the main loop when the clock
// set above falls due.
static void DoRescan() {
  rescan_pending = false;
  if (monitors_stale) {
    monitors_stale = false;
    RefreshMonitors();
  }
  ReconsiderPlacement();
}

// Parses an X11 hexadecimal colour specification into 16-bit components.
// The forms are #RGB, #RRGGBB, #RRRGGGBBB and #RRRRGGGGBBBB, each digit group
// being left-justified into 16 bits (so #f00 is full red, not 0x000f).
static bool ParseHexColour(const string& spec,
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

static uint32_t GetColour(const string& name) {
  // Beware: the AllocNamedColor *request* only knows the server's colour
  // database - the names in rgb.txt. It does not understand "#b3e0ff". Xlib's
  // XAllocNamedColor hid that, parsing hex specifications client-side before
  // sending a plain AllocColor. So must we; the default selection colour is a
  // hex specification, and handing it to AllocNamedColor gets you a panel
  // painted entirely black.
  const xcb_colormap_t cmap = screen->default_colormap;
  if (!name.empty() && name[0] == '#') {
    uint16_t r = 0, g = 0, b = 0;
    if (ParseHexColour(name, &r, &g, &b)) {
      Reply<xcb_alloc_color_reply_t> col(xcb_alloc_color_reply(
          conn, xcb_alloc_color(conn, cmap, r, g, b), nullptr));
      if (col) {
        return col->pixel;
      }
    }
    LOGE() << "Couldn't parse colour '" << name << "'; using black";
    return screen->black_pixel;
  }
  Reply<xcb_alloc_named_color_reply_t> col(xcb_alloc_named_color_reply(
      conn,
      xcb_alloc_named_color(conn, cmap, name.size(), name.c_str()), nullptr));
  if (!col) {
    LOGE() << "Couldn't allocate colour '" << name << "'; using black";
    return screen->black_pixel;
  }
  return col->pixel;
}

// Returns a short comprising two copies of the lowest byte in c.
// This converts an 8-bit r, g or b component into a 16-bit value as required
// by XRenderColor.
static unsigned short extend(uint32_t c) {
  unsigned short result = c & 0xff;
  return result | (result << 8);
}

XRenderColor GetXRenderColor(const string& name) {
  const uint32_t rgb = GetColour(name);
  return XRenderColor{extend(rgb >> 16), extend(rgb >> 8), extend(rgb), 0xffff};
}

// AllocFontColour turns a colour name into the XftColor used to draw text in
// it. This is the one part of the colour handling that has to stay on Xlib,
// because Xft's is the only interface that will take the result.
static void AllocFontColour(const string& name, XftColor* into) {
  XRenderColor xrc = GetXRenderColor(name);
  XftColorAllocValue(dpy, DefaultVisual(dpy, screen_num),
                     DefaultColormap(dpy, screen_num), &xrc, into);
}

// ItemColour and ItemFontColour are the cached lookups declared right at the
// top of the drawing code. What they cache is the round trip: an item naming
// the same colour every second would otherwise allocate it every second, and
// a colour nobody can allocate would log its complaint every second too.
static uint32_t ItemColour(const string& name) {
  static map<string, uint32_t> cache;
  const auto it = cache.find(name);
  if (it != cache.end()) {
    return it->second;
  }
  return cache[name] = GetColour(name);
}

static XftColor* ItemFontColour(const string& name) {
  static map<string, XftColor> cache;
  auto it = cache.find(name);
  if (it == cache.end()) {
    XftColor colour;
    AllocFontColour(name, &colour);
    it = cache.emplace(name, colour).first;
  }
  return &it->second;
}

// CreatePanelWindow makes one of our two panel-ish windows: they differ only
// in size and position, both being undecorated, both wanting the same
// events.
static xcb_window_t CreatePanelWindow(int x, int y, int width, int height) {
  ValueList attrs;
  attrs.Add(XCB_CW_BACK_PIXEL, colour_bg);
  attrs.Add(XCB_CW_BORDER_PIXEL, screen->black_pixel);
  attrs.Add(XCB_CW_OVERRIDE_REDIRECT, 0);
  attrs.Add(XCB_CW_EVENT_MASK,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS |
                XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_ENTER_WINDOW |
                XCB_EVENT_MASK_LEAVE_WINDOW);
  const xcb_window_t w = xcb_generate_id(conn);
  // Argument order again: the depth comes first here, where XCreateWindow had
  // it after the size, and the border width follows the size rather than
  // preceding the depth.
  xcb_create_window(conn, XCB_COPY_FROM_PARENT, w, screen->root, x, y, width,
                    height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    XCB_COPY_FROM_PARENT, attrs.Mask(), attrs.Values());
  return w;
}

// CreateTooltipWindow makes the tooltip's window, which differs from the other
// two in both the ways a window can. It is override-redirect, because no
// window manager should ever decorate, stack or focus a tooltip; and it asks
// for nothing but Expose, because pointer events arriving from it would be
// reported in its coordinates, and DoMouseMoved would read those as a
// position along the panel.
static xcb_window_t CreateTooltipWindow() {
  ValueList attrs;
  attrs.Add(XCB_CW_BACK_PIXEL, colour_bg);
  attrs.Add(XCB_CW_BORDER_PIXEL, screen->black_pixel);
  attrs.Add(XCB_CW_OVERRIDE_REDIRECT, 1);
  attrs.Add(XCB_CW_EVENT_MASK, XCB_EVENT_MASK_EXPOSURE);
  const xcb_window_t w = xcb_generate_id(conn);
  xcb_create_window(conn, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 100, 100,
                    0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
                    attrs.Mask(), attrs.Values());
  return w;
}

int main(int, char* argv[]) {
  struct sigaction sa = {};
  sa.sa_handler = RestartSelf;
  sigaddset(&(sa.sa_mask), SIGHUP);
  if (sigaction(SIGHUP, &sa, NULL)) {
    LOGF() << "SIGHUP sigaction failed: " << Log::Errno(errno);
  }

  // Open a connection to the X server. Xlib opens it, because Xft needs a
  // Display and there's no way to make one from an XCB connection; XCB then
  // takes it over. After XSetEventQueueOwner, Xlib's XNextEvent and friends
  // must not be called - which is the point, since we want a single event
  // queue and it should be the XCB one. Xft only makes requests, never reads
  // events, so it is unaffected.
  dpy = XOpenDisplay("");
  LOGF_IF(!dpy) << "can't open display";
  XSetErrorHandler(XlibErrorHandler);
  conn = XGetXCBConnection(dpy);
  LOGF_IF(!conn) << "can't get XCB connection";
  XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

  // XCB has no DefaultScreen: the setup carries the screens as a list, and
  // which one $DISPLAY asked for is a property of the display name, which only
  // Xlib parsed. So take the number from Xlib and walk to it.
  screen_num = DefaultScreen(dpy);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
  for (int i = 0; i < screen_num && it.rem; i++) {
    xcb_screen_next(&it);
  }
  screen = it.data;
  LOGF_IF(!screen) << "no screen " << screen_num;

  InternAllAtoms();

  map<string, string> x_resources = GetResources();

  // Find the screen's dimensions. This is the whole display, treated as one
  // monitor; RefreshMonitors replaces it with what RandR says as soon as we
  // know whether we have RandR at all.
  display_xmin = 0;
  display_ymin = 0;
  display_width = screen->width_in_pixels;
  monitors.push_back(
      Rect{0, 0, screen->width_in_pixels, screen->height_in_pixels});

  // Get font.
  g_font = XftFontOpenName(dpy, screen_num, x_resources[XRES_FONT].c_str());
  if (g_font == nullptr) {
    LOGE() << "couldn't find font " << x_resources[XRES_FONT]
           << "; trying default";
    g_font = XftFontOpenName(dpy, screen_num, "fixed");
    LOGF_IF(g_font == nullptr) << "can't find a font";
  }

  // Copy colours to static constants so we can access them from everywhere.
  colour_bg = GetColour(x_resources[XRES_BG]);
  colour_fg = GetColour(x_resources[XRES_FG]);
  colour_sel_bg = GetColour(x_resources[XRES_SEL_BG]);
  colour_sel_fg = GetColour(x_resources[XRES_SEL_FG]);

  // Create the window.
  g_font_height = 1.2 * (g_font->ascent + g_font->descent);
  g_font_yoff = 1.2 * g_font->ascent;
  window_height = g_font_height;
  window = CreatePanelWindow(0, 0, display_width, window_height);

  // Set struts on the window, so the window manager knows where not to place
  // other windows.
  SetWindowProps(window);
  SetStrut(true);

  // Create the objects needed to render text in the window.
  AllocFontColour(x_resources[XRES_FG], &g_font_color);
  AllocFontColour(x_resources[XRES_SEL_FG], &g_selected_font_color);

  dropdown_window = CreatePanelWindow(0, 0, 100, 100);
  SetDropDownWindowProps(dropdown_window);
  g_dropdown_font_draw =
      XftDrawCreate(dpy, dropdown_window, DefaultVisual(dpy, screen_num),
                    DefaultColormap(dpy, screen_num));

  tooltip_window = CreateTooltipWindow();
  ChangeAtomProperty(tooltip_window, atom_net_wm_window_type,
                     atom_net_wm_window_type_tooltip);
  g_tooltip_font_draw =
      XftDrawCreate(dpy, tooltip_window, DefaultVisual(dpy, screen_num),
                    DefaultColormap(dpy, screen_num));

  // Create GCs (note: the GCs for the main menu window are created in the Menu
  // class, as we use a pixmap for drawing so as to avoid flickering when the
  // clock updates.
  // dropdown_gc is used for clearing the drop-down window background, and for
  // drawing the partial border around the edge of the window.
  dropdown_gc = xcb_generate_id(conn);
  {
    ValueList values;
    values.Add(XCB_GC_FOREGROUND, colour_fg);
    values.Add(XCB_GC_BACKGROUND, colour_bg);
    xcb_create_gc(conn, dropdown_gc, dropdown_window, values.Mask(),
                  values.Values());
  }
  // dropdown_highlight_gc is used to draw a rectangle behind the text of the
  // currently-highlighted item.
  dropdown_highlight_gc = xcb_generate_id(conn);
  {
    ValueList values;
    values.Add(XCB_GC_FOREGROUND, colour_sel_bg);
    values.Add(XCB_GC_BACKGROUND, colour_bg);
    xcb_create_gc(conn, dropdown_highlight_gc, dropdown_window, values.Mask(),
                  values.Values());
  }
  // tooltip_gc draws the tooltip's border, in the same way and for the same
  // reason as dropdown_gc draws the drop-down's.
  tooltip_gc = xcb_generate_id(conn);
  {
    ValueList values;
    values.Add(XCB_GC_FOREGROUND, colour_fg);
    values.Add(XCB_GC_BACKGROUND, colour_bg);
    xcb_create_gc(conn, tooltip_gc, tooltip_window, values.Mask(),
                  values.Values());
  }

  // Create the menu items.
  ReadConfig();

  // Do we need to support XRandR? The version handshake isn't optional: RandR
  // rejects every other request until it has happened.
  const xcb_query_extension_reply_t* rr_ext =
      xcb_get_extension_data(conn, &xcb_randr_id);
  have_rr = rr_ext && rr_ext->present;
  if (have_rr) {
    Reply<xcb_randr_query_version_reply_t> version(xcb_randr_query_version_reply(
        conn, xcb_randr_query_version(conn, 1, 5), nullptr));
    have_rr = bool(version);
  }
  const uint8_t rr_event_base = have_rr ? rr_ext->first_event : 0;
  if (have_rr) {
    xcb_randr_select_input(conn, screen->root,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    RefreshMonitors();
  }

  // Watch the root window, so that we hear about other clients' windows
  // appearing, vanishing, moving and resizing. That's how we find out that
  // something has gone full screen over us. PropertyChange is for
  // _NET_CLIENT_LIST, which is how we hear about a window worth measuring
  // under a window manager which reparents its clients out of root.
  //
  // This sets gummiband's own event mask on root, and so takes nothing away
  // from the window manager: the masks which only one client may hold
  // (SubstructureRedirect and friends) aren't among these.
  {
    ValueList attrs;
    attrs.Add(XCB_CW_EVENT_MASK, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                                     XCB_EVENT_MASK_PROPERTY_CHANGE);
    xcb_change_window_attributes(conn, screen->root, attrs.Mask(),
                                 attrs.Values());
  }

  // Where the panel goes, and whether anything is already in its way: a game
  // may well have been running before we started.
  ReconsiderPlacement();

  // Bring up the window.
  MapRaised(window);

  // Make sure all our communication to the server got through.
  Sync();

  // The main event loop.
  static const int kRescanEveryIdleTicks = 5;
  int idle_ticks = 0;
  while (!forceRestart) {
    xcb_generic_event_t* ev = GetEvent();
    // Errors arrive on the event queue under XCB, rather than through a
    // callback at some unpredictable moment, and they're the one thing on it
    // with a response_type of zero.
    if (ev && ev->response_type == 0) {
      HandleError((const xcb_generic_error_t*)ev);
      free(ev);
      continue;
    }
    // Bit 0x80 says the event was sent with SendEvent rather than generated by
    // the server. We don't care which, but it has to come off before the type
    // can be compared with anything. A null event - nothing happened for a
    // second - is spelled -1 here, as it was under Xlib.
    const int type = ev ? (ev->response_type & 0x7f) : -1;
    if (have_rr && type == rr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
      // The monitor layout has changed. One reconfiguration produces several
      // of these - the server sends one per client-visible change, and a
      // single 'xrandr' invocation makes several - so this is deferred and
      // coalesced like any other rescan, rather than acted on here.
      //
      // Note what we deliberately don't do: pick some field out of the event
      // and skip the ones that look like repeats. This used to drop any event
      // whose config_timestamp matched the last one's, which sounds like it
      // identifies the reconfiguration but doesn't. config_timestamp is the
      // server's lastConfigTime, and that only moves when the set of available
      // outputs and modes changes - not when a CRTC is actually reconfigured.
      // So plugging a monitor in bumped it (and we handled that event, at
      // which point the new monitor was connected but not yet enabled), and
      // then the CRTC change which actually altered the layout arrived
      // carrying the same stamp and was thrown away. Nothing else re-reads the
      // layout, so the panel stayed the size of the old display until the
      // process was sent a SIGHUP.
      RequestLayoutRefresh();
      free(ev);
      continue;
    }
    if (rescan_pending && MillisUntil(rescan_due) == 0) {
      DoRescan();
    }
    if (updaters->Update()) {
      DoExpose(nullptr);
      // The item being hovered over may have just changed what it has to say,
      // and its tooltip is still showing what it said before.
      if (tooltip_shown && selected) {
        ShowTooltip(selected);
      }
    }
    if (tooltip_pending && MillisUntil(tooltip_due) == 0) {
      tooltip_pending = false;
      if (selected) {
        ShowTooltip(selected);
      }
    }
    switch (type) {
      case -1:  // Null event: repaint, in case the clock has ticked.
        DoExpose(nullptr);
        // Every so often, look again although nothing asked us to. This is
        // only a backstop: a window could in principle acquire
        // _NET_WM_STATE_FULLSCREEN without changing size, and none of the
        // events we watch would report that. Idle ticks are a second apart
        // and only happen when nothing else is going on, which is exactly
        // when the extra questions cost nothing.
        //
        // The monitor layout gets the same treatment, which is why this asks
        // for a layout refresh rather than a plain rescan. There is no known
        // screen change we fail to hear about, but this is the difference
        // between missing one and being wrong about the shape of the display
        // until someone restarts the panel.
        if (++idle_ticks >= kRescanEveryIdleTicks) {
          idle_ticks = 0;
          RequestLayoutRefresh();
        }
        break;
      case XCB_BUTTON_PRESS:
        DoButtonPress((const xcb_button_press_event_t*)ev);
        break;
      case XCB_BUTTON_RELEASE:
        DoButtonRelease();
        break;
      case XCB_MOTION_NOTIFY:
        DoMouseMoved((const xcb_motion_notify_event_t*)ev);
        break;
      case XCB_EXPOSE:
        DoExpose((const xcb_expose_event_t*)ev);
        break;
      case XCB_LEAVE_NOTIFY:
        DoLeave((const xcb_leave_notify_event_t*)ev);
        break;
      // Something in the window tree changed: a window was mapped, unmapped,
      // moved, resized, reparented or destroyed. Any of those could be a game
      // going full screen, or coming back out of it. Which window it was
      // doesn't matter, because the rescan looks at all of them anyway - and
      // it costs less to leave that to the rescan than to pick the field out
      // of five different event structures here.
      case XCB_MAP_NOTIFY:
      case XCB_UNMAP_NOTIFY:
      case XCB_DESTROY_NOTIFY:
      case XCB_REPARENT_NOTIFY:
      case XCB_CONFIGURE_NOTIFY:
        RequestRescan();
        break;
      case XCB_PROPERTY_NOTIFY:
        // The set of managed windows changed, so there may be a new one to
        // measure - or one we were measuring may have gone.
        if (((const xcb_property_notify_event_t*)ev)->atom ==
            atom_net_client_list) {
          RequestRescan();
        }
        break;
    }
    free(ev);
  }

  // Someone hit us with a SIGHUP: better exec ourselves to force a config
  // reload and cope with changing screen sizes.
  execvp(argv[0], argv);
}

static xcb_generic_event_t* GetEvent() {
  // Did we take one off the queue earlier and not use it?
  if (pushed_back) {
    xcb_generic_event_t* ev = pushed_back;
    pushed_back = nullptr;
    return ev;
  }

  // Is there a message waiting? poll_for_queued_event only looks at what's
  // already been read off the socket, which is what QLength used to report.
  if (xcb_generic_event_t* ev = xcb_poll_for_queued_event(conn)) {
    return ev;
  }

  // Beg... XCB never pushes requests to the server on its own, so anything we
  // queued above would otherwise sit in the output buffer while we block in
  // select() - which looks like the panel randomly freezing until you jiggle
  // the mouse. This is the one flush, in the one place.
  Flush();
  // Xlib had an IO error handler for this, which gummiband never installed;
  // the process would just die inside a library call. Say what happened.
  LOGF_IF(xcb_connection_has_error(conn)) << "lost connection to the X server";

  // Wait one second to see if a message arrives.
  int fd = xcb_get_file_descriptor(conn);
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(fd, &readfds);
  // Wait a second, or until a deferred rescan or a tooltip falls due,
  // whichever is soonest.
  int wait_ms = 1000;
  if (rescan_pending) {
    wait_ms = min(wait_ms, MillisUntil(rescan_due));
  }
  if (tooltip_pending) {
    wait_ms = min(wait_ms, MillisUntil(tooltip_due));
  }
  struct timeval tv;
  tv.tv_sec = wait_ms / 1000;
  tv.tv_usec = (wait_ms % 1000) * 1000;
  if (select(fd + 1, &readfds, 0, 0, &tv) == 1) {
    // This can still come back empty: what woke us might have been a reply
    // rather than an event. That's harmless - a null return is a null event,
    // which only means a repaint.
    return xcb_poll_for_event(conn);
  }

  // No message, so we have a null event.
  return nullptr;
}
