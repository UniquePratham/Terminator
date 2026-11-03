/* scripts.c — the required Bash-script subset interpreter.
 *
 * The interpreter is line/statement based and reuses the same lexer, parser
 * and executor as interactive input. It supports comments, multiple commands,
 * pipelines, redirection, command substitution, variables, positional
 * parameters, nested if/elif/else/fi with [ ... ] tests, and exit.
 *
 * Block structure is validated before execution so a malformed script (for
 * example a missing fi) is rejected rather than partially run.
 */
#include "scripts.h"
#include "expand.h"
#include "lexer.h"
#include "shell.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* File loading and logical-line splitting                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char **lines;   /* logical lines (continuations joined) */
    int   *lineno;  /* starting physical line number of each logical line */
    size_t n;
} script_t;

static void script_free(script_t *s)
{
    for (size_t i = 0; i < s->n; i++)
        free(s->lines[i]);
    free(s->lines);
    free(s->lineno);
}

/* Count unescaped quotes to decide whether a line is complete. */
static int line_incomplete(const char *s)
{
    int in_s = 0, in_d = 0;
    if (!s)
        return 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i++;
            continue;
        }
        if (c == '\'' && !in_d)
            in_s = !in_s;
        else if (c == '"' && !in_s)
            in_d = !in_d;
    }
    if (in_s || in_d)
        return 1;
    if (n > 0 && s[n - 1] == '\\')
        return 1;
    return 0;
}

static int load_script(const char *path, script_t *out, char *errbuf, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(errbuf, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    out->lines = NULL;
    out->lineno = NULL;
    out->n = 0;
    size_t cap = 0;

    char raw[8192];
    int phys = 0;
    buf_t cur;
    buf_init(&cur);
    int start_line = 0;

    while (fgets(raw, sizeof(raw), f)) {
        phys++;
        size_t rn = strlen(raw);
        while (rn > 0 && (raw[rn - 1] == '\n' || raw[rn - 1] == '\r'))
            raw[--rn] = '\0';
        if (cur.len == 0)
            start_line = phys;
        buf_append_str(&cur, raw);
        if (line_incomplete(cur.data)) {
            buf_append_char(&cur, '\n');
            continue;
        }
        if (out->n == cap) {
            cap = cap ? cap * 2 : 16;
            out->lines = xrealloc(out->lines, cap * sizeof(char *));
            out->lineno = xrealloc(out->lineno, cap * sizeof(int));
        }
        out->lines[out->n] = buf_steal(&cur);
        out->lineno[out->n] = start_line;
        out->n++;
        buf_init(&cur);
    }
    if (cur.len > 0) {
        if (out->n == cap) {
            cap = cap ? cap * 2 : 16;
            out->lines = xrealloc(out->lines, cap * sizeof(char *));
            out->lineno = xrealloc(out->lineno, cap * sizeof(int));
        }
        out->lines[out->n] = buf_steal(&cur);
        out->lineno[out->n] = start_line;
        out->n++;
    } else {
        buf_free(&cur);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Statement splitting (respecting quotes)                            */
/* ------------------------------------------------------------------ */

/* Split a logical line into statements on ';' and newlines, honouring quotes.
 * Returns a heap array of heap strings. */
static char **split_statements(const char *line, size_t *out_n)
{
    char **stmts = NULL;
    size_t n = 0, cap = 0;
    buf_t cur;
    buf_init(&cur);
    int in_s = 0, in_d = 0;
    size_t len = strlen(line);
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? line[i] : '\0';
        if (c == '\\' && i + 1 < len) {
            buf_append_char(&cur, c);
            buf_append_char(&cur, line[++i]);
            continue;
        }
        if (c == '\'' && !in_d) {
            in_s = !in_s;
            buf_append_char(&cur, c);
            continue;
        }
        if (c == '"' && !in_s) {
            in_d = !in_d;
            buf_append_char(&cur, c);
            continue;
        }
        if ((c == ';' || c == '\n' || c == '\0') && !in_s && !in_d) {
            char *s = str_trim(cur.data ? cur.data : "");
            if (*s) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 8;
                    stmts = xrealloc(stmts, cap * sizeof(char *));
                }
                stmts[n++] = xstrdup(s);
            }
            buf_clear(&cur);
            continue;
        }
        buf_append_char(&cur, c);
    }
    buf_free(&cur);
    *out_n = n;
    return stmts;
}

