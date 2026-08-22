#include "xdebugprint.h"

#include "client.h"
#include "ewmh.h"

std::ostream& operator<<(std::ostream& os, const XConfigureRequestEvent& e) {
  os << "ConfigRequestEvent " << WinID(e.window) << " (parent "
     << WinID(e.parent) << ")";
#define OUT(flag, var)     \
  if (e.value_mask & flag) \
    os << " " #var "->" << e.var;
  OUT(CWX, x);
  OUT(CWY, y);
  OUT(CWWidth, width);
  OUT(CWHeight, height);
  OUT(CWBorderWidth, border_width);
  OUT(CWSibling, above);
  OUT(CWStackMode, detail);
#undef OUT
  return os;
}

std::ostream& operator<<(std::ostream& os, const XConfigureEvent& e) {
  os << WinID(e.window) << " " << (e.send_event ? "S" : "s") << e.serial << " ";
  os << Rect::FromXYWH(e.x, e.y, e.width, e.height) << ", b=" << e.border_width;
  return os;
}

std::ostream& operator<<(std::ostream& os, const diff& d) {
  bool changed = false;
#define D(x)                                            \
  do {                                                  \
    if (d.o.x != d.n.x) {                               \
      changed = true;                                   \
      os << " " #x << " " << (d.o.x ? "t->f" : "f->t"); \
    }                                                   \
  } while (false)
  D(skip_taskbar);
  D(skip_pager);
  D(fullscreen);
  D(above);
  D(below);
#undef D
  if (!changed) {
    os << " no changes (" << d.n << ")";
  }
  return os;
}

namespace {

std::string describeFocusMode(int mode) {
  switch (mode) {
#define CASE_RETURN(x) \
  case x:              \
    return #x
    CASE_RETURN(NotifyNormal);
    CASE_RETURN(NotifyGrab);
    CASE_RETURN(NotifyUngrab);
#undef CASE_RETURN
  }
  return "Unknown";
}

std::string describeFocusDetail(int detail) {
  switch (detail) {
#define CASE_RETURN(x) \
  case x:              \
    return #x
    CASE_RETURN(NotifyAncestor);
    CASE_RETURN(NotifyVirtual);
    CASE_RETURN(NotifyInferior);
    CASE_RETURN(NotifyNonlinear);
    CASE_RETURN(NotifyNonlinearVirtual);
    CASE_RETURN(NotifyPointer);
    CASE_RETURN(NotifyPointerRoot);
    CASE_RETURN(NotifyDetailNone);
#undef CASE_RETURN
  }
  return "Unknown";
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const XFocusChangeEvent& e) {
  os << WinID(e.window) << " (serial: " << e.serial
     << ") mode: " << describeFocusMode(e.mode)
     << ", detail: " << describeFocusDetail(e.detail);
  return os;
}
