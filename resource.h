#ifndef LWM_RESOURCE_H_included
#define LWM_RESOURCE_H_included

#include <string.h>

#include <string>
#include <vector>

// Not "xlib.h": xfont.cc reads resources and is one of the two files that
// includes Xlib, which can't coexist with xlib.h's typedefs.
#include <xcb/xcb_xrm.h>

class Resources {
 public:
  static Resources* I;

  // Init must be called once, at program start.
  static void Init();

  // The types of string resource on offer.
  enum SR {
    S_BEGIN,  // Don't use this.
    TITLE_FONT,
    BUTTON1_COMMAND,
    BUTTON2_COMMAND,
    ALT_BUTTON1_TITLE_COMMAND,
    ALT_BUTTON2_TITLE_COMMAND,
    TITLE_BG_COLOUR,
    BORDER_COLOUR,
    INACTIVE_BORDER_COLOUR,
    WINDOW_HIGHLIGHT_COLOUR,
    TITLE_COLOUR,
    INACTIVE_TITLE_COLOUR,
    CLOSE_ICON_COLOUR,
    INACTIVE_CLOSE_ICON_COLOUR,
    POPUP_TEXT_COLOUR,
    POPUP_BACKGROUND_COLOUR,
    FOCUS_MODE,
    APP_ICON,
    S_END,  // This must be the last.
  };

  // The types of int resource on offer.
  enum IR {
    I_BEGIN,  // Don't use this.
    BORDER_WIDTH,
    TOP_BORDER_WIDTH,
    FOCUS_DELAY_MILLIS,
    I_END,  // This must be the last.
  };

  // Retrieve a string resource.
  const std::string& Get(SR r);

  // Retrieve a string resource as a colour.
  unsigned long GetColour(SR r);

  // A colour split into 16-bit-per-channel components, as X11 wants them.
  // Used for text rendering, which needs the components rather than a pixel
  // value. Deliberately not an XRenderColor: resource.h must not name Xlib
  // types (see docs/xcb-migration-plan.md, phase 0 step 2); xfont.cc converts.
  struct RGB {
    unsigned short r;
    unsigned short g;
    unsigned short b;
  };

  // Retrieve a string resource as separate colour components.
  RGB GetRGB(SR r);

  // Retrieve an int resource.
  int GetInt(IR r);

  // Retrieve the 'click to focus' resource (as a bool).
  bool ClickToFocus() {
    std::string fm = Get(FOCUS_MODE);
    // std::string== doesn't seem to ever return true; using old-fashioned
    // strcmp instead.
    return !strcmp(fm.c_str(), "click");
  }

  // Interpret the APP_ICON resource for the cases in which we need it.
  bool ProcessAppIcons() {
    std::string ai = Get(APP_ICON);
    return strcmp(ai.c_str(), "none");
  }
  bool AppIconInWindowTitle() {
    std::string ai = Get(APP_ICON);
    return !strcmp(ai.c_str(), "both") || !strcmp(ai.c_str(), "title");
  }
  bool AppIconInUnhideMenu() {
    std::string ai = Get(APP_ICON);
    return !strcmp(ai.c_str(), "both") || !strcmp(ai.c_str(), "menu");
  }

 private:
  Resources();
  void Set(SR res,
           xcb_xrm_database_t* db,
           const std::string& name,
           const std::string& dflt);
  void Set(IR res, xcb_xrm_database_t* db, const std::string& name, int dflt);

  std::vector<std::string> strings_;
  std::vector<int> ints_;
};

// Handy accessors which parse resources if necessary, and return the relevant
// bit of config info.
int borderWidth();
int topBorderWidth();

#endif  // LWM_RESOURCE_H_included
