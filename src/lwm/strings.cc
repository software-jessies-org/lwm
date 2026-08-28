#include "strings.h"

std::vector<std::string> Split(const std::string& in,
                               const std::string& split) {
  std::vector<std::string> res;
  size_t start = 0;
  while (true) {
    size_t end = in.find(split, start);
    if (end == std::string::npos) {
      res.push_back(in.substr(start));
      return res;
    }
    res.push_back(in.substr(start, end - start));
    start = end + split.size();
  }
}

std::string TruncateUtf8(const std::string& s, int maxChars) {
  if (static_cast<int>(s.size()) <= maxChars) {
    return s;
  }
  int chars = 0;
  int uniLeft = 0;
  for (int i = 0; i < static_cast<int>(s.size()); i++) {
    if (uniLeft && --uniLeft) {
      continue;  // Skip trailing UTF8 only.
    }
    char ch = s[i];
    chars++;
    if (chars == maxChars) {
      // i must be at the start of a unicode character (or ascii), and we've
      // seen as many visible characters as we wanted.
      return s.substr(0, i) + "...";
    }
    int mask = 0xf8;
    int val = 0xf0;
    uniLeft = 4;
    while (mask && uniLeft) {
      if ((ch & mask) == val) {
        break;
      }
      uniLeft--;
      mask = (mask << 1) & 0xff;
      val = (val << 1) & 0xff;
    }
  }
  // Dropped off the end? The string must be under maxChars UTF8 characters
  // in length then.
  return s;
}
