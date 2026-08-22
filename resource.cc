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

void Resources::Init() {
  I = new Resources();
}

Resources* Resources::I;

Resources::Resources() {
  strings_.resize(S_END);
  ints_.resize(I_END);

  XrmDatabase db = nullptr;
  char* resource_manager = XResourceManagerString(dpy);
  if (resource_manager) {
    XrmInitialize();
    db = XrmGetStringDatabase(resource_manager);
  }
  // Font used in title bars, and indeed everywhere we have fonts.
  Set(TITLE_FONT, db, "titleFont", "Font", "roboto-16");
  // Command to execute when button 1 (left) is clicked on root window.
  Set(BUTTON1_COMMAND, db, "button1", "Command", "");
  // Command to execute when button 2 (middle) is clicked on root window.
  Set(BUTTON2_COMMAND, db, "button2", "Command", "xterm");
  // Command to execute when button 1 (left) is clicked on title bar with Alt
  // held.
  Set(ALT_BUTTON1_TITLE_COMMAND, db, "altButton1Title", "Command",
      "force_title.sh");
  // Command to execute when button 2 (middle) is clicked on title bar with Alt
  // held.
  Set(ALT_BUTTON2_TITLE_COMMAND, db, "altButton2Title", "Command", "");
  // Background colour for title bar of the active window.
  Set(TITLE_BG_COLOUR, db, "titleBGColour", "String", "#A0522D");
  // Border background colour of the active window.
  Set(BORDER_COLOUR, db, "borderColour", "String", "#B87058");
  // Border and title background colour of inactive windows.
  Set(INACTIVE_BORDER_COLOUR, db, "inactiveBorderColour", "String", "#785840");
  // Colour of the window highlight box displayed when the popup (unhide)
  // menu is open and the pointer is hovering over an entry in that menu, and
  // which shows the display bounds of the corresponding window.
  Set(WINDOW_HIGHLIGHT_COLOUR, db, "windowHighlightColour", "String", "red");
  // Colour of the title bar text of the active window.
  Set(TITLE_COLOUR, db, "titleColour", "String", "white");
  // Colour of the title bar text of inactive windows.
  Set(INACTIVE_TITLE_COLOUR, db, "inactiveTitleColour", "String", "#afafaf");
  // Colour of the close icon (cross in top-left corner of the window frame).
  Set(CLOSE_ICON_COLOUR, db, "closeIconColour", "String", "white");
  // Colour of the close icon in inactive windows.
  Set(INACTIVE_CLOSE_ICON_COLOUR, db, "inactiveCloseIconColour", "String",
      "#afafaf");
  // Colour of text in the popup window (unhide menu and resize popup).
  Set(POPUP_TEXT_COLOUR, db, "popupTextColour", "String", "black");
  // Background colour of the popup window (unhide menu and resize popup).
  Set(POPUP_BACKGROUND_COLOUR, db, "popupBackgroundColour", "String", "white");
  // Click to focus enabled if this is the string "click".
  Set(FOCUS_MODE, db, "focus", "String", "sloppy");
  // APP_ICON describes where we show the application's icon, if there is one.
  // Valid values are "none", "title" (title bars of windows), "menu" (the
  // unhide menu) or "both" (both title bars and unhide menu).
  Set(APP_ICON, db, "appIcon", "String", "both");

  // The width of the border LWM adds to each window to allow resizing.
  Set(BORDER_WIDTH, db, "border", "Border", 6);
  // How many of the top pixels of the title bar will be treated as a resize
  // widget, as opposed to moving the window. If you set this to zero, the title
  // bar cannot be used to resize the window up and down (although the top-left
  // and top-right corners will work).
  Set(TOP_BORDER_WIDTH, db, "topBorder", "Border", 4);

  // The number of milliseconds between consecutive attempts to give focus to
  // a window because the mouse has entered it. This is a hack to avoid a race
  // condition whereby fast changes of window (moving the pointer from window
  // A to window B via a brief transition across window C) will end up giving
  // C focus, whereas it should be B.
  // If this value is set too low, you may get race conditions; set it too high
  // and you may get annoyingly long delays between when you expect to see
  // focus change, and when it does.
  Set(FOCUS_DELAY_MILLIS, db, "focusDelayMillis", "Border", 50);
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
  const std::string name = Get(sr);
  XColor colour, exact;
  XAllocNamedColor(dpy, DefaultColormap(dpy, LScr::kOnlyScreenIndex),
                   name.c_str(), &colour, &exact);
  return colour.pixel;
}

// Returns a short comprising two copies of the lowest byte in c.
// This converts an 8-bit r, g or b component into a 16-bit value as required
// by XRenderColor.
unsigned short extend(unsigned long c) {
  unsigned short result = c & 0xff;
  return result | (result << 8);
}

XRenderColor Resources::GetXRenderColor(SR sr) {
  const unsigned long rgb = GetColour(sr);
  return XRenderColor{extend(rgb >> 16), extend(rgb >> 8), extend(rgb), 0xffff};
}

// Retrieve an int resource.
int Resources::GetInt(IR ir) {
  if (ir < I_BEGIN || ir >= I_END) {
    return 0;
  }
  return ints_[ir];
}

bool tryGet(XrmDatabase db,
            const std::string& name,
            const char* cls,
            std::string* tgt) {
  if (!db) {
    return false;
  }
  char* type;
  XrmValue value;
  const std::string fullName = std::string("lwm.") + name;
  if (XrmGetResource(db, fullName.c_str(), cls, &type, &value)) {
    if (!strcmp(type, "String")) {
      *tgt = std::string(value.addr, value.size);
      return true;
    }
  }
  return false;
}

void Resources::Set(SR res,
                    XrmDatabase db,
                    const std::string& name,
                    const char* cls,
                    const std::string& dflt) {
  if (!tryGet(db, name, cls, &(strings_[res]))) {
    strings_[res] = dflt;
  }
}

void Resources::Set(IR res,
                    XrmDatabase db,
                    const std::string& name,
                    const char* cls,
                    int dflt) {
  ints_[res] = dflt;
  std::string strVal;
  if (!tryGet(db, name, cls, &strVal)) {
    return;
  }
  if (strVal == "") {
    return;
  }
  const int val = (int)strtol(strVal.c_str(), (char**)0, 0);
  // man strtol says that on failure, zero is returned and errno is EINVAL.
  ints_[res] = (errno == EINVAL) ? dflt : val;
}

// Border width is used a lot, so let's make it easily accessible.
int borderWidth() {
  return Resources::I->GetInt(Resources::BORDER_WIDTH);
}

int topBorderWidth() {
  return Resources::I->GetInt(Resources::TOP_BORDER_WIDTH);
}
