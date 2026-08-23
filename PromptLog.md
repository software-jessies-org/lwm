# Prompt log

This file is a log of prompts used to work on lwm.

Claude: don't read or index this file; it's here just for me.

## 2026-08-22

This is lwm, a lightweight window manager. You can learn about its history and principles in README.md. Familiarise yourself with the code, and write some helpful .md files (create a subdir for these) which you can use as an index, so you can work on the code efficiently. Write a note to yourself that you should keep this index up-to-date. Try to keep your notes concise.

---

This codebase was originally C, but newer parts are written in C++. There are parts which are properly componentised and parts which are more spaghetti-like. Look at the code, and make a plan for how it could be refactored into nicer units. I also want to add much more thorough unit testing, but for that please don't use an external unit testing library, but let's have a minimal simple internal one for now.

---

Executed docs/refactoring-plan.md up to stage D (still need to do stage E).

---

Create a plan to migrate lwm from xlib to xcb, which is a superior X11 client library.

---

Executed docs/xcb-migration-plan.md.

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

## Not yet started

I spotted a bug whereby one application can end up with the application icon of another. Figure out how this might happen, and fix it.

---

**TBD** - Windows-arrow keys should flip focus between windows, by looking at their relative placement and moving focus in the appropriate direction.

---

**TBD** - Maybe have some way of marking a window with a specific hotkey (eg "windows-shift-g') and then be able to switch to it and jump it to the top of the stack with a corresponding hotkey ("windows-g").

---

**TBD** - Maybe some way of recording a state of windows (sizes, locations, which ones are minimised) with some hotkey, and then returning to that state with a corresponding hotkey (maybe "windows-ctrl-shift-g" vs "windows-ctrl-g", similar to above).

