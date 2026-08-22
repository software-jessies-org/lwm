# LWM developer index

Notes-to-self for working on this codebase efficiently. Written for an agent
(or human) who needs to find the right file fast, not a tutorial.

> **MAINTENANCE NOTE (to my future self):** these files are an index, and a
> stale index is worse than none. Whenever you change structure — add/remove a
> source file, move a class, rename a major function, change the build or the
> debug CLI — update the relevant page here *in the same change*. Line numbers
> quoted below are hints, not contracts; treat a mismatch as a signal that the
> page needs a refresh. Keep it terse.

| Page | What's in it |
| --- | --- |
| [code-map.md](code-map.md) | One line per source file + where each major class lives |
| [architecture.md](architecture.md) | Start-up, event loop, event dispatch, who owns what |
| [concepts.md](concepts.md) | Frame vs content rects, edges, focus, struts, visible areas, icons |
| [dev-workflow.md](dev-workflow.md) | Build, test, Xephyr, debug CLI, debugging tools |
| [refactoring-plan.md](refactoring-plan.md) | Staged plan to break up `lwm.h`, tier the code, and add unit tests |

## Project shape at a glance

* C++17, X11 (Xlib), ~8k lines across 17 `.cc` files compiled into one binary.
* Reparenting window manager: every framed client gets an LWM-owned *frame*
  window (`Client::parent`) which draws the title bar, borders and close cross.
* Three global singletons: `dpy` (Display*), `LScr::I` (screen + client
  registry), `Resources::I` (Xresources config). Nearly everything reaches them
  directly.
* No hotkeys, no virtual desktops, no compositing, no colourmaps — see
  `../README.md` for the philosophy behind those omissions.
* Feature requests and known bugs live in `../BUGS` (it's a TODO list, not just
  bugs). `../ChangeLog` is history.
