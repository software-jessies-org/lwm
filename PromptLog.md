# Prompt log

This file is a log of prompts used to work on lwm.

Claude: don't read or index this file; it's here just for me.

## 2026-08-22

This is lwm, a lightweight window manager. You can learn about its history and principles in README.md. Familiarise yourself with the code, and write some helpful .md files (create a subdir for these) which you can use as an index, so you can work on the code efficiently. Write a note to yourself that you should keep this index up-to-date. Try to keep your notes concise.

---

This codebase was originally C, but newer parts are written in C++. There are parts which are properly componentised and parts which are more spaghetti-like. Look at the code, and make a plan for how it could be refactored into nicer units. I also want to add much more thorough unit testing, but for that please don't use an external unit testing library, but let's have a minimal simple internal one for now.

---

*Executed docs/refactoring-plan.md.*

---

Create a plan to migrate lwm from xlib to xcb, which is a superior X11 client library.

---

*Executed docs/xcb-migration-plan.md.*

---

## 2026-08-23

Add the following mouse actions, which act when the user performs a mouse action on a window background with the 'Windows' key held down:
* Left button click+drag: move the window.
* Right button click+drag: resize the closest window edge or corner. The edge/corner should be calculated by dividing the window's area into a 3x3 grid - the central square does nothing; the top-left square resizes the top-left corner; the middle-right square resizes the right edge, etc.
* Left button double-click: expand the closest window edge or corner (as defined in the 'right button click+drag' item above) to the near edge(s) of the closest window(s) in the given direction(s) (or to the inside of the monitor, whichever is closer). If in the central square of the 3x3 grid, expand all edges in the same way. When picking the inside of the monitor, take care of the xrandr monitor configuration - don't expand across monitor bounds.
* Right button double-click: same as the 'left button double-click' above, except always expand to the inside of the monitor; ignore other windows.

Add UI tests, using Xvfb.

---

Bug regarding hidden windows - when LWM starts up, it makes visible the window that Gummiband (source code is in ~/dev/jessies/x11-extras/gummiband.cpp) uses to display its menu, except the menu should be closed. If I then open the menu in gummiband and close it again, the window ends up hidden. It's only when lwm starts up and the window should be hidden that it erroneously makes it visible.

---

Change the mouse button actions over the client window (the ones with the 3x3 grid, enabled with the Windows key) so that the *middle* mouse button performs the actions previously given to the right button. Instead, Windows+right-button should hide the window (much like a right-button click on the window furniture does).

---

If we see a Windows+left **click** (with no movement), we should raise the window.

---

There's a bug in our struts handling. The gummiband program (~/dev/jessies/x11-extras/gummiband.cpp) defines a strut to keep windows off its own window (at the top of the screen), and lwm respects that *sometimes*. From what I can tell, it's to do with the start-up order: if gummiband starts first, lwm doesn't respect the strut; if lwm starts first (or if I restart gummiband), lwm does respect the strut. Diagnose and fix, and add a regression test using Xvfb.

---

LWM needs to work well with steam games. The main steam UI seems to work fine, but there are some problems with the games. For example, when I start Subnautica 2, the full-screen window initially shows only on the right half of my main monitor. I suspect this is because the width of the left-hand monitor is being added as an offset onto the main monitor (which is on the right), but I don't know that for sure. When I try to move the game window, it jumps to kind of the right place, but the window decorations then push it down and to the right, so that the title bar is *kind of* visible (except it gets drawn as plain white).

Also, key events aren't being propagated into the game - this is a serious problem.

If you can figure out the causes, please fix the issues. If you need more info, then add sufficient logging so that I can manually launch an extra-logging version of lwm and then you can diagnose based on the logs.

---

*A load of follow-up conversation which diagnosed and fixed a bunch of weird
goings-on in wine land.*

---

The main steam launcher window has no window decorations. It's one of those apps that provides its own close button and resize widget. However, I also can't move or resize it, as its own widgets aren't responding. I also can't resize or move it using lwm's 'windows key plus mouse drag/click' features. Diagnose this, and figure out how best to fix it. The steam launcher window is currently open (visible on the left-hand display).

