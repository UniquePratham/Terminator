/* executor.c — runs pipelines: fork, pipes, redirection, process groups.
 *
 * Design notes:
 *  - Redirection is applied only in forked children; the shell process's own
 *    descriptors are never changed.
 *  - The logical cwd is applied with chdir() only in the child, immediately
 *    before execvp().
 *  - Foreground pipelines are read synchronously by the shell worker (which is
 *    a thread in thread mode and a separate process in process mode), so the
 *    GUI thread is never blocked.
 *  - Background pipelines have their output connected to a pipe registered
 *    with the shell's async reader.
 */
#include "executor.h"
#include "expand.h"
#include "jobs.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Pre-opened redirection descriptors for one command. */
typedef struct {
    int in_fd;    /* -1 if none */
    int out_fd;   /* -1 if none */
} redir_fds_t;

static void free_argv(char **argv, size_t argc)
{
    for (size_t i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

/* Expand a command's words into a NULL-terminated argv. */
static char **build_argv(shell_t *sh, command_t *c, size_t *out_argc)
{
    strlist_t list;
    strlist_init(&list);
    for (size_t i = 0; i < c->argc; i++)
        expand_word(sh, &c->argv[i], &list);
    char **argv = xmalloc((list.n + 1) * sizeof(char *));
    for (size_t i = 0; i < list.n; i++)
        argv[i] = list.items[i];
    argv[list.n] = NULL;
    *out_argc = list.n;
    free(list.items); /* strings moved into argv */
    return argv;
}

/* Open redirection targets in the parent so missing input files are reported
 * before any child runs. Returns 0 on success, -1 on error. */
static int open_redirs(shell_t *sh, command_t *c, redir_fds_t *rf)
{
    rf->in_fd = -1;
    rf->out_fd = -1;
    for (size_t i = 0; i < c->nredirs; i++) {
        redir_t *r = &c->redirs[i];
        char *target = expand_word_single(sh, &r->target);
        int fd;
        if (r->type == REDIR_IN) {
            fd = open(target, O_RDONLY);
            if (fd < 0) {
                shell_printf(sh, "%s: %s\n", target, strerror(errno));
                free(target);
                return -1;
            }
            if (rf->in_fd >= 0)
                close(rf->in_fd);
            rf->in_fd = fd;
        } else {
            int flags = O_WRONLY | O_CREAT |
                        (r->type == REDIR_APPEND ? O_APPEND : O_TRUNC);
            fd = open(target, flags, 0644);
            if (fd < 0) {
                shell_printf(sh, "%s: %s\n", target, strerror(errno));
                free(target);
                if (rf->in_fd >= 0)
                    close(rf->in_fd);
                return -1;
            }
            if (rf->out_fd >= 0)
                close(rf->out_fd);
            rf->out_fd = fd;
        }
        free(target);
    }
    return 0;
}

static int is_shell_program(const char *program)
{
    if (!program)
        return 0;
    const char *base = strrchr(program, '/');
    base = base ? base + 1 : program;
    static const char *const names[] = {
        "bash", "sh", "dash", "zsh", "fish", "ksh", "csh", "tcsh"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(base, names[i]) == 0)
            return 1;
    return 0;
}

/* Child-side setup: process group, pipes, redirection, cwd, exec. */
static void child_exec(shell_t *sh, char **argv,
                       redir_fds_t *rf, pid_t pgid,
                       int in_fd, int out_fd, int err_fd)
{
    /* With job control off (multiWatch children) we deliberately stay in the
     * inherited process group so a single Ctrl+C reaches every descendant. */
    if (pgid > 0 && shell_job_control(sh))
        setpgid(0, pgid);

    if (in_fd >= 0)
        dup2(in_fd, STDIN_FILENO);
    if (out_fd >= 0)
        dup2(out_fd, STDOUT_FILENO);
    if (err_fd >= 0)
        dup2(err_fd, STDERR_FILENO);

    if (rf->in_fd >= 0)
        dup2(rf->in_fd, STDIN_FILENO);
    if (rf->out_fd >= 0)
        dup2(rf->out_fd, STDOUT_FILENO);

    /* Close every descriptor above 2 that we might have inherited. */
    for (int fd = 3; fd < 256; fd++)
        close(fd);

    const char *cwd = shell_cwd(sh);
    if (cwd && chdir(cwd) != 0) {
        /* Report and fail; do not run in the wrong directory. */
        dprintf(STDERR_FILENO, "cd: %s: %s\n", cwd, strerror(errno));
        _exit(1);
    }

    /* Scripts are interpreted by src/scripts.c. Never delegate command text
     * to an external shell, including `bash -c` or an expanded shell name. */
    if (is_shell_program(argv[0])) {
        dprintf(STDERR_FILENO,
                "terminator: external shell execution is disabled; "
                "use 'bash script.sh' for the built-in script interpreter\n");
        _exit(126);
    }

    execvp(argv[0], argv);
    dprintf(STDERR_FILENO, "%s: command not found\n", argv[0]);
    _exit(127);
}

/* Run a single built-in in-process (no pipeline). Returns 1 if handled.
 *
 * Redirections are honoured by temporarily pointing the shell's output at the
 * opened file descriptor; the shell's own descriptors are never changed. */
static int try_builtin(shell_t *sh, command_t *c)
{
    if (c->argc == 0)
        return 0;
    char *name = word_plain(&c->argv[0]);
    int is_builtin = name && shell_is_builtin(name);
    int shell_program = is_shell_program(name);
    /* A script invocation is only recognised when the command name is a plain
     * literal (no variables or command substitution), which avoids expanding
     * the words twice for ordinary external commands. */
    int maybe_script = 0;
    if (!is_builtin && name) {
        char *probe[2];
        probe[0] = name;
        probe[1] = NULL;
        maybe_script = shell_program || shell_is_script_invocation(probe, 1);
    }
    free(name);
    if (!is_builtin && !maybe_script)
        return 0;

    /* Expand the command words exactly once. */
    strlist_t list;
    strlist_init(&list);
    for (size_t i = 0; i < c->argc; i++)
        expand_word(sh, &c->argv[i], &list);
    char **argv = xmalloc((list.n + 1) * sizeof(char *));
    for (size_t i = 0; i < list.n; i++)
        argv[i] = list.items[i];
    argv[list.n] = NULL;
    free(list.items);

    int is_script = !is_builtin &&
                    shell_is_script_invocation(argv, (int)list.n);
    if (!is_builtin && !is_script) {
        if (is_shell_program(argv[0])) {
            shell_printf(sh,
                         "terminator: external shell execution is disabled; "
                         "use 'bash script.sh' for the built-in script interpreter\n");
            shell_set_last_status(sh, 126);
            free_argv(argv, list.n);
            return 1;
        }
        free_argv(argv, list.n);
        return 0;
    }

    /* Open redirections (if any) and point built-in output at them. */
    redir_fds_t rf;
    int saved_out = -1;
    int have_redir = 0;
    if (c->nredirs > 0) {
        if (open_redirs(sh, c, &rf) != 0) {
            shell_set_last_status(sh, 1);
            free_argv(argv, list.n);
            return 1;
        }
        have_redir = 1;
        if (rf.out_fd >= 0) {
            saved_out = shell_get_out_fd(sh);
            shell_set_out_fd(sh, rf.out_fd);
        }
    }

    if (is_builtin)
        shell_run_builtin(sh, argv[0], argv, (int)list.n);
    else
        shell_try_script(sh, argv, (int)list.n);

    if (have_redir) {
        if (rf.out_fd >= 0) {
            shell_set_out_fd(sh, saved_out);
            close(rf.out_fd);
        }
        if (rf.in_fd >= 0)
            close(rf.in_fd);
    }
    free_argv(argv, list.n);
    return 1;
}

static int run_pipeline(shell_t *sh, pipeline_t *p)
{
    /* One pipeline is the unit that runs concurrently. This function handles
     * expansion, redirections, child creation, pipe wiring, job registration,
     * and foreground/background waiting for all commands in the pipeline. */
    size_t n = p->ncmds;
    if (n == 0)
        return 0;

    /* A pipeline whose only command is empty (e.g. a bare `NAME=value`
     * assignment line) has nothing to execute; a successful assignment sets
     * $? to 0. */
    if (n == 1 && p->cmds[0].argc == 0) {
        shell_set_last_status(sh, 0);
        return 0;
    }

    /* Sole built-in (or script) command: run in-process so state changes
     * persist. Redirections are honoured by try_builtin() without touching the
     * shell's own descriptors. */
    if (n == 1 && !p->background) {
        if (try_builtin(sh, &p->cmds[0]))
            return shell_last_status(sh);
    }

    /* Expand argv for every command. */
    char ***argvs = xcalloc(n, sizeof(char *));
    size_t *argcs = xcalloc(n, sizeof(size_t));
    redir_fds_t *rfs = xcalloc(n, sizeof(redir_fds_t));
    for (size_t i = 0; i < n; i++) {
        argvs[i] = build_argv(sh, &p->cmds[i], &argcs[i]);
        if (open_redirs(sh, &p->cmds[i], &rfs[i]) != 0) {
            /* Clean up and abort without running anything. */
            for (size_t j = 0; j <= i; j++) {
                free_argv(argvs[j], argcs[j]);
                if (rfs[j].in_fd >= 0)
                    close(rfs[j].in_fd);
                if (rfs[j].out_fd >= 0)
                    close(rfs[j].out_fd);
            }
            free(argvs);
            free(argcs);
            free(rfs);
            shell_set_last_status(sh, 1);
            return 1;
        }
    }

    /* Create N-1 inter-command pipes. */
    int (*pipes)[2] = NULL;
    if (n > 1)
        pipes = xcalloc(n - 1, sizeof(int[2]));
    for (size_t i = 0; i + 1 < n; i++) {
        if (pipe(pipes[i]) != 0) {
            shell_printf(sh, "pipe: %s\n", strerror(errno));
            /* Clean up everything. */
            for (size_t j = 0; j < n; j++) {
                free_argv(argvs[j], argcs[j]);
                if (rfs[j].in_fd >= 0)
                    close(rfs[j].in_fd);
                if (rfs[j].out_fd >= 0)
                    close(rfs[j].out_fd);
            }
            for (size_t j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            free(argvs);
            free(argcs);
            free(rfs);
            shell_set_last_status(sh, 1);
            return 1;
        }
    }

    /* Capture pipe for the last command's stdout and all stderr. */
    int cap[2];
    if (pipe(cap) != 0) {
        shell_printf(sh, "pipe: %s\n", strerror(errno));
        for (size_t j = 0; j < n; j++) {
            free_argv(argvs[j], argcs[j]);
            if (rfs[j].in_fd >= 0)
                close(rfs[j].in_fd);
            if (rfs[j].out_fd >= 0)
                close(rfs[j].out_fd);
        }
        for (size_t j = 0; j + 1 < n; j++) {
            close(pipes[j][0]);
            close(pipes[j][1]);
        }
        free(pipes);
        free(argvs);
        free(argcs);
        free(rfs);
        shell_set_last_status(sh, 1);
        return 1;
    }

    pid_t *pids = xcalloc(n, sizeof(pid_t));
    pid_t pgid = 0;

    for (size_t i = 0; i < n; i++) {
        int in_fd = (i > 0) ? pipes[i - 1][0] : -1;
        int out_fd = (i + 1 < n) ? pipes[i][1] : -1;
        /* Last command: capture stdout unless redirected to a file. */
        if (i + 1 == n && rfs[i].out_fd < 0)
            out_fd = cap[1];
        int err_fd = cap[1];

        pid_t pid = fork();
        if (pid < 0) {
            shell_printf(sh, "fork: %s\n", strerror(errno));
            break;
        }
        if (pid == 0) {
            /* Child: close the capture read end and all pipe ends we don't use. */
            close(cap[0]);
            for (size_t j = 0; j + 1 < n; j++) {
                if ((int)j != (int)i - 1)
                    close(pipes[j][0]);
                if ((int)j != (int)i)
                    close(pipes[j][1]);
            }
            child_exec(sh, argvs[i], &rfs[i], pgid,
                       in_fd, out_fd, err_fd);
            _exit(127);
        }
        pids[i] = pid;
        if (pgid == 0)
            pgid = pid;
        if (shell_job_control(sh))
            setpgid(pid, pgid);
    }

    /* Parent: close all pipe ends and the capture write end. */
    for (size_t j = 0; j + 1 < n; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }
    close(cap[1]);
    for (size_t j = 0; j < n; j++) {
        if (rfs[j].in_fd >= 0)
            close(rfs[j].in_fd);
        if (rfs[j].out_fd >= 0)
            close(rfs[j].out_fd);
    }

    /* Build a display string for the job table. */
    buf_t disp;
    buf_init(&disp);
    for (size_t i = 0; i < n; i++) {
        if (i)
            buf_append_str(&disp, " | ");
        for (size_t k = 0; k < argcs[i]; k++) {
            if (k)
                buf_append_char(&disp, ' ');
            buf_append_str(&disp, argvs[i][k]);
        }
    }

    if (p->background) {
        /* Register the job and hand the capture read end to the async reader. */
        job_t *j = jobs_add(shell_jobs(sh), pgid, pids, n, disp.data, 1);
        (void)j;
        shell_add_async_fd(sh, cap[0]);
        shell_printf(sh, "[%d] %d\n", jobs_last(shell_jobs(sh))->id, (int)pgid);
    } else {
        job_t *j = jobs_add(shell_jobs(sh), pgid, pids, n, disp.data, 0);
        jobs_set_foreground(shell_jobs(sh), j->id);
        shell_notify_fg(sh, (long)pgid);
        shell_set_foreground_pgid(sh, pgid);

        /* Read captured output while the children run. poll() with a short
         * timeout lets us notice a stopped child (Ctrl+Z) even though the pipe
         * stays open and would otherwise block read() forever. */
        int last_status = 0;
        int stopped = 0;
        int done = 0;
        while (!done) {
            struct pollfd pfd;
            pfd.fd = cap[0];
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 100);
            if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                char rbuf[4096];
                ssize_t r = read(cap[0], rbuf, sizeof(rbuf));
                if (r > 0)
                    shell_write(sh, rbuf, (size_t)r);
                else if (r == 0)
                    done = 1; /* EOF: all writers closed */
            }
            /* Reap any child that exited or stopped. */
            for (size_t i = 0; i < n; i++) {
                if (pids[i] <= 0)
                    continue;
                int status = 0;
                pid_t w = waitpid(pids[i], &status, WNOHANG | WUNTRACED);
                if (w <= 0)
                    continue;
                if (WIFSTOPPED(status)) {
                    /* Keep the pid: the job is still alive and `fg`/`bg`
                     * must be able to signal it later. */
                    stopped = 1;
                    last_status = 128 + WSTOPSIG(status);
                } else {
                    pids[i] = -1;
                    if (WIFEXITED(status))
                        last_status = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status))
                        last_status = 128 + WTERMSIG(status);
                }
            }
            if (stopped)
                break;
            /* All children reaped: drain any remaining buffered output. */
            if (!stopped) {
                int alive = 0;
                for (size_t i = 0; i < n; i++)
                    if (pids[i] > 0)
                        alive = 1;
                if (!alive && done)
                    break;
            }
        }
        /* Drain whatever is left in the pipe (non-blocking). */
        {
            int fl = fcntl(cap[0], F_GETFL, 0);
            if (fl >= 0)
                fcntl(cap[0], F_SETFL, fl | O_NONBLOCK);
            char rbuf[4096];
            ssize_t r;
            while ((r = read(cap[0], rbuf, sizeof(rbuf))) > 0)
                shell_write(sh, rbuf, (size_t)r);
        }
        close(cap[0]);

        jobs_set_foreground(shell_jobs(sh), -1);
        shell_notify_fg(sh, 0);
        shell_set_foreground_pgid(sh, -1);
        if (stopped) {
            /* Leave the job in the table as stopped so `jobs`, `bg` and `fg`
             * can act on it. */
            j->state = JOB_STOPPED;
            j->status = last_status;
            j->notified = 0;
            shell_printf(sh, "\n[%d] Stopped  %s\n", j->id, j->cmdline);
        } else {
            j->state = JOB_DONE;
            j->status = last_status;
            j->notified = 1;
            jobs_prune(shell_jobs(sh));
        }
        shell_set_last_status(sh, last_status);
    }

    buf_free(&disp);
    for (size_t i = 0; i < n; i++)
        free_argv(argvs[i], argcs[i]);
    free(argvs);
    free(argcs);
    free(rfs);
    free(pipes);
    return shell_last_status(sh);
}

int executor_run(shell_t *sh, cmdline_t *cl)
{
    /* Semicolon/newline-separated pipelines run in source order; the final
     * pipeline supplies $? unless Ctrl+C cancels the remaining command line. */
    int status = shell_last_status(sh);
    for (size_t i = 0; i < cl->npipelines; i++) {
        status = run_pipeline(sh, &cl->pipelines[i]);
        /* A foreground command killed by SIGINT aborts the remainder of the
         * command line, matching POSIX shell behaviour (e.g. Ctrl+C on
         * "sleep 30; echo done" must not run the echo). */
        if (status == 128 + SIGINT)
            break;
    }
    return status;
}
