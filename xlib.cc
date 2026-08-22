#include "debug.h"
#include "hider.h"
#include "resource.h"
#include "screen.h"
#include "xlib.h"

#include <set>

MousePos getMousePosition() {
  Window root, child;
  MousePos res;
  memset(&res, 0, sizeof(res));
  int t1, t2;
  XQueryPointer(dpy, LScr::I->Root(), &root, &child, &res.x, &res.y, &t1, &t2,
                &res.modMask);
  return res;
}

namespace xlib {

extern int XMoveResizeWindow(Window w, const Rect& r) {
  return XMoveResizeWindow(w, r.xMin, r.yMin, r.width(), r.height());
}

int XMoveResizeWindow(Window w, int x, int y, unsigned width, unsigned height) {
  // https://tronche.com/gui/x/xlib/window/XMoveResizeWindow.html
  LOGD(w) << "XMoveResizeWindow(" << WinID(w) << ") -> " << x << "," << y << " "
          << width << "x" << height;
  int res = ::XMoveResizeWindow(dpy, w, x, y, width, height);
  // Possible errors: BadValue, BadWindow.
  return res;
}

int XMoveWindow(Window w, const Point& origin) {
  return XMoveWindow(w, origin.x, origin.y);
}

int XMoveWindow(Window w, int x, int y) {
  // https://tronche.com/gui/x/xlib/window/XMoveWindow.html
  LOGD(w) << "XMoveWindow(" << WinID(w) << ") -> " << x << "," << y;
  int res = ::XMoveWindow(dpy, w, x, y);
  // Possible errors: BadWindow.
  return res;
}

int XResizeWindow(Window w, const Area& area) {
  return XResizeWindow(w, area.width, area.height);
}

int XResizeWindow(Window w, unsigned width, unsigned height) {
  // https://tronche.com/gui/x/xlib/window/XResizeWindow.html
  LOGD(w) << "XResizeWindow(" << WinID(w) << ") -> " << width << "x" << height;
  int res = ::XResizeWindow(dpy, w, width, height);
  // Possible errors: BadValue, BadWindow.
  return res;
}

int XReparentWindow(Window w, Window new_parent, int x, int y) {
  // https://tronche.com/gui/x/xlib/window-and-session-manager/XReparentWindow.html
  LOGD(w) << "XReparentWindow(" << WinID(w)
          << ") -> new parent = " << WinID(new_parent) << " @ " << x << ","
          << y;
  int res = ::XReparentWindow(dpy, w, new_parent, x, y);
  // Possible errors: BadMatch, BadWindow.
  return res;
}

int XMapWindow(Window w) {
  // https://tronche.com/gui/x/xlib/window/XMapWindow.html
  LOGD(w) << "XMapWindow(" << WinID(w) << ")";
  int res = ::XMapWindow(dpy, w);
  // Possible errors: BadWindow.
  return res;
}

int XMapRaised(Window w) {
  // https://tronche.com/gui/x/xlib/window/XMapRaised.html
  LOGD(w) << "XMapRaised(" << WinID(w) << ")";
  int res = ::XMapRaised(dpy, w);
  // Possible errors: BadWindow.
  return res;
}

int XUnmapWindow(Window w) {
  // https://tronche.com/gui/x/xlib/window/XUnmapWindow.html
  LOGD(w) << "XUnmapWindow(" << WinID(w) << ")";
  int res = ::XUnmapWindow(dpy, w);
  // Possible errors: BadWindow.
  return res;
}

int XRaiseWindow(Window w) {
  // https://tronche.com/gui/x/xlib/window/XRaiseWindow.html
  LOGD(w) << "XRaiseWindow(" << WinID(w) << ")";
  int res = ::XRaiseWindow(dpy, w);
  // Possible errors: BadWindow.
  return res;
}

int XLowerWindow(Window w) {
  // https://tronche.com/gui/x/xlib/window/XLowerWindow.html
  LOGD(w) << "XLowerWindow(" << WinID(w) << ")";
  int res = ::XLowerWindow(dpy, w);
  // Possible errors: BadWindow.
  return res;
}

struct MaskedChanges {
  unsigned int mask;
  XWindowChanges* v;
};

std::ostream& operator<<(std::ostream& os, const MaskedChanges& mc) {
#define OUT(flag, var) \
  if (mc.mask & flag)  \
  os << " " #var "->" << mc.v->var
  OUT(CWX, x);
  OUT(CWY, y);
  OUT(CWWidth, width);
  OUT(CWHeight, height);
  OUT(CWBorderWidth, border_width);
  OUT(CWSibling, sibling);
  OUT(CWStackMode, stack_mode);
#undef OUT
  return os;
}

int XConfigureWindow(Window w, unsigned int val_mask, XWindowChanges* v) {
  // https://tronche.com/gui/x/xlib/window/XConfigureWindow.html
  LOGD(w) << "XConfigureWindow(" << WinID(w) << ")"
          << MaskedChanges{val_mask, v};
  int res = ::XConfigureWindow(dpy, w, val_mask, v);
  // Possible errors: BadMatch, BadValue, BadWindow.
  return res;
}

struct MaskedAttributes {
  unsigned int mask;
  XSetWindowAttributes* v;
};

std::ostream& operator<<(std::ostream& os, const MaskedAttributes& mc) {
#define OUT(flag, var) \
  if (mc.mask & flag)  \
  os << " " #var "->" << mc.v->var
  OUT(CWBackPixmap, background_pixmap);
  OUT(CWBackPixel, background_pixel);
  OUT(CWBorderPixmap, border_pixmap);
  OUT(CWBorderPixel, border_pixel);
  OUT(CWBitGravity, bit_gravity);
  OUT(CWWinGravity, win_gravity);
  OUT(CWBackingStore, backing_store);
  OUT(CWBackingPlanes, backing_planes);
  OUT(CWBackingPixel, backing_pixel);
  OUT(CWOverrideRedirect, override_redirect);
  OUT(CWSaveUnder, save_under);
  OUT(CWEventMask, event_mask);
  OUT(CWDontPropagate, do_not_propagate_mask);
  OUT(CWColormap, colormap);
  OUT(CWCursor, cursor);
#undef OUT
  return os;
}

int XChangeWindowAttributes(Window w,
                            unsigned int val_mask,
                            XSetWindowAttributes* v) {
  // https://tronche.com/gui/x/xlib/window/XChangeWindowAttributes.html
  LOGD(w) << "XChangeWindowAttributes(" << WinID(w) << ")"
          << MaskedAttributes{val_mask, v};
  int res = ::XChangeWindowAttributes(dpy, w, val_mask, v);
  // Possible errors: BadMatch, BadValue, BadWindow.
  return res;
}

void SendClientMessage(Window w, Atom a, long data0, long data1) {
  LOGD(w) << "SendClientMessage, atom " << a << ": " << data0 << ", " << data1;
  XEvent ev{};
  ev.xclient.type = ClientMessage;
  ev.xclient.window = w;
  ev.xclient.message_type = a;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = data0;
  ev.xclient.data.l[1] = data1;
  const long mask = (w == LScr::I->Root()) ? SubstructureRedirectMask : 0L;
  ::XSendEvent(dpy, w, false, mask, &ev);
}

extern XWindowAttributes XGetWindowAttributes(Window w) {
  XWindowAttributes res{};
  ::XGetWindowAttributes(dpy, w, &res);
  LOGD(w) << "XGetWindowAttributes: " << Rect::From(res);
  return res;
}

WindowGeometry XGetGeometry(Window w) {
  WindowGeometry res{};
  Window parent;
  int x, y;
  unsigned int width, height, border_width, bpp;
  // XGetGeometry returns a Status, which is 0 on failure.
  if (!::XGetGeometry(dpy, w, &parent, &x, &y, &width, &height, &border_width,
                      &bpp)) {
    return res;  // ok = false on creation.
  }
  res.ok = true;
  res.parent = parent;
  res.rect = Rect::FromXYWH(x, y, width, height);
  res.border_width = border_width;
  res.bpp = bpp;
  return res;
}

std::set<Window> lwm_owned_windows;

Window CreateNamedWindow(const std::string& name,
                         const Rect& rect,
                         unsigned int border_width,
                         unsigned long border_colour,
                         unsigned long background_colour) {
  // https://tronche.com/gui/x/xlib/window/XCreateWindow.html
  const Window w = XCreateSimpleWindow(
      dpy, LScr::I->Root(), rect.xMin, rect.yMin, rect.width(), rect.height(),
      border_width, border_colour, background_colour);
  lwm_owned_windows.insert(w);

  // There are two functions that could set the title of a window. We use the
  // more modern one, XSetWMName. The older one, XStoreName, returns BadRequest
  // errors, despite the fact that it works just fine.
  XTextProperty name_prop{};
  // const_cast needed because the xlib function takes a non-const pointer.
  char* c_name = const_cast<char*>(name.c_str());
  // XStringListToTextProperty returns 0 on failure.
  if (!XStringListToTextProperty(&c_name, 1, &name_prop)) {
    LOGE() << "TextProperty generation for " << name << " failed";
    return w;
  }
  // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XSetWMName.html
  XSetWMName(dpy, w, &name_prop);
  return w;
}

bool IsLWMWindow(Window w) {
  return lwm_owned_windows.count(w);
}

WindowTree WindowTree::Query(Display* dpy, Window w) {
  WindowTree res = {};
  Window* ch = nullptr;
  unsigned int num_ch = 0;
  // It doesn't matter which root window we give this call.
  XQueryTree(dpy, w, &res.root, &res.parent, &ch, &num_ch);
  XFreer ch_freer(ch);
  if (res.parent) {
    res.self = w;
  }
  res.children.reserve(num_ch);
  for (int i = 0; i < num_ch; i++) {
    res.children.push_back(ch[i]);
  }
  return res;
}

// Returns the parent window of w, or NULL if we hit the root or on error.
Window WindowTree::ParentOf(Window w) {
  WindowTree wt = WindowTree::Query(LScr::I->Dpy(), w);
  return (wt.parent == wt.root) ? 0 : wt.parent;
}

// targetImageIconSize returns the max size we want to use for window icons.
// This is determined by the minimum of the space available in the two places
// we display icons, which are the title bar of the window, and the 'unhide'
// menu.
int targetImageIconSize() {
  const int mh = menuItemHeight();
  const int th = titleBarHeight();
  return (mh < th) ? mh : th;
}

// static
void ImageIcon::ConfigureIconSizes() {
  // XSetIconSize sets the WM_ICON_SIZE property.
  // By requesting anything up to 1024 pixels on a side, we allow the app
  // to provide the largest icon size it's likely to have.
  // This means the app doesn't do any down-scaling for us. For Java apps this
  // is a good move, as while Java is perfectly capable of scaling down images
  // smoothly, it handily forgets this ability and uses the butt-ugly jagged
  // down-sampling method.
  const int min_size = targetImageIconSize();
  const int max_size = 1024;
  XIconSize sz = {min_size, min_size, max_size, max_size, 1, 1};
  XSetIconSizes(dpy, LScr::I->Root(), &sz, 1);
}

static std::map<unsigned long, ImageIcon*>* image_icon_cache;
// The refcounts are used to keep track of how many clones of the same image
// have been returned. The internally-kept ImageIcon doesn't count, only clones
// cause the refcount to be increased.
// When an image is cloned, an internal variable is set (gc_hash_), which causes
// the removeCacheRef function to be called when that ImageIcon is destroyed.
// Note (particularly for testing) that windows which specify a pixmap directly
// typically don't trigger the caching behaviour, as each window has its own
// copy of the pixmap. To definitely test the reference counting, use Chrome
// or Firefox, both of which have their icons as a bunch of pixels embedded in
// the _NET_WM_ICON property. As we calculate the hash based on the pixel data,
// this triggers reuse of scaled images.
static std::map<unsigned long, int>* image_cache_refcounts;

ImageIcon* fromCache(unsigned long hash) {
  if (!image_icon_cache) {
    return nullptr;
  }
  auto it = image_icon_cache->find(hash);
  if (it == image_icon_cache->end()) {
    return nullptr;
  }
  return it->second;
}

void removeCacheRef(unsigned long hash) {
  const int remainingRefs = --((*image_cache_refcounts)[hash]);
  if (remainingRefs == 0) {
    ImageIcon* icon = (*image_icon_cache)[hash];
    image_icon_cache->erase(hash);
    image_cache_refcounts->erase(hash);
    icon->destroyResources();
    delete icon;
  }
}

void toCache(unsigned long hash, ImageIcon* icon) {
  if (!image_icon_cache) {
    image_icon_cache = new std::map<unsigned long, ImageIcon*>;
    image_cache_refcounts = new std::map<unsigned long, int>;
  }
  (*image_icon_cache)[hash] = icon;
  // Don't add a refcount here; instead we increment refcounts only on clone.
}

unsigned long hashData(unsigned long* data, unsigned long len) {
  // For simplicity, coerce the data into a string, and then hash it.
  std::string s((char*)data, len * sizeof(unsigned long));
  std::hash<std::string> h;
  return h(s);
}

unsigned long hashPixmaps(Pixmap img, Pixmap mask) {
  // Assuming the same image and mask are used together (which is probably a
  // safe assumption), we can just use the img as the hash.
  mask = mask;
  return (unsigned long)img;
}

ImageIcon::ImageIcon(Pixmap active_img,
                     Pixmap inactive_img,
                     Pixmap menu_img,
                     unsigned int img_w,
                     unsigned int img_h,
                     unsigned int depth)
    : active_img_(active_img),
      inactive_img_(inactive_img),
      menu_img_(menu_img),
      img_w_(img_w),
      img_h_(img_h),
      depth_(depth) {}

ImageIcon::~ImageIcon() {
  if (gc_hash_) {
    removeCacheRef(gc_hash_);
  }
}

ImageIcon* ImageIcon::clone(unsigned long hash) {
  ImageIcon* res = new ImageIcon(active_img_, inactive_img_, menu_img_, img_w_,
                                 img_h_, depth_);
  res->gc_hash_ = hash;
  return res;
}

void ImageIcon::destroyResources() {
  XFreePixmap(dpy, active_img_);
  XFreePixmap(dpy, inactive_img_);
  XFreePixmap(dpy, menu_img_);
}

// copyWithScaling scales down the contents of src into dest.
// The src image *must* be at least as large as the dest image.
// This function applies some very simple anti-aliasing. It could be improved
// by using sub-pixel accuracy, and weighted averages, but this doesn't seem to
// be necessary, given the results we get from this much simpler approach.
void copyWithScaling(XImage* src, XImage* dest) {
  for (int y = 0; y < dest->height; y++) {
    const int src_min_y = y * src->height / dest->height;
    const int src_max_y = (y + 1) * src->height / dest->height;  // exclusive
    for (int x = 0; x < dest->width; x++) {
      const int src_min_x = x * src->width / dest->width;
      const int src_max_x = (x + 1) * src->width / dest->width;  // exclusive
      // An unsigned long has plenty of space to the left of even the red
      // component, so we don't bother shifting the components down and up
      // again. However, we must treat each component separately for the
      // averaging, otherwise we'll get bleed between the components due to
      // integer rounding when we divide by the number of pixels.
      unsigned long r = 0;
      unsigned long g = 0;
      unsigned long b = 0;
      for (int sy = src_min_y; sy < src_max_y; sy++) {
        for (int sx = src_min_x; sx < src_max_x; sx++) {
          const unsigned long val = XGetPixel(src, sx, sy);
          r += val & 0xff0000;
          g += val & 0xff00;
          b += val & 0xff;
        }
      }
      // We're never called in a case where dest is larger than src, so the
      // following calculation cannot divide by zero.
      const int div = (src_max_y - src_min_y) * (src_max_x - src_min_x);
      r = (r / div) & 0xff0000;
      g = (g / div) & 0xff00;
      b = (b / div) & 0xff;
      XPutPixel(dest, x, y, r | g | b);
    }
  }
}

Pixmap pixmapFromXImage(XImage* img) {
  Pixmap pm =
      XCreatePixmap(dpy, LScr::I->Root(), img->width, img->height, img->depth);
  GC igc = XCreateGC(dpy, pm, 0, nullptr);
  XPutImage(dpy, pm, igc, img, 0, 0, 0, 0, img->width, img->height);
  XFreeGC(dpy, igc);
  return pm;
}

void allocateDataForXImage(XImage* img) {
  img->data = (char*)calloc(img->height, img->bytes_per_line);
}

// Background is a little helper, used to provide a suitable background colour
// to the xImageDataToImage function.
// The case this is used in is when the user has configured a top border width,
// which means the top edge of the icon used for an active window needs the
// active border colour for its background, rather than the title background.
// For this reason, we only really need two colours, and a vertical index from
// which to use the second colour (so we ignore the X coordinate entirely).
// It should be noted that as xImageDataToImage works on the original image,
// baking in the expected background colour before scaling the image down,
// calls to .At(x, y) provide coordinates in the original image's coordinate
// system, not in the (usually more restricted) space of the destination image.
class Background {
 public:
  explicit Background(unsigned long colour)
      : top_(colour), boundary_(0), bottom_(colour) {}
  Background(unsigned long top, int boundary, unsigned long bottom)
      : top_(top), boundary_(boundary), bottom_(bottom) {}

