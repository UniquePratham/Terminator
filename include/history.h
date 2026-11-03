/* history.h — persistent, shared, searchable command history. */
#ifndef TERMINATOR_HISTORY_H
#define TERMINATOR_HISTORY_H

#include <stddef.h>

#define HISTORY_MAX      10000
#define HISTORY_DISPLAY  1000

typedef struct history history_t;

/* Create a history backed by `path`. If shared is non-zero the history uses
 * process-shared memory and a process-shared semaphore (process mode). */
history_t *history_new(const char *path, int shared);
void       history_free(history_t *h);

/* Append a command (empty/duplicate-of-last commands are ignored). */
void history_add(history_t *h, const char *cmd);

/* Number of stored entries. */
size_t history_count(history_t *h);

/* Entry by index (0 = oldest). Returns a per-thread snapshot, or NULL if out
 * of range. The pointer is overwritten by the next history_get() on this
 * thread. */
const char *history_get(history_t *h, size_t idx);

/* Most recent entry, or NULL. */
const char *history_last(history_t *h);

/* Search: exact match first, then longest-common-substring (>2 chars).
 * Returns a heap string (caller frees) or NULL when no match. */
char *history_search(history_t *h, const char *term);
/* Return the most recent exact match, or every command tied for the best
 * longest-common-substring score (> 2). Caller frees each string and array. */
char **history_search_matches(history_t *h, const char *term, size_t *count);

/* Persist to disk. */
void history_save(history_t *h);

#endif /* TERMINATOR_HISTORY_H */
