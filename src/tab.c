/* tab.c — per-tab state, input editing, and worker management.
 *
 * Thread mode: a worker thread runs the shell engine and writes output into
 * the tab's buffer under a mutex. Process mode: a worker process runs the
 * shell engine and sends framed IPC messages to the GUI over a pipe.
 */
#include "tab.h"
#include "completion.h"
#include "config.h"
#include "history.h"
#include "ipc.h"
#include "jobs.h"
#include "shell.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

struct tab {
    int      id;           /* stable identity used to tag worker messages */
    int      process_mode; /* chooses pthread worker or forked worker */

    /* Input editing (GUI thread only). */
    char    *input;
    size_t   input_len;
    size_t   input_cap;
    size_t   cursor;
    int      insert_mode;

    /* Numbered filename-completion choices stay active until the next edit. */
    char   **completion_matches;
    size_t   completion_count;
    size_t   completion_start;
    size_t   completion_cursor;
    char    *completion_prefix;
    char    *completion_display;

    /* Output scrollback (protected by out_mutex in thread mode). */
    buf_t    output;
    pthread_mutex_t out_mutex;

    /* Continuation buffer for multi-line input. */
    buf_t    pending;
    int      in_continuation;

    /* History navigation. */
    int      hist_pos;   /* -1 = editing new line */

    /* Shell engine. */
    shell_t *shell;

    /* Worker communication (both modes). */
    int      worker_in_fd;    /* GUI reads framed messages here */
    int      worker_out_fd;   /* worker writes framed messages here */
    buf_t    ipc_in;          /* partial-frame accumulation */

    /* Thread mode. */
    pthread_t worker;
    int       worker_started;
    int       worker_stop;
    int       wake_pipe[2];   /* GUI -> worker: a line is ready */

    /* Process mode. */
    pid_t     worker_pid;
    int       to_worker[2];   /* GUI -> worker */

    /* Shared services. */
    history_t   *history;
    job_table_t *jobs;
    config_t    *config;

    /* Foreground process group (process mode; reported by the worker). */
    pid_t     fg_pgid;

    int       exit_requested;
    int       exit_code;
    int       command_running;
    volatile int dirty;
};

static void completion_selection_clear(tab_t *t);

/* ------------------------------------------------------------------ */
/* Worker output sink (both modes)                                    */
/* ------------------------------------------------------------------ */

static void worker_output_sink(void *ctx, const char *data, size_t n)
{
    tab_t *t = ctx;
    if (n > 0)
        ipc_send(t->worker_out_fd, IPC_OUTPUT, (uint32_t)t->id, data, (uint32_t)n);
}

static void worker_fg_sink(void *ctx, long pgid)
{
    tab_t *t = ctx;
    /* Keep the worker's own copy so it can forward signals to the foreground
     * process group, and tell the GUI which group is in the foreground. */
    t->fg_pgid = (pid_t)pgid;
    int32_t v = (int32_t)pgid;
    ipc_send(t->worker_out_fd, IPC_FG_PGID, (uint32_t)t->id, &v, sizeof(v));
}

/* ------------------------------------------------------------------ */
/* Thread-mode worker                                                 */
/* ------------------------------------------------------------------ */