  unsigned long At(int x, int y) const {
    x = x;  // Unused, but the API looks a bit weird without X.
    return (y > boundary_ ? bottom_ : top_) | 0xff000000;
  }

 private:
  unsigned long top_;
  int boundary_;
  unsigned long bottom_;
};

void xImageDataToImage(XImage* dest,
                       XImage* orig,
                       XImage* mask,
                       const Background& background) {
  for (int y = 0; y < orig->height; y++) {
    for (int x = 0; x < orig->width; x++) {
      unsigned long rgb = XGetPixel(orig, x, y) | 0xff000000;
      XPutPixel(dest, x, y, XGetPixel(mask, x, y) ? rgb : background.At(x, y));
    }
  }
}

void pixelDataToImage(XImage* img,
                      unsigned long* data,
                      int width,
                      int height,
                      unsigned long background) {
  const unsigned long bgr = background & 0xff0000;
  const unsigned long bgg = background & 0xff00;
  const unsigned long bgb = background & 0xff;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const unsigned long argb = *data++;
      const unsigned long a = (argb >> 24) & 0xff;  // alpha for foreground.
      const unsigned long bga = 0xff - a;           // alpha for background.
      // Treat the 3 channels separately, to avoid cross-channel bleed (which
      // makes the icons of rhythmbox and xfce4-mixer look like CGA vomit).
      unsigned long r = (((argb & 0xff0000) * a + bgr * bga) / 0xff) & 0xff0000;
      unsigned long g = (((argb & 0xff00) * a + bgg * bga) / 0xff) & 0xff00;
      unsigned long b = (((argb & 0xff) * a + bgb * bga) / 0xff) & 0xff;
      XPutPixel(img, x, y, r | g | b);
    }
  }
}

