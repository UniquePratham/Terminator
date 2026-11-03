/* lexer.c — quote-aware tokenizer.
 *
 * The lexer is a state machine over one input string. It emits operators as
 * separate tokens and stores each word as literal/variable/substitution
 * segments, because flattening a word here would lose quote information that
 * later controls expansion and globbing. See docs/viva-guide.md for the path
 * from this token stream to execution.
 */
#include "lexer.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void word_init(word_t *w)
{
    w->segs = NULL;
    w->nsegs = 0;
    w->cap = 0;
    w->had_quote = 0;
}

void word_free(word_t *w)
{
    for (size_t i = 0; i < w->nsegs; i++)
        free(w->segs[i].text);
    free(w->segs);
    word_init(w);
}

void word_copy(word_t *dst, const word_t *src)
{
    word_init(dst);
    dst->had_quote = src->had_quote;
    for (size_t i = 0; i < src->nsegs; i++)
        word_add_seg(dst, src->segs[i].type, src->segs[i].text,
                     strlen(src->segs[i].text), src->segs[i].quoted);
}

static segment_t *word_push(word_t *w)
{
    if (w->nsegs == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 4;
        w->segs = xrealloc(w->segs, w->cap * sizeof(*w->segs));
    }
    segment_t *s = &w->segs[w->nsegs++];
    s->type = SEG_LITERAL;
    s->text = NULL;
    s->quoted = 0;
    return s;
}

void word_add_seg(word_t *w, seg_type_t type, const char *text, size_t n, int quoted)
{
    segment_t *s = word_push(w);
    s->type = type;
    s->text = xstrndup(text, n);
    s->quoted = quoted;
    if (quoted)
        w->had_quote = 1;
}

void word_add_literal(word_t *w, const char *text, size_t n, int quoted)
{
    if (n == 0 && !quoted)
        return;
    /* Merge into a trailing literal segment when quoting matches. */
    if (w->nsegs > 0) {
        segment_t *last = &w->segs[w->nsegs - 1];
        if (last->type == SEG_LITERAL && last->quoted == quoted) {
            size_t old = strlen(last->text);
            last->text = xrealloc(last->text, old + n + 1);
            memcpy(last->text + old, text, n);
            last->text[old + n] = '\0';
            if (quoted)
                w->had_quote = 1;
            return;
        }
    }
    word_add_seg(w, SEG_LITERAL, text, n, quoted);
}

char *word_plain(const word_t *w)
{
    buf_t b;
    buf_init(&b);
    for (size_t i = 0; i < w->nsegs; i++) {
        if (w->segs[i].type == SEG_LITERAL)
            buf_append_str(&b, w->segs[i].text);
    }
    return buf_steal(&b);
}

void token_list_init(token_list_t *tl)
{
    tl->toks = NULL;
    tl->ntoks = 0;
    tl->cap = 0;
}

void token_list_free(token_list_t *tl)
{
    for (size_t i = 0; i < tl->ntoks; i++) {
        if (tl->toks[i].type == TOK_WORD)
            word_free(&tl->toks[i].word);
    }
    free(tl->toks);
    token_list_init(tl);
}

static token_t *tok_push(token_list_t *tl, token_type_t type, int line)
{
    if (tl->ntoks == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 8;
        tl->toks = xrealloc(tl->toks, tl->cap * sizeof(*tl->toks));
    }
    token_t *t = &tl->toks[tl->ntoks++];
    t->type = type;
    t->line = line;
    word_init(&t->word);
    return t;
}

/* Lexer state. */
typedef struct {
    const char *s;
    size_t      i;
    int         line;
    char       *err;
    size_t      errlen;
} lexer_t;

static void set_err(lexer_t *lx, const char *msg)
{
    if (lx->err && lx->errlen)
        snprintf(lx->err, lx->errlen, "%s", msg);
}

static int scan_cmdsub_end(const char *s, size_t open, size_t *end)
{
    size_t j = open + 2;
    char quote = 0;
    int parens = 0;
    while (s[j]) {
        char c = s[j];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            j++;
            continue;
        }
        if (c == '\\' && s[j + 1]) { j += 2; continue; }
        if (quote == '"') {
            if (c == '"') { quote = 0; j++; continue; }
            if (c == '$' && s[j + 1] == '(') {
                if (!scan_cmdsub_end(s, j, &j)) return 0;
                j++;
                continue;
            }
            j++;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; j++; continue; }
        if (c == '$' && s[j + 1] == '(') {
            if (!scan_cmdsub_end(s, j, &j)) return 0;
            j++;
            continue;
        }
        if (c == '(') { parens++; j++; continue; }
        if (c == ')') {
            if (parens > 0) { parens--; j++; continue; }
            *end = j;
            return 1;
        }
        j++;
    }
    return 0;
}

