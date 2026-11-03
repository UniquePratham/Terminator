/* shell.c — shell engine: variables, built-ins, execution entry points.
 *
 * This is the boundary between user-entered text and the execution modules.
 * shell_exec_line() owns the high-level sequence (lex → parse → assignments
 * → execute), while the executor owns child-process details. The shell has
 * no GUI dependency; output is sent through the callback configured by tab.c.
 */
#include "shell.h"
#include "completion.h"
#include "config.h"
#include "executor.h"
#include "expand.h"
#include "history.h"
#include "jobs.h"
#include "lexer.h"
#include "multiwatch.h"
#include "parser.h"
#include "scripts.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char *name;
    char *value;
} var_t;

struct shell {
    var_t   *vars;
    size_t   nvars;
    size_t   vcap;

    char    *cwd;
    int      last_status;
    int      exit_requested;
    int      exit_code;

    char   **posargs;
    int      nposargs;

    shell_write_fn out_fn;
    void          *out_ctx;
    int            out_fd;    /* >=0 redirects built-in/script output to a file */

    shell_fg_fn   fg_fn;
    void         *fg_ctx;

    history_t   *history;
    job_table_t *jobs;
    config_t    *config;
    int          owns_jobs;   /* shell created its own job table */
    int          owns_history;

    int          process_mode;
    int          tab_id;

    /* When 0, forked children are NOT placed in their own process group.
     * multiWatch uses this so every descendant stays in the shared group and
     * a single Ctrl+C reaches them all. */
    int          job_control;

    /* Set when the GUI asks to interrupt the running foreground command.
     * Long-running built-ins (multiWatch) poll this so they stop even when no
     * child process is alive to receive the signal. */
    volatile sig_atomic_t interrupt;

    /* Process group of the currently running foreground job, or -1. The GUI
     * reads this (via shell_foreground_pgid) so that in process mode it can
     * signal the foreground job directly instead of stopping the worker. */
    pid_t        fg_pgid;

