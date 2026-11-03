/* test_main.c — headless test harness for the Terminator core modules.
 *
 * These tests link only the GUI-free core modules, so they run without an X
 * server. Each test prints PASS/FAIL and the harness exits non-zero if any
 * test fails. The test names group the feature being demonstrated; CHECK
 * messages describe the expected behavior and are useful viva examples of
 * the module contracts.
 */
#include "app.h"
#include "completion.h"
#include "config.h"
#include "expand.h"
#include "history.h"
#include "ipc.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "scripts.h"
#include "shell.h"
#include "tab.h"
#include "util.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

/* app.c is exercised without starting the X11 event loop in this harness. */
int gui_run(app_t *app)
{
    (void)app;
    return 0;
}

/* Count one boolean expectation and print the source location on failure. */
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* String variant: compare exact output and make NULL failures readable. */
#define CHECK_STR(actual, expected, msg) do { \
    const char *_a = (actual); const char *_e = (expected); \
    if (_a && strcmp(_a, _e) == 0) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s: got \"%s\" want \"%s\" (%s:%d)\n", \
        msg, _a ? _a : "(null)", _e, __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Capture sink                                                       */
/* ------------------------------------------------------------------ */

typedef struct { buf_t b; } sink_t;

static void sink_write(void *ctx, const char *data, size_t n)
{
    sink_t *s = ctx;
    buf_append(&s->b, data, n);
}

static shell_t *make_shell(sink_t *s)
{
    shell_t *sh = shell_new();
    buf_init(&s->b);
    shell_set_output(sh, sink_write, s);
    return sh;
}

/* A writable directory for temporary files. /tmp is not writable on some
 * platforms (e.g. Android/Termux), so honour TMPDIR/TMP when set. */
static const char *tmp_dir(void)
{
    const char *d = getenv("TMPDIR");
    if (d && *d)
        return d;
    d = getenv("TMP");
    if (d && *d)
        return d;
    return "/tmp";
}

#define TMP_TEMPLATE(buf, name) \
    snprintf((buf), sizeof(buf), "%s/" name "_XXXXXX", tmp_dir())

/* ------------------------------------------------------------------ */
/* Lexer tests                                                        */
/* ------------------------------------------------------------------ */

static void test_lexer(void)
{
    printf("lexer:\n");
    token_list_t tl;
    char err[256];

    /* Simple words. */
    token_list_init(&tl);
    CHECK(lex_line("echo hello world", &tl, err, sizeof(err)) == LEX_OK,
          "simple line lexes");
    CHECK(tl.ntoks == 4, "three words + EOF");
    token_list_free(&tl);

    /* Single quotes preserve spaces. */
    token_list_init(&tl);
    CHECK(lex_line("echo 'a b'", &tl, err, sizeof(err)) == LEX_OK,
          "single-quoted lexes");
    CHECK(tl.ntoks == 3, "echo + quoted word + EOF");
    if (tl.ntoks >= 2) {
        char *p = word_plain(&tl.toks[1].word);
        CHECK_STR(p, "a b", "single quotes preserve spaces");
        free(p);
    }
    token_list_free(&tl);

    /* Empty quoted argument is preserved. */
    token_list_init(&tl);
    CHECK(lex_line("echo ''", &tl, err, sizeof(err)) == LEX_OK,
          "empty quotes lex");
    if (tl.ntoks >= 2) {
        char *p = word_plain(&tl.toks[1].word);
        CHECK_STR(p, "", "empty quoted argument preserved");
        free(p);
    }
    token_list_free(&tl);

    /* Escaped space. */
    token_list_init(&tl);
    CHECK(lex_line("echo a\\ b", &tl, err, sizeof(err)) == LEX_OK,
          "escaped space lexes");
    if (tl.ntoks >= 2) {
        char *p = word_plain(&tl.toks[1].word);
        CHECK_STR(p, "a b", "backslash escapes space");
        free(p);
    }
    token_list_free(&tl);

    /* Operators. */
    token_list_init(&tl);
    CHECK(lex_line("a | b > c", &tl, err, sizeof(err)) == LEX_OK,
          "operators lex");
    CHECK(tl.ntoks == 6, "a | b > c + EOF");
    token_list_free(&tl);

    /* Incomplete: open quote. */
    token_list_init(&tl);
    CHECK(lex_line("echo 'abc", &tl, err, sizeof(err)) == LEX_INCOMPLETE,
          "open quote is incomplete");
    token_list_free(&tl);

    /* Incomplete: trailing backslash. */
    token_list_init(&tl);
    CHECK(lex_line("echo abc\\", &tl, err, sizeof(err)) == LEX_INCOMPLETE,
          "trailing backslash is incomplete");
    token_list_free(&tl);

    /* Incomplete: open command substitution. */
    token_list_init(&tl);
    CHECK(lex_line("echo $(date", &tl, err, sizeof(err)) == LEX_INCOMPLETE,
          "open $( is incomplete");
    token_list_free(&tl);

    /* Variable segment. */
    token_list_init(&tl);
    CHECK(lex_line("echo $HOME", &tl, err, sizeof(err)) == LEX_OK,
          "variable lexes");
    if (tl.ntoks >= 2) {
        CHECK(tl.toks[1].word.nsegs == 1 &&
              tl.toks[1].word.segs[0].type == SEG_VAR,
              "variable becomes SEG_VAR");
    }
    token_list_free(&tl);

    /* Multiline quotes remain one word and preserve their embedded newline. */
    token_list_init(&tl);
    CHECK(lex_line("echo 'first\nsecond'", &tl, err, sizeof(err)) == LEX_OK,
          "multiline single quote lexes");
    if (tl.ntoks >= 2) {
        char *p = word_plain(&tl.toks[1].word);
        CHECK_STR(p, "first\nsecond", "single quote preserves embedded newline");
        free(p);
    }
    token_list_free(&tl);

    /* An escaped physical newline joins words without inserting a newline. */
    token_list_init(&tl);
    CHECK(lex_line("echo ab\\\ncd", &tl, err, sizeof(err)) == LEX_OK,
          "escaped newline lexes");
    if (tl.ntoks >= 2) {
        char *p = word_plain(&tl.toks[1].word);
        CHECK_STR(p, "abcd", "escaped newline joins adjacent text");
        free(p);
    }
    token_list_free(&tl);

    /* Parentheses in quotes do not close a command substitution. */
    token_list_init(&tl);
    CHECK(lex_line("echo $(printf '%s' ')')", &tl, err, sizeof(err)) == LEX_OK,
          "quoted right parenthesis does not close substitution");
    token_list_free(&tl);

    /* Nested command substitutions balance from the inside out. */
    token_list_init(&tl);
    CHECK(lex_line("echo $(echo $(echo nested))", &tl, err, sizeof(err)) == LEX_OK,
          "nested command substitutions lex");
    token_list_free(&tl);

    token_list_init(&tl);
    CHECK(lex_line("echo \"$(echo $(echo nested))\"", &tl, err, sizeof(err)) == LEX_OK,
          "nested substitution inside double quotes lexes");
    token_list_free(&tl);

    token_list_init(&tl);
    CHECK(lex_line("echo $(printf '%s' \"right )\")", &tl, err, sizeof(err)) == LEX_OK,
          "parentheses inside nested double quotes are data");
    token_list_free(&tl);
}

