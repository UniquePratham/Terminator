/* history.c — persistent, shared, searchable command history.
 *
 * Thread mode uses a pthread mutex. Process mode uses a process-shared
 * semaphore over a shared-memory ring buffer. Both share the same on-disk
 * file so history survives restarts.
 */
#include "history.h"
#include "util.h"

#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define HIST_ENTRY_MAX 4096

/* Keep callers from reading ring entries while another tab overwrites them. */
static _Thread_local char history_entry_copy[HIST_ENTRY_MAX];

typedef struct {
    char  *entries[HISTORY_MAX];
    size_t count;   /* total ever added (for indexing) */
    size_t head;    /* next slot to write */
} hist_data_t;

struct history {
    char        *path;
    int          shared;
    hist_data_t *data;      /* points into shared memory in process mode */
    void        *shm;       /* mmap base (process mode) */
    size_t       shm_size;
    pthread_mutex_t mutex;  /* thread mode */
    sem_t       *sem;       /* process mode (in shared memory) */
};

/* Shared-memory layout: header (semaphore) + fixed-size entry table. */
typedef struct {
    sem_t sem;
    size_t count;
    size_t head;
    char   entries[HISTORY_MAX][HIST_ENTRY_MAX];
} hist_shm_t;

static void history_load(history_t *h)
{
    FILE *f = fopen(h->path, "r");
    if (!f)
        return;
    char line[HIST_ENTRY_MAX];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0)
            continue;
        history_add(h, line);
    }
    fclose(f);
}

history_t *history_new(const char *path, int shared)
{
    history_t *h = xcalloc(1, sizeof(*h));
    h->path = xstrdup(path);
    h->shared = shared;
    pthread_mutex_init(&h->mutex, NULL);

    if (shared) {
        h->shm_size = sizeof(hist_shm_t);
        h->shm = mmap(NULL, h->shm_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (h->shm == MAP_FAILED) {
            h->shm = NULL;
            h->shared = 0;
        } else {
            hist_shm_t *s = h->shm;
            sem_init(&s->sem, 1, 1);
            s->count = 0;
            s->head = 0;
            h->sem = &s->sem;
        }
    }
    history_load(h);
    return h;
}

void history_free(history_t *h)
{
    if (!h)
        return;
    history_save(h);
    if (h->shared && h->shm) {
        hist_shm_t *s = h->shm;
        sem_destroy(&s->sem);
        munmap(h->shm, h->shm_size);
    } else {
        for (size_t i = 0; i < HISTORY_MAX; i++)
            free(h->data ? h->data->entries[i] : NULL);
        free(h->data);
    }
    pthread_mutex_destroy(&h->mutex);
    free(h->path);
    free(h);
}

static void lock(history_t *h)
{
    if (h->shared)
        sem_wait(h->sem);
    else
        pthread_mutex_lock(&h->mutex);
}

static void unlock(history_t *h)
{
    if (h->shared)
        sem_post(h->sem);
    else
        pthread_mutex_unlock(&h->mutex);
}

void history_add(history_t *h, const char *cmd)
{
    if (!cmd || !*cmd)
        return;
    lock(h);
    if (h->shared) {
        hist_shm_t *s = h->shm;
        if (s->count > 0) {
            size_t last = (s->head + HISTORY_MAX - 1) % HISTORY_MAX;
            if (strcmp(s->entries[last], cmd) == 0) {
                unlock(h);
                return;
            }
        }
        snprintf(s->entries[s->head], HIST_ENTRY_MAX, "%s", cmd);
        s->head = (s->head + 1) % HISTORY_MAX;
        s->count++;
    } else {
        if (!h->data)
            h->data = xcalloc(1, sizeof(hist_data_t));
        hist_data_t *d = h->data;
        if (d->count > 0) {
            size_t last = (d->head + HISTORY_MAX - 1) % HISTORY_MAX;
            if (d->entries[last] && strcmp(d->entries[last], cmd) == 0) {
                unlock(h);
                return;
            }
        }
        free(d->entries[d->head]);
        d->entries[d->head] = xstrdup(cmd);
        d->head = (d->head + 1) % HISTORY_MAX;
        d->count++;
    }
    unlock(h);
}

size_t history_count(history_t *h)
{
    lock(h);
    size_t c = h->shared ? ((hist_shm_t *)h->shm)->count
                         : (h->data ? h->data->count : 0);
    unlock(h);
    return c;
}

const char *history_get(history_t *h, size_t idx)
{
    /* idx 0 = oldest retained. Return a per-thread snapshot so concurrent
     * writers cannot mutate a string while a caller formats or searches it. */
    lock(h);
    const char *entry = NULL;
    if (h->shared) {
        hist_shm_t *s = h->shm;
        size_t start = s->count > HISTORY_MAX ? s->count - HISTORY_MAX : 0;
        if (idx >= start && idx < s->count) {
            size_t slot = (s->head + HISTORY_MAX - (s->count - idx)) % HISTORY_MAX;
            entry = s->entries[slot];
        }
    } else if (h->data) {
        hist_data_t *d = h->data;
        size_t start = d->count > HISTORY_MAX ? d->count - HISTORY_MAX : 0;
        if (idx >= start && idx < d->count) {
            size_t slot = (d->head + HISTORY_MAX - (d->count - idx)) % HISTORY_MAX;
            entry = d->entries[slot];
        }
    }
    if (entry)
        snprintf(history_entry_copy, sizeof(history_entry_copy), "%s", entry);
    unlock(h);
    return entry ? history_entry_copy : NULL;
}

const char *history_last(history_t *h)
{
    size_t c = history_count(h);
    if (c == 0)
        return NULL;
    return history_get(h, c - 1);
}

/* Longest common substring length between a and b. */
static size_t lcs_len(const char *a, const char *b)
{
    /* Dynamic programming for longest common substring. Two rows are enough:
     * cur[j] depends only on prev[j-1], so memory is O(strlen(b)), not a full
     * matrix. A mismatch resets the substring length to zero. */
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0 || lb == 0)
        return 0;
    size_t *prev = xcalloc(lb + 1, sizeof(size_t));
    size_t *cur = xcalloc(lb + 1, sizeof(size_t));
    size_t best = 0;
    for (size_t i = 1; i <= la; i++) {
        for (size_t j = 1; j <= lb; j++) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
                if (cur[j] > best)
                    best = cur[j];
            } else {
                cur[j] = 0;
            }
        }
        size_t *t = prev;
        prev = cur;
        cur = t;
        memset(cur, 0, (lb + 1) * sizeof(size_t));
    }
    free(prev);
    free(cur);
    return best;
}

