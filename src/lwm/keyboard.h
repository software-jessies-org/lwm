#ifndef LWM_KEYBOARD_H_included
#define LWM_KEYBOARD_H_included

#include "xlib.h"

// Keyboard navigation: holding the Windows key and pressing an arrow moves
// the input focus to the next window that way, raises it, and puts the
// pointer on it, in the middle of the largest part of it which is actually
// visible. Adding Shift moves the focused window itself instead, to the edge
// of its monitor and then on to the next monitor.
//
// Super+Tab is the third gesture: it opens the unhide menu on the monitor the
// pointer is on, and hands the keyboard to it. While that menu is up lwm holds
// an active keyboard grab, so every press arrives here; the ones the menu uses
// (the arrows, Return, space and Escape) are passed to Hider, and the rest are
// swallowed, because with the keyboard grabbed there is nobody else they could
// sensibly go to.
//
// Both choices are pure geometry, and live in navigate.h. This file is the
// part that has to talk to X: asking for the keys, and turning a KeyPress
// into a focus change or a window move. disp.cc dispatches to it, in the same
// way it dispatches a ButtonPress to drag.cc's handler factory.

// Asks the server for Super+arrow, Super+Shift+arrow and Super+Tab on the root
// window, so lwm sees those presses whoever they were typed at. Safe to call
// repeatedly: each call releases the previous grabs first, which is what
// makes it the right response to a MappingNotify (the keycode carrying an
// arrow can move when the user switches keyboard layout).
//
// Grabbing on the root rather than per-client is deliberate. A key grab is a
// property of the window it's placed on, and lwm wants these keys wherever
// the pointer is and whatever holds focus - including over the desktop, where
// there is no client to have grabbed them on.
void GrabNavigationKeys();

// Acts on a Super+arrow, Super+Shift+arrow or Super+Tab press, or on a press
// belonging to the unhide menu while that menu has the keyboard. Returns false
// if the press wasn't one of ours, in which case nothing was done with it.
//
// A press which moves the focus also raises the window it moves to, and
// remembers where in the stacking order that window came from. The next such
// press puts it back there before raising its own window, so that flipping
// between two windows across a third leaves the third one where it was
// instead of stacking it on top of them both. keyboard.cc has the details.
bool HandleKeyPress(xcb_key_press_event_t* ev);

// Forgets that note of where the last window the arrow keys raised came from,
// leaving the stacking order alone. Only wmtest::World calls this, for the
// same reason it calls xlib::ForgetLWMWindows(): lwm's own moment for
// forgetting is process exit, but each test starts a fresh server whose
// window ids begin again, so a note left over from the last test would name a
// window this one has since handed to a different client.
void ForgetNavigationState();

#endif  // LWM_KEYBOARD_H_included
