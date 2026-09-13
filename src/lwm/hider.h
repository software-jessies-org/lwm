#ifndef LWM_HIDER_H_included
#define LWM_HIDER_H_included

#include <list>
#include <string>
#include <vector>

#include "xlib.h"

class Client;

// Also used by xlib.cc's targetImageIconSize(), to size icons for both the
// title bar and the unhide menu consistently.
extern int menuItemHeight();

// Hider implements all the logic to do with hiding and unhiding windows, and
// providing the unhide menu.
class Hider {
 public:
  Hider() = default;

  void Hide(Client* c);
  void Unhide(Client* c);
  
  void Paint();
  
  // Mouse-driven mode.
  void OpenMenu(const xcb_button_press_event_t* ev);
  void MouseMotion(const xcb_motion_notify_event_t* ev);
  void MouseRelease(const xcb_button_release_event_t* ev);
  
  // Keyboard-driven mode.
  void OpenMenuForKeyboard();
  bool KeyboardMenuIsOpen() const { return keyboard_menu_open_; }
  void KeyboardMenuMove(int delta);
  void KeyboardMenuSelect();
  void KeyboardMenuClose(bool warp_back);

 private:
  int itemAt(int x, int y) const;
  void drawHighlight(int itemIndex);
  void showHighlightBox(int itemIndex);
  void hideHighlightBox();
  void buildMenuContent();
  void setCurrentItem(int itemIndex);

  // The middle of the given menu item, in root coordinates.
  Point itemMiddle(int itemIndex) const;

  struct Item {
    Item(Window w, bool hidden) : w(w), hidden(hidden) {}

    Window w;
    std::string name;
    bool hidden;
  };

  // hidden_ is updated any time a window is hidden or unhidden.
  std::list<Window> hidden_;

  // The following fields are changed when the menu is opened, and then used
  // to display the menu, handle mouse events etc. It is not changed by windows
  // opening and closing while the hide menu is open.
  int x_min_ = 0;
  int y_min_ = 0;
  int width_ = 0;
  int height_ = 0;
  int current_item_ = 0;  // Index of currently-selected item.
  std::vector<Item> open_content_;

  // Set while the menu is open and under keyboard control.
  bool keyboard_menu_open_ = false;

  // Where the pointer was when the keyboard menu opened. Used to warp it back
  // again if the menu closes due to a keypress (but not if the user takes over
  // with the mouse).
  Point pointer_return_{0, 0};

  Window highlightL = 0;
  Window highlightR = 0;
  Window highlightT = 0;
  Window highlightB = 0;
};

#endif  // LWM_HIDER_H_included
