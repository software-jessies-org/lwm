#ifndef LWM_FOCUS_H_included
#define LWM_FOCUS_H_included

#include <cstdint>
#include <list>

#include "xlib.h"

class Client;

// The clock the Focuser reads, in milliseconds since some fixed point.
//
// Whether an enter-window notification is acted on at once or deferred turns
// entirely on how long ago the previous one arrived, so the A->B->C race this
// class exists to avoid is a property of the clock. Making it replaceable is
// what lets that be tested, rather than waited for. Defaults to
// CLOCK_MONOTONIC; see focus_test.cc.
namespace focus {

using ClockFn = uint64_t (*)();
extern ClockFn NowMillis;

}  // namespace focus

// The Focuser has the job of ensuring the right window gets focus at the
// right time. It maintains the focus history, so that when a client loses
// focus, it will give focus to the last window to have it.
// It also uses a timer file descriptor to buffer too-fast focus events caused
// by focus-follows-mouse. This is a common X11 race condition caused by the
// mouse moving quickly from window A, across B, and into C. If window B
// supports the 'please take focus' protocol, then it can be that the order of
// events is:
//  1: Mouse enters window B; LWM sends 'please take focus' to B.
//  2: Mouse enters window C; LWM sends 'please take focus' to C.
//  3: Client C reacts first, sending a "I got focus now".
//  4: Client B reacts after, grabbing focus itself.
// Now, rather than window C having focus as one might expect, window B has
// grabbed it. This happens quite often on some computers, if window B is a
// Java app.
// While other window managers solve this by just slowing everything down, or
// polling the mouse location, LWM works around this problem by reacting fast
// to the first 'mouse entered window' notification, and immediately granting
// focus. Then, if a second 'mouse entered window' comes in (for a different
// window) too soon after the first, we use a timer file descriptor to delay
// the processing of this for a configured number of milliseconds (default is
// 50, but it's an Xresource).
// So we get lightning-quick focus changes, while only degrading the speed
// slightly for the A->B->C case and avoiding the race condition.
class Focuser {
 public:
  Focuser();

  // Get hold of the file descriptor we're using to notify the main switch
  // thread that we should trigger delayed focus events. This should be called
  // by lwm.cc, when it's collecting the set of file descriptors it should
  // be switching on.
  int GetTimerFD() { return timer_fd_; }

  // Notification that the mouse pointer has entered a window. This may or
  // may not result in a change of input focus.
  void EnterWindow(Window w);

  // Called by the switch loop in lwm.cc, in response to the timer fd going off.
  void TimerFDTriggered();

  // Forces the focuser to forget the given client (either because the
  // window has been hidden or destroyed). If this was the currently-focused
  // client, focus will be transferred to the previously-focused client.
  void UnfocusClient(Client* c);

  // Forcibly gives focus to a client (possibly because the client requested
  // it, possibly because it's just been unhidden or created). Does nothing
  // if the given client already has input focus.
  void FocusClient(Client* c);

  // Re-asserts the X input focus on a client lwm already believes has it.
  // Reparenting a window unmaps it on the way, and the server hands the input
  // focus back to PointerRoot whenever the window holding it stops being
  // viewable - without anything happening that would change the focus
  // history, so FocusClient() sees a client which is already focused and
  // quite correctly does nothing. Does nothing if c isn't the focused client.
  void ReassertFocus(Client* c);

  Client* GetFocusedClient();

 private:
  int timer_fd_ = -1;
  uint64_t last_entry_time_millis_ = 0;
  uint64_t second_entry_delay_millis_ = 0;
  Window pending_entry_ = 0;

  void RemoveFromHistory(Client* c);

  // Focuses the pending window. This is called either from EnterWindow, or
  // from the main switch loop in lwm.cc, in case of a delayed focus-giving.
  void FocusPending();

  // Does the actual work of FocusClient, except without the safety-check
  // for 'do we currently have focus?'. This is needed to make the
  // refocusing of older-focused clients work when a window closes.
  void ReallyFocusClient(Client* c, bool give_focus);

  // last_entered_ is the last window the mouse pointer was seen entering.
  // It is *NOT* necessarily the window with input focus. In fact, if a new
  // window was opened and was given focus, the pointer may be over a
  // completely different (and unfocused) window.
  Window last_entered_ = 0;

  // The following list contains the history of focused windows. The Focuser
  // is notified of all window destructions, and must keep this list free of
  // Client pointers that are no longer valid.
  std::list<Client*> focus_history_;
};

#endif  // LWM_FOCUS_H_included
