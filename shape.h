#ifndef LWM_SHAPE_H_included
#define LWM_SHAPE_H_included

#include "xlib.h"

class Client;

extern int shapeEvent(XEvent*);
extern int serverSupportsShapes();
extern int isShaped(Window);
extern void setShape(Client*);

#endif  // LWM_SHAPE_H_included
