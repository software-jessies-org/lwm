#ifndef XDEBUGPRINT_H_included
#define XDEBUGPRINT_H_included

#include <iostream>

#include "ewmh.h"
#include "xlib.h"

// Debug-print helpers for raw X11 event structs, used by LOGD()/LOGI() call
// sites in disp.cc. Kept separate from the event handlers themselves since
// they're pure formatting, not dispatch logic.
std::ostream& operator<<(std::ostream& os, const xcb_configure_request_event_t& e);
std::ostream& operator<<(std::ostream& os, const xcb_configure_notify_event_t& e);
std::ostream& operator<<(std::ostream& os, const xcb_focus_in_event_t& e);

// Prints only the fields of an EWMHWindowState that changed between o (old)
// and n (new). Used for a single LOGD line in EvPropertyNotify.
struct diff {
  EWMHWindowState o;
  EWMHWindowState n;
};
std::ostream& operator<<(std::ostream& os, const diff& d);

#endif
