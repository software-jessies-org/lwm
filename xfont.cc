#include "xfont.h"

// This is the one translation unit allowed to know about Xlib and Xft; see
// the comment in xfont.h. The build enforces it (`make check-x11-boundary`).
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

#include "error.h"
#include "log.h"
#include "resource.h"

namespace xfont {
namespace {

// The Xlib display, fetched once in Init(). This is the same connection the
// rest of lwm talks XCB over; see xlib::XftDisplay().
Display* dpy;
int screen;

XftFont* g_font;
XftColor g_active_title;
XftColor g_inactive_title;
XftColor g_popup;

XftColor* colourFor(Colour c) {
  switch (c) {
    case Colour::ACTIVE_TITLE:
      return &g_active_title;
    case Colour::INACTIVE_TITLE:
      return &g_inactive_title;
    case Colour::POPUP:
      return &g_popup;
  }
  return &g_popup;  // Unreachable, but keeps the compiler happy.
}

void allocColour(Resources::SR res, XftColor* into) {
  const Resources::RGB rgb = Resources::I->GetRGB(res);
  XRenderColor xrc{rgb.r, rgb.g, rgb.b, 0xffff};
  XftColorAllocValue(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen),
                     &xrc, into);
}

}  // namespace

void Init() {
  dpy = static_cast<Display*>(xlib::XftDisplay());
  screen = DefaultScreen(dpy);

  const std::string titleFont = Resources::I->Get(Resources::TITLE_FONT);
  g_font = XftFontOpenName(dpy, screen, titleFont.c_str());
  if (g_font == nullptr) {
    LOGE() << "Couldn't find font " << titleFont << "; falling back";
    g_font = XftFontOpenName(dpy, screen, "fixed");
    if (g_font == nullptr) {
      panic("Can't find a font");
    }
  }
  allocColour(Resources::TITLE_COLOUR, &g_active_title);
  allocColour(Resources::INACTIVE_TITLE_COLOUR, &g_inactive_title);
  allocColour(Resources::POPUP_TEXT_COLOUR, &g_popup);
}

int TextHeight() {
  return g_font->height;
}

int TextAscent() {
  return g_font->ascent;
}

int TextWidth(const std::string& s) {
  XGlyphInfo extents;
  XftTextExtentsUtf8(dpy, g_font, reinterpret_cast<const FcChar8*>(s.c_str()),
                     s.size(), &extents);
  return extents.xOff;
}

void DrawString(Window w, int x, int y, const std::string& s, Colour c) {
  XftDraw* draw = XftDrawCreate(dpy, w, DefaultVisual(dpy, screen),
                                DefaultColormap(dpy, screen));
  XftDrawStringUtf8(draw, colourFor(c), g_font, x, y,
                    reinterpret_cast<const FcChar8*>(s.c_str()), s.size());
  XftDrawDestroy(draw);
}

}  // namespace xfont
