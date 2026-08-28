/*
 * lwm, a window manager for X11
 * Copyright (C) 1997-2016 Elliott Hughes, James Carter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef __linux__
#include <execinfo.h>
#endif

#include <unistd.h>

#include <vector>

#include "error.h"
#include "lwm.h"

namespace {

// A range of request sequence numbers whose errors of a particular code we've
// been asked to ignore. See ScopedIgnoreErrors.
struct IgnoredRange {
  uint8_t error_code;
  uint32_t first;  // Exclusive: the NoOp issued on scope entry.
  uint32_t last;   // Exclusive: the NoOp issued on scope exit.
};

// Small by construction: one entry per live-or-recently-closed scope, and
// RetireIgnoredSequences drops them as soon as the server has moved past.
std::vector<IgnoredRange> ignored_ranges;

// The 17 core protocol error codes. Extension errors get numbers above these,
// which we print numerically; lwm uses few enough extensions that a table for
// them would be more maintenance than it's worth.
const char* errorCodeName(uint8_t code) {
  static const char* kNames[] = {
      "Success",   "BadRequest", "BadValue",     "BadWindow",
      "BadPixmap", "BadAtom",    "BadCursor",    "BadFont",
      "BadMatch",  "BadDrawable", "BadAccess",   "BadAlloc",
      "BadColor",  "BadGC",      "BadIDChoice",  "BadName",
      "BadLength", "BadImplementation",
  };
  if (code < sizeof(kNames) / sizeof(kNames[0])) {
    return kNames[code];
  }
  return nullptr;
}

// Core protocol major opcodes. libxcb-errors would provide this (along with
// extension opcodes) but isn't packaged everywhere, and it's a static table
// either way.
const char* requestName(uint8_t opcode) {
  static const char* kNames[] = {
      nullptr,
      "CreateWindow", "ChangeWindowAttributes", "GetWindowAttributes",
      "DestroyWindow", "DestroySubwindows", "ChangeSaveSet", "ReparentWindow",
      "MapWindow", "MapSubwindows", "UnmapWindow", "UnmapSubwindows",
      "ConfigureWindow", "CirculateWindow", "GetGeometry", "QueryTree",
      "InternAtom", "GetAtomName", "ChangeProperty", "DeleteProperty",
      "GetProperty", "ListProperties", "SetSelectionOwner",
      "GetSelectionOwner", "ConvertSelection", "SendEvent", "GrabPointer",
      "UngrabPointer", "GrabButton", "UngrabButton",
      "ChangeActivePointerGrab", "GrabKeyboard", "UngrabKeyboard", "GrabKey",
      "UngrabKey", "AllowEvents", "GrabServer", "UngrabServer", "QueryPointer",
      "GetMotionEvents", "TranslateCoords", "WarpPointer", "SetInputFocus",
      "GetInputFocus", "QueryKeymap", "OpenFont", "CloseFont", "QueryFont",
      "QueryTextExtents", "ListFonts", "ListFontsWithInfo", "SetFontPath",
      "GetFontPath", "CreatePixmap", "FreePixmap", "CreateGC", "ChangeGC",
      "CopyGC", "SetDashes", "SetClipRectangles", "FreeGC", "ClearArea",
      "CopyArea", "CopyPlane", "PolyPoint", "PolyLine", "PolySegment",
      "PolyRectangle", "PolyArc", "FillPoly", "PolyFillRectangle",
      "PolyFillArc", "PutImage", "GetImage", "PolyText8", "PolyText16",
      "ImageText8", "ImageText16", "CreateColormap", "FreeColormap",
      "CopyColormapAndFree", "InstallColormap", "UninstallColormap",
      "ListInstalledColormaps", "AllocColor", "AllocNamedColor",
      "AllocColorCells", "AllocColorPlanes", "FreeColors", "StoreColors",
      "StoreNamedColor", "QueryColors", "LookupColor", "CreateCursor",
      "CreateGlyphCursor", "FreeCursor", "RecolorCursor", "QueryBestSize",
      "QueryExtension", "ListExtensions", "ChangeKeyboardMapping",
      "GetKeyboardMapping", "ChangeKeyboardControl", "GetKeyboardControl",
      "Bell", "ChangePointerControl", "GetPointerControl", "SetScreenSaver",
      "GetScreenSaver", "ChangeHosts", "ListHosts", "SetAccessControl",
      "SetCloseDownMode", "KillClient", "RotateProperties",
      "ForceScreenSaver", "SetPointerMapping", "GetPointerMapping",
      "SetModifierMapping", "GetModifierMapping",
  };
  if (opcode == 127) {
    return "NoOperation";
  }
  if (opcode < sizeof(kNames) / sizeof(kNames[0]) && kNames[opcode]) {
    return kNames[opcode];
  }
  return nullptr;
}

bool isIgnored(const xcb_generic_error_t* err) {
  for (const IgnoredRange& r : ignored_ranges) {
    if (err->error_code != r.error_code) {
      continue;
    }
    // The bracketing NoOps themselves can't fail, so the range is open at
    // both ends. A range whose scope hasn't closed yet has last == 0, and
    // suppresses everything from first onwards.
    if (err->full_sequence <= r.first) {
      continue;
    }
    if (r.last != 0 && err->full_sequence >= r.last) {
      continue;
    }
    return true;
  }
  return false;
}

}  // namespace

ScopedIgnoreErrors::ScopedIgnoreErrors(uint8_t error_code)
    : error_code_(error_code),
      first_sequence_(xlib::NextRequestSequence()) {
  // Register the range immediately, still open-ended, so that errors from
  // requests made inside the scope are suppressed even if they somehow reach
  // us before the scope closes.
  ignored_ranges.push_back(IgnoredRange{error_code_, first_sequence_, 0});
}

ScopedIgnoreErrors::~ScopedIgnoreErrors() {
  const uint32_t last = xlib::NextRequestSequence();
  for (IgnoredRange& r : ignored_ranges) {
    if (r.error_code == error_code_ && r.first == first_sequence_ &&
        r.last == 0) {
      r.last = last;
      return;
    }
  }
}

ScopedIgnoreBadWindow::ScopedIgnoreBadWindow()
    : window_(XCB_WINDOW), colour_(XCB_COLORMAP) {}

void RetireIgnoredSequences(uint32_t sequence) {
  for (size_t i = 0; i < ignored_ranges.size();) {
    const IgnoredRange& r = ignored_ranges[i];
    if (r.last != 0 && sequence >= r.last) {
      ignored_ranges.erase(ignored_ranges.begin() + i);
    } else {
      i++;
    }
  }
}

void panic(const char* s) {
  fprintf(stderr, "%s: %s\n", argv0, s);
  exit(EXIT_FAILURE);
}

#define MAX_STACK_DEPTH 10

void HandleXError(const xcb_generic_error_t* err) {
  if (isIgnored(err)) {
    return;
  }

  const char* code_name = errorCodeName(err->error_code);
  const char* req_name = requestName(err->major_code);
  fprintf(stderr, "%s: protocol request ", argv0);
  if (req_name) {
    fprintf(stderr, "%s", req_name);
  } else {
    fprintf(stderr, "opcode %d.%d", err->major_code, err->minor_code);
  }
  fprintf(stderr, " on resource %#x failed: ", err->resource_id);
  if (code_name) {
    fprintf(stderr, "%s\n", code_name);
  } else {
    fprintf(stderr, "error %d\n", err->error_code);
  }

#ifdef __linux__
  // Unlike Xlib's error handler, this doesn't run on the stack of the call
  // that caused the problem - errors arrive on the event queue long after.
  // The backtrace is still worth having, since it at least says which event
  // we were processing, but don't read it as pointing at the failed request.
  void* stack[MAX_STACK_DEPTH];
  size_t depth = backtrace(stack, MAX_STACK_DEPTH);
  backtrace_symbols_fd(stack, depth, STDERR_FILENO);
#endif

  if (is_initialising) {
    panic("can't initialise.");
  }
}
