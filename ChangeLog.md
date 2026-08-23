# Change Log for "lwm"

## 2026-08-23 (pn, Basel)

The mouse pointer now comes from the user's cursor theme instead of the core
X11 "cursor" font. That font is a fixed-size bitmap font from the 1980s: it
exists at exactly one size, around 16 pixels, and nothing scales it, so lwm
drew a tiny pointer over its own frames and the root window while every window
belonging to a GTK, Qt or Java application showed the large themed one the
toolkits load through libXcursor. On a high resolution display the difference
was hard to miss.

`CursorMap` now asks `xcb-cursor` - the XCB reimplementation of libXcursor -
for each cursor by name, so the theme comes from `XCURSOR_THEME` or the
`Xcursor.theme` resource and the size from `XCURSOR_SIZE`, `Xcursor.size`, or
failing both a size derived from the screen height. lwm therefore matches
whatever the rest of the desktop is doing without needing any setting of its
own. The names used are the traditional X ones ("left_ptr", "fleur",
"top_left_corner" and so on) rather than the newer freedesktop spellings,
because themes reliably ship those, and because `xcb_cursor_load_cursor` knows
how to fall back to the corresponding core font glyph for any name the theme
turns out not to have.

The cursor foreground and background colours are gone with the font: theme
cursors are full-colour images and there is nothing to recolour.


## 2026-08-23 (pn, Basel)

Added keyboard navigation of the input focus: holding the 'Windows' key
(Super/Mod4) and pressing an arrow key moves the focus to the next window in
that direction. Windows are compared by the centres of their frames, and the
one chosen is the nearest whose centre lies in the quarter-plane cone which
starts at the centre of the focused window and opens towards the arrow
pressed - so the delta along the arrow's own axis has to have the right sign,
and be at least as large as the delta along the other one. A window which is
mostly above or below the focused one is therefore not selected by Left or
Right however far to the side it also is, and a window exactly on the diagonal
belongs to both of the cones which meet there, so no window is unreachable.

Windows on other monitors are candidates too, which makes this the way to move
the focus between screens; hidden and withdrawn windows are not. If there is
no window in the direction pressed, the focus stays where it is. Only the
focus moves: the window is not raised, and the pointer is not warped.

This is lwm's first key binding, and the first passive grab it places on the
root window rather than on a client. The grab is repeated under both lock
modifiers, as the mouse gestures are, and is re-taken on MappingNotify, since
a change of keyboard layout can move the keycode an arrow sits on.


Added mouse gestures on the 'Windows' key (Super/Mod4), which act on a
window's own background rather than on its frame: left-drag moves, middle-drag
resizes the edge or corner picked out by a 3x3 grid over the window, and a
double click expands that edge (all four, from the middle of the grid) up to
the nearest window or the edge of the monitor - or, on the middle button,
straight to the monitor, ignoring other windows. Multi-monitor xrandr layouts
are respected: nothing expands across a monitor boundary.

The double click in the middle of a window is a toggle: once the window has
nowhere left to grow, the same gesture shrinks it back to the size it had
before it was expanded, and a window which is maximised because it asked to be
is un-maximised. A window with no earlier size on record is left where it is.

Super+Control+left click on a client window turns lwm's window furniture on or
off for it. The window's outer extent is preserved, so a window which loses
its title bar and borders grows into the space they were using, and one which
gains them shrinks to make room; nothing else on the desktop moves. This works
in both directions on any ordinary window, including ones lwm chose not to
decorate because the client said it draws its own title bar. Full-screen
windows, and window types which never get furniture in the first place, are
left alone. Turning the furniture off and on again puts the window back at
exactly the size it started at, even for a client like xterm which only
resizes in whole character cells.

Taking a window's furniture away no longer moves the window out of its frame:
the frame stays, shrunk to the size of the window inside it, so nothing of it
shows. Moving a window out to the root is how a window manager announces that
it has stopped managing that window, and clients which believe it stop
trusting the geometry they're given afterwards. Java applications were the
visible casualty: instead of growing into the space the title bar had been
using, they kept their old size and slid up and to the left into it.

Super+right click anywhere within the client window hides the window.

Super+left click on the client window raises the window.

Fixed a bug where a restart of lwm made gummiband's closed drop-down menu
reappear.

