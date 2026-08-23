#ifndef LWM_CLIENT_H_included
#define LWM_CLIENT_H_included

#include <cstdint>
#include <list>
#include <string>

#include "edge.h"
#include "ewmh.h"
#include "geometry.h"
#include "sizelimits.h"
#include "strings.h"
#include "strut.h"
#include "xlib.h"

/*
 * c->proto is a bitarray of these
 */
enum { Pdelete = 1, Ptakefocus = 2 };

// The X border width of the frame window LWM creates around a client. X puts a
// window's border *outside* its coordinate space, so a frame at (x, y) has its
// drawable area, and hence the client window inside it, at (x+1, y+1). That's
// harmless for an ordinary window, but a full-screen one has to line up with
// the monitor exactly, so EnterFullScreen drops it to zero and ExitFullScreen
// puts it back.
constexpr int kFrameBorderWidth = 1;

class Client {
 public:
  Client(Window w,
         const xlib::WindowAttributes& attr,
         const DimensionLimiter& x_limiter,
         const DimensionLimiter& y_limiter);

  ~Client() { delete icon_; }

  // Called on LWM shutdown to reparent the client window and give it back its
  // border.
  void Release();

  void SetName(const std::string& n) { name_ = n; }
  void SetVisibleName(const std::string& n) { visible_name_ = n; }
  const std::string& Name() const {
    return visible_name_ == "" ? name_ : visible_name_;
  }
  std::string MenuName() const;

  // Returns the edge corresponding to the action to be performed on the window.
  // The special cases 'EClose' and
  Edge EdgeAt(Window w, int x, int y) const;

  void Hide();
  void Unhide();

  void EnterFullScreen();
  void ExitFullScreen();

  // Sets which axes the window is maximised on (_NET_WM_STATE_MAXIMIZED_VERT
  // and _HORZ), and moves it to suit. Pass both false to un-maximise, which
  // puts the window back where it was before the first axis was set.
  //
  // Takes the new values rather than reading wstate itself, because it has to
  // see the transition: the geometry to restore is recorded on the way in.
  void SetMaximized(bool vert, bool horz);
  bool IsMaximized() const {
    return wstate.maximized_vert || wstate.maximized_horz;
  }

  // Forgets that the window is maximised, without moving it. For when the user
  // takes the geometry into their own hands: a window they've dragged or
  // resized isn't maximised any more, whatever the flags used to say.
  void DropMaximization();

  // The Windows-key double click grows a window in one shot, and from the
  // middle of the window it grows every edge, which on an otherwise empty
  // screen means "maximise". Doing it again in the middle puts the window
  // back, and these two are how it remembers where "back" is. There's no
  // state flag for "expanded" - such a window looks exactly like one the user
  // sized that way by hand - so the recorded rect is lwm's only record that
  // there is anything to undo.
  //
  // Call NotePreExpandRect() just before the window grows, and before
  // dropping any maximisation: a window that's big because it was maximised
  // should come back to the size it had before *that*, not to a screen-sized
  // rect.
  void NotePreExpandRect();

  // Puts the window back to the geometry NotePreExpandRect() recorded, or
  // un-maximises it if that's how it got big. Does nothing at all if there's
  // no record of an earlier size: an un-expand with nothing to go back to
  // leaves the window alone rather than inventing a size for it.
  void Unexpand();

  // Adds or removes lwm's window furniture, at the user's request (the
  // Super+Control click - see getSuperDragHandler). What stays put is the
  // window's outer extent: a window which loses its frame grows into the
  // space the furniture was using, and one which gains a frame shrinks to
  // make room, so the toggle doesn't shuffle the desktop about.
  //
  // Does nothing in the three cases where there's nothing sensible to do: a
  // window type which never gets furniture in the first place (see
  // ewmh_hasframe), a full-screen window, whose furniture is already off for
  // as long as that lasts, and a hidden one, which the Hider is holding by
  // whichever window this would change.
  void SetFurniture(bool want_furniture);

  // True while lwm is drawing a title bar and borders around this client.
  // That's less often than 'framed': a framed client has no furniture while
  // it's full screen, or while the user has turned it off with the
  // Super+Control click. In both of those the frame window is still there,
  // sized exactly to the client window, and FrameRect() is the content rect.
  bool HasFurniture() const {
    return framed && furniture_ && !wstate.fullscreen;
  }