/* ------------------------------------------------------------------ */
/* Parser tests                                                       */
/* ------------------------------------------------------------------ */

static void test_parser(void)
{
    printf("parser:\n");
    token_list_t tl;
    cmdline_t cl;
    char err[256];

    /* Pipeline of two. */
    token_list_init(&tl);
    lex_line("ls | wc -l", &tl, err, sizeof(err));
    cmdline_init(&cl);
    CHECK(parse_tokens(&tl, &cl, err, sizeof(err)) == 0, "pipeline parses");
    CHECK(cl.npipelines == 1, "one pipeline");
    CHECK(cl.pipelines[0].ncmds == 2, "two commands");
    token_list_free(&tl);

    cmdline_free(&cl);

    /* Redirection. */
    token_list_init(&tl);
    lex_line("cat < in > out", &tl, err, sizeof(err));
    cmdline_init(&cl);
    CHECK(parse_tokens(&tl, &cl, err, sizeof(err)) == 0, "redirection parses");
    CHECK(cl.pipelines[0].cmds[0].nredirs == 2, "two redirections");
    token_list_free(&tl);
    cmdline_free(&cl);

    /* Background. */
    token_list_init(&tl);
    lex_line("sleep 1 &", &tl, err, sizeof(err));
    cmdline_init(&cl);
    CHECK(parse_tokens(&tl, &cl, err, sizeof(err)) == 0, "background parses");
    CHECK(cl.pipelines[0].background == 1, "background flag set");
    token_list_free(&tl);
    cmdline_free(&cl);

    /* Missing redirection target is an error. */
    token_list_init(&tl);
    lex_line("cat >", &tl, err, sizeof(err));
    cmdline_init(&cl);
    CHECK(parse_tokens(&tl, &cl, err, sizeof(err)) != 0,
          "missing redirection target rejected");
    token_list_free(&tl);
    cmdline_free(&cl);

    /* Empty pipeline segment is an error. */
    token_list_init(&tl);
    lex_line("ls | | wc", &tl, err, sizeof(err));
    cmdline_init(&cl);
    CHECK(parse_tokens(&tl, &cl, err, sizeof(err)) != 0,
          "empty pipeline segment rejected");
    token_list_free(&tl);
    cmdline_free(&cl);
}

/* ------------------------------------------------------------------ */
/* Expansion tests                                                    */
/* ------------------------------------------------------------------ */

static void test_expand(void)
{
    printf("expand:\n");
    sink_t s;
    shell_t *sh = make_shell(&s);
    shell_set_var(sh, "NAME", "world");

    token_list_t tl;
    char err[256];
    token_list_init(&tl);
    lex_line("echo $NAME", &tl, err, sizeof(err));
    strlist_t out;
    strlist_init(&out);
    expand_word(sh, &tl.toks[1].word, &out);
    CHECK(out.n == 1, "variable expands to one word");
    if (out.n == 1)
        CHECK_STR(out.items[0], "world", "variable value");
    strlist_free(&out);
    token_list_free(&tl);

    /* Command substitution. */
    token_list_init(&tl);
    lex_line("echo $(echo hi)", &tl, err, sizeof(err));
    strlist_init(&out);
    expand_word(sh, &tl.toks[1].word, &out);
    CHECK(out.n == 1, "cmdsub expands to one word");
    if (out.n == 1)
        CHECK_STR(out.items[0], "hi", "command substitution output");
    strlist_free(&out);
    token_list_free(&tl);

    /* Command substitution inside double quotes (required by spec). */
    token_list_init(&tl);
    lex_line("echo \"a$(echo b)c\"", &tl, err, sizeof(err));
    strlist_init(&out);
    expand_word(sh, &tl.toks[1].word, &out);
    CHECK(out.n == 1, "quoted cmdsub expands to one word");
    if (out.n == 1)
        CHECK_STR(out.items[0], "abc", "quoted command substitution output");
    strlist_free(&out);
    token_list_free(&tl);

    /* Backtick substitution inside double quotes. */
    token_list_init(&tl);
    lex_line("echo \"x`echo y`z\"", &tl, err, sizeof(err));
    strlist_init(&out);
    expand_word(sh, &tl.toks[1].word, &out);
    CHECK(out.n == 1, "quoted backtick expands to one word");
    if (out.n == 1)
        CHECK_STR(out.items[0], "xyz", "quoted backtick substitution output");
    strlist_free(&out);
    token_list_free(&tl);

    token_list_init(&tl);
    CHECK(lex_line("echo \"$(echo $(echo nested))\"", &tl, err, sizeof(err)) == LEX_OK,
          "quoted nested command substitution lexes");
    strlist_init(&out);
    expand_word(sh, &tl.toks[1].word, &out);
    CHECK(out.n == 1, "quoted nested substitution expands to one word");
    if (out.n == 1)
        CHECK_STR(out.items[0], "nested", "quoted nested substitution output");
    strlist_free(&out);
    token_list_free(&tl);

    /* Legacy nested backticks use escaped inner delimiters. */
    token_list_init(&tl);
    CHECK(lex_line("echo `echo \\`echo legacy\\``", &tl, err, sizeof(err)) == LEX_OK,
          "nested legacy backticks lex");
    strlist_init(&out);
    expand_word(sh, &tl.toks[1].word, &out);
    CHECK(out.n == 1, "nested legacy substitution expands to one word");
    if (out.n == 1)
        CHECK_STR(out.items[0], "legacy", "nested legacy substitution output");
    strlist_free(&out);
    token_list_free(&tl);

    /* Nested substitution is evaluated by the inner shell first. */
    token_list_init(&tl);
    CHECK(lex_line("echo $(echo $(echo nested))", &tl, err, sizeof(err)) == LEX_OK,
          "nested command substitution lexes");
    strlist_init(&out);
    expand_word(sh, &tl.toks[1].word, &out);
    CHECK(out.n == 1, "nested substitution expands to one word");
    if (out.n == 1)
        CHECK_STR(out.items[0], "nested", "nested substitution output");
    strlist_free(&out);
    token_list_free(&tl);

    shell_free(sh);
    buf_free(&s.b);
}