Fixed a pile of bugs related to steam games.

Hiding an undecorated window now works. Hiding used to unmap the frame and let
the client window inside it go too, which for a window with no frame meant
asking the server to unmap the root - which it ignores, leaving the window on
screen, marked hidden, and unreachable from the unhide menu. Undecorated
windows also appear in the unhide menu now, alongside the decorated ones.

lwm now publishes `_NET_FRAME_EXTENTS` on every window it manages, and
advertises it in `_NET_SUPPORTED`. This is how a client finds out how much
space the title bar and borders take up around it; Wine, GTK and Chromium all
read it, and without it they have to assume zero and end up a title bar out
when they position or size themselves. Undecorated and full-screen windows
report zeroes, as they should.


## 2026-08-22 (pn, Basel)

LWM is entering the AI era. I'd given up trying to fix it, because Xlib is just
so damned gnarly and poorly-documented. However, Claude is pretty good these
days, and I'm pretty confident it's going to be able to help me get this thing
dusted off and working well. So let's have a go.


## 2022-08-13 (pn, Basel)

Debugged weird position effects when entering and leaving full screen
mode. Now, at least, chrome can enter and exit full screen mode without
getting weird window positions. This is really figuring out correct
behaviour by frobbing though - the X11 protocol is peculiar.


## 2020-07-04 (pn, Basel)

Added a delay timer which kicks in when focus-follows-mouse changes the
focus recipient too quickly. This can cause a race condition in X11,
so that the wrong window ends up stealing focus.


## 2020-06-13 (pn, Basel)

Added ability to run user-configured commands on an alt-click on the
title bar of a window. By default (assuming the new force_title.sh and
setvisname programs are on the user's path), alt-left-click on the title
bar of a window will pop up a zenity dialog box, and allow the user to
enter a new name for the window. This is then set as the
_NET_WM_VISIBLE_NAME property on the window, which LWM will then display
in preference to the client's own provided name.
This is mainly so that annoying web apps which animate the window title
can be muted. The 1Hz constant flapping that Google Hangouts Chat keeps
up has been annoying the hell out of me for months now.


## 2020-05-22 (pn, Basel)

Wrote 'ruler', a debug aid that displays a 2d ruler on the root window.
Fixed a few bad window lookup bugs, which were dumping stack traces.


## 2020-05-19 (pn, Basel)

Major refactor of how window sizing, movement is dealt with. Should fix
a lot of little bugs.


## 2020-04-26 (pn, Basel)

Moved geometry declarations from lwm.h to geometry.h.

Added CreateNamedWindow function in xlib.(cc|h), which does the normal
create window call but also assigns it a name. This means we can now
see what each LWM window is for in 'xwininfo -root -tree'.


## 2020-04-25 (pn, Basel)

Gave xlib.(cc|h) a namespace, and added there a bunch of shim functions
which call same-named Xlib functions. Changed code around LWM to call
through these new shim layers. The idea is to make it easier to track
events sent to clients, and thus make it simpler to debug problems such
as the disappearing ImageMagick windows.


## 2020-04-18 (pn, Basel)

Deleted support for colormaps. 24bpp has been standard for decades.


## 2020-04-15 (pn, Basel)

Was bitten by a Google Chrome update, which caused Chrome windows to
drop all keyboard input for windows which weren't freshly opened.
Figured out a fix for it, which still works for Java.


## 2019-09-15 (pn, Basel)

Stopped 'withdrawn' windows from showing up in the unhide menu. This
solves a bug where the rhythmbox preferences dialog would remain in the
unhide menu even when closed, because the window was never destroyed,
merely withdrawn (and reused again when next opened).


## 2019-09-01 (pn, Basel)

Removed the poisonous 'PointerMotionHintMask', which was causing us to
miss out on mouse motion events, and preventing the cursor from being
properly updated according to which bit of the window furniture it was
on. Also fixed a silly bug where we'd fail to switch to the
'move window' cursor should the pointer move from the client window
into the title bar.

Got rid of the long-standing bug where we'd allow the window to be moved
if the mouse pointer is on the furniture window's 1px border.


## 2019-08-29 (pn, Basel)

Got rid of static functions to improve debugging.

Added backtrace printing (for linux only so far) on X proto errors.

Added reference counting and proper clean-ups of image icons for deleted
windows. This fixes a pixmap leak, and also some bad over-cacheing
behaviour where sometimes apps get each other's icons.


## 2019-05-25 (pn, Basel)

Fixed sloppy divide-by-zero bug in xrandr window moving code.


## 2019-05-20 (pn, Basel)

New algorithm for calculating where to position windows after a change
of screen layout.

This also introduces 'tests.cc', LWM's first ever unit tests! They're
not using any standard library, and are compiled into the main binary
(call by running 'lwm -test'). This isn't strictly proper unit testing,
but it means I don't need to bother with a more complicated Makefile.
And since LWM really isn't set up for easy unit testing (still having
nearly everything in lwm.h), doing things properly would involve serious
refactoring. Anyway, as far as I can tell, adding the tests in this way
increases the binary size by 71KiB, which isn't going to hurt anyone.


