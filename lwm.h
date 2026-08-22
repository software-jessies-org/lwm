#ifndef LWM_LWM_H_included
#define LWM_LWM_H_included
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

// lwm.h is what's left of the old god header: main()'s own globals and the
// handful of functions lwm.cc itself provides. It also pulls in every other
// header, purely as a convenience umbrella for main() (which really does
// touch nearly everything at start-up) and for tests.cc; other .cc files
// should include only the specific headers they actually use rather than
// this file. See docs/refactoring-plan.md.

#include <string>

#include "client.h"
#include "cursor.h"
#include "debug.h"
#include "disp.h"
#include "error.h"
#include "ewmh.h"
#include "manage.h"
#include "resource.h"
#include "screen.h"
#include "session.h"
#include "shape.h"
#include "strings.h"
#include "xlib.h"

/* lwm.cc */
extern bool is_initialising;  // Set during start-up, cleared after.

extern Atom _mozilla_url;
extern Atom motif_wm_hints;
extern Atom wm_state;
extern Atom wm_change_state;
extern Atom wm_protocols;
extern Atom wm_delete;
extern Atom wm_take_focus;
extern Atom compound_text;
extern bool shape;
extern int shape_event;

// Handles a RandR ScreenChangeNotify, returning false if the event wasn't
// one. Like shapeEvent(), this can't live in DispatchXEvent's switch, because
// extension event numbers are only known at run time.
extern bool randrEvent(xcb_generic_event_t* ev);
extern char* argv0;
extern bool forceRestart;
extern void shell(int button);

// Runs <command> in a child process.
extern void RunCommand(const std::string& command);

// tests.cc
extern bool RunAllTests();

#endif  // LWM_LWM_H_included
