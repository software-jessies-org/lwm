#include "focus.h"

#include <time.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "client.h"
#include "debug.h"
#include "ewmh.h"
#include "lwm.h"
#include "resource.h"
#include "screen.h"
#include "xlib.h"

namespace {

void focusChildrenOf(Client* c, Window parent) {
  xlib::WindowTree wtree = xlib::WindowTree::Query(parent);
  for (Window win : wtree.children) {
    const xlib::WindowAttributes attr = xlib::XGetWindowAttributes(win);
    if (attr.all_event_masks & XCB_EVENT_MASK_FOCUS_CHANGE) {
      LOGD(c) << "  Focusing child " << WinID(win);
      xlib::XSetInputFocus(win, XCB_INPUT_FOCUS_POINTER_ROOT,
                           XCB_CURRENT_TIME);
    }
  }
}

uint64_t RealTimeMilliseconds() {
  struct timespec spec = {};
  clock_gettime(CLOCK_MONOTONIC, &spec);
  return (uint64_t(spec.tv_sec) * 1000) + (uint64_t(spec.tv_nsec) / 1e6);
}

}  // namespace

namespace focus {

ClockFn NowMillis = RealTimeMilliseconds;

}  // namespace focus

// The timer fd is non-blocking because the only read of it happens in
// response to select() saying it's ready. If that ever turns out not to be
// true, the window manager should carry on rather than stop dead inside
// read().
Focuser::Focuser()
    : timer_fd_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)),
      second_entry_delay_millis_(
          Resources::I->GetInt(Resources::FOCUS_DELAY_MILLIS)) {}

void Focuser::EnterWindow(Window w) {
  // There isn't any point in doing anything if we're in click-to-focus mode.
  if (Resources::I->ClickToFocus()) {
    return;
  }
  Client* c = LScr::I->GetClient(w, false);
  // If there's no client, we don't ever give it focus. It's on its own.
  if (!c) {
    return;
  }
  // Normalise the window we're talking about so it's the main client window.
  // This mainly dedups a lot of noise, but including this:
  // 1: Mouse pointer is over window X.
  // 2: Window Y is opened and is given focus.
  // 3: Mouse pointer is moved such that it crosses into a different window in
  //    the client of X.
  // In this situation, window Y should still keep focus.
  w = c->window;
  LOGD(c) << "EnterWindow " << WinID(w);
  if (pending_entry_ && (pending_entry_ == w)) {
    // Yeah, we're working on this one. It's a dup.
    return;
  }
  if (!pending_entry_ && (last_entered_ == w)) {
    // Trying to set focus to same window we're already in, and we're not in
    // the process of waiting to change focus window to pending_entry_. So this
    // is a nop.
    return;
  }
  // At this point, we need the time.
  uint64_t now = focus::NowMillis();
  // Determine whether this is to be an immediate focus change, or we should
  // delay it. We always delay if there's a delayed focus already going on,
  // and we also delay if the last change of focus was too recent.
  const bool should_delay =
      (pending_entry_ != 0) ||
      ((now - last_entry_time_millis_) < second_entry_delay_millis_);
  // Update last_entry_time_millis_, so the next time we're called we use that.
  // This means if we get a whole sequence of mouse-entered events, very close
  // together, the timer will continue to be put off into the future, but
  // that's OK.
  last_entry_time_millis_ = now;
  // Stick w in pending_entry_ so that the code that actually grants focus can
  // always give it to whatever is in pending_entry_.
  pending_entry_ = w;
  if (!should_delay) {
    FocusPending();
    return;
  }
  // Trigger the timer to go off in second_entry_delay_millis_ time to
  // actually change focus.
  struct itimerspec spec = {};
  spec.it_value.tv_sec = second_entry_delay_millis_ / 1000;
  spec.it_value.tv_nsec = (second_entry_delay_millis_ % 1000) * 1000 * 1000;
  timerfd_settime(timer_fd_, 0 /* no flags */, &spec, nullptr);
  return;
}

void Focuser::TimerFDTriggered() {
  // We must read a single uint64_t value from the file descriptor, to silence
  // it and stop it continually pinging the switch loop. The value is a count
  // of expirations, which we don't care about, and a short read only means the
  // timer hasn't fired - the fd is non-blocking. Either way, focus the pending
  // window.
  uint64_t buf;
  const ssize_t ignored = read(timer_fd_, &buf, sizeof(uint64_t));
  (void)ignored;
  // Good. Now actually focus the pending window.
  FocusPending();
}

void Focuser::FocusPending() {
  // Sanity check first.
  if (pending_entry_ == 0) {
    return;
  }
  // Move pending_entry_ to last_focused_, and clear the former. We always
  // assume we're waiting for the timer if pending_entry_ isn't zero, so we must
  // ensure we properly set it back to zero when appropriate.
  last_entered_ = pending_entry_;
  pending_entry_ = 0;
  // The following lookup could fail, because the client might have been
  // deleted, particularly if this is a delayed focus. If we don't find the
  // client now, then just ignore this call.
  Client* c = LScr::I->GetClient(last_entered_);
  if (c) {
    FocusClient(c);
  }
}

void Focuser::UnfocusClient(Client* c) {
  const bool had_focus = c->HasFocus();
  RemoveFromHistory(c);
  if (!had_focus) {
    return;
  }
  // The given client used to have input focus; give focus to the next in line.
  if (focus_history_.empty()) {
    return;  // No one left to give focus to.
  }
  ReallyFocusClient(focus_history_.front(), true);
}

