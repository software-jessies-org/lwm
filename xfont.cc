#include "xfont.h"

// One of only two translation units allowed to include Xlib; see xfont.h.
// The build enforces it (`make check-x11-boundary`).
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

#include "error.h"
#include "log.h"
#include "resource.h"
#include "xbridge.h"

namespace xfont {
namespace {

// The Xlib display, fetched once in Init(). This is the same connection the
// rest of lwm talks XCB over.
Display* dpy;
int screen;

XftFont* g_font;
// Non-zero once InitForTest has been called, in which case g_font is null and
// these stand in for it.
int g_test_height;
int g_test_ascent;
int g_test_char_width;
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
  XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
                     DefaultColormap(dpy, screen), &xrc, into);
}

}  // namespace

void InitForTest(int height, int ascent, int char_width) {
  g_font = nullptr;
  g_test_height = height;
  g_test_ascent = ascent;
  g_test_char_width = char_width;
}

void Init() {
  dpy = static_cast<Display*>(xbridge::Display());
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
  return g_font ? g_font->height : g_test_height;
}

int TextAscent() {
  return g_font ? g_font->ascent : g_test_ascent;
}

int TextWidth(const std::string& s) {
  if (!g_font) {
    return int(s.size()) * g_test_char_width;
  }
  XGlyphInfo extents;
  XftTextExtentsUtf8(dpy, g_font, reinterpret_cast<const FcChar8*>(s.c_str()),
                     s.size(), &extents);
  return extents.xOff;
}

void DrawString(xcb_window_t w,
                int x,
                int y,
                const std::string& s,
                Colour c) {
  if (!g_font) {
    return;
  }
  XftDraw* draw = XftDrawCreate(dpy, w, DefaultVisual(dpy, screen),
                                DefaultColormap(dpy, screen));
  XftDrawStringUtf8(draw, colourFor(c), g_font, x, y,
                    reinterpret_cast<const FcChar8*>(s.c_str()), s.size());
  XftDrawDestroy(draw);
}

}  // namespace xfont
