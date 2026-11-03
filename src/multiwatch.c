/* multiwatch.c — the multiWatch built-in.
 *
 * Runs each listed command concurrently, one process per command, capturing
 * each command's output through its own pipe. Output is printed as it arrives
 * in the required format. Each command is re-run every `interval` seconds
 * (default 2). Ctrl+C (SIGINT) delivered to the shared process group stops all
 * children and returns to the prompt.
 */
#include "multiwatch.h"
#include "config.h"
#include "jobs.h"
#include "shell.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MW_MAX_CMDS 64

typedef struct {
    char   *cmd;
    pid_t   pid;
    int     fd;          /* read end of the output pipe, -1 when idle */
    int     header;      /* header printed for the current run */
    time_t  started;
    time_t  next_run;    /* when to start the next run */
    int     stop;        /* stop re-running this command */
} mw_cmd_t;

static void print_header(shell_t *sh, mw_cmd_t *c)
{
    shell_printf(sh, "\"%s\", %ld:\n", c->cmd, (long)c->started);
    shell_puts(sh, "----------------------------------------------------\n");
}

static void print_footer(shell_t *sh)
{
    shell_puts(sh, "----------------------------------------------------\n");
}

/* Child-side sink: write straight to the pipe (fd 1) with no stdio buffering,
 * so output is not lost when the child calls _exit(). */
static void mw_child_write(void *ctx, const char *data, size_t n)
{
    (void)ctx;
    write_all(STDOUT_FILENO, data, n);
}

/* Fork a child that runs `cmd` with stdout connected to a new pipe. */
static int start_cmd(shell_t *sh, mw_cmd_t *c, pid_t pgid)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        /* Die on Ctrl+C like a normal foreground command. */
        signal(SIGINT, SIG_DFL);
        /* Join the shared process group. The first child (pgid == 0) makes
         * itself a group leader immediately so it is never left behind in the
         * parent's group — otherwise Ctrl+C would not reach it. */
        if (pgid > 0)
            setpgid(0, pgid);
        else
            setpgid(0, 0);
        shell_t *child = shell_new();
        /* Inherit variables, cwd and positional parameters. */
        shell_copy_vars(child, sh);
        shell_set_cwd(child, shell_cwd(sh));
        /* Keep every descendant in the shared process group so one Ctrl+C
         * reaches the whole tree (no per-command process groups). */
        shell_set_job_control(child, 0);
        /* Write straight to the pipe: the child exits with _exit(), so any
         * stdio-buffered output would be discarded. */
        shell_set_output(child, mw_child_write, NULL);
        int npos = shell_nposargs(sh);
        if (npos > 0) {
            char **pa = xmalloc((size_t)npos * sizeof(*pa));
            for (int k = 0; k < npos; k++)
                pa[k] = (char *)shell_posarg(sh, k + 1);
            shell_set_posargs(child, pa, npos);
            free(pa);
        }
        shell_exec_line(child, c->cmd);
        _exit(shell_last_status(child));
    }
    close(fds[1]);
    /* Mirror the child's setpgid so the group exists regardless of which side
     * wins the race. The first child becomes its own group leader. */
    setpgid(pid, pgid > 0 ? pgid : pid);
    c->pid = pid;
    c->fd = fds[0];
    c->header = 0;
    c->started = time(NULL);
    return 0;
}