/* ------------------------------------------------------------------ */
/* Execution tests                                                    */
/* ------------------------------------------------------------------ */

static void test_execution(void)
{
    printf("execution:\n");
    sink_t s;
    shell_t *sh = make_shell(&s);

    shell_exec_line(sh, "echo hello");
    CHECK_STR(s.b.data, "hello\n", "echo output");
    CHECK(shell_last_status(sh) == 0, "echo status 0");

    buf_clear(&s.b);
    shell_exec_line(sh, "true");
    CHECK(shell_last_status(sh) == 0, "true status 0");

    shell_exec_line(sh, "false");
    CHECK(shell_last_status(sh) == 1, "false status 1");

    buf_clear(&s.b);
    shell_exec_line(sh, "echo $?");
    CHECK_STR(s.b.data, "1\n", "$? reflects previous status");

    /* Demo 2: external command finishes, then the next list item runs. */
    buf_clear(&s.b);
    shell_exec_line(sh, "sleep 1; echo process-finished");
    CHECK_STR(s.b.data, "process-finished\n",
              "sleep demo returns to the prompt and runs the next command");

    /* Unknown command must not crash and must report an error. */
    buf_clear(&s.b);
    shell_exec_line(sh, "definitely_not_a_command_xyz");
    CHECK(shell_last_status(sh) != 0, "unknown command non-zero status");
    CHECK(s.b.len > 0, "unknown command prints a diagnostic");

    /* TA constraint: only this project's script interpreter may handle
     * scripts; interactive shell -c delegation must never reach execvp. */
    buf_clear(&s.b);
    shell_exec_line(sh, "bash -c 'echo forbidden_shell_output'");
    CHECK(shell_last_status(sh) == 126, "external bash execution is rejected");
    CHECK(strstr(s.b.data, "external shell execution is disabled") != NULL,
          "external shell rejection is explained");
    CHECK(strstr(s.b.data, "forbidden_shell_output") == NULL,
          "bash -c payload is not executed");

    /* Variable assignment persists. */
    shell_exec_line(sh, "FOO=bar");
    CHECK_STR(shell_get_var(sh, "FOO"), "bar", "assignment persists");

    /* A bare assignment must not try to execute an empty command. */
    buf_clear(&s.b);
    shell_exec_line(sh, "BAR=baz");
    CHECK_STR(shell_get_var(sh, "BAR"), "baz", "bare assignment persists");
    CHECK(s.b.len == 0, "bare assignment produces no output");
    CHECK(shell_last_status(sh) == 0, "bare assignment status 0");

    shell_free(sh);
    buf_free(&s.b);
}

/* ------------------------------------------------------------------ */
/* Redirection tests                                                  */
/* ------------------------------------------------------------------ */

static void test_redirection(void)
{
    printf("redirection:\n");
    sink_t s;
    shell_t *sh = make_shell(&s);

    char tmpl[256];
    TMP_TEMPLATE(tmpl, "terminator_test");
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0, "mkstemp");
    if (fd >= 0) {
        CHECK(write(fd, "line1\nline2\n", 12) == 12, "write input fixture");
        close(fd);

        buf_clear(&s.b);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "cat < %s", tmpl);
        shell_exec_line(sh, cmd);
        CHECK_STR(s.b.data, "line1\nline2\n", "input redirection");

        /* Output redirection. */
        char outfile[512];
        snprintf(outfile, sizeof(outfile), "%s.out", tmpl);
        snprintf(cmd, sizeof(cmd), "echo redirected > %s", outfile);
        shell_exec_line(sh, cmd);
        FILE *f = fopen(outfile, "r");
        CHECK(f != NULL, "output file created");
        if (f) {
            char buf[64] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            CHECK_STR(buf, "redirected\n", "output redirection content");
            fclose(f);
        }

        /* Demo 4: write a file, then read it through input redirection. */
        buf_clear(&s.b);
        snprintf(cmd, sizeof(cmd),
                 "printf 'redirected\\n' > %s; cat < %s", outfile, outfile);
        shell_exec_line(sh, cmd);
        CHECK_STR(s.b.data, "redirected\n",
                  "combined output and input redirection demo");
        unlink(outfile);

        /* Missing input file: command must not run, error reported. */
        buf_clear(&s.b);
        shell_exec_line(sh, "cat < /nonexistent/terminator/xyz");
        CHECK(shell_last_status(sh) != 0, "missing input file non-zero");
        CHECK(s.b.len > 0, "missing input file reports error");

        unlink(tmpl);
    }

    shell_free(sh);
    buf_free(&s.b);
}

/* ------------------------------------------------------------------ */
/* Pipeline tests                                                     */
/* ------------------------------------------------------------------ */

static void test_pipelines(void)
{
    printf("pipelines:\n");
    sink_t s;
    shell_t *sh = make_shell(&s);

    buf_clear(&s.b);
    shell_exec_line(sh, "echo hello | cat");
    CHECK_STR(s.b.data, "hello\n", "two-stage pipeline");

    buf_clear(&s.b);
    shell_exec_line(sh, "printf 'a\\nb\\nc\\n' | grep b | cat");
    CHECK_STR(s.b.data, "b\n", "three-stage pipeline");

    /* Demo 5: filter one line through a three-stage pipeline. */
    buf_clear(&s.b);
    shell_exec_line(sh, "printf 'alpha\\nbeta\\n' | grep beta | wc -l");
    CHECK_STR(s.b.data, "1\n", "filter and count pipeline demo");

    /* Five-stage pipeline must not hang. */
    buf_clear(&s.b);
    shell_exec_line(sh, "echo x | cat | cat | cat | cat");
    CHECK_STR(s.b.data, "x\n", "five-stage pipeline");

    /* Pipeline plus redirection. */
    char tmpl[256];
    TMP_TEMPLATE(tmpl, "terminator_pipe");
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        CHECK(write(fd, "alpha\nbeta\n", 11) == 11, "write pipeline fixture");
        close(fd);
        buf_clear(&s.b);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cat < %s | grep beta", tmpl);
        shell_exec_line(sh, cmd);
        CHECK_STR(s.b.data, "beta\n", "pipeline with redirection");
        unlink(tmpl);
    }

    shell_free(sh);
    buf_free(&s.b);
}

