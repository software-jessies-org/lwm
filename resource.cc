/*
 * lwm, a window manager for X11
 * Copyright (C) 1997-2016 Elliott Hughes, James Carter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "resource.h"
#include "screen.h"
#include "xlib.h"

void Resources::Init() {
  I = new Resources();
}

Resources* Resources::I;

Resources::Resources() {
  strings_.resize(S_END);
  ints_.resize(I_END);

  // xcb-xrm reads the RESOURCE_MANAGER property off the root window itself,
  // so there's no separate fetch-the-string step. A null database just means
  // no resources are set, which every Set() below copes with.
  xcb_xrm_database_t* db =
      conn ? xcb_xrm_database_from_default(conn) : nullptr;
  // Font used in title bars, and indeed everywhere we have fonts.
  Set(TITLE_FONT, db, "titleFont", "roboto-16");
  // Command to execute when button 1 (left) is clicked on root window.
  Set(BUTTON1_COMMAND, db, "button1", "");
  // Command to execute when button 2 (middle) is clicked on root window.
  Set(BUTTON2_COMMAND, db, "button2", "xterm");
  // Command to execute when button 1 (left) is clicked on title bar with Alt
  // held.
  Set(ALT_BUTTON1_TITLE_COMMAND, db, "altButton1Title", "force_title.sh");
  // Command to execute when button 2 (middle) is clicked on title bar with Alt
  // held.
  Set(ALT_BUTTON2_TITLE_COMMAND, db, "altButton2Title", "");
  // Background colour for title bar of the active window.
  Set(TITLE_BG_COLOUR, db, "titleBGColour", "#A0522D");
  // Border background colour of the active window.
  Set(BORDER_COLOUR, db, "borderColour", "#B87058");
  // Border and title background colour of inactive windows.
  Set(INACTIVE_BORDER_COLOUR, db, "inactiveBorderColour", "#785840");
  // Colour of the window highlight box displayed when the popup (unhide)
  // menu is open and the pointer is hovering over an entry in that menu, and
  // which shows the display bounds of the corresponding window.
  Set(WINDOW_HIGHLIGHT_COLOUR, db, "windowHighlightColour", "red");
  // Colour of the title bar text of the active window.
  Set(TITLE_COLOUR, db, "titleColour", "white");
  // Colour of the title bar text of inactive windows.
  Set(INACTIVE_TITLE_COLOUR, db, "inactiveTitleColour", "#afafaf");
  // Colour of the close icon (cross in top-left corner of the window frame).
  Set(CLOSE_ICON_COLOUR, db, "closeIconColour", "white");
  // Colour of the close icon in inactive windows.
  Set(INACTIVE_CLOSE_ICON_COLOUR, db, "inactiveCloseIconColour", "#afafaf");
  // Colour of text in the popup window (unhide menu and resize popup).
  Set(POPUP_TEXT_COLOUR, db, "popupTextColour", "black");
  // Background colour of the popup window (unhide menu and resize popup).
  Set(POPUP_BACKGROUND_COLOUR, db, "popupBackgroundColour", "white");
  // Click to focus enabled if this is the string "click".
  Set(FOCUS_MODE, db, "focus", "sloppy");
  // APP_ICON describes where we show the application's icon, if there is one.
  // Valid values are "none", "title" (title bars of windows), "menu" (the
  // unhide menu) or "both" (both title bars and unhide menu).
  Set(APP_ICON, db, "appIcon", "both");

  // The width of the border LWM adds to each window to allow resizing.
  Set(BORDER_WIDTH, db, "border", 6);
  // How many of the top pixels of the title bar will be treated as a resize
  // widget, as opposed to moving the window. If you set this to zero, the title
  // bar cannot be used to resize the window up and down (although the top-left
  // and top-right corners will work).
  Set(TOP_BORDER_WIDTH, db, "topBorder", 4);

  // The number of milliseconds between consecutive attempts to give focus to
  // a window because the mouse has entered it. This is a hack to avoid a race
  // condition whereby fast changes of window (moving the pointer from window
  // A to window B via a brief transition across window C) will end up giving
  // C focus, whereas it should be B.
  // If this value is set too low, you may get race conditions; set it too high
  // and you may get annoyingly long delays between when you expect to see
  // focus change, and when it does.
  Set(FOCUS_DELAY_MILLIS, db, "focusDelayMillis", 50);

  if (db) {
    xcb_xrm_database_free(db);
  }
}

const std::string& Resources::Get(SR sr) {
  if (sr < S_BEGIN || sr >= S_END) {
    return strings_[S_BEGIN];  // Will be empty string, because we never init
                               // it.
  }
  if (strings_[sr] == "") {
    fprintf(stderr, "WARNING! No string for resource with ID %d\n", sr);
  }
  return strings_[sr];
}

unsigned long Resources::GetColour(SR sr) {
  return xlib::ColourByName(Get(sr));
}

// Returns a short comprising two copies of the lowest byte in c.
// This converts an 8-bit r, g or b component into the 16-bit value X11 uses
// for colour components.
unsigned short extend(unsigned long c) {
  unsigned short result = c & 0xff;
  return result | (result << 8);
}

Resources::RGB Resources::GetRGB(SR sr) {
  const unsigned long rgb = GetColour(sr);
  return RGB{extend(rgb >> 16), extend(rgb >> 8), extend(rgb)};
}

// Retrieve an int resource.
int Resources::GetInt(IR ir) {
  if (ir < I_BEGIN || ir >= I_END) {
    return 0;
  }
  return ints_[ir];
}

bool tryGet(xcb_xrm_database_t* db,
            const std::string& name,
            std::string* tgt) {
  if (!db) {
    return false;
  }
  const std::string fullName = std::string("lwm.") + name;
  char* value = nullptr;
  // The class argument must be null: xcb-xrm fails the whole lookup unless the
  // class string has exactly as many dot-separated components as the name, so
  // anything else here silently turns every resource into its default. lwm has
  // no class hierarchy to supply - the classes this used to pass were single
  // words like "Font" and "String", which Xlib's XrmGetResource ignored for
  // the same reason. Matching is by name alone, as it always effectively was.
  if (xcb_xrm_resource_get_string(db, fullName.c_str(), nullptr, &value) < 0) {
    return false;
  }
  if (!value) {
    return false;
  }
  *tgt = value;
  free(value);
  return true;
}

void Resources::Set(SR res,
                    xcb_xrm_database_t* db,
                    const std::string& name,
                    const std::string& dflt) {
  if (!tryGet(db, name, &(strings_[res]))) {
    strings_[res] = dflt;
  }
}

void Resources::Set(IR res,
                    xcb_xrm_database_t* db,
                    const std::string& name,
                    int dflt) {
  ints_[res] = dflt;
  std::string strVal;
  if (!tryGet(db, name, &strVal)) {
    return;
  }
  // strtol reports failure by leaving end where it started; errno is no use
  // here, because it's only ever set, never cleared, by earlier calls.
  const char* start = strVal.c_str();
  char* end = nullptr;
  const long val = strtol(start, &end, 0);
  if (end != start) {
    ints_[res] = (int)val;
  }
}

// Border width is used a lot, so let's make it easily accessible.
int borderWidth() {
  return Resources::I->GetInt(Resources::BORDER_WIDTH);
}

int topBorderWidth() {
  return Resources::I->GetInt(Resources::TOP_BORDER_WIDTH);
}
