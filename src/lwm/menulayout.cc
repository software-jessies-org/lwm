#include "menulayout.h"

int MenuIconSize(const MenuStyle& style) {
  return style.ItemHeight() - MenuStyle::kIconYPad * 2;
}

int MenuLHighlight(const MenuStyle& style) {
  return style.ItemHeight() + MenuStyle::kIconXPad * 2;
}

int MenuRHighlight(const MenuStyle& style) {
  return style.ItemHeight() - MenuStyle::kIconXPad;
}

int MenuHighlightMargins(const MenuStyle& style) {
  return MenuLHighlight(style) + MenuRHighlight(style);
}

int MenuLMargin(const MenuStyle& style) {
  return style.ItemHeight() + MenuStyle::kIconXPad * 3;
}

int MenuRMargin(const MenuStyle& style) {
  return style.ItemHeight();
}

int MenuMargins(const MenuStyle& style) {
  return MenuLMargin(style) + MenuRMargin(style);
}