/* ------------------------------------------------------------------ */
/* History tests                                                      */
/* ------------------------------------------------------------------ */

static void test_history(void)
{
    printf("history:\n");
    char tmpl[256];
    TMP_TEMPLATE(tmpl, "terminator_hist");
    int fd = mkstemp(tmpl);
    if (fd >= 0)
        close(fd);
    unlink(tmpl);

    history_t *h = history_new(tmpl, 0);
    history_add(h, "ls -l");
    history_add(h, "echo hello");
    history_add(h, "git status");
    history_add(h, "echo abc one");
    history_add(h, "run abc two");
    CHECK(history_count(h) == 5, "five entries");
    CHECK_STR(history_last(h), "run abc two", "last entry");

    /* Exact match. */
    char *m = history_search(h, "echo hello");
    CHECK(m != NULL, "exact match found");
    if (m)
        CHECK_STR(m, "echo hello", "exact match value");
    free(m);

    size_t match_count = 0;
    char **matches = history_search_matches(h, "abc", &match_count);
    CHECK(match_count == 2, "search returns every tied best substring match");
    if (match_count == 2) {
        CHECK_STR(matches[0], "run abc two", "substring results newest first");
        CHECK_STR(matches[1], "echo abc one", "second tied substring result");
    }
    for (size_t i = 0; i < match_count; i++)
        free(matches[i]);
    free(matches);

    /* Longest common substring (>2 chars). */
    m = history_search(h, "stat");
    CHECK(m != NULL, "substring match found");
    free(m);

    /* No match. */
    m = history_search(h, "zzzzzzzz");
    CHECK(m == NULL, "no match returns NULL");
    free(m);

    history_free(h);
    h = history_new(tmpl, 0);
    CHECK(history_count(h) == 5, "history is restored from its file");
    CHECK_STR(history_last(h), "run abc two", "persisted newest history entry");
    history_free(h);
    unlink(tmpl);

    TMP_TEMPLATE(tmpl, "terminator_shared_hist");
    h = history_new(tmpl, 1);
    pid_t writer = fork();
    if (writer == 0) {
        history_add(h, "from_child_tab");
        _exit(0);
    }
    CHECK(writer > 0, "fork process-shared history writer");
    if (writer > 0) {
        int status = 0;
        CHECK(waitpid(writer, &status, 0) == writer &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "process-shared writer exits");
        CHECK_STR(history_last(h), "from_child_tab",
                  "history written by child is visible in parent");
    }
    history_free(h);
    unlink(tmpl);
}

static void test_ipc_payload(void)
{
    printf("ipc:\n");
    int fds[2];
    int pipe_rc = pipe(fds);
    CHECK(pipe_rc == 0, "create IPC test pipe");
    if (pipe_rc != 0)
        return;
    const char message[] = "process-mode payload";
    CHECK(ipc_send(fds[1], 7, 3, message, (uint32_t)(sizeof(message) - 1)) == 0,
          "send a length-delimited IPC string");
    uint32_t type = 0, tab_id = 0, len = 0;
    char *payload = NULL;
    CHECK(ipc_recv(fds[0], &type, &tab_id, &payload, &len) == 0,
          "receive an IPC string payload");
    CHECK(type == 7 && tab_id == 3 && len == sizeof(message) - 1,
          "preserve IPC frame metadata and payload length");
    CHECK(payload && strcmp(payload, message) == 0,
          "received IPC payload is safely null-terminated");
    free(payload);
    close(fds[0]);
    close(fds[1]);
}

/* ------------------------------------------------------------------ */
/* Completion tests                                                   */
/* ------------------------------------------------------------------ */