/* ------------------------------------------------------------------ */
/* Test evaluation: [ ... ]                                           */
/* ------------------------------------------------------------------ */

static int is_integer(const char *s, long *out)
{
    if (!s || !*s)
        return 0;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return 0;
    *out = v;
    return 1;
}

/* Evaluate the tokens of a [ ... ] test. Returns 1 (true) or 0 (false). */
/* Resolve a file name using the tab's logical cwd. Thread-mode tabs cannot
 * call chdir() in the shared process, so process-relative stat() would inspect
 * the wrong directory after a tab-local `cd`. */
static char *shell_path(shell_t *sh, const char *path)
{
    if (!path || path[0] == '/')
        return xstrdup(path ? path : "");
    const char *cwd = shell_cwd(sh);
    if (!cwd || !*cwd)
        cwd = ".";
    size_t n = strlen(cwd) + strlen(path) + 2;
    char *resolved = xmalloc(n);
    snprintf(resolved, n, "%s/%s", cwd, path);
    return resolved;
}

static int eval_test_tokens(shell_t *sh, char **t, int n)
{
    if (n == 0)
        return 0;
    if (strcmp(t[0], "!") == 0)
        return !eval_test_tokens(sh, t + 1, n - 1);
    if (n == 1)
        return t[0][0] != '\0'; /* non-empty string is true */
    if (n == 2) {
        if (strcmp(t[0], "-f") == 0) {
            struct stat st;
            char *path = shell_path(sh, t[1]);
            int result = stat(path, &st) == 0 && S_ISREG(st.st_mode);
            free(path);
            return result;
        }
        if (strcmp(t[0], "-d") == 0) {
            struct stat st;
            char *path = shell_path(sh, t[1]);
            int result = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
            free(path);
            return result;
        }
        if (strcmp(t[0], "-e") == 0) {
            struct stat st;
            char *path = shell_path(sh, t[1]);
            int result = stat(path, &st) == 0;
            free(path);
            return result;
        }
        if (strcmp(t[0], "-z") == 0)
            return t[1][0] == '\0';
        if (strcmp(t[0], "-n") == 0)
            return t[1][0] != '\0';
        return 0;
    }
    if (n == 3) {
        const char *op = t[1];
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
            return strcmp(t[0], t[2]) == 0;
        if (strcmp(op, "!=") == 0)
            return strcmp(t[0], t[2]) != 0;
        long a, b;
        if (!is_integer(t[0], &a) || !is_integer(t[2], &b))
            return 0;
        if (strcmp(op, "-eq") == 0)
            return a == b;
        if (strcmp(op, "-ne") == 0)
            return a != b;
        if (strcmp(op, "-gt") == 0)
            return a > b;
        if (strcmp(op, "-lt") == 0)
            return a < b;
        if (strcmp(op, "-ge") == 0)
            return a >= b;
        if (strcmp(op, "-le") == 0)
            return a <= b;
    }
    return 0;
}

/* Evaluate a condition string. If it is a [ ... ] test, evaluate directly;
 * otherwise run it as a command and use its exit status. */
static int eval_condition(shell_t *sh, const char *cond)
{
    char *s = xstrdup(cond);
    char *trimmed = str_trim(s);
    int result;
    if (trimmed[0] == '[') {
        /* Use the regular lexer and expander so quoted positional parameters
         * such as "$1" behave like they do in command arguments. */
        token_list_t parsed;
        token_list_init(&parsed);
        char err[128];
        if (lex_line(trimmed, &parsed, err, sizeof(err)) != LEX_OK) {
            token_list_free(&parsed);
            free(s);
            return 0;
        }
        char *toks[16];
        int nt = 0;
        int saw_open = 0;
        for (size_t i = 0; i < parsed.ntoks && nt < 16; i++) {
            token_t *tok = &parsed.toks[i];
            if (tok->type != TOK_WORD)
                break;
            char *value = expand_word_single(sh, &tok->word);
            if (!saw_open && strcmp(value, "[") == 0) {
                saw_open = 1;
                free(value);
                continue;
            }
            if (strcmp(value, "]") == 0) {
                free(value);
                break;
            }
            toks[nt++] = value;
        }
        result = saw_open ? eval_test_tokens(sh, toks, nt) : 0;
        for (int i = 0; i < nt; i++)
            free(toks[i]);
        token_list_free(&parsed);
        free(s);
        return result;
    }
    /* Fall back to running the condition as a command. */
    result = shell_exec_line(sh, trimmed) == 0;
    free(s);
    return result;
}

