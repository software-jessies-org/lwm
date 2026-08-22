# Prompt log

This file is a log of prompts used to work on lwm.

Claude: don't read or index this file; it's here just for me.

## 2026-08-22

This is lwm, a lightweight window manager. You can learn about its history and principles in README.md. Familiarise yourself with the code, and write some helpful .md files (create a subdir for these) which you can use as an index, so you can work on the code efficiently. Write a note to yourself that you should keep this index up-to-date. Try to keep your notes concise.

---

This codebase was originally C, but newer parts are written in C++. There are parts which are properly componentised and parts which are more spaghetti-like. Look at the code, and make a plan for how it could be refactored into nicer units. I also want to add much more thorough unit testing, but for that please don't use an external unit testing library, but let's have a minimal simple internal one for now.

---

Create a plan to migrate lwm from xlib to xcb, which is a superior X11 client library.

---

Add the following mouse actions, which act when the user click-drags on a window background with the 'Windows' key held down:
* Left button: move the window.
* Right button: resize the closest window edge or corner. The edge/corner should be calculated by dividing the window's area into a 3x3 grid - the central square does nothing; the top-left square resizes the top-left corner; the middle-right square resizes the right edge, etc.

Add UI tests, using Xvfb.

---

