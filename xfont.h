#ifndef LWM_XFONT_H_included
#define LWM_XFONT_H_included

#include <string>

#include <xcb/xcb.h>

// xfont is lwm's entire text-drawing surface. It exists to keep Xft - and
// therefore Xlib - out of the rest of the tree.
//
// Note what this header does *not* include: xlib.h. Xlib and XCB both define
// `Window`, to different widths, so a translation unit may include one set of
// headers or the other but never both. xfont.cc picks Xlib, so its interface
// is spelled in XCB's own types (xcb_window_t is what xlib.h calls Window,
// so call sites don't notice).
//
// Xft is the one piece of lwm with no XCB equivalent, and so the only reason
// libX11 is still linked. Replacing this implementation with Pango +
// cairo-xcb (docs/xcb-migration-plan.md, phase 4) would drop libX11 entirely
// and requires no changes above this interface.
namespace xfont {

// Which of the three colours a piece of text should be drawn in. The colours
// themselves come from Resources, and are allocated once at Init() time.
enum class Colour {
  ACTIVE_TITLE,
  INACTIVE_TITLE,
  POPUP,
};

// Init opens the configured title font and allocates the three text colours.
// Must be called once, after Resources::Init() and after the connection to
// the X server is open. Panics if no usable font can be found.
extern void Init();

// The height of a line of text, in pixels. This is what the title bar height
// is derived from, so it is effectively a layout constant.
extern int TextHeight();

// The distance from the top of a line of text to its baseline. Call sites
// that position text need this because DrawString takes a baseline y.
extern int TextAscent();

// The width, in pixels, of s rendered in the LWM font.
extern int TextWidth(const std::string& s);

// Draws s onto w, with its left edge at x and its baseline at y.
extern void DrawString(xcb_window_t w,
                       int x,
                       int y,
                       const std::string& s,
                       Colour c);

}  // namespace xfont

#endif  // LWM_XFONT_H_included