int multiwatch_run(shell_t *sh, char **argv, int argc)
{
    /* Launch each requested command as its own child/process group member,
     * poll their output pipes together, print a refresh, then repeat at the
     * configured interval until interrupted. This is implemented here rather
     * than by forwarding text to another shell. */
    if (argc < 2) {
        shell_puts(sh, "multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");
        return 1;
    }
    if (argc - 1 > MW_MAX_CMDS) {
        shell_puts(sh, "multiWatch: too many commands\n");
        return 1;
    }

    int interval = 2;
    config_t *cfg = shell_config(sh);
    if (cfg && cfg->multiwatch_interval > 0)
        interval = cfg->multiwatch_interval;

    /* Start from a clean interrupt state. */
    shell_clear_interrupt(sh);

    size_t n = (size_t)(argc - 1);
    mw_cmd_t *cmds = xcalloc(n, sizeof(*cmds));
    for (size_t i = 0; i < n; i++) {
        cmds[i].cmd = xstrdup(argv[i + 1]);
        cmds[i].fd = -1;
    }

    /* One process group for all children so Ctrl+C reaches them together. */
    pid_t pgid = 0;
    for (size_t i = 0; i < n; i++) {
        if (start_cmd(sh, &cmds[i], pgid) != 0) {
            shell_printf(sh, "multiWatch: failed to start \"%s\"\n", cmds[i].cmd);
            cmds[i].stop = 1;
            continue;
        }
        if (pgid == 0)
            pgid = cmds[i].pid;
        else
            setpgid(cmds[i].pid, pgid);
    }

    /* Register as a foreground job so the GUI forwards Ctrl+C here. */
    job_t *job = NULL;
    if (pgid > 0 && shell_jobs(sh)) {
        pid_t *pids = xcalloc(n, sizeof(pid_t));
        for (size_t i = 0; i < n; i++)
            pids[i] = cmds[i].pid;
        job = jobs_add(shell_jobs(sh), pgid, pids, n, "multiWatch", 0);
        jobs_set_foreground(shell_jobs(sh), job->id);
        /* Tell the GUI/worker which process group is in the foreground so
         * Ctrl+C is forwarded here (process mode). */
        shell_notify_fg(sh, (long)pgid);
    }

    int active = 0;
    for (size_t i = 0; i < n; i++)
        if (!cmds[i].stop)
            active++;

    while (active > 0) {
        if (shell_interrupted(sh))
            break;
        struct pollfd *pfds = xcalloc(n, sizeof(*pfds));
        size_t np = 0;
        for (size_t i = 0; i < n; i++) {
            if (cmds[i].fd >= 0) {
                pfds[np].fd = cmds[i].fd;
                pfds[np].events = POLLIN;
                pfds[np].revents = 0;
                np++;
            }
        }
        int timeout_ms = 200;
        int r = poll(pfds, np, timeout_ms);
        if (shell_interrupted(sh)) {
            free(pfds);
            break;
        }
        if (r > 0) {
            size_t k = 0;
            for (size_t i = 0; i < n; i++) {
                if (cmds[i].fd < 0)
                    continue;
                short rev = pfds[k++].revents;
                if (!(rev & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                char buf[4096];
                ssize_t got = read(cmds[i].fd, buf, sizeof(buf));
                if (got > 0) {
                    if (!cmds[i].header) {
                        print_header(sh, &cmds[i]);
                        cmds[i].header = 1;
                    }
                    shell_write(sh, buf, (size_t)got);
                } else if (got == 0) {
                    /* EOF: finish this run. */
                    if (cmds[i].header)
                        print_footer(sh);
                    close(cmds[i].fd);
                    cmds[i].fd = -1;
                    int status = 0;
                    while (waitpid(cmds[i].pid, &status, 0) < 0 && errno == EINTR)
                        ;
                    cmds[i].pid = -1;
                    if (WIFSIGNALED(status) &&
                        (WTERMSIG(status) == SIGINT || WTERMSIG(status) == SIGTERM)) {
                        cmds[i].stop = 1;
                        active--;
                    } else {
                        cmds[i].next_run = time(NULL) + interval;
                    }
                }
            }
        }

        /* Start any commands whose interval has elapsed. */
        time_t now = time(NULL);
        for (size_t i = 0; i < n; i++) {
            if (cmds[i].stop || cmds[i].fd >= 0)
                continue;
            if (cmds[i].next_run != 0 && now >= cmds[i].next_run) {
                if (start_cmd(sh, &cmds[i], pgid) == 0)
                    cmds[i].next_run = 0;
            }
        }
        free(pfds);
    }

    /* If interrupted, stop and reap any children still running. */
    if (shell_interrupted(sh)) {
        if (pgid > 0)
            kill(-pgid, SIGINT);
        for (size_t i = 0; i < n; i++) {
            if (cmds[i].pid > 0) {
                int st;
                while (waitpid(cmds[i].pid, &st, 0) < 0 && errno == EINTR)
                    ;
                cmds[i].pid = -1;
            }
        }
    }

    if (job) {
        jobs_set_foreground(shell_jobs(sh), -1);
        shell_notify_fg(sh, 0);
        job->state = JOB_DONE;
        job->notified = 1;
        jobs_prune(shell_jobs(sh));
    }
    for (size_t i = 0; i < n; i++) {
        if (cmds[i].fd >= 0)
            close(cmds[i].fd);
        free(cmds[i].cmd);
    }
    free(cmds);
    return 0;
}
