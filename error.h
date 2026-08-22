#ifndef LWM_ERROR_H_included
#define LWM_ERROR_H_included

#include "xlib.h"

// Create one of these in a scope to temporary switch off reporting of
// 'BadWindow' errors. This is needed in some cases because some events can
// happen on windows which have already been deleted.
// This will save and restore the previous ignore state, so it's safe to nest
// these.
class ScopedIgnoreBadWindow {
 public:
  ScopedIgnoreBadWindow();
  ~ScopedIgnoreBadWindow();

 private:
  bool old_;
};

class ScopedIgnoreBadMatch {
 public:
  ScopedIgnoreBadMatch();
  ~ScopedIgnoreBadMatch();

 private:
  bool old_;
};

extern int errorHandler(Display*, XErrorEvent*);
extern void panic(const char*);

#endif  // LWM_ERROR_H_included
