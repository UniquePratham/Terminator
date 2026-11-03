/* main.c — entry point and launch-mode argument parsing.
 *
 * Usage:
 *   ./terminator                  thread mode (default)
 *   ./terminator useProcess=True  process mode
 */
#include "app.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [useProcess=True]\n"
            "  (no argument)        thread mode: one shell thread per tab\n"
            "  useProcess=True      process mode: one child process per tab\n",
            prog);
}

int main(int argc, char **argv)
{
    /* The only accepted option selects process mode; no option keeps the
     * default thread mode. The GUI and shell behavior stay shared either way. */
    int process_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0) {
            process_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "terminator: unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    /* A closed pipe must not kill the GUI; writes are checked explicitly. */
    signal(SIGPIPE, SIG_IGN);

    /* app_new owns shared services and creates the first tab before X11 starts. */
    app_t *app = app_new(process_mode);
    /* app_run blocks in the GUI event loop and returns when the window closes. */
    int rc = app_run(app);
    /* Stop workers and release shared services after the event loop exits. */
    app_free(app);
    return rc;
}