/* Read a balanced $( ... ) starting at lx->i which points at '$'. */
static char *read_cmdsub(lexer_t *lx, int *ok)
{
    *ok = 0;
    size_t start = lx->i + 2;
    size_t end;
    if (!scan_cmdsub_end(lx->s, lx->i, &end)) return NULL;
    char *inner = xstrndup(lx->s + start, end - start);
    lx->i = end + 1;
    *ok = 1;
    return inner;
}

/* Read a backtick substitution starting at lx->i which points at '`'. */
static char *read_backtick(lexer_t *lx, int *ok)
{
    *ok = 0;
    size_t start = lx->i + 1;
    size_t j = start;
    char quote = 0;
    while (lx->s[j]) {
        char c = lx->s[j];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            j++;
            continue;
        }
        if (lx->s[j] == '\\' && lx->s[j + 1]) {
            /* In legacy backticks, escaped backticks belong to the body. */
            j += 2;
            continue;
        }
        if (quote == '"') {
            if (c == '"') quote = 0;
            j++;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            j++;
            continue;
        }
        if (c == '`') {
            char *inner = xstrndup(lx->s + start, j - start);
            /* Escaped backticks are escaped only for the outer delimiter;
             * inside the captured command they delimit the nested form. */
            size_t r = 0, w = 0;
            while (inner[r]) {
                if (inner[r] == '\\' && inner[r + 1] == '`') r++;
                inner[w++] = inner[r++];
            }
            inner[w] = '\0';
            lx->i = j + 1;
            *ok = 1;
            return inner;
        }
        j++;
    }
    return NULL;
}

/* Parse a variable name starting after '$'. Returns length consumed. */
static size_t var_name_len(const char *s)
{
    size_t n = 0;
    if (s[0] == '{') {
        n = 1;
        while (s[n] && s[n] != '}')
            n++;
        if (s[n] == '}')
            n++;
        return n;
    }
    if (s[0] == '?' || s[0] == '#' || (s[0] >= '0' && s[0] <= '9'))
        return 1;
    while (s[n] && (isalnum((unsigned char)s[n]) || s[n] == '_'))
        n++;
    return n;
}

