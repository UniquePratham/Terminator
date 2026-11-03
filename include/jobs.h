/* jobs.h — job table, process groups, and job-control built-ins. */
#ifndef TERMINATOR_JOBS_H
#define TERMINATOR_JOBS_H

#include <sys/types.h>
#include <stddef.h>

typedef enum {
    JOB_RUNNING, /* at least one process in the group is still executing */
    JOB_STOPPED, /* foreground/background group was suspended */
    JOB_DONE     /* all children exited or were terminated */
} job_state_t;

typedef struct {
    int          id;        /* job number (%1, %2, ...) */
    pid_t        pgid;      /* process group id */
    pid_t       *pids;      /* member pids */
    size_t       npids;      /* number of valid PIDs in pids[] */
    char        *cmdline;   /* owned display copy, freed with this job */
    job_state_t  state;     /* most recently observed group lifecycle state */
    int          status;    /* shell-style status when the job is done */
    int          background;/* nonzero when started with trailing & */
    int          notified;  /* completion already reported */
} job_t;

typedef struct job_table job_table_t;

job_table_t *jobs_new(void);
void         jobs_free(job_table_t *jt);

/* Register a new job. Takes ownership of the pids array. */
job_t *jobs_add(job_table_t *jt, pid_t pgid, pid_t *pids, size_t npids,
                const char *cmdline, int background);

job_t *jobs_find(job_table_t *jt, int id);
job_t *jobs_last(job_table_t *jt);
size_t jobs_count(job_table_t *jt);
job_t *jobs_at(job_table_t *jt, size_t idx);

/* Foreground job tracking (used to route Ctrl+C / Ctrl+Z). */
void  jobs_set_foreground(job_table_t *jt, int id);
int   jobs_foreground_id(job_table_t *jt);
pid_t jobs_foreground_pgid(job_table_t *jt);

/* Send a signal to a job's process group. */
void jobs_signal(job_table_t *jt, int id, int sig);

/* Reap children non-blockingly; returns number of state changes. */
int  jobs_reap(job_table_t *jt);

/* Remove finished jobs that have been reported. */
void jobs_prune(job_table_t *jt);

#endif /* TERMINATOR_JOBS_H */
