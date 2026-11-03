/* app.c — application lifecycle, shared services and tab management. */
#include "app.h"
#include "config.h"
#include "gui.h"
#include "history.h"
#include "jobs.h"
#include "shell.h"
#include "util.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

struct app {
    int          process_mode; /* selects thread workers or per-tab processes */
    int          quit;         /* event-loop shutdown flag */

    history_t   *history;      /* shared across every tab */
    job_table_t *jobs;         /* job table visible to the shared shell state */
    config_t    *config;       /* shared key bindings and shell settings */

    tab_t      **tabs;         /* owned tab pointers, including inactive tabs */
    int          ntabs;        /* number of live entries in tabs[] */
    int          cap;          /* allocated number of tabs[] slots */
    int          active;       /* selected array index, or -1 when empty */
};

/* IDs identify live tabs rather than their array positions. Reuse the lowest
 * free ID after a tab closes; tabs are still appended to tabs[] in creation
 * order, so an older tab never gets renamed or moved. */
static int available_tab_id(app_t *a)
{
    for (int candidate = 1; candidate <= APP_MAX_TABS; candidate++) {
        int used = 0;
        for (int i = 0; i < a->ntabs; i++) {
            if (tab_id(a->tabs[i]) == candidate) {
                used = 1;
                break;
            }
        }
        if (!used)
            return candidate;
    }
    return 0;
}

/* Build a path in the user's home directory. Caller frees. */
static char *home_path(const char *name)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = (pw && pw->pw_dir) ? pw->pw_dir : ".";
    }
    size_t n = strlen(home) + strlen(name) + 2;
    char *p = xmalloc(n);
    snprintf(p, n, "%s/%s", home, name);
    return p;
}

app_t *app_new(int process_mode)
{
    /* Centralize shared-resource creation here so tabs all receive the same
     * history, jobs, and configuration objects. */
    app_t *a = xcalloc(1, sizeof(*a));
    a->process_mode = process_mode;
    a->active = -1;

    char *hist_path = home_path(".terminator_history");
    a->history = history_new(hist_path, process_mode);
    free(hist_path);

    char *cfg_path = home_path(".terminatorrc");
    a->config = config_load(cfg_path);
    free(cfg_path);

    a->jobs = jobs_new();
    return a;
}

void app_free(app_t *a)
{
    if (!a)
        return;
    for (int i = 0; i < a->ntabs; i++)
        tab_free(a->tabs[i]);
    free(a->tabs);
    jobs_free(a->jobs);
    config_free(a->config);
    history_free(a->history);
    free(a);
}

int app_run(app_t *a)
{
    return gui_run(a);
}

/* ------------------------------------------------------------------ */
/* Tab management                                                     */
/* ------------------------------------------------------------------ */

static void tabs_reserve(app_t *a)
{
    if (a->ntabs < a->cap)
        return;
    a->cap = a->cap ? a->cap * 2 : 4;
    a->tabs = xrealloc(a->tabs, (size_t)a->cap * sizeof(*a->tabs));
}

tab_t *app_new_tab(app_t *a)
{
    /* Enforce the limit here so every caller (button, shortcut, startup code)
     * observes the same bound before the tab array or workers are allocated. */
    if (a->ntabs >= APP_MAX_TABS)
        return NULL;
    tabs_reserve(a);
    int id = available_tab_id(a);
    if (id == 0)
        return NULL;
    /* A new tab gets an independent shell/editor but references app-wide
     * shared resources and inherits the selected execution mode. Its lowest
     * free ID is separate from its position in the creation-order array. */
    tab_t *t = tab_new(id, a->process_mode,
                       a->history, a->jobs, a->config);
    a->tabs[a->ntabs++] = t;
    a->active = a->ntabs - 1;
    return t;
}

void app_close_tab(app_t *a, int idx)
{
    if (idx < 0 || idx >= a->ntabs)
        return;
    tab_free(a->tabs[idx]);
    for (int i = idx; i < a->ntabs - 1; i++)
        a->tabs[i] = a->tabs[i + 1];
    a->ntabs--;
    if (a->ntabs == 0) {
        a->active = -1;
        return;
    }
    if (a->active >= a->ntabs)
        a->active = a->ntabs - 1;
    else if (a->active > idx)
        a->active--;
}

int    app_tab_count(app_t *a) { return a->ntabs; }
tab_t *app_tab(app_t *a, int idx)
{
    if (idx < 0 || idx >= a->ntabs)
        return NULL;
    return a->tabs[idx];
}

tab_t *app_active_tab(app_t *a)
{
    return app_tab(a, a->active);
}

int  app_active_index(app_t *a) { return a->active; }

void app_set_active(app_t *a, int idx)
{
    if (idx >= 0 && idx < a->ntabs)
        a->active = idx;
}

history_t   *app_history(app_t *a) { return a->history; }
job_table_t *app_jobs(app_t *a) { return a->jobs; }
config_t    *app_config(app_t *a) { return a->config; }
int          app_process_mode(app_t *a) { return a->process_mode; }

void app_quit(app_t *a) { a->quit = 1; }
int  app_should_quit(app_t *a) { return a->quit; }