/* Lex a single word. Returns 0 on success, -1 on incomplete, -2 on error. */
static int lex_word(lexer_t *lx, word_t *w)
{
    /* Walk one shell word until unquoted whitespace or an operator. Quote and
     * escape state decides whether special characters are syntax or data. */
    int any = 0;
    while (lx->s[lx->i]) {
        char c = lx->s[lx->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '|' || c == '<' ||
            c == '>' || c == '&' || c == ';')
            break;

        if (c == '\\') {
            if (!lx->s[lx->i + 1])
                return -1; /* trailing backslash: continuation */
            if (lx->s[lx->i + 1] == '\n') {
                lx->i += 2; /* escaped newline continues the same word */
                continue;
            }
            word_add_literal(w, &lx->s[lx->i + 1], 1, 1);
            lx->i += 2;
            any = 1;
            continue;
        }
        if (c == '\'') {
            lx->i++;
            size_t start = lx->i;
            while (lx->s[lx->i] && lx->s[lx->i] != '\'')
                lx->i++;
            if (!lx->s[lx->i])
                return -1; /* unterminated single quote */
            word_add_literal(w, lx->s + start, lx->i - start, 1);
            lx->i++; /* closing quote */
            any = 1;
            continue;
        }
        if (c == '"') {
            lx->i++;
            while (lx->s[lx->i] && lx->s[lx->i] != '"') {
                char d = lx->s[lx->i];
                if (d == '\\') {
                    char e = lx->s[lx->i + 1];
                    if (e == '\n') {
                        lx->i += 2;
                        continue;
                    }
                    if (e == '"' || e == '\\' || e == '$' || e == '`') {
                        word_add_literal(w, &e, 1, 1);
                        lx->i += 2;
                        continue;
                    }
                    word_add_literal(w, &d, 1, 1);
                    lx->i++;
                    continue;
                }
                if (d == '$') {
                    if (lx->s[lx->i + 1] == '(') {
                        int ok = 0;
                        char *inner = read_cmdsub(lx, &ok);
                        if (!ok) {
                            free(inner);
                            return -1;
                        }
                        word_add_seg(w, SEG_CMDSUB, inner, strlen(inner), 1);
                        free(inner);
                        continue;
                    }
                    size_t vn = var_name_len(lx->s + lx->i + 1);
                    if (vn > 0) {
                        const char *name = lx->s + lx->i + 1;
                        size_t off = (name[0] == '{') ? 1 : 0;
                        size_t len = vn - (name[0] == '{' ? 2 : 0);
                        word_add_seg(w, SEG_VAR, name + off, len, 1);
                        lx->i += 1 + vn;
                        continue;
                    }
                    word_add_literal(w, "$", 1, 1);
                    lx->i++;
                    continue;
                }
                if (d == '`') {
                    int ok = 0;
                    char *inner = read_backtick(lx, &ok);
                    if (!ok) {
                        free(inner);
                        return -1;
                    }
                    word_add_seg(w, SEG_CMDSUB, inner, strlen(inner), 1);
                    free(inner);
                    continue;
                }
                word_add_literal(w, &d, 1, 1);
                lx->i++;
            }
            if (!lx->s[lx->i])
                return -1; /* unterminated double quote */
            lx->i++;
            any = 1;
            continue;
        }
        if (c == '$') {
            if (lx->s[lx->i + 1] == '(') {
                int ok = 0;
                char *inner = read_cmdsub(lx, &ok);
                if (!ok) {
                    free(inner);
                    return -1;
                }
                word_add_seg(w, SEG_CMDSUB, inner, strlen(inner), 0);
                free(inner);
                any = 1;
                continue;
            }
            size_t vn = var_name_len(lx->s + lx->i + 1);
            if (vn > 0) {
                const char *name = lx->s + lx->i + 1;
                size_t off = (name[0] == '{') ? 1 : 0;
                size_t len = vn - (name[0] == '{' ? 2 : 0);
                word_add_seg(w, SEG_VAR, name + off, len, 0);
                lx->i += 1 + vn;
                any = 1;
                continue;
            }
            word_add_literal(w, "$", 1, 0);
            lx->i++;
            any = 1;
            continue;
        }
        if (c == '`') {
            int ok = 0;
            char *inner = read_backtick(lx, &ok);
            if (!ok) {
                free(inner);
                return -1;
            }
            word_add_seg(w, SEG_CMDSUB, inner, strlen(inner), 0);
            free(inner);
            any = 1;
            continue;
        }
        /* Ordinary character. */
        word_add_literal(w, &c, 1, 0);
        lx->i++;
        any = 1;
    }
    return any ? 0 : -1;
}

lex_status_t lex_line(const char *input, token_list_t *out,
                      char *errbuf, size_t errlen)
{
    /* This is the lexer entry point: initialize scanning state, repeatedly
     * emit words/operators, then report complete, incomplete, or malformed
     * syntax so the tab can decide whether to request another input line. */
    lexer_t lx = { input, 0, 1, errbuf, errlen };
    if (errbuf && errlen)
        errbuf[0] = '\0';

    for (;;) {
        char c = lx.s[lx.i];
        if (c == ' ' || c == '\t') {
            lx.i++;
            continue;
        }
        if (c == '\0') {
            tok_push(out, TOK_EOF, lx.line);
            return LEX_OK;
        }
        if (c == '\n') {
            tok_push(out, TOK_NEWLINE, lx.line);
            lx.i++;
            lx.line++;
            continue;
        }
        if (c == '|') {
            tok_push(out, TOK_PIPE, lx.line);
            lx.i++;
            continue;
        }
        if (c == ';') {
            tok_push(out, TOK_SEMI, lx.line);
            lx.i++;
            continue;
        }
        if (c == '&') {
            tok_push(out, TOK_BG, lx.line);
            lx.i++;
            continue;
        }
        if (c == '<') {
            tok_push(out, TOK_REDIR_IN, lx.line);
            lx.i++;
            continue;
        }
        if (c == '>') {
            if (lx.s[lx.i + 1] == '>') {
                tok_push(out, TOK_REDIR_APPEND, lx.line);
                lx.i += 2;
            } else {
                tok_push(out, TOK_REDIR_OUT, lx.line);
                lx.i++;
            }
            continue;
        }
        /* Start of a word. */
        token_t *t = tok_push(out, TOK_WORD, lx.line);
        int r = lex_word(&lx, &t->word);
        if (r == -1) {
            /* Roll back the partial word token. */
            word_free(&t->word);
            out->ntoks--;
            return LEX_INCOMPLETE;
        }
        if (r == -2) {
            set_err(&lx, "syntax error");
            return LEX_ERROR;
        }
    }
}