  // Tells this Client that lwm has just asked the server to unmap the client
  // window itself. An unmap lwm asked for is not the client withdrawing its
  // window, and the UnmapNotify looks exactly the same either way, so the
  // ones we cause have to be counted off as they arrive: see EvUnmapNotify.
  //
  // Reparenting counts. The server unmaps a mapped window on its way out of
  // its old parent and maps it again afterwards, so the one reparent
  // SetFurniture can still cause announces one of these.
  void ExpectUnmap() { expected_unmaps_++; }

  // Consumes one expected unmap, returning true if there was one to consume -
  // that is, if this UnmapNotify is lwm's own doing and must be ignored.
  bool TakeExpectedUnmap();

  Window window = 0;  // Client's window.
  Window parent = 0;  // Window manager frame.
  Window trans = 0;   // Window that client is a transient for.

  // True if lwm is maintaining a frame window for this client - which is to
  // say if 'parent' is that frame rather than the root. Whether the frame has
  // any furniture on it is a separate question: see HasFurniture().
  bool framed = false;

 public:
  int State() const { return state_; }
  void SetState(int state);
  bool IsHidden() const { return state_ == IconicState; }
  bool IsWithdrawn() const { return state_ == WithdrawnState; }
  bool IsNormal() const { return state_ == NormalState; }

  bool HasFocus() const;
  static Client* FocusedClient();

  void SendConfigureNotify();

  // Notifications to the Client that it has gained or lost focus.
  void FocusGained();
  void FocusLost();

  // Asks the server for the button presses which make up the 'Windows' key
  // mouse gestures. Without these, a click on the client's own window goes
  // straight to the application and lwm never hears about it.
  // Call this once the window is framed, and again after anything which
  // releases the client window's grabs (click-to-focus does).
  void GrabSuperButtons();

  // Lower this window in the window stack.
  void Lower();

  // Raise this window in the window stack, plus any other windows which are
  // 'transient' for it. Transient windows are things like dialogs which open
  // over the top of their corresponding client window.
  void Raise();

  // Tells the client to kill its window, in response to a user clicking on the
  // close button.
  void Close();

  // Destroys this client, removing it from the LScr, in response to a
  // notification that the client's window has been destroyed.
  void Remove();

  // Draws the contents of the furniture window.
  void DrawBorder();

  bool HasStruts() const {
    return strut.top || strut.bottom || strut.left || strut.right;
  }

  // Rect defining the bounds of the window, either including LWM's window
  // furniture (WithBorder) or not (NoBorder).
  // ContentRectRelative returns the content rect relative to the frame.
  Rect FrameRect() const;
  Rect ContentRect() const { return content_rect_; }
  Rect ContentRectRelative() const;

  // Convert between frame and content rectangles.
  static Rect ContentFromFrameRect(const Rect& r);
  static Rect FrameFromContentRect(const Rect& r);

  // Returns a string of the form "123 x 460" describing the size of the window
  // that it is appropriate to display. This takes account of the size
  // increment, base size etc.
  std::string SizeString() const;

  // Returns a new Rect based on the suggested resize, but which honours the
  // client's limits on its min and max size, and size change increment.
  Rect LimitResize(const Rect& suggested);

  // Moves the client to the given rectangle. Make sure that the size of the old
  // and new rectangle is identical, otherwise we'll crash (deliberately).
  // No visibility bounds checking is done here, so you have to be sure you're
  // not moving us miles off the screen.
  void MoveTo(const Rect& new_content_rect);

  // Resize, and possibly move, the visible rectangle to the new one. You must
  // ensure that the new rectangle is OK for the client, by calling LimitResize
  // and doing whatever other checks are necessary.
  void MoveResizeTo(const Rect& new_content_rect);

  // Only call this once per client.
  void FurnishAt(Rect rect);

 private:
  Rect content_rect_;

  // pre_full_screen_content_rect_ stores the original size of the client window
  // when it enters full screen state, so it can be correctly brought out of
  // full screen state again.
  Rect pre_full_screen_content_rect_ = {};

  // Likewise for maximisation, which differs in that it has two axes: a window
  // maximised only vertically keeps its x from here and takes its y from the
  // screen. Only meaningful while IsMaximized().
  Rect pre_maximize_content_rect_ = {};

