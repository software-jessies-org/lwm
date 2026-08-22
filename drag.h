#ifndef LWM_DRAG_H_included
#define LWM_DRAG_H_included

#include "disp.h"

// Picks the DragHandler appropriate to a ButtonPress event: on the root
// window, on window furniture (title bar / edge / close icon), or on a
// client's frame - or nullptr if this press doesn't start a drag. The
// concrete DragHandler subclasses (moving, resizing, closing/hiding/
// lowering by click, the unhide menu, running a configured shell command)
// are implementation details of this factory and not exposed here.
DragHandler* getDragHandlerForEvent(const xcb_button_press_event_t* ev);

#endif  // LWM_DRAG_H_included
