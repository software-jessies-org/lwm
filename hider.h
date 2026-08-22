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

  void OpenMenu(const xcb_button_press_event_t* ev);
  void Paint();
  void MouseMotion(const xcb_motion_notify_event_t* ev);
  void MouseRelease(const xcb_button_release_event_t* ev);

 private:
  int itemAt(int x, int y) const;
  void drawHighlight(int itemIndex);
  void showHighlightBox(int itemIndex);
  void hideHighlightBox();

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

  Window highlightL = 0;
  Window highlightR = 0;
  Window highlightT = 0;
  Window highlightB = 0;
};

#endif  // LWM_HIDER_H_included