// static
ImageIcon* ImageIcon::Create(Pixmap img, Pixmap mask) {
  if (!img) {
    return nullptr;
  }
  const unsigned long pm_hash = hashPixmaps(img, mask);
  ImageIcon* result = fromCache(pm_hash);
  if (result) {
    return result->clone(pm_hash);
  }

  const WindowGeometry geom = XGetGeometry(img);
  // Not going to bother trying to paint stuff that's not colourful enough.
  if (!geom.ok || geom.bpp != 24) {
    return nullptr;
  }
  // The image is too large for our needs. Figure out how big to make the
  // width and height dimensions.
  const int targetSize = targetImageIconSize();
  const int width =
      (geom.rect.width() < targetSize) ? geom.rect.width() : targetSize;
  const int height =
      (geom.rect.height() < targetSize) ? geom.rect.height() : targetSize;

  XImage* orig_img = XGetImage(dpy, img, 0, 0, geom.rect.width(),
                               geom.rect.height(), 0xffffff, ZPixmap);
  XImage* mask_img = nullptr;
  if (mask) {
    mask_img = XGetImage(dpy, mask, 0, 0, geom.rect.width(), geom.rect.height(),
                         1, ZPixmap);
  }

  // src_img will be filled in with the data from orig_img, but with the mask
  // (and background) applied.
  XImage* src_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, geom.rect.width(), geom.rect.height(), 32, 0);
  XImage* dest_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, width, height, 32, 0);
  if (!src_img || !dest_img) {
    return nullptr;
  }
  allocateDataForXImage(src_img);
  allocateDataForXImage(dest_img);

  // For each possible background, generate the RGB values by applying the
  // image values and background value with the alpha channel.
  // If the user has configured a top border width, the 'active' icon has two
  // background colours, one for the top edge and one for the rest. Note that
  // the vertical separation is at topBorderWidth(), but we scale that to the
  // source image's coordinates, as xImageDataToImage runs before we scale the
  // image down to our target size.
  xImageDataToImage(
      src_img, orig_img, mask_img,
      Background(LScr::I->ActiveBorder(),
                 topBorderWidth() * geom.rect.height() / targetSize,
                 Resources::I->GetColour(Resources::TITLE_BG_COLOUR)));
  copyWithScaling(src_img, dest_img);
  const Pixmap active_pm = pixmapFromXImage(dest_img);

  xImageDataToImage(src_img, orig_img, mask_img,
                    Background(LScr::I->InactiveBorder()));
  copyWithScaling(src_img, dest_img);
  const Pixmap inactive_pm = pixmapFromXImage(dest_img);

  xImageDataToImage(
      src_img, orig_img, mask_img,
      Background(Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR)));
  copyWithScaling(src_img, dest_img);
  const Pixmap menu_pm = pixmapFromXImage(dest_img);

  XDestroyImage(orig_img);
  if (mask_img) {
    XDestroyImage(mask_img);
  }
  XDestroyImage(src_img);
  XDestroyImage(dest_img);

  result = new ImageIcon(active_pm, inactive_pm, menu_pm, width, height, 24);
  toCache(pm_hash, result);
  return result->clone(pm_hash);
}