## 2019-05-07 (pn, Basel)

Fixed calculation of the title bar bounds, so that we don't have a
little square at the right edge of the title bar, which is a resize
widget when it should be a move one.

Fixed the parsing of numeric Xresources so they don't treat all 0 values
as being parse failures. This allows the top-of-window size-change area
to be switched off (previously it would have fallen back to the
default).


## 2019-05-05 (pn, Basel)

Added back the ability to resize windows by dragging the top edge of the
window frame. There's a new xresource called 'topBorder' which describes
how many pixels thick this is (so it can be a different thickness to the
normal window border); setting this to zero makes the whole title bar
a move, not resize, widget. JFC likes resizing from the top, so this
should make this branch of LWM more appealing.

Added an 'appIcon' Xresource, which allows application icons to be
enabled or disabled for title bars and the unhide menu.

Fixed another little bug where LWM was moving menu's window on shutdown.
This was caused by Client_MakeSane always subtracting textHeight() from
the Y position, irrespective of whether the client is framed. The
treatment of window sizes *really* needs more structure.


## 2019-05-03 (pn, Basel)

Fixed another silly bug where strutted windows would be forcibly pushed
out of their reserved areas.


## 2019-05-02 (pn, Basel)

Fixed a silly bug where strutted windows would be forcibly pushed out of
their own reserved areas on an xrandr screen change.


## 2019-05-01 (pn, Basel)

Added 'xdbg.cc', a simple debug tool to measure locations on screen.
Useful for debugging mouse positioning issues with LWM.

Finally fixed LWM's issues with popup windows being opened outside
the visible area of the screen (happens after a few laptop external
screen add/remove sequences, with evergreen and terminator).


## 2019-02-27 (pn, Basel)

Embellished debug handler a bit, added support for enabling debug
logging per client. Should help me figure out various bugs.


## 2019-02-23 (pn, Basel)

Rewrite of the click/drag handling. This gets rid of the global vars
tracking which 'mode' we're in, and which window is being dragged and
from where. Instead, on commencement of a drag we create an instance
of a class which knows how to handle the specific action.
Because we're no longer assuming the window with input focus is the one
being dragged, this eliminates a whole bunch of annoying bugs, like a
new window appearing during a drag causing the new window to suddenly
be the one being moved.


## 2019-02-09 (pn, Basel)

Rewrite of the way we handle xrandr notifications, so we can support odd
shapes caused by multiple screens (which can cause inaccessible areas).


## 2019-01-09 (pn, Basel)

Fixed a regression in the way the mouse pointer's cursor was set, such
that we don't keep showing the 'resize corner' icon (or whatever LWM
last set) when the pointer is hovering over the client window. This was
particularly evident in clients that aren't too militant in their cursor
setting, like Rhythmbox for example.


## 2019-01-02 (pn, Basel)

Discovered XSetIconSizes, and used it to specify LWM's preferred icon
size range. This prevents Java from using butt-ugly down-sampling to
squeeze the image into 16x16 pixels (which is the default if no
WM_ICON_SIZE property is present). Now Evergreen and Terminator app
icons look much nicer (although we could do better by not down-sampling
ourselves).

Changed window icon handling so we create 3 scaled-down versions, for
the menu, active and inactive window titles. The scaling now uses
simple anti-aliasing (made possible by the baked-in alpha channel).
This makes window icons look *MUCH* smoother.