  // Where the content rect was when the user last turned the furniture off,
  // and the rect it got in exchange. Both empty while the furniture is on.
  //
  // Turning it back on could just do the arithmetic in reverse, and nearly
  // always does. The exception is a client with size increments: it rounds
  // down to a whole cell every time it's asked to fit a new space, so an
  // xterm toggled back and forth would shed a row and a column on every
  // trip. undecorated_content_rect_ is how SetFurniture knows nothing has
  // moved the window since, and so that going straight back is safe.
  Rect pre_undecorated_content_rect_ = {};
  Rect undecorated_content_rect_ = {};

  // Where the window was before the Windows-key double click grew it. Empty
  // if it hasn't been grown, or if we've already put it back: Unexpand()
  // spends the record rather than keeping it, so a window which has been
  // restored has nothing left to restore to.
  Rect pre_expand_content_rect_ = {};

  // The rect this window should occupy, given `restored` as its un-maximised
  // geometry and wstate's two maximisation flags. Returns `restored` itself if
  // neither is set.
  Rect MaximizedRect(const Rect& restored) const;

  // Nudges a content rect back onto the monitors that exist now. The
  // geometries we restore to were recorded some time ago, and the xrandr
  // layout can have changed since: don't put a window back on a monitor
  // that's been unplugged.
  Rect MakeContentRectVisible(const Rect& content) const;

  // The two halves of SetFurniture. Both expect content_rect_ and furniture_
  // to have been set already, so that FrameRect() and ContentRectRelative()
  // describe where the two windows are going.
  void ShowFurniture();
  void HideFurniture();

  // False once the user has turned the furniture off with the Super+Control
  // click. Only meaningful while framed: an unframed client has nowhere to
  // put furniture in the first place.
  bool furniture_ = true;

  const DimensionLimiter x_limiter_;
  const DimensionLimiter y_limiter_;

  // We store the original border width of the child window when we reparent it,
  // so we can restore it to its original size when we unparent it on exit.
  // This value should not be used for anything during normal LWM running.
  int original_border_width_ = 0;
  int state_ = 0;  // Window state. See ICCCM and <X11/Xutil.h>

  // Unmaps of the client window which lwm asked for and hasn't yet seen the
  // UnmapNotify for. See ExpectUnmap().
  int expected_unmaps_ = 0;

 public:
  bool hidden = false;  // true if this client is hidden.
  int proto = 0;

  bool accepts_focus = true;  // Does this window want keyboard events?

  // indicates which cursor is being used for parent window
  Edge cursor = ENone;

  EWMHWindowType wtype = WTypeNone;
  EWMHWindowState wstate = {};
  EWMHStrut strut = {};  // reserved areas

  // SetIcon sets the window's title bar icon. If called with null, it will do
  // nothing (and leave any previously-set icon in place).
  void SetIcon(xlib::ImageIcon* icon);
  xlib::ImageIcon* Icon() { return icon_; }

 private:
  Rect EdgeBounds(Edge e) const;

  // name_ is the frame title as specified by the client.
  std::string name_;
  // If the user has set a name themselves, it is stored in visible_name_.
  // If this is set, it is used in preference to 'name_'.
  // The main purpose for this is to allow the user to stop browser window
  // titles from flip-flopping between different strings (which, for example,
  // Google's "Hangouts Chat" tends to do at a rate of
  std::string visible_name_;
  xlib::ImageIcon* icon_ = nullptr;

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
};

// WinID is only used to pretty-print window IDs in hex.
struct WinID {
  explicit WinID(Window w) : w(w) {}
  Window w;
};

// AtomName is only used to pretty-print atoms.
struct AtomName {
  explicit AtomName(Atom a) : a(a) {}
  Atom a;
};

std::ostream& operator<<(std::ostream& os, const Client& c);
std::ostream& operator<<(std::ostream& os, const WinID& w);
std::ostream& operator<<(std::ostream& os, const AtomName& an);

extern void Client_SizeFeedback();
extern void size_expose();
extern void Client_FreeAll();
extern void Client_ResetAllCursors();
extern int titleBarHeight();

#endif  // LWM_CLIENT_H_included