    /* Background output descriptors drained by the GUI event loop. */
    int         *async_fds;
    size_t       nasync;
    size_t       acap;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

shell_t *shell_new(void)
{
    shell_t *sh = xcalloc(1, sizeof(*sh));
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        sh->cwd = xstrdup(cwd);
    else
        sh->cwd = xstrdup("/");
    sh->last_status = 0;
    sh->tab_id = -1;
    sh->out_fd = -1;
    sh->job_control = 1;
    sh->fg_pgid = -1;
    /* A standalone shell (tests, command-substitution children) needs its own
     * job table; the application injects a shared one via shell_set_jobs(). */
    sh->jobs = jobs_new();
    sh->owns_jobs = 1;
    return sh;
}

void shell_free(shell_t *sh)
{
    if (!sh)
        return;
    for (size_t i = 0; i < sh->nvars; i++) {
        free(sh->vars[i].name);
        free(sh->vars[i].value);
    }
    free(sh->vars);
    for (int i = 0; i < sh->nposargs; i++)
        free(sh->posargs[i]);
    free(sh->posargs);
    free(sh->cwd);
    free(sh->async_fds);
    if (sh->owns_jobs)
        jobs_free(sh->jobs);
    if (sh->owns_history)
        history_free(sh->history);
    free(sh);
}

void shell_set_output(shell_t *sh, shell_write_fn fn, void *ctx)
{
    sh->out_fn = fn;
    sh->out_ctx = ctx;
}

void shell_set_out_fd(shell_t *sh, int fd)
{
    sh->out_fd = fd;
}

int shell_get_out_fd(shell_t *sh)
{
    return sh->out_fd;
}

void shell_set_fg_notify(shell_t *sh, shell_fg_fn fn, void *ctx)
{
    sh->fg_fn = fn;
    sh->fg_ctx = ctx;
}

void shell_notify_fg(shell_t *sh, long pgid)
{
    if (sh->fg_fn)
        sh->fg_fn(sh->fg_ctx, pgid);
}

void shell_set_history(shell_t *sh, history_t *h)
{
    if (sh->owns_history)
        history_free(sh->history);
    sh->history = h;
    sh->owns_history = 0;
}
void shell_set_jobs(shell_t *sh, job_table_t *j)
{
    if (sh->owns_jobs)
        jobs_free(sh->jobs);
    sh->jobs = j;
    sh->owns_jobs = 0;
}
void shell_set_config(shell_t *sh, config_t *c) { sh->config = c; }
void shell_set_mode(shell_t *sh, int process_mode, int tab_id)
{
    sh->process_mode = process_mode;
    sh->tab_id = tab_id;
}

void shell_set_job_control(shell_t *sh, int on) { sh->job_control = on; }
int  shell_job_control(shell_t *sh) { return sh->job_control; }

void shell_request_interrupt(shell_t *sh) { sh->interrupt = 1; }
void shell_clear_interrupt(shell_t *sh) { sh->interrupt = 0; }
int  shell_interrupted(shell_t *sh) { return sh->interrupt; }

history_t   *shell_history(shell_t *sh) { return sh->history; }
job_table_t *shell_jobs(shell_t *sh) { return sh->jobs; }
config_t    *shell_config(shell_t *sh) { return sh->config; }
int          shell_tab_id(shell_t *sh) { return sh->tab_id; }
int          shell_process_mode(shell_t *sh) { return sh->process_mode; }

void  shell_set_foreground_pgid(shell_t *sh, pid_t pgid) { sh->fg_pgid = pgid; }
pid_t shell_foreground_pgid(shell_t *sh) { return sh->fg_pgid; }

/* ------------------------------------------------------------------ */
/* Variables                                                          */
/* ------------------------------------------------------------------ */

static var_t *find_var(shell_t *sh, const char *name)
{
    for (size_t i = 0; i < sh->nvars; i++)
        if (strcmp(sh->vars[i].name, name) == 0)
            return &sh->vars[i];
    return NULL;
}

const char *shell_get_var(shell_t *sh, const char *name)
{
    if (!name)
        return NULL;
    /* Special parameters. */
    if (strcmp(name, "?") == 0) {
        static char st[16];
        snprintf(st, sizeof(st), "%d", sh->last_status);
        return st;
    }
    if (strcmp(name, "#") == 0) {
        static char st[16];
        snprintf(st, sizeof(st), "%d", sh->nposargs);
        return st;
    }
    if (name[0] >= '1' && name[0] <= '9' && name[1] == '\0') {
        int idx = name[0] - '0';
        if (idx <= sh->nposargs)
            return sh->posargs[idx - 1];
        return "";
    }
    var_t *v = find_var(sh, name);
    return v ? v->value : NULL;
}

void shell_set_var(shell_t *sh, const char *name, const char *value)
{
    if (!name || !*name)
        return;
    var_t *v = find_var(sh, name);
    if (v) {
        free(v->value);
        v->value = xstrdup(value ? value : "");
        return;
    }
    if (sh->nvars == sh->vcap) {
        sh->vcap = sh->vcap ? sh->vcap * 2 : 16;
        sh->vars = xrealloc(sh->vars, sh->vcap * sizeof(*sh->vars));
    }
    sh->vars[sh->nvars].name = xstrdup(name);
    sh->vars[sh->nvars].value = xstrdup(value ? value : "");
    sh->nvars++;
}

void shell_unset_var(shell_t *sh, const char *name)
{
    for (size_t i = 0; i < sh->nvars; i++) {
        if (strcmp(sh->vars[i].name, name) == 0) {
            free(sh->vars[i].name);
            free(sh->vars[i].value);
            sh->vars[i] = sh->vars[sh->nvars - 1];
            sh->nvars--;
            return;
        }
    }
}

void shell_copy_vars(shell_t *dst, shell_t *src)
{
    for (size_t i = 0; i < src->nvars; i++)
        shell_set_var(dst, src->vars[i].name, src->vars[i].value);
}

int  shell_last_status(shell_t *sh) { return sh->last_status; }
void shell_set_last_status(shell_t *sh, int st) { sh->last_status = st; }

const char *shell_cwd(shell_t *sh) { return sh->cwd; }

void shell_set_cwd(shell_t *sh, const char *cwd)
{
    free(sh->cwd);
    sh->cwd = xstrdup(cwd);
}

void shell_set_posargs(shell_t *sh, char *const *args, int n)
{
    for (int i = 0; i < sh->nposargs; i++)
        free(sh->posargs[i]);
    free(sh->posargs);
    sh->posargs = NULL;
    sh->nposargs = 0;
    if (n > 0) {
        sh->posargs = xcalloc((size_t)n, sizeof(char *));
        for (int i = 0; i < n; i++)
            sh->posargs[i] = xstrdup(args[i]);
        sh->nposargs = n;
    }
}

const char *shell_posarg(shell_t *sh, int i)
{
    if (i < 1 || i > sh->nposargs)
        return NULL;
    return sh->posargs[i - 1];
}

int shell_nposargs(shell_t *sh) { return sh->nposargs; }

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

void shell_write(shell_t *sh, const char *data, size_t n)
{
    if (sh->out_fd >= 0) {
        write_all(sh->out_fd, data, n);
        return;
    }
    if (sh->out_fn)
        sh->out_fn(sh->out_ctx, data, n);
    else
        fwrite(data, 1, n, stdout);
}

void shell_puts(shell_t *sh, const char *s)
{
    shell_write(sh, s, strlen(s));
}

void shell_printf(shell_t *sh, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    shell_write(sh, s, (size_t)n);
    free(s);
}

int  shell_exit_requested(shell_t *sh) { return sh->exit_requested; }
int  shell_exit_code(shell_t *sh) { return sh->exit_code; }
void shell_request_exit(shell_t *sh, int code)
{
    sh->exit_requested = 1;
    sh->exit_code = code;
}

void shell_add_async_fd(shell_t *sh, int fd)
{
    if (sh->nasync == sh->acap) {
        sh->acap = sh->acap ? sh->acap * 2 : 4;
        sh->async_fds = xrealloc(sh->async_fds, sh->acap * sizeof(int));
    }
    sh->async_fds[sh->nasync++] = fd;
}

/* ------------------------------------------------------------------ */
/* Built-ins                                                          */
/* ------------------------------------------------------------------ */

static const char *BUILTINS[] = {
    "cd", "clear", "exit", "history", "multiWatch", "jobs", "fg", "bg",
    "source", NULL
};

int shell_is_builtin(const char *name)
{
    for (int i = 0; BUILTINS[i]; i++)
        if (strcmp(BUILTINS[i], name) == 0)
            return 1;
    return 0;
}

static void builtin_cd(shell_t *sh, char **argv, int argc)
{
    const char *target;
    if (argc < 2) {
        target = shell_get_var(sh, "HOME");
        if (!target)
            target = "/";
    } else {
        target = argv[1];
    }
    /* Resolve relative to the logical cwd. */
    char resolved[PATH_MAX];
    if (target[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", target);
    } else {
        snprintf(resolved, sizeof(resolved), "%s/%s", shell_cwd(sh), target);
    }
    /* Normalise "." and ".." components lexically. */
    char norm[PATH_MAX];
    size_t o = 0;
    norm[o++] = '/';
    const char *p = resolved;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - seg);
        if (len == 1 && seg[0] == '.')
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 1 && norm[o - 1] != '/')
                o--;
            if (o > 1)
                o--;
            continue;
        }
        if (o > 1)
            norm[o++] = '/';
        if (o + len >= sizeof(norm))
            break;
        memcpy(norm + o, seg, len);
        o += len;
    }
    norm[o] = '\0';
    if (o == 0) {
        norm[0] = '/';
        norm[1] = '\0';
    }

    struct stat st;
    if (stat(norm, &st) != 0) {
        shell_printf(sh, "cd: %s: %s\n", target, strerror(errno));
        shell_set_last_status(sh, 1);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        shell_printf(sh, "cd: %s: not a directory\n", target);
        shell_set_last_status(sh, 1);
        return;
    }
    shell_set_cwd(sh, norm);
    shell_set_last_status(sh, 0);
}

