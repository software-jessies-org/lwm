#ifndef LWM_XBRIDGE_H_included
#define LWM_XBRIDGE_H_included

#include <stdint.h>

#include <string>

#include <xcb/xcb.h>

// xbridge is the last of libX11, quarantined.
//
// lwm speaks XCB, but Xft has no XCB port and is the only way it has of
// drawing text, so libX11 has to stay in the link. The two share one socket:
// we open it with XOpenDisplay, hand the event queue to XCB with
// XSetEventQueueOwner, and pull the XCB connection out with
// XGetXCBConnection.
//
// Nothing here names an Xlib type, and this header deliberately does not
// include xlib.h. That's the rule that keeps the two libraries apart: they
// both define `Window`, to different widths, so a translation unit may
// include Xlib's headers or lwm's, never both. xbridge.cc and xfont.cc are
// the two that pick Xlib; everything else gets lwm's.
//
// This whole file goes away if Xft is ever replaced by Pango + cairo-xcb
// (docs/xcb-migration-plan.md, phase 4).
namespace xbridge {

// Reports an X error. Takes plain integers so this header need not name
// either library's error struct.
typedef void (*ErrorFunc)(uint8_t error_code,
                          uint32_t resource_id,
                          uint8_t major_code,
                          uint16_t minor_code,
                          uint32_t sequence);

// Opens the connection and returns the XCB view of it, or null on failure.
// on_error is installed as Xlib's error handler: Xlib routes errors for
// requests it made to its own handler rather than onto the XCB queue, and
// its default handler exits the process, which would let a stray Xft error
// kill the window manager.
extern xcb_connection_t* Open(ErrorFunc on_error);

extern void Close();

// Pushes Xlib's request buffer to the server. Sharing a socket is not sharing
// an output buffer: requests issued through Xlib queue in Xlib's own buffer,
// which xcb_flush() knows nothing about. Whoever flushes XCB must flush this
// too, or half of lwm's requests sit unsent.
extern void Flush();

// The Xlib Display, as a void*, for xfont.cc to cast back. The only reason
// any of this exists.
extern void* Display();

// The value of $DISPLAY to hand to child processes.
extern std::string DisplayName();

}  // namespace xbridge

#endif  // LWM_XBRIDGE_H_included