Added a little extra padding in the unhide menu, so that the mouse-over
highlight extends a little to the left and right of the text. Just a
minor prettiness improvement, but it does look nicer.

Added back click-to-focus mode. Also added a testXresources file, so
the interpretation and support for the various Xresources can be easily
tested.

Fixed a regression which caused unhidden windows to appear lower than
they should be.


## 2019-01-01 (pn, Basel)

Added support for fetching the window icon from the _NET_WM_ICON
property. This method of specifying icons is used by Chromium/Chrome.

Added (cached) auto-rescaling of window icons, to shrink them down to
the size we want for displaying. This makes Firefox and Chromium icons
look like they should, rather than just a tiny swatch from their middle.

Close icon now still drawn for inactive windows, but with a different
colour (defaults to the same grey as inactive window titles).


## 2018-12-31 (pn, Basel)

Replaced the ugly global 'current' pointer use (and static 'last_focus')
for tracking input focus with a proper focus-tracking class, that can
maintain more than one item of history. By cleaning the focus-tracking
code up, the annoying Terminator focus bug now goes away.

Changed the close icon action bounds to be larger, down to the client
window and across to the title bar area, which works more nicely
particularly on large 4k screens.


## 2018-12-08 (pn, Basel)

Updated this ChangeLog from the log I'd been keeping elsewhere.

Updated BUGS and other files. Will be tracking bugs/feature ideas in
the BUGS file from now on. It contains some useful debugging tools too.

Made a bunch more colours configurable, including all the text, popup
background and foreground.


## 2018-12-07 (pn, Basel)

Updated man page.

Fixed treatment of pending windows so we keep track of the frame
window ID, rather than a pointer to a Client that might disappear from under us.


## 2018-12-01 (pn, Basel)

Lots of changes. Give it nicer colours than black+white, added simple
icon support, rewrote hiding logic, etc.

Added display of a red box showing where the to-be-unhidden window is,
when the pointer is hovering over the Hider menu.


## 2018-09-10 (pn, Basel)

Fixed an annoying crash bug that can happen on closing windows, due to
global Client pointers being kept around.

Sanitised header files a bit, adding include guards and moving most of
the X11/ .h includes to xlib.h.


## 2018-09-02 (pn, Basel)

Hardened calls to XGetWindowProperty a bit, zero-initialising stuff.
This is an attempt to fix some random occasional crashes I see on Linux
when restoring colour maps.

Added some padding to the 'hidden windows' menu, as the contents was
hard to read.


## 2018-08-24 (pn, Basel)

Fixed the Y positioning of title bar text - it was a bit low.

Changed the pop-up menu to use freetype fonts.

Changed the menu text creation to handle UTF8 truncation properly.

Changed the menu text creation to run each time it's needed, to cope
better with screen size changes.


## 2018-08-18 (pn, Basel)

Fixed crash bug, caused by not creating a Client in the C++ way.

Changed title bar font rendering to use freetype - looks much nicer.

Removed special cases for where three's no UTF8 support - there always
is these days.


## 2018-05-18 (pn, Basel)

Close the 'resizing window dimensions' window if we flip out of weird
dragging mode.


## 2018-05-13 (pn, Basel)

Switched Bool/True/False to bool/true/false, and simplified some code.

Moved away from global variables for Resources.

Switched from old C strings to std::string for window names.


## 2018-05-10 (pn, Basel)

Broke out XQueryTree into a C++ function which can return a struct
which is initialised safely, and has a save 'destructor' (C-style;
switching to C++ entirely will take time).

Renamed all .c files to .cc, and fixed the code up so it worked with
g++ (eg 'class' -> 'c_class').


## 2018-05-03 (pn, Basel)

Fixed a crash bug where an uninitialised variable was iterated over
(last arg to XQueryTree).


## 2018-05-01 (pn, Basel)

Always give focus to newly-opened windows. This fixes the annoying
problem of newly-opened Chrome windows being unfocused, and me entering
random URLs and search terms into GMail.

Fix last-focused restoration, so that closing a just-opened window
(eg ctrl-w in chrome) gives focus back to where it just came from.


## 2018-04-27 (pn, Basel)

