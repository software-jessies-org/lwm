#ifndef LWM_DISP_H_included
#define LWM_DISP_H_included

#include "xlib.h"

#define HIDE_BUTTON Button3
#define MOVE_BUTTON Button2
#define RESHAPE_BUTTON Button1

// MOVING_BUTTON_MASK describes the bits which are set in the mouse statis mask
// value while either of the mouse buttons we can use for dragging/reshaping
// is down.
#define MOVING_BUTTON_MASK (Button1Mask | Button2Mask)

#define EDGE_RESIST 32

/**
 * EWMH direction for _NET_WM_MOVERESIZE
 */
enum EWMHDirection {
  DSizeTopLeft,
  DSizeTop,
  DSizeTopRight,
  DSizeRight,
  DSizeBottomRight,
  DSizeBottom,
  DSizeBottomLeft,
  DSizeLeft,
  DMove,
  DSizeKeyboard,
  DMoveKeyboard
};

extern void DispatchXEvent(XEvent*);

class DragHandler {
 public:
  DragHandler() = default;
  virtual ~DragHandler() = default;

  virtual void Start(XEvent* ev) = 0;
  // Return false to cancel the action immediately.
  virtual bool Move(XEvent* ev) = 0;
  virtual void End(XEvent* ev) = 0;
};

#endif  // LWM_DISP_H_included
