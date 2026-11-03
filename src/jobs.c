/* jobs.c — job table, process groups, and reaping.
 *
 * A job represents a process group (possibly several pipeline children), not
 * a single PID. waitpid() updates its running/stopped/done state; the table
 * keeps stable job numbers for jobs/fg/bg and removes reported completions.
 */
#include "jobs.h"
#include "util.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

struct job_table {
    job_t  *jobs;
    size_t  n;
    size_t  cap;
    int     next_id;
    int     fg_id;
};

job_table_t *jobs_new(void)
{
    job_table_t *jt = xcalloc(1, sizeof(*jt));
    jt->next_id = 1;
    jt->fg_id = -1;
    return jt;
}

static void job_free(job_t *j)
{
    free(j->pids);
    free(j->cmdline);
}

void jobs_free(job_table_t *jt)
{
    if (!jt)
        return;
    for (size_t i = 0; i < jt->n; i++)
        job_free(&jt->jobs[i]);
    free(jt->jobs);
    free(jt);
}

job_t *jobs_add(job_table_t *jt, pid_t pgid, pid_t *pids, size_t npids,
                const char *cmdline, int background)
{
    if (jt->n == jt->cap) {
        jt->cap = jt->cap ? jt->cap * 2 : 8;
        jt->jobs = xrealloc(jt->jobs, jt->cap * sizeof(*jt->jobs));
    }
    job_t *j = &jt->jobs[jt->n++];
    j->id = jt->next_id++;
    j->pgid = pgid;
    j->pids = pids;
    j->npids = npids;
    j->cmdline = xstrdup(cmdline ? cmdline : "");
    j->state = JOB_RUNNING;
    j->status = 0;
    j->background = background;
    j->notified = 0;
    return j;
}

job_t *jobs_find(job_table_t *jt, int id)
{
    for (size_t i = 0; i < jt->n; i++)
        if (jt->jobs[i].id == id)
            return &jt->jobs[i];
    return NULL;
}

job_t *jobs_last(job_table_t *jt)
{
    if (jt->n == 0)
        return NULL;
    return &jt->jobs[jt->n - 1];
}

size_t jobs_count(job_table_t *jt) { return jt->n; }
job_t *jobs_at(job_table_t *jt, size_t idx)
{
    return idx < jt->n ? &jt->jobs[idx] : NULL;
}

void jobs_set_foreground(job_table_t *jt, int id) { jt->fg_id = id; }
int  jobs_foreground_id(job_table_t *jt) { return jt->fg_id; }

pid_t jobs_foreground_pgid(job_table_t *jt)
{
    job_t *j = jobs_find(jt, jt->fg_id);
    return j ? j->pgid : -1;
}

void jobs_signal(job_table_t *jt, int id, int sig)
{
    job_t *j = jobs_find(jt, id);
    if (!j)
        return;
    if (j->pgid > 0)
        kill(-j->pgid, sig);
    else
        for (size_t i = 0; i < j->npids; i++)
            kill(j->pids[i], sig);
}

int jobs_reap(job_table_t *jt)
{
    int changes = 0;
    for (size_t i = 0; i < jt->n; i++) {
        job_t *j = &jt->jobs[i];
        if (j->state == JOB_DONE)
            continue;
        int alive = 0;
        for (size_t k = 0; k < j->npids; k++) {
            if (j->pids[k] <= 0)
                continue;
            int status = 0;
            pid_t r = waitpid(j->pids[k], &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (r == 0) {
                alive = 1;
                continue;
            }
            if (r < 0) {
                j->pids[k] = -1;
                continue;
            }
            if (WIFEXITED(status)) {
                j->status = WEXITSTATUS(status);
                j->pids[k] = -1;
                changes++;
            } else if (WIFSIGNALED(status)) {
                j->status = 128 + WTERMSIG(status);
                j->pids[k] = -1;
                changes++;
            } else if (WIFSTOPPED(status)) {
                j->state = JOB_STOPPED;
                alive = 1;
                changes++;
            } else if (WIFCONTINUED(status)) {
                j->state = JOB_RUNNING;
                alive = 1;
                changes++;
            }
        }
        if (!alive && j->state != JOB_STOPPED) {
            j->state = JOB_DONE;
            changes++;
        }
    }
    return changes;
}

void jobs_prune(job_table_t *jt)
{
    size_t w = 0;
    for (size_t i = 0; i < jt->n; i++) {
        if (jt->jobs[i].state == JOB_DONE && jt->jobs[i].notified) {
            job_free(&jt->jobs[i]);
            continue;
        }
        jt->jobs[w++] = jt->jobs[i];
    }
    jt->n = w;
}