Changed window drag code to auto-disable if the mouse buttons aren't
down. Should fix the random-drag bug.

Refactored the getMousePosition function so it returns a little struct
with x, y, and now modMask.


## 2018-04-16 (pn, Basel)

Added support for the desktop dimensions changing, with auto window
rearrangement. Now I can use LWM seamlessly on my work laptop :-)


## 2018-04-15 (pn, Basel)

Fixed annoying resize bug where a window's bottom-right edge would zoom
away from the screen edge if the window was resized from the top-left
corner to violently.


## 2018-04-08 (pn, Basel)

Changed 'unhide' menu to include visible windows, separated from the
hidden ones with a dotted line.

Added more nice event debugging printers.

Fixed: opening new windows didn't display them as having focus.


## 2018-04-07 (pn, Basel)

Added some debugging, so I can see what's happening to it.

Added a --debug-events flag, to switch on/off detailed event display.

Fixed the bug which prevented Terminator's find box from opening.


## 2018-03-25 (pn, Basel)

Created my own LWM branch.

Wrote better sloppy focus fix, got it pushed to github.

A few minor free statement improvements.


## 2018-03-24 (pn, Basel)

Figured out cause of sloppy focus bug for Java, made first hacky fix.


## 2016-02-12 (jfc, York)

Patches from Greg Kenneky and other fixes for fullscreen windows.

Released lvm-1.2.4.

## 2013-07-09 (jfc, York)

Applied a couple of minor patches suggested by Jari Aalto, the
Debian package maintainer.

Released lwm-1.2.3.


## 2009-11-24 (jfc, York)

Released lwm-1.2.2.

## 2009-11-20 (jfc, York)

Improved performance by only checking for pending X events when the
socket it ready for reading.

Fixed applyGravity() bug that caused frameless windows to be
mis-positioned.

Applied a workaround in destroy() to avoid error reports when closing
windows.

## 2005-01-28 (jfc, York)


Applied a patch from Chris Reece that ensures that the popup
menu does not disappear off the bottom of the screen.

## 2004-09-30 (jfc, York)


Fixed an issue with IRIX 6.5 and lwm, where the root menu could
not be used because motion events had coordinates with respect to
the popup, not the root. Fixed by explicitly using the root window
coordinates.

Released lwm-1.2.1.

## 2004-09-28 (jfc, York)


Added missing -lSM to no_xmkmf_makefile.

## 2003-12-09 (jfc, York)


Fixed bug (reported by Matthew Wilcox) where windows with extremely
long names could cause the pop menu to be unusable. Fixed by
maintaining a separate, shortened name for the menu, if
necessary ("this is a very very [...] ry long window name"). This
takes no account of UTF-8 names as yet.

## 2003-12-08 (jfc, York)


Fixed bug (reported by Eugene Wong) where resizing the top of
a window would cause it to jump up several pixels. The height of
the titlebar was not being considered when calculating mouse
motion in reshaping_motionnotify().

Modified manage() to avoid autoplacing windows during
initialisation.

Released lwm-1.2.0.

## 2003-12-03 (jfc, York)


Applied patch from Elliott that gives focus to new windows in
click-to-focus mode.

Changed the buttonpress code in disp.c to ignore scroll wheel
"clicks".

Modified Client_Remove so that, in click-to-focus mode, it
refocuses on the most sensible window (either the top window,
or the window that the closing window was a transient for).

Attempted to fix the edit placement bug by adding titleHeight()
to the supplied X coordinate during a ConfigureRequest event,
and not attempting to fix clients that don't supply a border
width during a configure request.

Fixed fullscreen-mode bug where galeon windows appeared to jump
up and to the left after the first click.

Released lwm-1.1.7.

## 2003-11-28 (jfc, York)


Changed the behaviour when unhiding a window in click-to-focus mode.
An unhidden window now automatically gets focus in this mode.

Fixed a small bug in the session management code that could
cause a crash when lwm quit.

Moved a call to ewmh_set_client_list make before ewmh was
initialised.

Changed lwm's behaviour when minimising windows. Button three must
now be pressed and released before the window is hidden (or
moved to the bottom of the stack). This ensures that lwm swallows
all the events generated during the operation, and allows the user
to back out of the operation by moving the mouse out of the window
before releasing the button.

