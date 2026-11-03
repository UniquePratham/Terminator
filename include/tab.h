/* tab.h — per-tab state and lifecycle.
 *
 * A tab owns its logical cwd, input line/cursor, output scrollback, shell
 * engine and (in thread mode) a worker thread or (in process mode) a worker
 * process. Hidden tabs keep collecting output.
 */
#ifndef TERMINATOR_TAB_H
#define TERMINATOR_TAB_H

#include <stddef.h>
#include "shell.h"

typedef struct tab tab_t;

/* Create a tab. In process mode a worker process is forked; in thread mode a
 * worker thread is started. */
tab_t *tab_new(int id, int process_mode, history_t *hist,
               job_table_t *jobs, config_t *cfg);
void   tab_free(tab_t *t);

int      tab_id(tab_t *t);
shell_t *tab_shell(tab_t *t);
int      tab_process_mode(tab_t *t);

/* Input editing (GUI thread). Cursor offsets are UTF-8 byte offsets kept on
 * character boundaries; insertion and overwrite share the same buffer. */
void        tab_input_insert(tab_t *t, const char *text);
void        tab_input_backspace(tab_t *t);
void        tab_input_delete(tab_t *t);
void        tab_cursor_left(tab_t *t);
void        tab_cursor_right(tab_t *t);
void        tab_cursor_home(tab_t *t);
void        tab_cursor_end(tab_t *t);
/* Insert is the default. Toggling switches between insert and overwrite. */
int         tab_insert_mode(tab_t *t);
void        tab_toggle_insert_mode(tab_t *t);
void        tab_delete_to_start(tab_t *t);
void        tab_delete_to_end(tab_t *t);
void        tab_delete_word(tab_t *t);
const char *tab_input(tab_t *t);
size_t      tab_cursor(tab_t *t);
void        tab_set_input(tab_t *t, const char *s);
void        tab_clear_input(tab_t *t);

/* Output/scrollback. The returned raw pointer is for single-thread use;
 * rendering alongside a worker should use the owned snapshot copy. */
void        tab_output_append(tab_t *t, const char *data, size_t n);
const char *tab_output(tab_t *t);
size_t      tab_output_len(tab_t *t);
/* Copy output under its mutex for readers that run beside the worker. */
char       *tab_output_snapshot(tab_t *t, size_t *len_out);
void        tab_output_clear(tab_t *t);
/* Temporary completion choices shown above the live prompt, not in scrollback. */
const char *tab_completion_display(tab_t *t);

/* Submit the current input line for execution. The worker owns a copied line;
 * the GUI may clear/edit its input buffer after this call. */
void tab_submit(tab_t *t);

/* Continuation state (open quote / trailing backslash). */
int  tab_in_continuation(tab_t *t);
/* True while the worker is executing a submitted command list. */
int  tab_command_running(tab_t *t);

/* Prompt string for display. */
const char *tab_prompt(tab_t *t);

/* File descriptor the GUI should poll for output readiness, or -1. */
int  tab_poll_fd(tab_t *t);
/* Drain all currently available output/notifications without blocking.
 * Returns 1 if rendering-relevant tab state changed. */
int  tab_drain(tab_t *t);

/* True once the tab's shell has requested exit. */
int  tab_exit_requested(tab_t *t);
int  tab_exit_code(tab_t *t);

/* Forward a signal to the tab's foreground job (Ctrl+C / Ctrl+Z). */
void tab_send_signal(tab_t *t, int sig);

/* Completion support: complete the word before the cursor. */
void tab_complete(tab_t *t);

/* History navigation. */
void tab_history_prev(tab_t *t);
void tab_history_next(tab_t *t);

#endif /* TERMINATOR_TAB_H */
