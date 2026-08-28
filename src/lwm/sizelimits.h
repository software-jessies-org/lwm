#ifndef SIZELIMITS_H_included
#define SIZELIMITS_H_included

class DimensionLimiter {
 public:
  DimensionLimiter() : DimensionLimiter(0, 0, 0, 1) {}
  DimensionLimiter(int min, int max, int base, int increment);

  // Given an old and new range, the new range being passed by reference,
  // adjusts the new range according to the limits. If one of the new min/max
  // values must be adjusted, Limit picks the one that has changed from its
  // old value, and fixes that one.
  void Limit(int oldMin, int oldMax, int& newMin, int& newMax) const;

  // Returns the size that should be displayed to the user. This takes into
  // account any size increments and base sizes. For example, if the window has
  // no limits or increments set, this just returns v. If, however, this is
  // something like an xterm, which has size increments equal to the character
  // size, and maybe a base equal to the size of the scrollbar, then the value
  // returned is the number of increments above the base, and thus the number
  // of characters. Thus, we end up showing "80 x 24", for example, for a
  // normal-sized xterm.
  int DisplayableSize(int v) const;

 private:
  int min_;
  int max_;
  int base_;
  int increment_;
};

#endif