/* ------------------------------------------------------------------ */
/* Block structure validation                                         */
/* ------------------------------------------------------------------ */

/* Count if/fi balance across statements. Returns 0 if balanced. */
static int validate_blocks(const script_t *s, char *errbuf, size_t errlen)
{
    int depth = 0;
    for (size_t i = 0; i < s->n; i++) {
        size_t ns;
        char **stmts = split_statements(s->lines[i], &ns);
        for (size_t k = 0; k < ns; k++) {
            char *st = str_trim(stmts[k]);
            if (str_starts_with(st, "if ") || strcmp(st, "if") == 0)
                depth++;
            else if (str_starts_with(st, "fi") &&
                     (st[2] == '\0' || isspace((unsigned char)st[2])))
                depth--;
            if (depth < 0) {
                snprintf(errbuf, errlen, "line %d: 'fi' without matching 'if'",
                         s->lineno[i]);
                for (size_t j = 0; j < ns; j++)
                    free(stmts[j]);
                free(stmts);
                return -1;
            }
        }
        for (size_t j = 0; j < ns; j++)
            free(stmts[j]);
        free(stmts);
    }
    if (depth != 0) {
        snprintf(errbuf, errlen, "missing 'fi' (unclosed if block)");
        return -1;
    }
    return 0;
}

int scripts_validate(const char *path, char *errbuf, size_t errlen)
{
    script_t s;
    if (load_script(path, &s, errbuf, errlen) != 0)
        return -1;
    int r = validate_blocks(&s, errbuf, errlen);
    script_free(&s);
    return r;
}

/* ------------------------------------------------------------------ */
/* Execution                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int taken;    /* a branch has already been taken */
    int active;   /* the current branch is executing */
} frame_t;

static int run_script(shell_t *sh, const script_t *s)
{
    /* Each stack frame represents one nested if block. `taken` prevents a
     * later elif/else from running after a branch succeeded; `active` also
     * reflects whether every enclosing block currently allows execution. */
    frame_t stack[64];
    int depth = 0;
    int status = 0;

    for (size_t i = 0; i < s->n; i++) {
        size_t ns;
        char **stmts = split_statements(s->lines[i], &ns);
        for (size_t k = 0; k < ns; k++) {
            char *st = str_trim(stmts[k]);
            if (*st == '\0' || *st == '#')
                continue;

            int active = 1;
            for (int d = 0; d < depth; d++)
                if (!stack[d].active)
                    active = 0;

            if (str_starts_with(st, "if ") || strcmp(st, "if") == 0) {
                const char *cond = st + 2;
                while (*cond == ' ')
                    cond++;
                /* Strip a trailing "then". */
                char *condbuf = xstrdup(cond);
                char *then = strstr(condbuf, " then");
                if (then)
                    *then = '\0';
                int r = active ? eval_condition(sh, condbuf) : 0;
                if (depth < 64) {
                    stack[depth].taken = r;
                    stack[depth].active = active && r;
                    depth++;
                }
                free(condbuf);
                continue;
            }
            if (str_starts_with(st, "then") &&
                (st[4] == '\0' || isspace((unsigned char)st[4]))) {
                char *rest = str_trim(st + 4);
                if (*rest && active)
                    status = shell_exec_line(sh, rest);
                continue;
            }
            if (str_starts_with(st, "elif ") || strcmp(st, "elif") == 0) {
                const char *cond = st + 4;
                while (*cond == ' ')
                    cond++;
                char *condbuf = xstrdup(cond);
                char *then = strstr(condbuf, " then");
                if (then)
                    *then = '\0';
                if (depth > 0) {
                    frame_t *fr = &stack[depth - 1];
                    int outer_active = 1;
                    for (int d = 0; d < depth - 1; d++)
                        if (!stack[d].active)
                            outer_active = 0;
                    if (outer_active && !fr->taken) {
                        int r = eval_condition(sh, condbuf);
                        fr->active = r;
                        if (r)
                            fr->taken = 1;
                    } else {
                        fr->active = 0;
                    }
                }
                free(condbuf);
                continue;
            }
            if (str_starts_with(st, "else") &&
                (st[4] == '\0' || isspace((unsigned char)st[4]))) {
                char *rest = str_trim(st + 4);
                if (depth > 0) {
                    frame_t *fr = &stack[depth - 1];
                    int outer_active = 1;
                    for (int d = 0; d < depth - 1; d++)
                        if (!stack[d].active)
                            outer_active = 0;
                    fr->active = outer_active && !fr->taken;
                    if (fr->active)
                        fr->taken = 1;
                }
                if (*rest && active)
                    status = shell_exec_line(sh, rest);
                continue;
            }
            if (str_starts_with(st, "fi") &&
                (st[2] == '\0' || isspace((unsigned char)st[2]))) {
                if (depth > 0)
                    depth--;
                char *rest = str_trim(st + 2);
                if (*rest && active)
                    status = shell_exec_line(sh, rest);
                continue;
            }

            if (active) {
                status = shell_exec_line(sh, st);
                if (shell_exit_requested(sh))
                    goto done;
            }
        }
        for (size_t j = 0; j < ns; j++)
            free(stmts[j]);
        free(stmts);
    }
done:
    return status;
}