static void test_completion(void)
{
    printf("completion:\n");
    char dir[256];
    TMP_TEMPLATE(dir, "terminator_comp");
    char *d = mkdtemp(dir);
    CHECK(d != NULL, "mkdtemp");
    if (!d)
        return;

    char p[256];
    snprintf(p, sizeof(p), "%s/alpha.txt", d);
    int fd = open(p, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    snprintf(p, sizeof(p), "%s/alpha2.txt", d);
    fd = open(p, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    snprintf(p, sizeof(p), "%s/beta.txt", d);
    fd = open(p, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);

    /* No match. */
    completion_t c = completion_run(d, "zzz");
    CHECK(c.kind == COMPLETE_NONE, "no match");
    completion_free(&c);

    /* One match. */
    c = completion_run(d, "beta");
    CHECK(c.kind == COMPLETE_ONE, "one match");
    if (c.kind == COMPLETE_ONE)
        CHECK_STR(c.replacement, "beta.txt", "one match replacement");
    completion_free(&c);

    /* Common prefix. */
    c = completion_run(d, "alpha");
    CHECK(c.kind == COMPLETE_PREFIX || c.kind == COMPLETE_AMBIGUOUS,
          "common prefix or ambiguous");
    if (c.kind == COMPLETE_PREFIX)
        CHECK_STR(c.replacement, "alpha", "common prefix value");
    completion_free(&c);

    /* Cleanup. */
    snprintf(p, sizeof(p), "%s/alpha.txt", d);
    unlink(p);
    snprintf(p, sizeof(p), "%s/alpha2.txt", d);
    unlink(p);
    snprintf(p, sizeof(p), "%s/beta.txt", d);
    unlink(p);
    rmdir(d);
}

/* ------------------------------------------------------------------ */
/* Script tests                                                       */
/* ------------------------------------------------------------------ */

static void test_scripts(void)
{
    printf("scripts:\n");
    char tmpl[256];
    TMP_TEMPLATE(tmpl, "terminator_script");
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0, "mkstemp script");
    if (fd < 0)
        return;

    const char *script =
        "# a comment\n"
        "NAME=world\n"
        "echo hello $NAME\n"
        "if [ 1 -eq 1 ]; then\n"
        "  echo equal\n"
        "fi\n"
        "if [ 2 -gt 1 ]; then\n"
        "  echo greater\n"
        "else\n"
        "  echo notgreater\n"
        "fi\n"
        "if [ -f /etc/hosts ]; then\n"
        "  echo file_test\n"
        "fi\n"
        "if [ -d /tmp ]; then\n"
        "  echo dir_test\n"
        "fi\n"
        "if [ -f \"$1\" ]; then\n"
        "  echo positional_file_test\n"
        "fi\n"
        "if [ alpha = alpha ]; then\n"
        "  echo string_test\n"
        "fi\n"
        "if [ 1 -eq 2 ]; then\n"
        "  echo wrong_branch\n"
        "elif [ 3 -gt 2 ]; then\n"
        "  echo elif_test\n"
        "else\n"
        "  echo wrong_branch\n"
        "fi\n"
        "if [ 1 -eq 1 ]; then\n"
        "  if [ 4 -gt 3 ]; then\n"
        "    echo nested_test\n"
        "  fi\n"
        "fi\n"
        "false\n"
        "echo status_is_$?\n"
        "exit 3\n";
    CHECK(write(fd, script, strlen(script)) == (ssize_t)strlen(script),
          "write script fixture");
    close(fd);

    sink_t s;
    shell_t *sh = make_shell(&s);
    char arg_path[128];
    snprintf(arg_path, sizeof(arg_path), "/tmp/terminator_script_arg_%ld",
             (long)getpid());
    int arg_fd = open(arg_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    CHECK(arg_fd >= 0, "create positional-argument file fixture");
    if (arg_fd >= 0)
        close(arg_fd);
    const char *arg_name = strrchr(arg_path, '/') + 1;
    char *script_args[] = { (char *)arg_name };
    shell_set_cwd(sh, "/tmp");
    int st = scripts_run_file(sh, tmpl, script_args, 1);
    CHECK(st == 3, "script exit status");
    CHECK(strstr(s.b.data, "hello world") != NULL, "script variable expansion");
    CHECK(strstr(s.b.data, "equal") != NULL, "script if -eq");
    CHECK(strstr(s.b.data, "greater") != NULL, "script if -gt");
    CHECK(strstr(s.b.data, "file_test") != NULL, "script if -f");
    CHECK(strstr(s.b.data, "dir_test") != NULL, "script if -d");
    CHECK(strstr(s.b.data, "positional_file_test") != NULL,
          "quoted positional parameter expands in relative file test");
    CHECK(strstr(s.b.data, "string_test") != NULL, "script string equality test");
    CHECK(strstr(s.b.data, "elif_test") != NULL, "script elif branch");
    CHECK(strstr(s.b.data, "nested_test") != NULL, "nested script condition");
    CHECK(strstr(s.b.data, "status_is_1") != NULL, "script exit status expansion");

    /* Malformed script: missing fi. */
    char tmpl2[256];
    TMP_TEMPLATE(tmpl2, "terminator_bad");
    int fd2 = mkstemp(tmpl2);
    if (fd2 >= 0) {
        const char *bad = "if [ 1 -eq 1 ]; then\n  echo x\n";
        CHECK(write(fd2, bad, strlen(bad)) == (ssize_t)strlen(bad),
              "write malformed script fixture");
        close(fd2);
        char err[256];
        CHECK(scripts_validate(tmpl2, err, sizeof(err)) != 0,
              "missing fi detected");
        unlink(tmpl2);
    }

    shell_free(sh);
    buf_free(&s.b);
    unlink(arg_path);
    unlink(tmpl);
}

/* ------------------------------------------------------------------ */
/* Config tests                                                       */
/* ------------------------------------------------------------------ */

static void test_config(void)
{
    printf("config:\n");
    char tmpl[256];
    TMP_TEMPLATE(tmpl, "terminator_cfg");
    int fd = mkstemp(tmpl);
    if (fd >= 0)
        close(fd);
    unlink(tmpl);

    config_t *c = config_load(tmpl);
    CHECK(c != NULL, "config loads");
    CHECK(config_action_for_key(c, "Left") == ACT_CURSOR_LEFT,
          "default Left binding");
    CHECK(config_action_for_key(c, "Ctrl+A") == ACT_LINE_START,
          "default Ctrl+A binding");
    CHECK(config_action_for_key(c, "Tab") == ACT_COMPLETE,
          "default Tab binding");
    config_free(c);

    FILE *custom = fopen(tmpl, "w");
    CHECK(custom != NULL, "open custom config fixture");
    if (custom) {
        fputs("bind clear-screen = Ctrl+Shift+L\n"
              "bind copy = NotARealKey\n"
              "bind paste = Ctrl+Shift+C\n", custom);
        fclose(custom);
        c = config_load(tmpl);
        CHECK(config_action_for_key(c, "Ctrl+Shift+L") == ACT_CLEAR_SCREEN,
              "valid custom binding loads from file");
        CHECK(config_action_for_key(c, "Ctrl+Shift+C") == ACT_COPY,
              "invalid and conflicting bindings retain defaults");
        CHECK(config_action_for_key(c, "Ctrl+Shift+V") == ACT_PASTE,
              "conflicting binding does not replace default");
        config_free(c);
    }
    unlink(tmpl);
}

/* ------------------------------------------------------------------ */
/* Jobs tests                                                         */
/* ------------------------------------------------------------------ */

static void test_jobs(void)
{
    printf("jobs:\n");
    job_table_t *jt = jobs_new();
    pid_t *pids = xmalloc(sizeof(pid_t));
    pids[0] = 12345;
    job_t *j = jobs_add(jt, 12345, pids, 1, "sleep 10", 1);
    CHECK(j != NULL, "job added");
    CHECK(jobs_count(jt) == 1, "one job");
    CHECK(jobs_find(jt, j->id) == j, "find by id");
    jobs_free(jt);
}

/* Find the pid of a direct child of `parent` whose comm matches `name`.
 * Returns 0 if not found. Used to locate the `sleep` grandchild so the test
 * can stop it the way Ctrl+Z would (without stopping the shell itself). */
static pid_t find_child_named(pid_t parent, const char *name)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    pid_t found = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        int pid = 0, ppid = 0;
        char comm[64] = {0};
        if (fscanf(f, "%d %63s %*c %d", &pid, comm, &ppid) == 3) {
            /* comm is "(name)" — strip the parentheses. */
            char *p = comm;
            if (*p == '(')
                p++;
            size_t l = strlen(p);
            if (l > 0 && p[l - 1] == ')')
                p[l - 1] = '\0';
            if (ppid == (int)parent && strcmp(p, name) == 0)
                found = (pid_t)pid;
        }
        fclose(f);
        if (found)
            break;
    }
    closedir(d);
    return found;
}

/* Ctrl+Z must stop a foreground command and return control to the shell, and
 * the stopped job must remain in the table so `jobs`/`bg`/`fg` can act on it.
 * Regression test for the "shell hangs on Ctrl+Z" bug. */