Removed include of Xm/MwmUtil.h in manage.c, and the HAVE_MOTIF
kludge from the Imakefile, in favour of copying the few lines
that are required from Xm/MwmUtil.h (LessTif, so hopefully no
licensing issues).

Fixed bug that caused the last cursor displayed in a frame
to be incorrectly used when moving into the frame when the
root menu was on screen. This is done by brute force -
see Client_ResetAllCursors().

Released lwm-1.1.6.

## 2003-11-26 (jfc, York)


Fixed bug in Client_MakeSane that caused occasional crashes
during window moves/resizing. Should investigate why it occasionally
gets called with a NULL client.

## 2003-11-03 (jfc, York)


Added an entry for LeaveNotify in the dispatch table (disp.c).

## 2003-08-13 (jfc, York)


Removed "error" message when lwm fails to connect to a session
manager. This isn't actually an error and the message is confusing.

## 2003-08-01 (jfc, York)


Fixed bug that allowed clients to grab the focus and confuse lwm.

Cleaned up the code for raising and lowering clients, and added
code to prevent a client from being raised above its transients.

Retired disp.old, and CLOSE_PATCH.txt.

Added an edge resistance to the workarea, so that window may
be moved to the edge of the workarea without precise mousing,
as requested by MAD. EDGE_RESIST in lwm.h defines the number of
pixels of resistance and may be safely set to zero.

Released lwm-1.1.5.

## 2003-07-31 (jfc, York)


In click-to-focus mode, always draw the box in the frame.

## 2003-07-29 (jfc, York)


Added a click-to-focus mode. The default remains (sloppy)
enter-to-focus.

Released lwm-1.1.4.


## 2003-07-28 (jfc, York)


Updated no_xmkmf_makefile to reflect the changes made since 1.01.

## 2003-07-10 (jfc, York)


Fixed a bug in manage.c than prevented lwm compiling on systems
with no variety of Motif installed. If this means you, remove
_DHAVE_MOTIF from Imakefile.

Released lwm-1.1.3.

## 2003-07-08 (jfc, York)


Added support for NET_MOVERESIZE, but I cannot find any
applications the want to use it, apart from the keyboard
variants. I don't know what to do about the keyboard move/resize.

## 2003-07-03 (jfc, York)


Fixed a few buglets thrown up by running lwm through the compiler
with all warnings on.

## 2003-07-02 (jfc, York)


In Client_MakeSane(), added a check to prevent windows being
moved into a position where they might be completely obscured
by panels/docks.

Changed ewmh_set_strut() to run Client_MakeSane() across all
clients when the work area changes. This avoids clients getting
lost behind panels/docks.

Added support for _NET_WM_STATE_ABOVE and
_NET_WM_STATE_BELOW. Added fix_stack() to maintain the window
stack as dictated by the EWMH spec.

## 2003-07-01 (jfc, York)


Added support for _NET_WM_STRUT. lwm now maintains _NET_WORKAREA
correctly, and takes the reserved space into account in its
window placing algorithm.

Released lwm-1.1.2.

## 2003-06-30 (jfc, York)


Fixed bug that caused tk menus to be badly placed placed by
sending a configure notify where appropriate in setactive().

Removed compile time option of prepending window title's with
the client machines's name (PREPEND_CLIENT_MACHINE).

Added i18n support for window titles, using UTF8 names from
_NET_WM_NAME where available and supported (ie XFree86).

Added code in disp.c to change the pointer in some areas of the
frame to indicate the action taken by button1. I didn't allow the
"move" pointer in the titlebar because it looked nasty. Added
the xkill pointer for the the box. This was a TODO item.

## 2003-06-28 (jfc, York)


Added GPL headers to all the source files.

Released lwm-1.1.1.

## 2003-06-27 (jfc, York)


Fixed the bug where each GTK window generated an extra
window when lwm shut down by unmapping all the clients in
Client_FreeAll(). Elliott thinks this is bad magic, and that
the X server should lose the windows, but this doesn't happen
with XFree86.

Fixed bug, reported by Ed Porter, that caused moving the mouse
wheel to generate xterms. Wheel mice generate button press events
on buttons 4 and 5 and shell() wasn't taking this into account.

