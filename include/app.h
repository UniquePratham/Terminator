/* app.h — application lifecycle, shared services and tab management.
 *
 * The app owns the shared history, job table and configuration, plus the list
 * of tabs. The GUI layer drives the event loop and manipulates tabs through
 * this interface; only the GUI layer calls Xlib.
 */
#ifndef TERMINATOR_APP_H
#define TERMINATOR_APP_H

#include "tab.h"

typedef struct app app_t;

/* Keep tab creation and GUI polling bounded in either execution mode. */
#define APP_MAX_TABS 16

/* Create the application. `process_mode` selects the launch mode. The returned
 * app owns its shared services and all tabs created through app_new_tab(). */
app_t *app_new(int process_mode);
/* Stop and free tabs first, then release jobs/history/config and app storage. */
void   app_free(app_t *a);

/* Run the GUI event loop until the app quits. Returns the process exit code. */
int    app_run(app_t *a);

/* Tab management (used by the GUI). app_tab() uses a zero-based array index;
 * app_active_index() reports which index is selected. app_new_tab() returns
 * NULL when APP_MAX_TABS has already been reached. A tab keeps its ID while
 * alive; a new tab reuses the lowest vacant ID and is appended at the end. */
int    app_tab_count(app_t *a);
tab_t *app_tab(app_t *a, int idx);
tab_t *app_active_tab(app_t *a);
int    app_active_index(app_t *a);
void   app_set_active(app_t *a, int idx);
tab_t *app_new_tab(app_t *a);
void   app_close_tab(app_t *a, int idx);

/* Borrowed pointers to app-owned shared services; callers must not free them. */
history_t   *app_history(app_t *a);
job_table_t *app_jobs(app_t *a);
config_t    *app_config(app_t *a);
int          app_process_mode(app_t *a);

/* Shutdown control. */
void   app_quit(app_t *a);
int    app_should_quit(app_t *a);

#endif /* TERMINATOR_APP_H */