static void builtin_history(shell_t *sh, char **argv, int argc)
{
    (void)argv;
    (void)argc;
    if (!sh->history) {
        shell_set_last_status(sh, 0);
        return;
    }
    size_t c = history_count(sh->history);
    size_t start = c > HISTORY_DISPLAY ? c - HISTORY_DISPLAY : 0;
    for (size_t i = start; i < c; i++) {
        const char *e = history_get(sh->history, i);
        if (e)
            shell_printf(sh, "%5zu  %s\n", i + 1, e);
    }
    shell_set_last_status(sh, 0);
}

/* Ask the terminal frontend to clear the visible screen and scrollback. */
static void builtin_clear(shell_t *sh)
{
    shell_puts(sh, "\033[2J");
    shell_set_last_status(sh, 0);
}

static void builtin_jobs(shell_t *sh, char **argv, int argc)
{
    (void)argv;
    (void)argc;
    if (!sh->jobs) {
        shell_set_last_status(sh, 0);
        return;
    }
    jobs_reap(sh->jobs);
    size_t n = jobs_count(sh->jobs);
    for (size_t i = 0; i < n; i++) {
        job_t *j = jobs_at(sh->jobs, i);
        const char *state = j->state == JOB_RUNNING ? "Running" :
                            j->state == JOB_STOPPED ? "Stopped" : "Done";
        shell_printf(sh, "[%d] %s  %s\n", j->id, state, j->cmdline);
    }
    shell_set_last_status(sh, 0);
}

