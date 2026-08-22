#ifndef MENULAYOUT_H_included
#define MENULAYOUT_H_included

// Layout geometry for the unhide-window popup menu. Everything here is a
// function of the configured font's text height (see CurrentMenuStyle() in
// mouse.cc); the padding constants themselves aren't user-configurable.
struct MenuStyle {
  static constexpr int kYPadding = 6;
  static constexpr int kIconYPad = 1;
  static constexpr int kIconXPad = 5;

  int text_height = 0;

  int ItemHeight() const { return text_height + kYPadding; }
};

int MenuIconSize(const MenuStyle& style);
int MenuLHighlight(const MenuStyle& style);
int MenuRHighlight(const MenuStyle& style);
int MenuHighlightMargins(const MenuStyle& style);
int MenuLMargin(const MenuStyle& style);
int MenuRMargin(const MenuStyle& style);
int MenuMargins(const MenuStyle& style);

#endif