// Use Google Chrome or Chromium to test CreateFromPixels.
// static
ImageIcon* ImageIcon::CreateFromPixels(unsigned long* data, unsigned long len) {
  if (data == nullptr || len < 2) {
    return nullptr;
  }
  const unsigned long pm_hash = hashData(data, len);
  ImageIcon* result = fromCache(pm_hash);
  if (result) {
    return result->clone(pm_hash);
  }

  const int src_width = data[0];
  const int src_height = data[1];
  if (src_width == 0 || src_height == 0 || len < (2 + src_width * src_height)) {
    fprintf(stderr, "Invalid width (%d) vs height (%d) vs size (%d)\n",
            src_width, src_height, int(len));
    return nullptr;
  }

  // Calculate the destination size of the icon, either the same as source, or
  // the desired size if that's smaller.
  // This code assumes the icon is square (if not, it will be distorted).
  // But it's almost guaranteed to be so.
  const int targetSize = targetImageIconSize();
  const int width = (src_width < targetSize) ? src_width : targetSize;
  const int height = (src_height < targetSize) ? src_height : targetSize;

  XImage* src_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, src_width, src_height, 32, 0);
  XImage* dest_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, width, height, 32, 0);
  if (!src_img || !dest_img) {
    return nullptr;
  }
  allocateDataForXImage(src_img);
  allocateDataForXImage(dest_img);

  // For each possible background, generate the RGB values by applying the
  // image values and background value with the alpha channel.
  pixelDataToImage(src_img, data + 2, src_width, src_height,
                   Resources::I->GetColour(Resources::TITLE_BG_COLOUR));
  copyWithScaling(src_img, dest_img);
  const Pixmap active_pm = pixmapFromXImage(dest_img);

  pixelDataToImage(src_img, data + 2, src_width, src_height,
                   LScr::I->InactiveBorder());
  copyWithScaling(src_img, dest_img);
  const Pixmap inactive_pm = pixmapFromXImage(dest_img);

  pixelDataToImage(src_img, data + 2, src_width, src_height,
                   Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR));
  copyWithScaling(src_img, dest_img);
  const Pixmap menu_pm = pixmapFromXImage(dest_img);

  XDestroyImage(src_img);
  XDestroyImage(dest_img);
  result = new ImageIcon(active_pm, inactive_pm, menu_pm, width, height, 24);
  toCache(pm_hash, result);
  return result->clone(pm_hash);
}

void ImageIcon::paint(Window w,
                      Pixmap pm,
                      int x,
                      int y,
                      int width,
                      int height) {
  if (!pm) {
    return;
  }
  const int xo = (width - (int)img_w_) / 2;
  const int yo = (height - (int)img_h_) / 2;
  XGCValues gv;
  gv.function = GXcopy;
  GC gc = XCreateGC(dpy, w, GCFunction, &gv);
  // If the pixmap is smaller than what we want to draw, adjust the coordinates
  // so we draw within the bounding box.
  if (xo > 0) {
    x += xo;
    width = img_w_;
  }
  if (yo > 0) {
    y += yo;
    height = img_h_;
  }
  // If the pixmap is bigger than what we want to draw, adjust the src
  // coordinates to draw something in the middle of the source pixmap.
  const int src_x = (xo < 0) ? -xo : 0;
  const int src_y = (yo < 0) ? -yo : 0;
  XCopyArea(dpy, pm, w, gc, src_x, src_y, width, height, x, y);
  XFreeGC(dpy, gc);
}

}  // namespace xlib