static int parse_jobspec(const char *s)
{
    if (!s)
        return -1;
    if (s[0] == '%')
        s++;
    return atoi(s);
}

static void builtin_fg(shell_t *sh, char **argv, int argc)
{
    if (!sh->jobs) {
        shell_set_last_status(sh, 1);
        return;
    }
    int id = argc >= 2 ? parse_jobspec(argv[1]) : -1;
    job_t *j = id > 0 ? jobs_find(sh->jobs, id) : jobs_last(sh->jobs);
    if (!j) {
        shell_puts(sh, "fg: no such job\n");
        shell_set_last_status(sh, 1);
        return;
    }
    shell_printf(sh, "%s\n", j->cmdline);
    jobs_set_foreground(sh->jobs, j->id);
    if (j->state == JOB_STOPPED) {
        jobs_signal(sh->jobs, j->id, SIGCONT);
        j->state = JOB_RUNNING;
    }
    /* Wait for the job to finish or stop. */
    int status = 0;
    for (size_t i = 0; i < j->npids; i++) {
        if (j->pids[i] <= 0)
            continue;
        while (waitpid(j->pids[i], &status, WUNTRACED) < 0 && errno == EINTR)
            ;
    }
    jobs_set_foreground(sh->jobs, -1);
    if (WIFSTOPPED(status)) {
        j->state = JOB_STOPPED;
        shell_printf(sh, "\n[%d] Stopped  %s\n", j->id, j->cmdline);
    } else {
        j->state = JOB_DONE;
        j->notified = 1;
        if (WIFEXITED(status))
            shell_set_last_status(sh, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            shell_set_last_status(sh, 128 + WTERMSIG(status));
    }
}

static void builtin_bg(shell_t *sh, char **argv, int argc)
{
    if (!sh->jobs) {
        shell_set_last_status(sh, 1);
        return;
    }
    int id = argc >= 2 ? parse_jobspec(argv[1]) : -1;
    job_t *j = id > 0 ? jobs_find(sh->jobs, id) : jobs_last(sh->jobs);
    if (!j) {
        shell_puts(sh, "bg: no such job\n");
        shell_set_last_status(sh, 1);
        return;
    }
    jobs_signal(sh->jobs, j->id, SIGCONT);
    j->state = JOB_RUNNING;
    shell_printf(sh, "[%d] %s &\n", j->id, j->cmdline);
    shell_set_last_status(sh, 0);
}

static void builtin_source(shell_t *sh, char **argv, int argc)
{
    if (argc < 2) {
        shell_puts(sh, "source: filename required\n");
        shell_set_last_status(sh, 1);
        return;
    }
    int st = scripts_source(sh, argv[1], argv + 2, argc - 2);
    shell_set_last_status(sh, st);
}

static void builtin_exit(shell_t *sh, char **argv, int argc)
{
    int code = sh->last_status;
    if (argc >= 2)
        code = atoi(argv[1]) & 0xff;
    shell_request_exit(sh, code);
    shell_set_last_status(sh, code);
}

void shell_run_builtin(shell_t *sh, const char *name, char **argv, int argc)
{
    if (strcmp(name, "cd") == 0)
        builtin_cd(sh, argv, argc);
    else if (strcmp(name, "clear") == 0)
        builtin_clear(sh);
    else if (strcmp(name, "exit") == 0)
        builtin_exit(sh, argv, argc);
    else if (strcmp(name, "history") == 0)
        builtin_history(sh, argv, argc);
    else if (strcmp(name, "jobs") == 0)
        builtin_jobs(sh, argv, argc);
    else if (strcmp(name, "fg") == 0)
        builtin_fg(sh, argv, argc);
    else if (strcmp(name, "bg") == 0)
        builtin_bg(sh, argv, argc);
    else if (strcmp(name, "source") == 0)
        builtin_source(sh, argv, argc);
    else if (strcmp(name, "multiWatch") == 0)
        shell_set_last_status(sh, multiwatch_run(sh, argv, argc));
}

/* Is `path` a readable regular file? */
static int is_readable_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Avoid treating arbitrary executables (including this application) as
 * scripts just because they are regular files. Direct extensionless scripts
 * are recognized by their shebang; .sh files may omit one. */
static int is_script_file(const char *path)
{
    if (!is_readable_file(path))
        return 0;
    if (str_ends_with(path, ".sh"))
        return 1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int first = fgetc(f);
    int second = fgetc(f);
    fclose(f);
    return first == '#' && second == '!';
}

/* Does argv describe a script invocation we should interpret ourselves?
 * Recognises `bash script.sh`, `sh script.sh`, `./script.sh`, `../x.sh`,
 * `/abs/path.sh` and any readable file ending in `.sh`. */
int shell_is_script_invocation(char **argv, int argc)
{
    if (argc < 1 || !argv[0])
        return 0;
    if ((strcmp(argv[0], "bash") == 0 || strcmp(argv[0], "sh") == 0) &&
        argc >= 2 && is_readable_file(argv[1]))
        return 1;
    if ((argv[0][0] == '/' || str_starts_with(argv[0], "./") ||
         str_starts_with(argv[0], "../") || str_ends_with(argv[0], ".sh")) &&
        is_script_file(argv[0]))
        return 1;
    return 0;
}

int shell_try_script(shell_t *sh, char **argv, int argc)
{
    if (!shell_is_script_invocation(argv, argc))
        return 0;
    int st;
    if (strcmp(argv[0], "bash") == 0 || strcmp(argv[0], "sh") == 0)
        st = scripts_run_file(sh, argv[1], argv + 2, argc - 2);
    else
        st = scripts_run_file(sh, argv[0], argv + 1, argc - 1);
    shell_set_last_status(sh, st);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Line execution                                                     */
/* ------------------------------------------------------------------ */

/* Detect a leading NAME=value assignment. Returns 1 and fills name/value. */
static int parse_assignment(const word_t *w, char **name, char **value)
{
    /* Only a plain literal prefix "NAME=" followed by the value. */
    if (w->nsegs == 0)
        return 0;
    segment_t *first = &w->segs[0];
    if (first->type != SEG_LITERAL)
        return 0;
    const char *eq = strchr(first->text, '=');
    if (!eq || eq == first->text)
        return 0;
    /* Validate the name. */
    for (const char *p = first->text; p < eq; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    }
    if (first->text[0] >= '0' && first->text[0] <= '9')
        return 0;
    *name = xstrndup(first->text, (size_t)(eq - first->text));
    /* Value = remainder of the first segment plus any following segments. */
    buf_t b;
    buf_init(&b);
    buf_append_str(&b, eq + 1);
    for (size_t i = 1; i < w->nsegs; i++) {
        if (w->segs[i].type == SEG_LITERAL)
            buf_append_str(&b, w->segs[i].text);
    }
    *value = buf_steal(&b);
    return 1;
}

int shell_exec_line(shell_t *sh, const char *line)
{
    /* Keep this order in mind for viva: syntax must be tokenized and parsed
     * before assignments or commands can be safely interpreted. */
    /* A new command line starts with a clean interrupt state. */
    sh->interrupt = 0;
    token_list_t tl;
    token_list_init(&tl);
    char err[256];
    lex_status_t ls = lex_line(line, &tl, err, sizeof(err));
    if (ls == LEX_INCOMPLETE) {
        token_list_free(&tl);
        return sh->last_status;
    }
    if (ls == LEX_ERROR) {
        shell_printf(sh, "syntax error: %s\n", err);
        token_list_free(&tl);
        shell_set_last_status(sh, 2);
        return 2;
    }

    cmdline_t cl;
    cmdline_init(&cl);
    if (parse_tokens(&tl, &cl, err, sizeof(err)) != 0) {
        shell_printf(sh, "%s\n", err);
        token_list_free(&tl);
        cmdline_free(&cl);
        shell_set_last_status(sh, 2);
        return 2;
    }
    token_list_free(&tl);

    /* Handle leading variable assignments (persist in the shell context). */
    for (size_t i = 0; i < cl.npipelines; i++) {
        pipeline_t *p = &cl.pipelines[i];
        if (p->ncmds != 1)
            continue;
        command_t *c = &p->cmds[0];
        size_t consumed = 0;
        while (consumed < c->argc) {
            char *name = NULL, *value = NULL;
            if (!parse_assignment(&c->argv[consumed], &name, &value))
                break;
            shell_set_var(sh, name, value);
            free(name);
            free(value);
            consumed++;
        }
        if (consumed > 0) {
            /* Remove the assignment words from the command. */
            for (size_t k = 0; k < consumed; k++)
                word_free(&c->argv[k]);
            memmove(c->argv, c->argv + consumed,
                    (c->argc - consumed) * sizeof(*c->argv));
            c->argc -= consumed;
        }
    }

    int status = executor_run(sh, &cl);
    cmdline_free(&cl);
    return status;
}

/* ------------------------------------------------------------------ */
/* Command substitution capture                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    buf_t b;
} capture_ctx_t;

/* Child-side sink: write straight to the pipe (fd 1) with no stdio buffering. */
static void capture_child_write(void *ctx, const char *data, size_t n)
{
    (void)ctx;
    write_all(STDOUT_FILENO, data, n);
}

char *shell_capture(shell_t *sh, const char *cmd)
{
    /* Run the command in a forked child with stdout redirected to a pipe so
     * the parent shell state is untouched. */
    int fds[2];
    if (pipe(fds) != 0)
        return xstrdup("");
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return xstrdup("");
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        /* Execute the inner command in a fresh shell context. */
        shell_t *child = shell_new();
        shell_set_output(child, capture_child_write, NULL);
        /* Inherit variables and cwd. */
        for (size_t i = 0; i < sh->nvars; i++)
            shell_set_var(child, sh->vars[i].name, sh->vars[i].value);
        shell_set_cwd(child, shell_cwd(sh));
        shell_set_posargs(child, sh->posargs, sh->nposargs);
        shell_exec_line(child, cmd);
        _exit(shell_last_status(child));
    }
    close(fds[1]);
    capture_ctx_t ctx;
    buf_init(&ctx.b);
    char rbuf[4096];
    ssize_t r;
    while ((r = read(fds[0], rbuf, sizeof(rbuf))) > 0)
        buf_append(&ctx.b, rbuf, (size_t)r);
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return buf_steal(&ctx.b);
}

/* ------------------------------------------------------------------ */
/* Completeness check                                                 */
/* ------------------------------------------------------------------ */

int shell_line_complete(const char *line)
{
    token_list_t tl;
    token_list_init(&tl);
    char err[256];
    lex_status_t ls = lex_line(line, &tl, err, sizeof(err));
    token_list_free(&tl);
    return ls != LEX_INCOMPLETE;
}
