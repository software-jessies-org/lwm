// Tests for the title bar icon cache (xlib.cc's ImageIcon).
//
// The cache exists so that the twenty windows of one application don't each
// pay for scaling the same icon and holding three pixmaps of it. What makes it
// worth testing is what happens at the edges of an entry's life: an icon must
// not outlive the last window using it, and - the bug these tests were written
// for - an icon must never be handed to an application that didn't ask for it.

#include <vector>

#include "client.h"
#include "screen.h"
#include "test.h"
#include "wmtest.h"
#include "xlib.h"

namespace {

// A solid block of colour, large enough that the icon code scales it down.
std::vector<uint32_t> solidIcon(uint32_t colour) {
  return std::vector<uint32_t>(64 * 64, colour);
}

// How many pixmaps the icon code has asked the server to render since the
// call log was last cleared. Rendering an icon takes three: one per background
// it might be drawn against (active title bar, inactive title bar, menu).
size_t pixmapsRendered(wmtest::World& world) {
  return world.server().CallsMatching("CreatePixmapFromPixels(").size();
}

size_t pixmapsFreed(wmtest::World& world) {
  return world.server().CallsMatching("FreePixmap(").size();
}

}  // namespace

TEST(IconCache, ARecycledPixmapIdDoesNotResurrectTheOldIcon) {
  wmtest::World world;
  // An application hands us an icon in WM_HINTS, and later exits.
  const Pixmap p =
      world.server().CreatePixmapWithPixels(64, 64, 24, solidIcon(0xff0000));
  xlib::ImageIcon* first = xlib::ImageIcon::Create(p, 0);
  ASSERT_TRUE(first != nullptr);
  delete first;
  world.server().FreePixmap(p);

  // A different application connects, and the server hands it the resource ID
  // the first one had finished with. Its icon is not the first one's icon.
  world.server().ClearCalls();
  const Pixmap reused =
      world.server().CreatePixmapWithPixels(64, 64, 24, solidIcon(0x00ff00), p);
  ASSERT_EQ(reused, p);
  xlib::ImageIcon* second = xlib::ImageIcon::Create(reused, 0);
  ASSERT_TRUE(second != nullptr);
  EXPECT_EQ(pixmapsRendered(world), size_t(3))
      << "the second application's icon must be rendered from its own pixels, "
         "not fetched from the cache under the recycled pixmap ID";
  delete second;
}

TEST(IconCache, IdenticalIconsShareOneRenderedCopy) {
  wmtest::World world;
  // Two windows of the same application: same icon, but each with its own
  // pixmap, so only the contents can tell us they're the same picture.
  const Pixmap p1 =
      world.server().CreatePixmapWithPixels(64, 64, 24, solidIcon(0x0000ff));
  const Pixmap p2 =
      world.server().CreatePixmapWithPixels(64, 64, 24, solidIcon(0x0000ff));
  xlib::ImageIcon* first = xlib::ImageIcon::Create(p1, 0);
  ASSERT_TRUE(first != nullptr);

  world.server().ClearCalls();
  xlib::ImageIcon* second = xlib::ImageIcon::Create(p2, 0);
  ASSERT_TRUE(second != nullptr);
  EXPECT_EQ(pixmapsRendered(world), size_t(0))
      << "the second window's icon should come out of the cache";

  // The rendered pixmaps live until the last window using them has gone.
  delete first;
  EXPECT_EQ(pixmapsFreed(world), size_t(0))
      << "one window closing must not take the other's icon with it";
  delete second;
  EXPECT_EQ(pixmapsFreed(world), size_t(3));
}

TEST(IconCache, DifferentIconsGetDifferentCacheEntries) {
  wmtest::World world;
  const Pixmap p1 =
      world.server().CreatePixmapWithPixels(64, 64, 24, solidIcon(0x808000));
  const Pixmap p2 =
      world.server().CreatePixmapWithPixels(64, 64, 24, solidIcon(0x008080));
  xlib::ImageIcon* first = xlib::ImageIcon::Create(p1, 0);
  ASSERT_TRUE(first != nullptr);

  world.server().ClearCalls();
  xlib::ImageIcon* second = xlib::ImageIcon::Create(p2, 0);
  ASSERT_TRUE(second != nullptr);
  EXPECT_EQ(pixmapsRendered(world), size_t(3));
  delete first;
  delete second;
}

TEST(IconCache, ReplacingAWindowsIconReleasesTheOldOne) {
  wmtest::World world;
  Client* c = world.MapClientWindow(Rect::FromXYWH(100, 100, 300, 200));
  ASSERT_TRUE(c != nullptr);
  // manage() does this: the WM_HINTS icon first, then the _NET_WM_ICON one on
  // top of it if the window has both.
  const Pixmap p =
      world.server().CreatePixmapWithPixels(64, 64, 24, solidIcon(0x123456));
  c->SetIcon(xlib::ImageIcon::Create(p, 0));
  ASSERT_TRUE(c->Icon() != nullptr);

  world.server().ClearCalls();
  const std::vector<uint32_t> pixels = {64, 64};
  std::vector<uint32_t> ewmh_icon(pixels);
  ewmh_icon.resize(2 + 64 * 64, 0xff654321);
  c->SetIcon(
      xlib::ImageIcon::CreateFromPixels(ewmh_icon.data(), ewmh_icon.size()));
  EXPECT_EQ(pixmapsFreed(world), size_t(3))
      << "the icon that was replaced must give up its cache entry, or its "
         "pixmap ID stays claimed for as long as lwm runs";
}