void Focuser::FocusClient(Client* c) {
  // If this window is already focused, ignore.
  if (!c->HasFocus()) {
    ReallyFocusClient(c, true);
    // Old LWM seems to always have raised the window being focused, so let's
    // copy that. Maybe it should be a separate resource option though?
    if (Resources::I->ClickToFocus()) {
      c->Raise();
    }
  } else {
    LOGD(c) << "Ignoring FocusClient request";
  }
}

void Focuser::ReassertFocus(Client* c) {
  if (!c || !c->HasFocus()) {
    return;
  }
  LOGD(c) << "Re-asserting input focus";
  ReallyFocusClient(c, true);
}

void Focuser::ReallyFocusClient(Client* c, bool give_focus) {
  Client* was_focused = GetFocusedClient();
  RemoveFromHistory(c);
  focus_history_.push_front(c);

  // Note that we do not delete _NET_ACTIVE_WINDOW before setting it below.
  // XChangeProperty generates a PropertyNotify whether or not the value
  // actually changed, so the delete bought us nothing, and it briefly told
  // every EWMH client on the display that there was no active window at all.
  // Wine acts on that: it logs "unexpected _NET_ACTIVE_WINDOW (nil)" and
  // deactivates the window it thought was in the foreground.
  // There was a check for 'c->IsHidden()' here. Needed?
  if (give_focus) {
    if (c->accepts_focus) {
      // If the top-level window accepts focus, we must only give focus to it,
      // not to its children. Google Chrome (a web browser) won't work if we
      // also give focus to its children, as it now has a child window which,
      // if given focus, will just drop everything on the floor. The effect of
      // this is to make Chrome windows impossible to type into (nor use
      // hotkeys in) if they lose then regain focus. When a window is newly
      // opened it will respond to keypresses, but not on focus regain.
      LOGD(c) << "Focusing main window " << WinID(c->window);
      xlib::XSetInputFocus(c->window, XCB_INPUT_FOCUS_POINTER_ROOT,
                           XCB_CURRENT_TIME);
      if (c->proto & Ptakefocus) {
        xlib::SendClientMessage(c->window, wm_protocols, wm_take_focus,
                                XCB_CURRENT_TIME);
      }
    } else if (c->proto & Ptakefocus) {
      // This is ICCCM section 4.1.7's "globally active" input model: the client
      // says it doesn't want us to hand it the focus, but it does understand
      // WM_TAKE_FOCUS, which means it wants to decide for itself which of its
      // windows gets it. So tell it, and let it call XSetInputFocus.
      //
      // Every Wine/Proton window works this way (winex11.drv's UseTakeFocus
      // defaults on, which makes it set WM_HINTS input=False and list
      // WM_TAKE_FOCUS in WM_PROTOCOLS), so this is the path every Steam game
      // takes. Without this message the input focus was simply never moved:
      // XGetInputFocus stayed at PointerRoot, Wine never saw itself become the
      // foreground window, and the game got no key events at all.
      LOGD(c) << "Sending WM_TAKE_FOCUS to " << WinID(c->window);
      xlib::SendClientMessage(c->window, wm_protocols, wm_take_focus,
                              XCB_CURRENT_TIME);
      // Java apps are also in this category, but don't act on WM_TAKE_FOCUS in
      // a way that works for us: they have two windows inside the main window,
      // one called 'FocusProxy' and the other 'Content window', and we have to
      // focus the FocusProxy ourselves. There's no obvious way to tell which
      // child is the right one, so ping them all. Clients like Wine's, which
      // keep no children of their own, are unaffected by this.
      focusChildrenOf(c, c->window);
    } else {
      // ICCCM 4.1.7's "no input" model: the client neither wants the focus nor
      // understands WM_TAKE_FOCUS, so there is nowhere to put it - xclock and
      // xload are the classic examples. The focus goes to the root window,
      // which is not the same as putting it nowhere.
      //
      // This used to be XCB_NONE, and that broke every one of lwm's keyboard
      // gestures for as long as such a window was the focused client. With no
      // focus window at all the server discards key events entirely, and - the
      // part that actually bites - a passive grab never activates: XGrabKey
      // fires only when the grab window is the focus window, an ancestor of
      // it, or a descendant of it containing the pointer, and none of those
      // can hold when there is no focus window. lwm's arrow grabs are on the
      // root, so Super+arrow simply stopped existing whenever the pointer was
      // over an xclock. Focusing the root satisfies the first case and costs
      // the client nothing: it asked for no key events and it still gets none.
      xlib::XSetInputFocus(LScr::I->Root(), XCB_INPUT_FOCUS_POINTER_ROOT,
                           XCB_CURRENT_TIME);
    }
  }
  xlib::XChangeProperty(LScr::I->Root(), ewmh_atom[_NET_ACTIVE_WINDOW],
                        XCB_ATOM_WINDOW, 32, &c->window, 1);

  if (was_focused && (was_focused != c)) {
    was_focused->FocusLost();
  }
  c->FocusGained();
}

void Focuser::RemoveFromHistory(Client* c) {
  for (std::list<Client*>::iterator it = focus_history_.begin();
       it != focus_history_.end(); it++) {
    if (*it == c) {
      focus_history_.erase(it);
      return;
    }
  }
}

Client* Focuser::GetFocusedClient() {
  if (focus_history_.empty()) {
    return nullptr;
  }
  return focus_history_.front();
}