static void test_job_stop(void)
{
    printf("job stop:\n");
    char path[256];
    snprintf(path, sizeof(path), "%s/terminator_stop_%d.txt",
             tmp_dir(), (int)getpid());
    unlink(path);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: run "sleep 30" through the shell with output to a file. */
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            _exit(1);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        setvbuf(stdout, NULL, _IONBF, 0);
        shell_t *c = shell_new();
        shell_exec_line(c, "sleep 30");
        /* If the shell returned (because the child was stopped), report it. */
        printf("SHELL_RETURNED\n");
        _exit(0);
    }
    CHECK(pid > 0, "fork job-stop child");
    if (pid <= 0)
        return;

    /* Locate the `sleep` grandchild and stop it (as Ctrl+Z would). The shell
     * itself must keep running so it can report the stopped job. */
    pid_t sleep_pid = 0;
    for (int i = 0; i < 30 && sleep_pid == 0; i++) {
        usleep(100 * 1000);
        sleep_pid = find_child_named(pid, "sleep");
    }
    CHECK(sleep_pid > 0, "found foreground sleep child");
    if (sleep_pid > 0)
        kill(-sleep_pid, SIGSTOP);

    /* The shell must return promptly (not block forever on the stopped child)
     * and print the "Stopped" notice. */
    int waited = 0;
    int returned = 0;
    while (waited < 3000) {
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[4096] = {0};
            size_t got = fread(buf, 1, sizeof(buf) - 1, f);
            buf[got] = '\0';
            fclose(f);
            if (strstr(buf, "Stopped") || strstr(buf, "SHELL_RETURNED")) {
                returned = 1;
                break;
            }
        }
        usleep(100 * 1000);
        waited += 100;
    }
    CHECK(returned, "shell returns after Ctrl+Z (child stopped)");
    if (sleep_pid > 0) {
        kill(-sleep_pid, SIGCONT);
        kill(-sleep_pid, SIGKILL);
    }
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(path);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
/* Script invocation forms                                            */
/* ------------------------------------------------------------------ */

static void test_script_invocation(void)
{
    printf("script invocation:\n");
    char path[256];
    snprintf(path, sizeof(path), "%s/terminator_inv_%d.sh",
             tmp_dir(), (int)getpid());
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "create script");
    if (!f)
        return;
    fputs("SRC_VAR=sourced\n"
          "echo from_source $1 $#\n", f);
    fclose(f);

    sink_t s;
    shell_t *sh = make_shell(&s);
    char cmd[1024];

    /* source: runs in the current shell, so variables persist. */
    buf_clear(&s.b);
    snprintf(cmd, sizeof(cmd), "source %s arg1", path);
    shell_exec_line(sh, cmd);
    CHECK(strstr(s.b.data, "from_source arg1 1") != NULL, "source output");
    CHECK_STR(shell_get_var(sh, "SRC_VAR"), "sourced", "source persists vars");

    /* bash script.sh: handled by our own interpreter, not /bin/bash. */
    buf_clear(&s.b);
    snprintf(cmd, sizeof(cmd), "bash %s arg2", path);
    shell_exec_line(sh, cmd);
    CHECK(strstr(s.b.data, "from_source arg2 1") != NULL, "bash form output");

    /* ./script.sh / absolute path form. */
    buf_clear(&s.b);
    snprintf(cmd, sizeof(cmd), "%s arg3", path);
    shell_exec_line(sh, cmd);
    CHECK(strstr(s.b.data, "from_source arg3 1") != NULL, "path form output");

    /* Redirection on a script invocation must write to the file, not the
     * shell's normal output sink. */
    char outpath[256];
    snprintf(outpath, sizeof(outpath), "%s/terminator_inv_out_%d.txt",
             tmp_dir(), (int)getpid());
    unlink(outpath);
    buf_clear(&s.b);
    snprintf(cmd, sizeof(cmd), "source %s arg4 > %s", path, outpath);
    shell_exec_line(sh, cmd);
    FILE *rf = fopen(outpath, "r");
    CHECK(rf != NULL, "script redirection creates file");
    if (rf) {
        char line[256] = {0};
        size_t got = fread(line, 1, sizeof(line) - 1, rf);
        line[got] = '\0';
        fclose(rf);
        CHECK(strstr(line, "from_source arg4 1") != NULL,
              "script redirection writes to file");
    }
    CHECK(strstr(s.b.data, "from_source arg4") == NULL,
          "script redirection suppresses sink output");
    unlink(outpath);

    shell_free(sh);
    buf_free(&s.b);
    unlink(path);
}

/* ------------------------------------------------------------------ */
/* multiWatch usage                                                   */
/* ------------------------------------------------------------------ */

static void test_multiwatch_usage(void)
{
    printf("multiwatch:\n");
    sink_t s;
    shell_t *sh = make_shell(&s);
    shell_exec_line(sh, "multiWatch");
    CHECK(strstr(s.b.data, "usage") != NULL, "multiWatch usage message");
    shell_free(sh);
    buf_free(&s.b);
}

/* multiWatch must emit the required header/footer format and re-run commands.
 * It loops until SIGINT, so run it in a child and stop it after a short wait. */
static void test_multiwatch_output(void)
{
    printf("multiwatch output:\n");
    char path[256];
    snprintf(path, sizeof(path), "%s/terminator_mw_%d.txt",
             tmp_dir(), (int)getpid());
    unlink(path);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: run multiWatch with output redirected to a file. Use an
         * unbuffered stream so output survives the SIGINT that stops it. */
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            _exit(1);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        setvbuf(stdout, NULL, _IONBF, 0);
        shell_t *sh = shell_new();
        shell_exec_line(sh, "multiWatch 'echo tick' 'echo tock'");
        _exit(0);
    }
    CHECK(pid > 0, "fork multiWatch child");
    if (pid <= 0)
        return;
    /* Let it run for at least one interval (default 2s) plus slack. */
    usleep(2500 * 1000);
    kill(pid, SIGINT);
    int status = 0;
    waitpid(pid, &status, 0);

    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "multiWatch wrote output");
    if (f) {
        char buf[8192] = {0};
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        fclose(f);
        CHECK(strstr(buf, "\"echo tick\", ") != NULL, "multiWatch header format");
        CHECK(strstr(buf, "----------------------------------------------------")
              != NULL, "multiWatch separator");
        CHECK(strstr(buf, "tick") != NULL, "multiWatch command output");
        CHECK(strstr(buf, "tock") != NULL, "multiWatch second command output");
    }
    unlink(path);
}

/* multiWatch must stop when the shell's cooperative interrupt flag is set,
 * even if no child process is running at that instant (the GUI sets this flag
 * on Ctrl+C). Regression test for the "unresponsive after multiWatch" bug. */
static shell_t *g_mwi_shell;