Fixed silly bug in motifWouldDecorate(): windows should have a
frame is MWM_DECOR_ALL is set.

## 2003-06-26 (jfc, York)


Shaped windows now work again. I'm not sure what I changed
to break it, but the fix was to process shaped windows in
scanWindowTree (they were previously ignored). They had to be
clients anyway, if they were to appear in _NET_CLIENT_LIST.

## 2003-06-25 (jfc, York)


Fixed bug that caused frameless windows to be immoveable.

In manage.c, allowed lwm to fall back on Motif hints when
deciding if a window should have a frame, if _NET_WM_WINDOW_TYPE
is not set. This breaks the EWMH spec, in that a window
without _NET_WM_WINDOW_TYPE should be assumed to have
_NET_WM_WINDOW_TYPE_NORMAL, but it's the only way for older
apps to indicate that they don't want decorating, and in the
absence of Motif hints the default state is
_NET_WM_WINDOW_TYPE_NORMAL.

## 2003-06-24 (jfc, York)


Fixed the following TODO item:
	allow users to back out of closing a window if
	they leave the box before letting go of the button.
Implemented by adding an extra wm_closing_window mode rather
than adding to the Client structure, as per AMidthune's
patch. Not sure which is the better solution, though.

Added initial support for _NET_WM_STATE, but only for
_NET_WM_STATE_SKIP_TASKBAR, _NET_WM_STATE_SKIP_PAGER and
_NET_WM_HIDDEN.

Added simple hardwired _NET_WM_ALLOWED_ACTIONS support, and
support for the _NET_CLOSE_WINDOW client message.

First attempt an _WM_STATE_FULLSCREEN and a full-screen mode.
It's not quite right yet, but useable.


## 2003-06-23 (jfc, York)


Fixed some silly bugs in the session management code.

Added initial EWMH code using the 1.2 spec:
http://www.freedesktop.org/standards/wm-spec/1.2/html/
Initial support covers the mechanisms for announcing support
for EWMH (_NET_SUPPORTED, _NET_SUPPORTING_WM_CHECK), the
client list and active client (_NET_CLIENT_LIST and
_NET_ACTIVE_WINDOW), and the window type (_NET_WM_WINDOW_TYPE).
Windows may now be frameless if their window type indicates.

## 2003-06-21 (jfc, York)


Added session management so that GNOME2's gnome-session does
not wait a long timeout when starting the window manager.

## 2000-02-08 (enh, Basel)


Tried out a patch from Robert Bauer so that it's possible to move
windows with button 1, if you're in the ``titlebar'' (i.e. not touching
the top border). This makes it easier for Windows users to cope
with lwm, and easier for those with two-button mice (or laptops)
too. At the moment, "mv disp.old disp.c" will give back the old
behaviour.

## 1999-11-11 (enh, Basel)


Fixed a cut-and-paste bug in client.c that made the check for
a window being too large or too small wrong. This bug was found
by Mike Meyer.

## 1999-09-22 (enh, Basel)


Altered the button-press code so that it's now easier for unhappy
users to alter which button performs which function. Simply edit
lwm.h and modify the three relevant #define statements.

## 1999-07-19 (enh, Basel)


Added a handler for circulation events so that other programs
can offer "Alt-Tab" functionality.

## 1999-07-08 (enh, Basel)


Fixed the cosmetic problem with titlebars of dialogue boxes. If
this looks to be OK, I can think about another lwm release.

## 1999-06-10 (enh, Basel)


Incorporated bug fix by Adrian Colley regarding the attempt in
manage.c to call XSetWindowBorderWidth on an InputOnly window,
and moved the #include of <signal.h> after <sys/types.h> so that
lwm can compile on Solaris 2.6. Cosmetic change to move the close
box to line up with the client window. The effect is spoilt if the
child insists on drawing a black border around itself, though.

## 1999-02-07 (enh, Basel)


Title-bars no longer pop up and down. An inactive window has a
grey title instead. This means less load on the server, no annoying
"I want to type the information from one window's title-bar into
the current window but can't" syndrome, and a final solution to
the race condition that's been with us since the very beginning.

The size feedback no longer pops up as soon as you grab a window,
because that made it almost impossible to grab a window without
resizing it.

## 1998-11-03 (enh, Basel)


