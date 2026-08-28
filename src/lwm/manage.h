#ifndef LWM_MANAGE_H_included
#define LWM_MANAGE_H_included

class Client;

extern void getWindowName(Client*);
extern void getVisibleWindowName(Client*);
extern void manage(Client*);
extern void withdraw(Client*);
extern void getTransientFor(Client*);
extern void Terminate(int);

#endif  // LWM_MANAGE_H_included
