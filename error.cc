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

#include "error.h"
#include "lwm.h"

static bool ignore_badwindow;
static bool ignore_badmatch;

ScopedIgnoreBadWindow::ScopedIgnoreBadWindow() : old_(ignore_badwindow) {
  ignore_badwindow = true;
}

ScopedIgnoreBadWindow::~ScopedIgnoreBadWindow() {
  ignore_badwindow = old_;
}

ScopedIgnoreBadMatch::ScopedIgnoreBadMatch() : old_(ignore_badmatch) {
  ignore_badmatch = true;
}

ScopedIgnoreBadMatch::~ScopedIgnoreBadMatch() {
  ignore_badmatch = old_;
}

void panic(const char* s) {
  fprintf(stderr, "%s: %s\n", argv0, s);
  exit(EXIT_FAILURE);
}

#define MAX_STACK_DEPTH 10

int errorHandler(Display* d, XErrorEvent* e) {
  char msg[80];
  char req[80];
  char number[80];

  if (is_initialising && e->request_code == X_ChangeWindowAttributes &&
      e->error_code == BadAccess) {
    panic("another window manager is already running.");
  }

  if (ignore_badwindow &&
      (e->error_code == BadWindow || e->error_code == BadColor)) {
    return 0;
  }
  if (ignore_badmatch && (e->error_code == BadMatch)) {
    return 0;
  }

  XGetErrorText(d, e->error_code, msg, sizeof(msg));
  sprintf(number, "%d", e->request_code);
  XGetErrorDatabaseText(d, "XRequest", number, number, req, sizeof(req));

  fprintf(stderr, "%s: protocol request %s on resource %#x failed: %s\n", argv0,
          req, (unsigned int)e->resourceid, msg);

#ifdef __linux__
  void* stack[MAX_STACK_DEPTH];
  size_t depth = backtrace(stack, MAX_STACK_DEPTH);
  backtrace_symbols_fd(stack, depth, STDERR_FILENO);
#endif

  if (is_initialising) {
    panic("can't initialise.");
  }

  return 0;
}
