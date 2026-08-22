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

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include "lwm.h"
#include "manage.h"
#include "session.h"

struct SmProperty {
  SmProp p;
  SmPropValue v;
};

int ice_fd = -1;
static IceConn ice_conn;
static SmcConn smc_conn = NULL;
static int session_argc;
static char** session_argv;
static char* client_id = NULL;

void ice_error(IceConn) {
  // We only bother catching ice i/o errors because metacity claims the
  // default handler calls exit. twm doesn't bother, so it might not
  // be necessary.
  fprintf(stderr, "%s: ICE I/O error\n", argv0);
}

void session_save_yourself(SmcConn smc_conn, SmPointer, int, Bool, int, Bool) {
  SmProperty program;
  SmProperty user_id;
  SmProperty restart_style_hint;
  SmProperty pid;
  SmProperty gsm_priority;
  SmProp clone_command;
  SmProp restart_command;
  enum prop_nums {
    prop_program,
    prop_user_id,
    prop_restart_style_hint,
    prop_pid,
    prop_gsm_priority,
    prop_clone_command,
    prop_restart_command,
    prop_LAST
  };
  SmProp* props[prop_LAST];
  struct passwd* pw;
  char hint = SmRestartImmediately;
  char pid_s[32];
  char priority = 20;

  program.p.name = (char*)SmProgram;
  program.p.type = (char*)SmARRAY8;
  program.p.num_vals = 1;
  program.p.vals = &program.v;
  program.v.value = (char*)"lwm";
  program.v.length = 3;
  props[prop_program] = &program.p;

  pw = getpwuid(getuid());
  user_id.p.name = (char*)SmUserID;
  user_id.p.type = (char*)SmARRAY8;
  user_id.p.num_vals = 1;
  user_id.p.vals = &user_id.v;
  user_id.v.value = pw ? pw->pw_name : NULL;
  user_id.v.length = pw ? strlen(pw->pw_name) : 0;
  props[prop_user_id] = &user_id.p;

  restart_style_hint.p.name = (char*)SmRestartStyleHint;
  restart_style_hint.p.type = (char*)SmCARD8;
  restart_style_hint.p.num_vals = 1;
  restart_style_hint.p.vals = &restart_style_hint.v;
  restart_style_hint.v.value = &hint;
  restart_style_hint.v.length = 1;
  props[prop_restart_style_hint] = &restart_style_hint.p;

  snprintf(pid_s, sizeof(pid_s), "%d", getpid());
  pid.p.name = (char*)SmProcessID;
  pid.p.type = (char*)SmARRAY8;
  pid.p.num_vals = 1;
  pid.p.vals = &pid.v;
  pid.v.value = pid_s;
  pid.v.length = strlen(pid_s);
  props[prop_pid] = &pid.p;

  gsm_priority.p.name = (char*)"_GSM_Priority";
  gsm_priority.p.type = (char*)SmCARD8;
  gsm_priority.p.num_vals = 1;
  gsm_priority.p.vals = &gsm_priority.v;
  gsm_priority.v.value = &priority;
  gsm_priority.v.length = 1;
  props[prop_gsm_priority] = &gsm_priority.p;

  clone_command.name = (char*)SmCloneCommand;
  clone_command.type = (char*)SmLISTofARRAY8;
  clone_command.num_vals = session_argc;
  clone_command.vals = (SmPropValue*)malloc(sizeof(SmPropValue) * session_argc);
  for (int i = 0; i < session_argc; i++) {
    clone_command.vals[i].value = session_argv[i];
    clone_command.vals[i].length = strlen(session_argv[i]);
  }
  props[prop_clone_command] = &clone_command;

  restart_command.name = (char*)SmRestartCommand;
  restart_command.type = (char*)SmLISTofARRAY8;
  restart_command.num_vals = session_argc + 2;
  restart_command.vals =
      (SmPropValue*)malloc(sizeof(SmPropValue) * (session_argc + 2));
  int i;
  for (i = 0; i < session_argc; i++) {
    restart_command.vals[i].value = session_argv[i];
    restart_command.vals[i].length = strlen(session_argv[i]);
  }
  restart_command.vals[i].value = (char*)"-s";
  restart_command.vals[i].length = 2;
  i++;
  restart_command.vals[i].value = client_id;
  restart_command.vals[i].length = strlen(client_id);
  props[prop_restart_command] = &restart_command;

  SmcSetProperties(smc_conn, prop_LAST, props);

  free(clone_command.vals);
  free(restart_command.vals);

  SmcSaveYourselfDone(smc_conn, true);
}

void session_end() {
  if (smc_conn == NULL) {
    return;
  }
  SmcCloseConnection(smc_conn, 0, NULL);
}

void session_die(SmcConn, SmPointer) {
  Terminate(0);
}

void session_save_complete(SmcConn, SmPointer) {}

void session_shutdown_cancelled(SmcConn, SmPointer) {}

void session_init(int argc, char* argv[]) {
  char* previd = NULL;

  session_argv = (char**)malloc(sizeof(char*) * (argc + 2));
  session_argc = 0;
  for (int i = 0; i < argc; i++) {
    if (i != 0 && strcmp(argv[i], "-s") == 0) {
      if ((i + 1) < argc) {
        i++;
        previd = argv[i];
      }
    } else {
      session_argv[session_argc] = argv[i];
      session_argc++;
    }
  }

  unsigned long mask = SmcSaveYourselfProcMask | SmcDieProcMask |
                       SmcSaveCompleteProcMask | SmcShutdownCancelledProcMask;

  SmcCallbacks callbacks;
  callbacks.save_yourself.callback = session_save_yourself;
  callbacks.save_yourself.client_data = NULL;
  callbacks.die.callback = session_die;
  callbacks.die.client_data = NULL;
  callbacks.save_complete.callback = session_save_complete;
  callbacks.save_complete.client_data = NULL;
  callbacks.shutdown_cancelled.callback = session_shutdown_cancelled;
  callbacks.shutdown_cancelled.client_data = NULL;

  char err[256];
  smc_conn =
      SmcOpenConnection(NULL, NULL, SmProtoMajor, SmProtoMinor, mask,
                        &callbacks, previd, &client_id, sizeof(err) - 1, err);

  if (smc_conn == NULL) {
    // This isn't actually an error; treating it as such can cause confusion.
    return;
  }
  if (client_id == NULL) {
    fprintf(stderr, "%s: session manager returned NULL connection\n", argv0);
    return;
  }

  IceSetIOErrorHandler(ice_error);
  ice_conn = SmcGetIceConnection(smc_conn);
  ice_fd = IceConnectionNumber(ice_conn);
}

void session_process() {
  IceProcessMessages(ice_conn, NULL, NULL);
}
