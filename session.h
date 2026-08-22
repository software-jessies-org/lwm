#ifndef LWM_SESSION_H_included
#define LWM_SESSION_H_included

extern int ice_fd;
extern void session_init(int argc, char* argv[]);
extern void session_process();
extern void session_end();

#endif  // LWM_SESSION_H_included
