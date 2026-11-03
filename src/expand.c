/* expand.c — quote-aware expansion: variables, command substitution, globbing.
 *
 * Expansion consumes the segment metadata produced by the lexer: variable and
 * command-substitution segments are evaluated, while quoted literals remain
 * protected from pathname globbing. One input word may become several argv
 * entries when an unquoted glob matches multiple files.
 */
#include "expand.h"
#include "util.h"

#include <glob.h>
#include <stdlib.h>
#include <string.h>

void strlist_init(strlist_t *l)
{
    l->items = NULL;
    l->n = 0;
    l->cap = 0;
}

void strlist_free(strlist_t *l)
{
    for (size_t i = 0; i < l->n; i++)
        free(l->items[i]);
    free(l->items);
    strlist_init(l);
}

void strlist_push(strlist_t *l, char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = xrealloc(l->items, l->cap * sizeof(*l->items));
    }
    l->items[l->n++] = s;
}

/* Strip trailing newlines from captured command output. */
static void strip_trailing_newlines(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

/* Build the fully-expanded string for a word (no globbing). */
static char *expand_to_string(shell_t *sh, const word_t *w)
{
    buf_t b;
    buf_init(&b);
    for (size_t i = 0; i < w->nsegs; i++) {
        segment_t *s = &w->segs[i];
        switch (s->type) {
        case SEG_LITERAL:
            buf_append_str(&b, s->text);
            break;
        case SEG_VAR: {
            const char *v = shell_get_var(sh, s->text);
            if (v)
                buf_append_str(&b, v);
            break;
        }
        case SEG_CMDSUB: {
            char *out = shell_capture(sh, s->text);
            if (out) {
                strip_trailing_newlines(out);
                buf_append_str(&b, out);
                free(out);
            }
            break;
        }
        }
    }
    return buf_steal(&b);
}

/* Does the word contain an unquoted glob metacharacter? */
static int word_has_glob(const word_t *w)
{
    for (size_t i = 0; i < w->nsegs; i++) {
        segment_t *s = &w->segs[i];
        if (s->type != SEG_LITERAL || s->quoted)
            continue;
        if (strpbrk(s->text, "*?["))
            return 1;
    }
    return 0;
}

void expand_word(shell_t *sh, const word_t *w, strlist_t *out)
{
    /* Build the expanded string first; only then glob it, and only when the
     * original word contains an unquoted wildcard. */
    char *s = expand_to_string(sh, w);
    if (word_has_glob(w)) {
        glob_t g;
        memset(&g, 0, sizeof(g));
        int r = glob(s, GLOB_NOCHECK, NULL, &g);
        if (r == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++)
                strlist_push(out, xstrdup(g.gl_pathv[i]));
            globfree(&g);
            free(s);
            return;
        }
        globfree(&g);
    }
    strlist_push(out, s);
}

char *expand_word_single(shell_t *sh, const word_t *w)
{
    return expand_to_string(sh, w);
}