char **history_search_matches(history_t *h, const char *term, size_t *count)
{
    /* Prefer exact matches. If none exists, rank commands by longest common
     * substring and return all ties only when the shared substring is useful. */
    if (count)
        *count = 0;
    if (!term || !*term)
        return NULL;

    size_t n = history_count(h);
    if (n == 0)
        return NULL;

    /* Exact matches take priority and only the newest exact entry is shown. */
    for (size_t i = n; i-- > 0;) {
        const char *entry = history_get(h, i);
        if (entry && strcmp(entry, term) == 0) {
            char **result = xcalloc(1, sizeof(*result));
            result[0] = xstrdup(entry);
            if (count)
                *count = 1;
            return result;
        }
    }

    size_t best = 0;
    for (size_t i = 0; i < n; i++) {
        const char *entry = history_get(h, i);
        if (!entry)
            continue;
        size_t score = lcs_len(entry, term);
        if (score > best)
            best = score;
    }
    if (best <= 2)
        return NULL;

    char **result = NULL;
    size_t found = 0;
    /* Search newest-first so the returned display order prioritizes recent use. */
    for (size_t i = n; i-- > 0;) {
        const char *entry = history_get(h, i);
        if (!entry || lcs_len(entry, term) != best)
            continue;
        result = xrealloc(result, (found + 1) * sizeof(*result));
        result[found++] = xstrdup(entry);
    }
    if (count)
        *count = found;
    return result;
}

char *history_search(history_t *h, const char *term)
{
    if (!term || !*term)
        return NULL;
    size_t c = history_count(h);
    if (c == 0)
        return NULL;

    /* 1. Most recent exact match. */
    for (size_t i = c; i-- > 0;) {
        const char *e = history_get(h, i);
        if (e && strcmp(e, term) == 0)
            return xstrdup(e);
    }
    /* 2. Best longest-common-substring match, if > 2. */
    size_t best_len = 0;
    char *best = NULL;
    for (size_t i = 0; i < c; i++) {
        const char *e = history_get(h, i);
        if (!e)
            continue;
        size_t l = lcs_len(e, term);
        if (l > best_len) {
            best_len = l;
            free(best);
            best = xstrdup(e);
        }
    }
    if (best_len > 2)
        return best;
    free(best);
    return NULL;
}

void history_save(history_t *h)
{
    FILE *f = fopen(h->path, "w");
    if (!f)
        return;
    size_t c = history_count(h);
    size_t start = c > HISTORY_MAX ? c - HISTORY_MAX : 0;
    for (size_t i = start; i < c; i++) {
        const char *e = history_get(h, i);
        if (e)
            fprintf(f, "%s\n", e);
    }
    fclose(f);
}
