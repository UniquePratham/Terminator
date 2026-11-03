/* gui.h — X11 window, event loop and rendering.
 *
 * This is the only module that calls Xlib. It owns the window, font, GC and
 * the presentation state (scroll offset, search prompt) and drives the
 * application event loop.
 */
#ifndef TERMINATOR_GUI_H
#define TERMINATOR_GUI_H

#include "app.h"

/* Create the window and run the event loop until the app quits.
 * Returns the process exit code (0 on clean exit, non-zero on failure). */
int gui_run(app_t *a);

#endif /* TERMINATOR_GUI_H */