The size feedback now pops up as soon as you grab a window,
rather than waiting for you to actually move.

## 1998-10-06 (enh, Basel)


Al pointed out that my Sun actually has two framebuffers. One
monitor-lugging later, and I suddenly have a need for a window
manager that can cope with multiple screens. And here it is!

## 1998-05-29 (enh, Basel)


Fixed window minimum/maximum height code so that it no longer
includes the title decoration. Menu now pops down if a window
disappears while the menu is up.

## 1998-03-23 (enh, Basel)


Removed unused constant. A little tidying up, renaming.  Some
debugging code removed. The width of the size-feedback window
is now calculated at run-time depending on the size of the screen.

## 1998-02-05 (enh, Basel)


Fixed bug found by Marty Olevitch: lwm's automatic window
placement heuristics broke down when either the right or bottom
of the display were reached.
Changed menu placement to ensure that the menu is fully
on-screen.

## 1998-01-06 (enh, Basel)


Fixed bug found by J. Han whereby lwm dumped core if a window
disappeared while being reshaped.

## 1997-09-01 (enh, Basel)


"Push to back" functionality moved from button 3 click in box
to button 3 click anywhere in frame with Shift held down.

## 1997-08-29 (enh, Basel)


Simple version numbering introduced.

## 1997-08-25 (enh, Basel)


Fixed stupid mistake introduced with the last change, with regard
to setting the input focus.

## 1997-08-22 (enh, Basel)


Xt applications (strictly, applications whose window title is
the same as their class hint resource name) no longer have a
title bar. This means it's more awkward to kill them, but that
they don't have pointless decoration.

## 1997-08-07 (enh, Basel)


Bug related to hiding windows fixed.

## 1997-08-06 (enh, Basel)


The size indictor now has the correct GC settings. Whoops!
Improved handling of WM_NORMAL_HINTS. Amongst other things,
this means that size reporting of xterm et al is more reliable.

## 1997-07-31 (enh, Basel)


Reshaping now uses the popup to display the current width and
height of the window being reshaped (in whatever units it uses).

## 1997-07-04 (enh, Swanwick)


Clicking button 3 on the "box" pushes the window to the bottom.
Changing image in xv no longer causes the window to gravitate to
the southeast. There's an ICCCM convention that clients should
set the border width with each ConfigureWindow request. As usual,
many clients fail to follow this convention. I get the distinct
impression that the very reason for the existance of the Xt
library is because the X11 protocol and ICCCM are so messy and
involved that the only way to make X11 bearable was to write
this code once and for all. The menu code has been rewritten,
changing as a side-effect the order in which hidden windows
appear on the menu. The rewrite now means that the order is very
easy to change for experiments like alphabetical ordering etc. I
like it as it is: a stack.

## 1997-06-24 (enh, York)


Now handles NoExpose events. Better protocol error reporting.
Default minimum size calculation improved.

## 1997-06-23 (enh, York)


Both button 1 and 2 can now have commands associated with them.
See the documentation for details. Windows whose minimum and
maximum sizes are identical can no longer be resized. The
oscillation race condition is now less likely to occur. Some
dead code removed.

## 1997-05-25 (enh, York)


lwm now does the right thing with respect to hidden windows on
exit and startup. a hidden window is now re-hidden if lwm exits
and is then restarted.

## 1997-05-21 (enh, York)


Fixed a bug that meant a client could confuse lwm by remapping
a hidden window: the menu of hidden windows wasn't being updated.

## 1997-05-16 (enh, York)


A bug relating to ConfigureRequests on the current window caused
the title-bar to be redrawn incorrectly. Once again, this came
to light with xv.

The "New Shell" command has gone from the button 3 menu, and
button 2 now performs this function.

## 1997-05-09 (enh, York)


This version fixes a bug relating to ConfigureRequests. Client
windows that were resized under program control were resized,
but the client was misinformed as to what change had actually
taken place. xv's optimised redraw, for example, missed out on
part of the window because of this.
The behaviour with regard to hidden windows on exit has also
changed. They're now remapped, but lowered in the window stack.
This means that you don't lose them, but that they don't
obliterate the more important windows on your screen if you kill
the window manager.

- Initial announcement on comp.windows.x.announce -
