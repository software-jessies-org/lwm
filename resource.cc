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

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lwm.h"

static Resources* resource_cache;

static void setResource(XrmDatabase *db, const char *name, const char *cls,
                        std::string* target) {
  char *type;
  XrmValue value;
  if (XrmGetResource(*db, name, cls, &type, &value)) {
    if (!strcmp(type, "String")) {
      *target = std::string(value.addr, value.size);
    }
  }
}

static std::string getResource(XrmDatabase *db, const char *name,
                               const char *cls) {
  std::string res;
  setResource(db, name, cls, &res);
  return res;
}

Resources* parseResources() {
  Resources* res = new Resources();
  res->font_name = DEFAULT_TITLE_FONT;
  res->border = DEFAULT_BORDER;
  res->btn2_command = DEFAULT_TERMINAL;

  char *resource_manager = XResourceManagerString(dpy);
  if (!resource_manager) {
    return res;
  }
  XrmInitialize();
  XrmDatabase db = XrmGetStringDatabase(resource_manager);
  if (!db) {
    return res;
  }

  // Simple string resources.
  setResource(&db, "lwm.titleFont", "Font", &(res->font_name));
  setResource(&db, "lwm.button1", "Command", &(res->btn1_command));
  setResource(&db, "lwm.button2", "Command", &(res->btn2_command));

  // Resources that require some interpretation.
  const std::string brdr = getResource(&db, "lwm.border", "Border");
  if (brdr != "") {
    res->border = (int)strtol(brdr.c_str(), (char **)0, 0);
  }
  return res;
}

Resources* resources() {
  if (!resource_cache) {
    resource_cache = parseResources();
  }
  return resource_cache;
}

int borderWidth() {
  return resources()->border;
}