static void *worker_main(void *arg)
{
    /* Thread-mode worker sleeps on the wake pipe. The GUI writes one byte per
     * submitted line; the worker executes it and sends a prompt-ready frame. */
    tab_t *t = arg;
    for (;;) {
        char c;
        ssize_t r = read(t->wake_pipe[0], &c, 1);
        if (r <= 0)
            break;
        if (t->worker_stop)
            break;

        char *line = xstrdup(t->pending.data ? t->pending.data : "");
        buf_clear(&t->pending);

        shell_exec_line(t->shell, line);
        free(line);

        if (shell_exit_requested(t->shell)) {
            t->exit_requested = 1;
            t->exit_code = shell_exit_code(t->shell);
            ipc_send(t->worker_out_fd, IPC_EXIT, (uint32_t)t->id, NULL, 0);
            break;
        }
        ipc_send(t->worker_out_fd, IPC_PROMPT, (uint32_t)t->id, NULL, 0);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Process-mode worker                                                */
/* ------------------------------------------------------------------ */

/* The worker process runs the shell engine. While a long-running built-in
 * (multiWatch) is active it is not reading the IPC pipe, so the GUI signals
 * the worker process directly. This handler records the request so the
 * built-in can stop cooperatively. */
static shell_t *g_worker_shell;

static void worker_sigint_handler(int sig)
{
    (void)sig;
    if (g_worker_shell)
        shell_request_interrupt(g_worker_shell);
}

static void proc_worker_loop(tab_t *t)
{
    /* Process-mode counterpart to worker_main(): receive framed input from
     * the GUI and execute it inside this tab's child process. */
    g_worker_shell = t->shell;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = worker_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: interrupt blocking reads */
    sigaction(SIGINT, &sa, NULL);

    for (;;) {
        uint32_t type, tab_id, len;
        char *payload = NULL;
        if (ipc_recv(t->to_worker[0], &type, &tab_id, &payload, &len) != 0)
            break;
        if (type == IPC_INPUT) {
            char *line = xstrdup(payload ? payload : "");
            shell_exec_line(t->shell, line);
            free(line);
            if (shell_exit_requested(t->shell)) {
                ipc_send(t->worker_out_fd, IPC_EXIT, (uint32_t)t->id, NULL, 0);
                free(payload);
                break;
            }
            ipc_send(t->worker_out_fd, IPC_PROMPT, (uint32_t)t->id, NULL, 0);
        } else if (type == IPC_SIGNAL) {
            int sig = 0;
            if (payload && len >= sizeof(int))
                memcpy(&sig, payload, sizeof(int));
            /* Cooperative interrupt for long-running built-ins. */
            if (sig == SIGINT)
                shell_request_interrupt(t->shell);
            pid_t pg = t->fg_pgid;
            if (pg > 0)
                kill(-pg, sig);
        } else if (type == IPC_EXIT) {
            free(payload);
            break;
        }
        free(payload);
    }
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

tab_t *tab_new(int id, int process_mode, history_t *hist,
               job_table_t *jobs, config_t *cfg)
{
    tab_t *t = xcalloc(1, sizeof(*t));
    t->id = id;
    t->process_mode = process_mode;
    t->history = hist;
    t->jobs = jobs;
    t->config = cfg;
    t->hist_pos = -1;
    t->insert_mode = 1;
    t->fg_pgid = -1;
    t->worker_in_fd = -1;
    t->worker_out_fd = -1;
    pthread_mutex_init(&t->out_mutex, NULL);
    buf_init(&t->output);
    buf_init(&t->pending);
    buf_init(&t->ipc_in);
    t->wake_pipe[0] = t->wake_pipe[1] = -1;
    t->to_worker[0] = t->to_worker[1] = -1;

    t->shell = shell_new();
    shell_set_history(t->shell, hist);
    shell_set_jobs(t->shell, jobs);
    shell_set_config(t->shell, cfg);
    shell_set_mode(t->shell, process_mode, id);

    if (process_mode) {
        int to_w[2], from_w[2];
        if (pipe(to_w) != 0 || pipe(from_w) != 0) {
            process_mode = t->process_mode = 0;
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                /* Child: become the tab worker. */
                close(to_w[1]);
                close(from_w[0]);
                t->to_worker[0] = to_w[0];
                t->worker_out_fd = from_w[1];
                shell_set_output(t->shell, worker_output_sink, t);
                shell_set_fg_notify(t->shell, worker_fg_sink, t);
                proc_worker_loop(t);
                _exit(0);
            }
            if (pid < 0) {
                close(to_w[0]);
                close(to_w[1]);
                close(from_w[0]);
                close(from_w[1]);
                process_mode = t->process_mode = 0;
            } else {
                t->worker_pid = pid;
                close(to_w[0]);
                close(from_w[1]);
                t->to_worker[0] = -1;
                t->to_worker[1] = to_w[1];
                t->worker_in_fd = from_w[0];
                set_nonblock(t->worker_in_fd);
                return t;
            }
        }
    }

    /* Thread mode. */
    shell_set_output(t->shell, worker_output_sink, t);
    int wake[2], out[2];
    if (pipe(wake) != 0 || pipe(out) != 0) {
        t->worker_started = 0;
        return t;
    }
    t->wake_pipe[0] = wake[0];
    t->wake_pipe[1] = wake[1];
    t->worker_in_fd = out[0];
    t->worker_out_fd = out[1];
    set_nonblock(t->worker_in_fd);
    if (pthread_create(&t->worker, NULL, worker_main, t) == 0)
        t->worker_started = 1;
    return t;
}

void tab_free(tab_t *t)
{
    if (!t)
        return;
    if (t->process_mode) {
        if (t->worker_pid > 0) {
            ipc_send(t->to_worker[1], IPC_EXIT, (uint32_t)t->id, NULL, 0);
            kill(t->worker_pid, SIGTERM);
            int status;
            while (waitpid(t->worker_pid, &status, 0) < 0 && errno == EINTR)
                ;
        }
        if (t->to_worker[1] >= 0)
            close(t->to_worker[1]);
        if (t->worker_in_fd >= 0)
            close(t->worker_in_fd);
    } else if (t->worker_started) {
        t->worker_stop = 1;
        char c = 'Q';
        write_all(t->wake_pipe[1], &c, 1);
        pthread_join(t->worker, NULL);
        if (t->wake_pipe[0] >= 0)
            close(t->wake_pipe[0]);
        if (t->wake_pipe[1] >= 0)
            close(t->wake_pipe[1]);
        if (t->worker_in_fd >= 0)
            close(t->worker_in_fd);
        if (t->worker_out_fd >= 0)
            close(t->worker_out_fd);
    }
    shell_free(t->shell);
    buf_free(&t->output);
    buf_free(&t->pending);
    buf_free(&t->ipc_in);
    completion_selection_clear(t);
    free(t->input);
    pthread_mutex_destroy(&t->out_mutex);
    free(t);
}

int      tab_id(tab_t *t) { return t->id; }
shell_t *tab_shell(tab_t *t) { return t->shell; }
int      tab_process_mode(tab_t *t) { return t->process_mode; }

/* ------------------------------------------------------------------ */
/* Input editing                                                      */
/* ------------------------------------------------------------------ */

static void input_reserve(tab_t *t, size_t extra)
{
    size_t need = t->input_len + extra + 1;
    if (need <= t->input_cap)
        return;
    size_t cap = t->input_cap ? t->input_cap : 64;
    while (cap < need)
        cap *= 2;
    t->input = xrealloc(t->input, cap);
    t->input_cap = cap;
}

static size_t utf8_prev(const char *s, size_t pos)
{
    if (pos == 0)
        return 0;
    size_t start = pos - 1;
    while (start > 0 &&
           ((unsigned char)s[start] & 0xC0) == 0x80)
        start--;
    return start;
}

static void completion_selection_clear(tab_t *t)
{
    for (size_t i = 0; i < t->completion_count; i++)
        free(t->completion_matches[i]);
    free(t->completion_matches);
    free(t->completion_prefix);
    t->completion_matches = NULL;
    t->completion_count = 0;
    t->completion_prefix = NULL;
    free(t->completion_display);
    t->completion_display = NULL;
    t->dirty = 1;
}

static int completion_selection_apply(tab_t *t, size_t choice)
{
    if (choice >= t->completion_count || t->cursor != t->completion_cursor)
        return 0;

    buf_t replacement;
    buf_init(&replacement);
    buf_append_str(&replacement, t->completion_prefix);
    buf_append_str(&replacement, t->completion_matches[choice]);

    size_t tail_len = t->input_len - t->completion_cursor;
    char *tail = xstrndup(t->input + t->completion_cursor, tail_len);
    size_t repl_len = replacement.len;
    size_t new_len = t->completion_start + repl_len + tail_len;
    if (new_len < t->completion_start) {
        free(tail);
        buf_free(&replacement);
        completion_selection_clear(t);
        return 0;
    }
    input_reserve(t, new_len - t->input_len);
    memcpy(t->input + t->completion_start, replacement.data, repl_len);
    memcpy(t->input + t->completion_start + repl_len, tail, tail_len + 1);
    t->input_len = new_len;
    t->cursor = t->completion_start + repl_len;
    t->dirty = 1;
    free(tail);
    buf_free(&replacement);
    completion_selection_clear(t);
    return 1;
}

static size_t utf8_next(const char *s, size_t pos, size_t len)
{
    if (pos >= len)
        return len;
    size_t next = pos + 1;
    while (next < len &&
           ((unsigned char)s[next] & 0xC0) == 0x80)
        next++;
    return next;
}

void tab_input_insert(tab_t *t, const char *text)
{
    size_t n = strlen(text);
    if (n == 0)
        return;
    if (t->completion_count > 0) {
        if (n == 1 && text[0] >= '1' && text[0] <= '9' &&
            completion_selection_apply(t, (size_t)(text[0] - '1')))
            return;
        completion_selection_clear(t);
    }
    int had_input = t->input != NULL;
    input_reserve(t, n);
    /* The first insertion has no existing NUL byte to move. Initialize the
     * newly allocated editor buffer before memmove reads its old tail. */
    if (!had_input)
        t->input[0] = '\0';
    size_t replace_end = t->cursor;
    if (!t->insert_mode) {
        for (size_t i = 0; i < n && replace_end < t->input_len;
             i = utf8_next(text, i, n))
            replace_end = utf8_next(t->input, replace_end, t->input_len);
    }
    size_t tail_len = t->input_len - replace_end;
    memmove(t->input + t->cursor + n, t->input + replace_end, tail_len + 1);
    memcpy(t->input + t->cursor, text, n);
    t->input_len = t->input_len - (replace_end - t->cursor) + n;
    t->cursor += n;
    t->input[t->input_len] = '\0';
    t->dirty = 1;
}

void tab_input_backspace(tab_t *t)
{
    completion_selection_clear(t);
    if (t->cursor == 0)
        return;
    size_t start = utf8_prev(t->input, t->cursor);
    size_t removed = t->cursor - start;
    memmove(t->input + start, t->input + t->cursor,
            t->input_len - t->cursor + 1);
    t->cursor = start;
    t->input_len -= removed;
    t->dirty = 1;
}

void tab_input_delete(tab_t *t)
{
    completion_selection_clear(t);
    if (t->cursor >= t->input_len)
        return;
    size_t end = utf8_next(t->input, t->cursor, t->input_len);
    size_t removed = end - t->cursor;
    memmove(t->input + t->cursor, t->input + end,
            t->input_len - end + 1);
    t->input_len -= removed;
    t->dirty = 1;
}

void tab_cursor_left(tab_t *t)
{
    completion_selection_clear(t);
    t->cursor = utf8_prev(t->input, t->cursor);
    t->dirty = 1;
}

void tab_cursor_right(tab_t *t)
{
    completion_selection_clear(t);
    t->cursor = utf8_next(t->input, t->cursor, t->input_len);
    t->dirty = 1;
}
void tab_cursor_home(tab_t *t) { completion_selection_clear(t); t->cursor = 0; t->dirty = 1; }
void tab_cursor_end(tab_t *t) { completion_selection_clear(t); t->cursor = t->input_len; t->dirty = 1; }
int tab_insert_mode(tab_t *t) { return t->insert_mode; }
void tab_toggle_insert_mode(tab_t *t)
{
    completion_selection_clear(t);
    t->insert_mode = !t->insert_mode;
    t->dirty = 1;
}

void tab_delete_to_start(tab_t *t)
{
    completion_selection_clear(t);
    memmove(t->input, t->input + t->cursor, t->input_len - t->cursor + 1);
    t->input_len -= t->cursor;
    t->cursor = 0;
    t->dirty = 1;
}

void tab_delete_to_end(tab_t *t)
{
    completion_selection_clear(t);
    t->input[t->cursor] = '\0';
    t->input_len = t->cursor;
    t->dirty = 1;
}

void tab_delete_word(tab_t *t)
{
    completion_selection_clear(t);
    size_t i = t->cursor;
    while (i > 0 && t->input[i - 1] == ' ')
        i--;
    while (i > 0 && t->input[i - 1] != ' ')
        i--;
    memmove(t->input + i, t->input + t->cursor, t->input_len - t->cursor + 1);
    t->input_len -= (t->cursor - i);
    t->cursor = i;
    t->dirty = 1;
}

const char *tab_input(tab_t *t) { return t->input ? t->input : ""; }
size_t      tab_cursor(tab_t *t) { return t->cursor; }

void tab_set_input(tab_t *t, const char *s)
{
    completion_selection_clear(t);
    size_t n = strlen(s);
    input_reserve(t, n);
    memcpy(t->input, s, n + 1);
    t->input_len = n;
    t->cursor = n;
    t->dirty = 1;
}

void tab_clear_input(tab_t *t)
{
    completion_selection_clear(t);
    if (t->input)
        t->input[0] = '\0';
    t->input_len = 0;
    t->cursor = 0;
    t->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

void tab_output_append(tab_t *t, const char *data, size_t n)
{
    pthread_mutex_lock(&t->out_mutex);
    /* Handle ANSI Erase Display from `clear` in both worker modes. This also
     * removes the echoed command from the visible scrollback. */
    static const char clear_screen[] = "\033[2J";
    const size_t clear_len = sizeof(clear_screen) - 1;
    size_t start = 0;
    for (size_t i = 0; i + clear_len <= n; i++) {
        if (memcmp(data + i, clear_screen, clear_len) == 0) {
            if (i > start)
                buf_append(&t->output, data + start, i - start);
            buf_clear(&t->output);
            i += clear_len - 1;
            start = i + 1;
        }
    }
    if (start < n)
        buf_append(&t->output, data + start, n - start);
    t->dirty = 1;
    pthread_mutex_unlock(&t->out_mutex);
}

const char *tab_output(tab_t *t) { return t->output.data ? t->output.data : ""; }
size_t      tab_output_len(tab_t *t) { return t->output.len; }

char *tab_output_snapshot(tab_t *t, size_t *len_out)
{
    pthread_mutex_lock(&t->out_mutex);
    size_t len = t->output.len;
    char *copy = xstrndup(t->output.data ? t->output.data : "", len);
    pthread_mutex_unlock(&t->out_mutex);
    if (len_out)
        *len_out = len;
    return copy;
}

void tab_output_clear(tab_t *t)
{
    pthread_mutex_lock(&t->out_mutex);
    buf_clear(&t->output);
    t->dirty = 1;
    pthread_mutex_unlock(&t->out_mutex);
}

const char *tab_completion_display(tab_t *t)
{
    return t->completion_display ? t->completion_display : "";
}

/* ------------------------------------------------------------------ */
/* Submission                                                         */
/* ------------------------------------------------------------------ */

int tab_in_continuation(tab_t *t) { return t->in_continuation; }
int tab_command_running(tab_t *t) { return t->command_running; }

const char *tab_prompt(tab_t *t)
{
    if (t->in_continuation)
        return "> ";

    /* "user@host:cwd$ " — built once per call into a static buffer. The GUI
     * renders single-threaded and worker threads never read the prompt, so a
     * static buffer is safe here. */
    static char prompt[512];
    const char *user = getenv("USER");
    if (!user || !*user)
        user = "user";

    char host[128];
    struct utsname un;
    if (uname(&un) == 0 && un.nodename[0])
        snprintf(host, sizeof(host), "%s", un.nodename);
    else
        snprintf(host, sizeof(host), "localhost");

    const char *cwd = shell_cwd(tab_shell(t));
    if (!cwd || !*cwd)
        cwd = "~";
    /* Abbreviate $HOME to ~ for a tidy prompt. */
    const char *home = getenv("HOME");
    char shortcwd[256];
    if (home && *home && strncmp(cwd, home, strlen(home)) == 0)
        snprintf(shortcwd, sizeof(shortcwd), "~%s", cwd + strlen(home));
    else
        snprintf(shortcwd, sizeof(shortcwd), "%s", cwd);

    snprintf(prompt, sizeof(prompt), "%s@%s:%s$ ", user, host, shortcwd);
    return prompt;
}

void tab_submit(tab_t *t)
{
    completion_selection_clear(t);
    const char *line = tab_input(t);
    int was_continuing = t->in_continuation;
    /* Accumulate continuation lines. */
    if (t->in_continuation) {
        buf_append_str(&t->pending, "\n");
        buf_append_str(&t->pending, line);
    } else {
        buf_clear(&t->pending);
        buf_append_str(&t->pending, line);
    }

    if (!shell_line_complete(t->pending.data)) {
        /* A continued command spans multiple GUI input buffers. Preserve each
         * submitted line in scrollback so the first line does not disappear
         * when tab_clear_input() resets the editable line. */
        buf_t echo;
        buf_init(&echo);
        buf_append_str(&echo, was_continuing ? "> " : tab_prompt(t));
        buf_append_str(&echo, line);
        buf_append_char(&echo, '\n');
        tab_output_append(t, echo.data, echo.len);
        buf_free(&echo);
        t->in_continuation = 1;
        tab_clear_input(t);
        return;
    }
    t->in_continuation = 0;

    /* Echo the command into the scrollback. */
    buf_t echo;
    buf_init(&echo);
    buf_append_str(&echo, was_continuing ? "> " : tab_prompt(t));
    buf_append_str(&echo, line);
    buf_append_char(&echo, '\n');
    tab_output_append(t, echo.data, echo.len);
    buf_free(&echo);

    if (t->history)
        history_add(t->history, t->pending.data);

    if (t->process_mode) {
        t->command_running = 1;
        if (ipc_send(t->to_worker[1], IPC_INPUT, (uint32_t)t->id,
                     t->pending.data, (uint32_t)t->pending.len) != 0)
            t->command_running = 0;
    } else if (t->worker_started) {
        t->command_running = 1;
        char c = 'R';
        if (write_all(t->wake_pipe[1], &c, 1) != 0)
            t->command_running = 0;
    } else {
        /* Degraded synchronous mode. */
        t->command_running = 1;
        shell_exec_line(t->shell, t->pending.data);
        t->command_running = 0;
        if (shell_exit_requested(t->shell)) {
            t->exit_requested = 1;
            t->exit_code = shell_exit_code(t->shell);
        }
    }
    tab_clear_input(t);
    t->hist_pos = -1;
}

/* ------------------------------------------------------------------ */
/* Polling / draining                                                 */
/* ------------------------------------------------------------------ */

int tab_poll_fd(tab_t *t)
{
    return t->worker_in_fd;
}

int tab_drain(tab_t *t)
{
    int changed = 0;
    if (t->worker_in_fd < 0)
        return 0;

    char buf[8192];
    ssize_t r;
    while ((r = read(t->worker_in_fd, buf, sizeof(buf))) > 0)
        buf_append(&t->ipc_in, buf, (size_t)r);
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        t->exit_requested = 1;
        changed = 1;
    }

    for (;;) {
        if (t->ipc_in.len < sizeof(ipc_header_t))
            break;
        ipc_header_t h;
        memcpy(&h, t->ipc_in.data, sizeof(h));
        if (h.length > 16u * 1024u * 1024u) {
            t->exit_requested = 1;
            changed = 1;
            break;
        }
        size_t total = sizeof(h) + h.length;
        if (t->ipc_in.len < total)
            break;
        const char *payload = t->ipc_in.data + sizeof(h);
        if (h.type == IPC_OUTPUT) {
            tab_output_append(t, payload, h.length);
            changed = 1;
        } else if (h.type == IPC_PROMPT) {
            t->command_running = 0;
            changed = 1;
        } else if (h.type == IPC_FG_PGID) {
            if (h.length >= sizeof(int32_t)) {
                int32_t v;
                memcpy(&v, payload, sizeof(v));
                t->fg_pgid = v;
            }
        } else if (h.type == IPC_EXIT) {
            t->exit_requested = 1;
            changed = 1;
        }
        memmove(t->ipc_in.data, t->ipc_in.data + total, t->ipc_in.len - total);
        t->ipc_in.len -= total;
        if (t->ipc_in.data)
            t->ipc_in.data[t->ipc_in.len] = '\0';
    }
    return changed;
}

int tab_exit_requested(tab_t *t) { return t->exit_requested; }
int tab_exit_code(tab_t *t) { return t->exit_code; }

void tab_send_signal(tab_t *t, int sig)
{
    /* Cooperative interrupt: lets long-running built-ins (multiWatch) stop
     * even when no child process is alive to receive the signal. */
    if (sig == SIGINT && t->shell)
        shell_request_interrupt(t->shell);
    if (t->process_mode) {
        int s = sig;
        ipc_send(t->to_worker[1], IPC_SIGNAL, (uint32_t)t->id, &s, sizeof(s));
        /* The worker is usually blocked waiting for the foreground job, so it
         * cannot read the IPC message until the job finishes. Signal the
         * foreground job's process group directly — that is the reliable path.
         * Never signal the worker itself with SIGTSTP: that would suspend the
         * shell instead of the job. */
        if (t->fg_pgid > 0)
            kill(-t->fg_pgid, sig);
        /* SIGINT must also reach the worker so long-running built-ins
         * (multiWatch) can stop cooperatively. */
        if (sig == SIGINT && t->worker_pid > 0)
            kill(t->worker_pid, sig);
    } else {
        pid_t pg = jobs_foreground_pgid(t->jobs);
        if (pg > 0)
            kill(-pg, sig);
    }
}

/* ------------------------------------------------------------------ */
/* Completion and history navigation                                  */
/* ------------------------------------------------------------------ */

void tab_complete(tab_t *t)
{
    completion_selection_clear(t);
    /* Find the word before the cursor. */
    size_t start = t->cursor;
    while (start > 0 && t->input[start - 1] != ' ')
        start--;
    char *word = xstrndup(t->input + start, t->cursor - start);
    completion_t c = completion_run(shell_cwd(t->shell), word);
    free(word);

    if (c.kind == COMPLETE_ONE || c.kind == COMPLETE_PREFIX) {
        /* Replace the word, preserving text after the cursor. */
        size_t after_len = t->input_len - t->cursor;
        char *after = xstrndup(t->input + t->cursor, after_len);
        size_t repl = strlen(c.replacement);
        size_t newlen = start + repl + after_len;
        input_reserve(t, newlen);
        memcpy(t->input + start, c.replacement, repl);
        memcpy(t->input + start + repl, after, after_len + 1);
        t->input_len = newlen;
        t->cursor = start + repl;
        free(after);
        t->dirty = 1;
    } else if (c.kind == COMPLETE_AMBIGUOUS) {
        buf_t b;
        buf_init(&b);
        buf_append_char(&b, '\n');
        for (size_t i = 0; i < c.nmatches; i++)
            buf_appendf(&b, "%zu) %s\n", i + 1, c.matches[i]);
        t->completion_display = xstrndup(b.data, b.len);
        buf_free(&b);
        const char *slash = strrchr(t->input + start, '/');
        size_t prefix_len = slash ? (size_t)(slash - (t->input + start)) + 1 : 0;
        t->completion_prefix = xstrndup(t->input + start, prefix_len);
        t->completion_matches = xcalloc(c.nmatches, sizeof(*t->completion_matches));
        for (size_t i = 0; i < c.nmatches; i++)
            t->completion_matches[i] = xstrdup(c.matches[i]);
        t->completion_count = c.nmatches;
        t->completion_start = start;
        t->completion_cursor = t->cursor;
        t->dirty = 1;
    }
    completion_free(&c);
}

void tab_history_prev(tab_t *t)
{
    if (!t->history)
        return;
    size_t c = history_count(t->history);
    if (c == 0)
        return;
    if (t->hist_pos < 0)
        t->hist_pos = (int)c - 1;
    else if (t->hist_pos > 0)
        t->hist_pos--;
    const char *e = history_get(t->history, (size_t)t->hist_pos);
    if (e)
        tab_set_input(t, e);
}

void tab_history_next(tab_t *t)
{
    if (!t->history)
        return;
    size_t c = history_count(t->history);
    if (t->hist_pos < 0)
        return;
    if ((size_t)(t->hist_pos + 1) < c) {
        t->hist_pos++;
        const char *e = history_get(t->history, (size_t)t->hist_pos);
        if (e)
            tab_set_input(t, e);
    } else {
        t->hist_pos = -1;
        tab_clear_input(t);
    }
}