---

In our last discussion, you uncovered a bug introduced during the xlib->xcb transition, caused by function call argument order changing between the two APIs, and the LWM code not being correctly modified. Go through all of the LWM code that touches xcb, and check that the argument ordering is correct.

---

We identified that lwm doesn't publish _NET_FRAME_EXTENTS. We should do that.

---

Extend the 'Windows-double-click' in the middle of a window - currently it maximises the window, but if the window is already maximised, I want it to restore it to its pre-maximised size. This will involve storing the pre-maximised coordinates when maximisation takes place, so it can be restored. Ensure that the de-maximisation code properly checks for whether we have a pre-maximised rectangle, so that if it's missing we don't open the window at silly locations/sizes.

---

I want a new hotkey: windows-ctrl-click on a client window will toggle whether it has lwm decorations. The client window will resize so that the outer extent is preserved.

---

When using the Windows-Ctrl-Click functionality to toggle window decorations, while some applications (eg grezvany, rhythmbox, chrome) react properly, keeping the external extent of the window the same as where the decorations where (effectively resizing the client window), some apps (eg Evergreen, which is a java app) retain their original client window size, effectively moving the client window up and to the left, to be rooted at the corner previously occupied by the top-left of the window decorations. Figure out how to solve this problem - it's a weird difference in behaviour between different X clients, and looks really weird.

---

I want to add key handling for switching input focus between windows. When the Windows key is held down, the arrow keys should pick the next visible window in the indicated direction, and move input focus to that window. The 'next visible window' is determined according to the centre of the window in relation with the centre of the window that currently has focus. However, the choice of windows is restricted by direction - if, for example, we hit Windows+left, the middle-X of the target window must be smaller than the middle-X of the window currently with focus, but also the absolute delta-X must be equal or larger than the absolute delta-Y between the two windows. In this way, the possible windows have their centres in a cone whose point is the centre of the window currently with focus, and which opens towards the direction indicated by which arrow is pressed.

---

The mouse pointer is too small on my main monitor when lwm decides how it should be drawn. On apps like grezvany and Java apps a larger mouse pointer is shown, but lwm shows it as tiny. Why is that, and how should we best fix it?

---

## 2026-08-24

I want you to make several window-moving changes:
* Windows-shift-arrow should move the current window in that direction, to the inner edge of the monitor it's on in the direction of the arrow. If the window is already at the inner edge, it should be moved to the monitor to the left (in multi-monitor displays.
* Dragging (or moving with windows-shift-arrow) a window into a monitor that is too small to take it should resize the window so it fits.

---

## 2026-08-25

Continued discussion on ctrl+shift+windows moving windows across different monitors, to make it work more naturally.

---

I spotted a bug whereby one application can end up with the application icon of another in its title bar display. Figure out how this might happen, and fix it. I don't know what exact steps are needed to reproduce it.

---

Change the way Windows-arrow works such that the mouse pointer also warps to the window that receives input focus. Place the mouse pointer in the middle of the largest contiguous visible rectangle, taking into account occlusion by other windows.

---

## Not yet started

In a previous discussion, you reported this:

One unrelated thing I tripped over: running lwm -debugcli=... < /dev/null spins the event loop on stdin-at-EOF — it wrote 3.4 GB to stdout in about a minute and filled /tmp. It's pre-existing (the unpatched binary does it too) and only affects that invocation, so I left it alone, but you may want it fixed.

Fix this.

---

**TBD** - Maybe have some way of marking a window with a specific hotkey (eg "windows-shift-g') and then be able to switch to it and jump it to the top of the stack with a corresponding hotkey ("windows-g").

---

**TBD** - Maybe some way of recording a state of windows (sizes, locations, which ones are minimised) with some hotkey, and then returning to that state with a corresponding hotkey (maybe "windows-ctrl-shift-g" vs "windows-ctrl-g", similar to above).

