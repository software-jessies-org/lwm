#ifndef LWM_DRAG_H_included
#define LWM_DRAG_H_included

#include "disp.h"
#include "edge.h"

class Client;

// Picks the DragHandler appropriate to a ButtonPress event: on the root
// window, on window furniture (title bar / edge / close icon), or on a
// client's frame - or nullptr if this press doesn't start a drag. The
// concrete DragHandler subclasses (moving, resizing, closing/hiding/
// lowering by click, the unhide menu, running a configured shell command)
// are implementation details of this factory and not exposed here.
DragHandler* getDragHandlerForEvent(const xcb_button_press_event_t* ev);

// Picks the DragHandler for a _NET_WM_MOVERESIZE request: a client asking lwm
// to take over a move or resize which the user has started on the client's own
// widgets. This is how a window that draws its own title bar and resize grip -
// Steam's launcher, GTK's client-side decorations, Chrome - moves and resizes
// itself, since lwm gives such windows no furniture to drag.
//
// edge is ENone for a move, or the edge/corner being dragged. button is the
// mouse button the user is holding, from the client message. Returns nullptr
// if the request can't be honoured, which for now means a button lwm can't
// track (the keyboard-driven directions have no button at all, and a drag lwm
// can't see the end of is worse than no drag).
DragHandler* getMoveResizeHandler(Client* c, Edge edge, int button);

#endif  // LWM_DRAG_H_included