static void mwi_sigint(int sig)
{
    (void)sig;
    if (g_mwi_shell)
        shell_request_interrupt(g_mwi_shell);
}

static void test_multiwatch_interrupt(void)
{
    printf("multiwatch interrupt:\n");
    char path[256];
    snprintf(path, sizeof(path), "%s/terminator_mwi_%d.txt",
             tmp_dir(), (int)getpid());
    unlink(path);

    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            _exit(1);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        setvbuf(stdout, NULL, _IONBF, 0);
        shell_t *sh = shell_new();
        g_mwi_shell = sh;
        /* Mirror the process-mode worker: SIGINT sets the cooperative flag. */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = mwi_sigint;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        shell_exec_line(sh, "multiWatch 'echo tick' 'echo tock'");
        _exit(0);
    }
    CHECK(pid > 0, "fork multiWatch interrupt child");
    if (pid <= 0)
        return;
    /* Let it run for at least one interval, then interrupt it. */
    usleep(2500 * 1000);
    kill(pid, SIGINT);
    /* It must return promptly rather than looping forever. */
    int status = 0;
    int waited = 0;
    pid_t w;
    while ((w = waitpid(pid, &status, WNOHANG)) == 0 && waited < 5000) {
        usleep(100 * 1000);
        waited += 100;
    }
    if (w == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        CHECK(0, "multiWatch returns after interrupt");
    } else {
        CHECK(1, "multiWatch returns after interrupt");
    }
    unlink(path);
}

/* ------------------------------------------------------------------ */
/* Tab / worker tests (both launch modes)                             */
/* ------------------------------------------------------------------ */

/* Poll a tab until its output contains `needle` or the timeout elapses. */
static int wait_for_output(tab_t *t, const char *needle, int ms)
{
    for (int i = 0; i < ms; i++) {
        tab_drain(t);
        char *o = tab_output_snapshot(t, NULL);
        if (o && strstr(o, needle)) {
            free(o);
            return 1;
        }
        free(o);
        usleep(1000);
    }
    return 0;
}

static int wait_for_tab_idle(tab_t *t, int ms)
{
    for (int i = 0; i < ms; i++) {
        tab_drain(t);
        if (!tab_command_running(t))
            return 1;
        usleep(1000);
    }
    return 0;
}

/* Commands are echoed before execution, so wait for both the echoed command
 * and its output when the same marker appears in each. */
static int wait_for_output_count(tab_t *t, const char *needle,
                                 size_t required, int ms)
{
    for (int i = 0; i < ms; i++) {
        tab_drain(t);
        char *o = tab_output_snapshot(t, NULL);
        size_t count = 0;
        const char *p = o;
        while (p && (p = strstr(p, needle)) != NULL) {
            count++;
            p += strlen(needle);
        }
        if (count >= required)
        {
            free(o);
            return 1;
        }
        free(o);
        usleep(1000);
    }
    return 0;
}

