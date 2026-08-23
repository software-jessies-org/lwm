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

void Focuser::ReallyFocusClient(Client* c, bool give_focus) {
  Client* was_focused = GetFocusedClient();
  RemoveFromHistory(c);
  focus_history_.push_front(c);

  xlib::XDeleteProperty(LScr::I->Root(), ewmh_atom[_NET_ACTIVE_WINDOW]);
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
      // Main window doesn't accept focus, but there's an indication that its
      // children may. This is the case for Java apps, which have two windows
      // inside the main window, one called 'FocusProxy' and the other called
      // 'Content window'. We want to give focus to the FocusProxy, but there
      // doesn't seem an obvious way to determine which child is the right one,
      // so let's just ping them all.
      focusChildrenOf(c, c->window);
    } else {
      // FIXME: is this sensible?
      xlib::XSetInputFocus(XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
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