int scripts_run_file(shell_t *sh, const char *path, char *const *args, int nargs)
{
    char err[256];
    script_t s;
    char *resolved = shell_path(sh, path);
    if (load_script(resolved, &s, err, sizeof(err)) != 0) {
        shell_printf(sh, "%s\n", err);
        free(resolved);
        return 127;
    }
    if (validate_blocks(&s, err, sizeof(err)) != 0) {
        shell_printf(sh, "%s: %s\n", path, err);
        script_free(&s);
        free(resolved);
        return 2;
    }

    /* Save and install positional parameters for the script context. */
    char **saved = NULL;
    int nsaved = shell_nposargs(sh);
    if (nsaved > 0) {
        saved = xcalloc((size_t)nsaved, sizeof(char *));
        for (int i = 0; i < nsaved; i++)
            saved[i] = xstrdup(shell_posarg(sh, i + 1));
    }
    shell_set_posargs(sh, args, nargs);

    int status = run_script(sh, &s);

    shell_set_posargs(sh, saved, nsaved);
    for (int i = 0; i < nsaved; i++)
        free(saved[i]);
    free(saved);
    script_free(&s);
    free(resolved);
    return status;
}

int scripts_source(shell_t *sh, const char *path, char *const *args, int nargs)
{
    char err[256];
    script_t s;
    char *resolved = shell_path(sh, path);
    if (load_script(resolved, &s, err, sizeof(err)) != 0) {
        shell_printf(sh, "source: %s\n", err);
        free(resolved);
        return 1;
    }
    if (validate_blocks(&s, err, sizeof(err)) != 0) {
        shell_printf(sh, "source: %s: %s\n", path, err);
        script_free(&s);
        free(resolved);
        return 2;
    }
    /* source runs in the current context; positional parameters are only
     * replaced when explicitly supplied. */
    char **saved = NULL;
    int nsaved = shell_nposargs(sh);
    if (nargs > 0) {
        if (nsaved > 0) {
            saved = xcalloc((size_t)nsaved, sizeof(char *));
            for (int i = 0; i < nsaved; i++)
                saved[i] = xstrdup(shell_posarg(sh, i + 1));
        }
        shell_set_posargs(sh, args, nargs);
    }
    int status = run_script(sh, &s);
    if (nargs > 0) {
        shell_set_posargs(sh, saved, nsaved);
        for (int i = 0; i < nsaved; i++)
            free(saved[i]);
        free(saved);
    }
    script_free(&s);
    free(resolved);
    return status;
}