static void test_tab_mode(int process_mode)
{
    printf("tab (%s mode):\n", process_mode ? "process" : "thread");
    history_t *h = history_new("/dev/null", 0);
    job_table_t *jt = jobs_new();
    config_t *cfg = config_load("/dev/null");

    tab_t *t = tab_new(1, process_mode, h, jt, cfg);
    CHECK(t != NULL, "tab created");
    if (!t) {
        config_free(cfg);
        jobs_free(jt);
        history_free(h);
        return;
    }

    /* Input editing. */
    CHECK_STR(tab_input(t), "", "new tab starts with an empty input buffer");
    tab_input_insert(t, "pwd");
    CHECK_STR(tab_input(t), "pwd", "first typed command has no garbage suffix");
    tab_clear_input(t);
    tab_input_insert(t, "A+echo");
    CHECK_STR(tab_input(t), "A+echo",
              "first typed uppercase and symbol characters are preserved");
    tab_clear_input(t);
    tab_set_input(t, "echo hi");
    CHECK_STR(tab_input(t), "echo hi", "input set");
    tab_cursor_home(t);
    tab_cursor_right(t);
    tab_input_insert(t, "X");
    CHECK_STR(tab_input(t), "eXcho hi", "insert at cursor");
    tab_input_backspace(t);
    CHECK_STR(tab_input(t), "echo hi", "backspace");
    tab_cursor_end(t);
    tab_delete_word(t);
    CHECK_STR(tab_input(t), "echo ", "delete word");

    CHECK(tab_insert_mode(t), "new tabs start in insert mode");
    tab_set_input(t, "abcd");
    tab_cursor_home(t);
    tab_toggle_insert_mode(t);
    tab_input_insert(t, "XY");
    CHECK_STR(tab_input(t), "XYcd", "overwrite mode replaces characters at cursor");
    CHECK(tab_cursor(t) == 2 && !tab_insert_mode(t),
          "overwrite mode advances cursor and remains selected");
    tab_toggle_insert_mode(t);
    tab_set_input(t, "abcd");
    tab_cursor_home(t);
    tab_input_insert(t, "X");
    CHECK_STR(tab_input(t), "Xabcd", "insert mode inserts without replacing");

    /* Ambiguous completion displays sorted choices and accepts a number.
     * Text after the cursor must survive replacing the completed word. */
    char completion_dir[1024];
    snprintf(completion_dir, sizeof(completion_dir), "%s",
             shell_cwd(tab_shell(t)));
    char prefix[96], first_name[112], second_name[112];
    snprintf(prefix, sizeof(prefix), "term_complete_pick_%ld_", (long)getpid());
    snprintf(first_name, sizeof(first_name), "%sa", prefix);
    snprintf(second_name, sizeof(second_name), "%sb", prefix);
    char first_path[1200], second_path[1200];
    snprintf(first_path, sizeof(first_path), "%s/%s", completion_dir, first_name);
    snprintf(second_path, sizeof(second_path), "%s/%s", completion_dir, second_name);
    int first_fd = open(first_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    int second_fd = open(second_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    CHECK(first_fd >= 0 && second_fd >= 0, "create completion choice fixtures");
    if (first_fd >= 0) close(first_fd);
    if (second_fd >= 0) close(second_fd);
    char completion_input[192], completion_expected[224];
    snprintf(completion_input, sizeof(completion_input), "echo %s later", prefix);
    snprintf(completion_expected, sizeof(completion_expected), "echo %sa later", prefix);
    tab_set_input(t, completion_input);
    for (size_t i = 0; i < strlen(" later"); i++)
        tab_cursor_left(t);
    tab_complete(t);
    CHECK(strstr(tab_completion_display(t), first_name) != NULL,
          "ambiguous completion choices are available as a temporary display");
    char *completion_scrollback = tab_output_snapshot(t, NULL);
    CHECK(strstr(completion_scrollback, first_name) == NULL,
          "temporary completion choices are not written into scrollback");
    free(completion_scrollback);
    tab_input_insert(t, "1");
    CHECK_STR(tab_input(t), completion_expected,
              "numbered completion selects first match and preserves text after cursor");
    CHECK_STR(tab_completion_display(t), "",
              "selecting a completion clears its temporary display");
    unlink(first_path);
    unlink(second_path);

    /* Submit a command and wait for the worker to produce output. */
    tab_set_input(t, "echo hello_from_tab");
    tab_submit(t);
    CHECK(wait_for_output_count(t, "hello_from_tab", 2, 3000),
          "worker produced output");

    /* A second command proves the worker loop keeps running. */
    tab_set_input(t, "echo second_line");
    tab_submit(t);
    CHECK(wait_for_output_count(t, "second_line", 2, 3000),
          "worker handled second command");

    /* While a foreground command is still running, the GUI should leave a
     * blank line for its blinking caret; the prompt returns on IPC_PROMPT. */
    /* Keep the slow pipeline stage alive long enough that a loaded CI/WSL
     * host cannot finish it between the output check and busy-state check. */
    tab_set_input(t, "sleep 10 | echo sleep_finished");
    tab_submit(t);
    CHECK(tab_command_running(t), "tab marks command as running after submit");
    CHECK(wait_for_output(t, "sleep_finished", 6000),
          "pipeline output appears while sleep is still running");
    CHECK(tab_command_running(t),
          "prompt stays hidden while another pipeline process sleeps");
    CHECK(wait_for_tab_idle(t, 12000),
          "prompt-ready notification clears busy state");

    /* Continuation lines stay visible and the completed quoted command runs. */
    tab_set_input(t, "echo \"first");
    tab_submit(t);
    CHECK(tab_in_continuation(t), "open quote requests continuation");
    char *shown_lines = tab_output_snapshot(t, NULL);
    CHECK(strstr(shown_lines, "echo \"first") != NULL,
          "first multiline input remains visible in scrollback");
    free(shown_lines);
    tab_set_input(t, "second\"");
    tab_submit(t);
    CHECK(!tab_in_continuation(t), "closed quote ends continuation");
    int multiline_output = wait_for_output(t, "first\nsecond", 10000);
    CHECK(multiline_output, "multiline quoted command executes as one command");
    shown_lines = tab_output_snapshot(t, NULL);
    CHECK(strstr(shown_lines, "> second\"") != NULL,
          "continuation line remains visible in scrollback");
    free(shown_lines);

    tab_free(t);
    config_free(cfg);
    jobs_free(jt);
    history_free(h);
}

/* Verify that closed tab IDs are reusable without renaming existing tabs, new
 * tabs remain in creation order, and the simultaneous-tab limit stays 16. */
static void test_app_tab_ids(void)
{
    printf("app tabs:\n");
    char home[] = "/tmp/terminator_app_home_XXXXXX";
    char *made_home = mkdtemp(home);
    CHECK(made_home != NULL, "create isolated app home");
    if (!made_home)
        return;

    const char *old_home_value = getenv("HOME");
    char *old_home = old_home_value ? strdup(old_home_value) : NULL;
    CHECK(setenv("HOME", home, 1) == 0, "isolate app config and history");

    app_t *app = app_new(0);
    CHECK(app != NULL, "create thread-mode app state");
    if (app) {
        for (int id = 1; id <= 5; id++) {
            tab_t *tab = app_new_tab(app);
            CHECK(tab != NULL, "create initial tab");
            if (tab)
                CHECK(tab_id(tab) == id, "initial tabs receive IDs 1 through 5");
        }

        CHECK(app_tab_count(app) == 5, "five initial tabs are open");
        app_close_tab(app, 0);
        app_close_tab(app, 0);
        app_close_tab(app, 0);
        CHECK(app_tab_count(app) == 2, "closing IDs 1 through 3 leaves two tabs");
        CHECK(tab_id(app_tab(app, 0)) == 4 &&
              tab_id(app_tab(app, 1)) == 5,
              "remaining tabs keep IDs 4 and 5 in their original order");

        const int expected_ids[] = { 1, 2, 3, 6, 7 };
        for (size_t i = 0; i < sizeof(expected_ids) / sizeof(expected_ids[0]); i++) {
            tab_t *tab = app_new_tab(app);
            CHECK(tab != NULL, "create replacement tab");
            if (tab)
                CHECK(tab_id(tab) == expected_ids[i],
                      "new tab reuses the lowest free ID");
            CHECK(tab_id(app_tab(app, 0)) == 4 &&
                  tab_id(app_tab(app, 1)) == 5,
                  "existing IDs stay fixed while new tabs append");
            if (tab)
                CHECK(tab_id(app_tab(app, (int)i + 2)) == expected_ids[i],
                      "new tab appears after older tabs in creation order");
        }

        while (app_tab_count(app) < APP_MAX_TABS) {
            tab_t *tab = app_new_tab(app);
            CHECK(tab != NULL, "fill remaining tab slots");
            if (!tab)
                break;
        }
        CHECK(app_new_tab(app) == NULL,
              "the 16-open-tab limit still rejects tab 17");

        app_close_tab(app, 0); /* release ID 4 without renaming the others */
        tab_t *reused = app_new_tab(app);
        CHECK(reused != NULL && tab_id(reused) == 4,
              "the lowest ID becomes reusable when its tab closes");
        CHECK(tab_id(app_tab(app, 0)) == 5,
              "closing and reusing ID 4 leaves tab 5 unchanged");
        app_free(app);
    }

    if (old_home)
        setenv("HOME", old_home, 1);
    else
        unsetenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s/.terminator_history", home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.terminatorrc", home);
    unlink(path);
    rmdir(home);
    free(old_home);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Run each suite independently so failures identify the responsible
     * subsystem; the aggregate exit status is suitable for `make test`. */
    printf("=== Terminator core tests ===\n");
    test_lexer();
    test_parser();
    test_expand();
    test_execution();
    test_redirection();
    test_pipelines();
    test_history();
    test_ipc_payload();
    test_completion();
    test_scripts();
    test_config();
    test_jobs();
    test_job_stop();
    test_script_invocation();
    test_multiwatch_usage();
    test_multiwatch_output();
    test_multiwatch_interrupt();
    test_tab_mode(0);
    test_tab_mode(1);
    test_app_tab_ids();

    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
