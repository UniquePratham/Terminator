/* expand.h — quote-aware expansion of words into argument strings. */
#ifndef TERMINATOR_EXPAND_H
#define TERMINATOR_EXPAND_H

#include "lexer.h"
#include "shell.h"

/* A growable list of heap strings. */
typedef struct {
    char  **items; /* each entry is an owned expanded argument */
    size_t  n;     /* number of initialized entries */
    size_t  cap;   /* allocated pointer slots */
} strlist_t;

void strlist_init(strlist_t *l);
void strlist_free(strlist_t *l);
void strlist_push(strlist_t *l, char *s); /* takes ownership */

/* Expand a word into zero or more strings (globbing may yield many).
 * Results are appended to `out`. */
void expand_word(shell_t *sh, const word_t *w, strlist_t *out);

/* Expand a word to a single string (no globbing); used for redirection
 * targets and variable values. Caller frees. */
char *expand_word_single(shell_t *sh, const word_t *w);

#endif /* TERMINATOR_EXPAND_H */
