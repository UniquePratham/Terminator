/* shell.h — shell engine: variables, built-ins, execution entry points.
 *
 * The shell engine is GUI-free. Output is delivered through a callback so the
 * same engine can feed a thread-mode tab buffer or a process-mode IPC pipe.
 */
#ifndef TERMINATOR_SHELL_H
#define TERMINATOR_SHELL_H

#include <stddef.h>
#include <sys/types.h>

typedef struct shell shell_t;
typedef struct history history_t;
typedef struct job_table job_table_t;
typedef struct config config_t;

/* Output sink: receives bytes produced by commands and built-ins. */
typedef void (*shell_write_fn)(void *ctx, const char *data, size_t n);

/* Foreground process-group notification (used by process mode to tell the GUI
 * which process group to signal for Ctrl+C / Ctrl+Z). */
typedef void (*shell_fg_fn)(void *ctx, long pgid);

shell_t *shell_new(void);
void     shell_free(shell_t *sh);

void shell_set_output(shell_t *sh, shell_write_fn fn, void *ctx);
/* Redirect built-in/script output to a file descriptor (>=0), or back to the
 * normal output callback (-1). Used to honour redirections on built-ins. */
void shell_set_out_fd(shell_t *sh, int fd);
int  shell_get_out_fd(shell_t *sh);
void shell_set_fg_notify(shell_t *sh, shell_fg_fn fn, void *ctx);
void shell_notify_fg(shell_t *sh, long pgid);
void shell_set_history(shell_t *sh, history_t *h);
void shell_set_jobs(shell_t *sh, job_table_t *j);
void shell_set_config(shell_t *sh, config_t *c);
void shell_set_mode(shell_t *sh, int process_mode, int tab_id);
/* Job control: when off, forked children are not placed in their own process
 * group, so all descendants share one group (used by multiWatch). */
void shell_set_job_control(shell_t *sh, int on);
int  shell_job_control(shell_t *sh);

/* Cooperative interrupt: the GUI sets this when Ctrl+C is pressed so
 * long-running built-ins (multiWatch) can stop even when no child process is
 * alive to receive the signal. Cleared before each new command line. */
void shell_request_interrupt(shell_t *sh);
void shell_clear_interrupt(shell_t *sh);
int  shell_interrupted(shell_t *sh);

history_t  *shell_history(shell_t *sh);
job_table_t *shell_jobs(shell_t *sh);
config_t   *shell_config(shell_t *sh);
int         shell_tab_id(shell_t *sh);
int         shell_process_mode(shell_t *sh);

/* Process group of the currently running foreground job (or -1). In process
 * mode the GUI reads this so it can signal the foreground job directly rather
 * than stopping the worker process. */
void  shell_set_foreground_pgid(shell_t *sh, pid_t pgid);
pid_t shell_foreground_pgid(shell_t *sh);

/* Shell variables use owned name/value strings. These accessors keep their
 * table private and let expansion/script modules share the same context. */
const char *shell_get_var(shell_t *sh, const char *name);
void        shell_set_var(shell_t *sh, const char *name, const char *value);
void        shell_unset_var(shell_t *sh, const char *name);
/* Copy all variables from src into dst (used when forking a child shell). */
void        shell_copy_vars(shell_t *dst, shell_t *src);

/* Exit status ($?): the most recently completed command or pipeline result. */
int  shell_last_status(shell_t *sh);
void shell_set_last_status(shell_t *sh, int st);

/* Logical current directory (never applied with chdir() in the shell). */
const char *shell_cwd(shell_t *sh);
void        shell_set_cwd(shell_t *sh, const char *cwd);

/* Positional parameters are 1-based, matching the supported script syntax. */
void        shell_set_posargs(shell_t *sh, char *const *args, int n);
const char *shell_posarg(shell_t *sh, int i);
int         shell_nposargs(shell_t *sh);

/* Output helpers route bytes through the configured callback or redirected fd. */
void shell_write(shell_t *sh, const char *data, size_t n);
void shell_puts(shell_t *sh, const char *s);
void shell_printf(shell_t *sh, const char *fmt, ...)
         __attribute__((format(printf, 2, 3)));

/* Exit request (from the `exit` built-in). */
int  shell_exit_requested(shell_t *sh);
int  shell_exit_code(shell_t *sh);
void shell_request_exit(shell_t *sh, int code);

/* Built-ins. */
int  shell_is_builtin(const char *name);
void shell_run_builtin(shell_t *sh, const char *name, char **argv, int argc);

/* If argv describes a script invocation (`./script.sh`, `bash script.sh`),
 * run it with the built-in interpreter and return 1. Otherwise return 0. */
int  shell_try_script(shell_t *sh, char **argv, int argc);

/* True when argv describes a script invocation (without running it). */
int  shell_is_script_invocation(char **argv, int argc);

/* Background output: register a read end whose data is drained by the GUI
 * event loop (thread mode) or forwarded over IPC (process mode). */
void shell_add_async_fd(shell_t *sh, int fd);

/* Execute a full input line (may contain several pipelines). Returns the exit
 * status of the last pipeline. */
int shell_exec_line(shell_t *sh, const char *line);

/* Run a command string and capture its stdout (for command substitution).
 * Returns a heap string with trailing newlines stripped. */
char *shell_capture(shell_t *sh, const char *cmd);

/* True when the line is syntactically complete (no continuation needed). */
int shell_line_complete(const char *line);

#endif /* TERMINATOR_SHELL_H */
