#ifndef LWM_SCREEN_H_included
#define LWM_SCREEN_H_included

#include <map>
#include <vector>

#include "client.h"
#include "focus.h"
#include "geometry.h"
#include "hider.h"
#include "placement.h"
#include "strut.h"
#include "xlib.h"

class CursorMap;

// Screen information.
class LScr {
 public:
  LScr();

  // Init must be called once, immediately after the global LScr::I instance
  // has been assigned to this instance.
  void Init();

  Window Root() const { return root_; }
  Window Popup() const { return popup_; }
  Window Menu() const { return menu_; }

  int Width() const { return width_; }
  int Height() const { return height_; }
  void ChangeScreenDimensions(int nScrWidth, int nScrHeight);

  unsigned long InactiveBorder() const { return inactive_border_; }
  unsigned long ActiveBorder() const { return active_border_; }
  CursorMap* Cursors() const { return cursor_map_; }

  GC GetCloseIconGC(bool active) { return active ? gc_ : inactive_gc_; }
  GC GetMenuGC() { return menu_gc_; }
  GC GetTitleGC() { return title_gc_; }

  // Sets the screen areas which are visible.
  // For one-monitor systems, this will be a single rectangle.
  // For multi-screen systems this will consist of one rect for each screen.
  // These may form a larger rectangle (eg 2 identical-sized monitors), or
  // there may be several unevenly-sized screens, and at arbitrary relative
  // positions. Essentially, anything supported by xrandr.
  // This includes areas which overlap.
  // Calling this function will cause all client windows to be resized and
  // repositioned if necessary to ensure they're still accessible.
  void SetVisibleAreas(std::vector<Rect> visible_areas);

  // Returns the rectangle describing the 'main' screen area. This is chosen
  // essentially by finding the largest monitor, and if there are several with
  // the same size, tie-breaking according to which has the lower Y, followed
  // by which has the lower X.
  // If withStruts is true, only the part of the visible area not used by
  // strutting furniture will be returned.
  Rect GetPrimaryVisibleArea(bool withStruts) const;

  // Returns all the visible areas. The areas returned are returned in no
  // specific order, and will abut *or overlap*.
  std::vector<Rect> VisibleAreas(bool withStruts) const;

  // Expose the utf8 string atom. This is used by ewmh.cc. Not sure why it can't
  // go in the main enumerated set of atoms, and indeed this whole atom support
  // looks like it needs refactoring. For now, though, ugly hack here:
  Atom GetUTF8StringAtom() const { return utf8_string_atom_; }

  const EWMHStrut& Strut() const { return strut_; }
  // ChangeStrut returns true if the new struts are different from the old.
  bool ChangeStrut(const EWMHStrut& strut);

  // Returns the position at which to place the next auto-placed (i.e. no
  // position hint of its own) window of the given size, cascading down and
  // to the right within the primary visible area on successive calls.
  Point NextAutoPosition(const Area& client_area) {
    return auto_placer_.NextPosition(client_area, GetPrimaryVisibleArea(true));
  }

  // GetClient returns the Client which owns the given window (including if w
  // is a sub-window of the main client window). Returns nullptr if there is
  // no client allocated for this window.
  // The scan_parents=true argument is usually desirable, as we want to know
  // the client corresponding to sub-windows too. However, we really don't want
  // to do a search for the client during a DestroyNotify, as all the windows
  // are gone.
  Client* GetClient(Window w, bool scan_parents = true) const;

  // GetOrAddClient either returns the existing client, or creates a new one
  // and generates relevant window furniture. This may return nullptr if the
  // window should not be owned.
  Client* GetOrAddClient(Window w, bool is_startup_scan);

  void Furnish(Client* c);

  // The reverse: destroys the client's frame window and forgets it, leaving
  // the client parented to the root. The caller is expected to have already
  // reparented the client window out of the frame - this only disposes of the
  // frame itself. See Client::SetFramed.
  void Unfurnish(Client* c);

  void Remove(Client* client);

  Hider* GetHider() { return &hider_; }
  Focuser* GetFocuser() { return &focuser_; }

  // Clients() returns the map of all clients, for iteration.
  const std::map<Window, Client*>& Clients() const { return clients_; }

  // This is used as a static pointer to the global LScr instance, initialised
  // on start-up in lwm.cc.
  static LScr* I;

  static constexpr int kOnlyScreenIndex = 0;

 private:
  void InitEWMH();
  void ScanWindowTree();
  // AddClient takes the window's already-fetched attributes and hints,
  // because the start-up scan queries every window on screen in one batch
  // rather than one at a time.
  Client* AddClient(Window w,
                    bool is_startup_scan,
                    const xlib::WindowInfo& info);
  unsigned long black() const { return xlib::Black(); }
  unsigned long white() const { return xlib::White(); }

  Window root_ = 0;
  int width_ = 0;
  int height_ = 0;
  std::vector<Rect> visible_areas_;
  CursorMap* cursor_map_;

  Hider hider_;
  Focuser focuser_;

  // The clients_ map is keyed by the top-level client Window ID. The values
  // are owned.
  std::map<Window, Client*> clients_;

  // The parents_ map is keyed by the LWM furniture windows when they are
  // created. It does not own the values (they're just pointers to the same
  // clients as in the clients_ map).
  std::map<Window, Client*> parents_;

  Atom utf8_string_atom_;

  Window popup_ = 0;
  Window menu_ = 0;
  Window ewmh_compat_ = 0;

  EWMHStrut strut_;  // reserved areas
  AutoPlacer auto_placer_;

  GC gc_;
  GC inactive_gc_;
  GC menu_gc_;
  GC title_gc_;

  // Extra colours.
  unsigned long inactive_border_ = 0;
  unsigned long active_border_ = 0;
};

#endif  // LWM_SCREEN_H_included
