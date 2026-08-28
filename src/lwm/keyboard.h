#ifndef LWM_KEYBOARD_H_included
#define LWM_KEYBOARD_H_included

#include "xlib.h"

// Keyboard navigation: holding the Windows key and pressing an arrow moves
// the input focus to the next window that way, raises it, and puts the
// pointer on it, in the middle of the largest part of it which is actually
// visible. Adding Shift moves the focused window itself instead, to the edge
// of its monitor and then on to the next monitor.
//
// Both choices are pure geometry, and live in navigate.h. This file is the
// part that has to talk to X: asking for the keys, and turning a KeyPress
// into a focus change or a window move. disp.cc dispatches to it, in the same
// way it dispatches a ButtonPress to drag.cc's handler factory.

// Asks the server for Super+arrow and Super+Shift+arrow on the root window,
// so lwm sees those presses whoever they were typed at. Safe to call
// repeatedly: each call releases the previous grabs first, which is what
// makes it the right response to a MappingNotify (the keycode carrying an
// arrow can move when the user switches keyboard layout).
//
// Grabbing on the root rather than per-client is deliberate. A key grab is a
// property of the window it's placed on, and lwm wants these keys wherever
// the pointer is and whatever holds focus - including over the desktop, where
// there is no client to have grabbed them on.
void GrabNavigationKeys();

// Acts on a Super+arrow or Super+Shift+arrow press. Returns false if the
// press wasn't one of ours, in which case nothing was done with it.
bool HandleKeyPress(xcb_key_press_event_t* ev);

#endif  // LWM_KEYBOARD_H_included
