/* parser.c — builds a compact AST from the token stream.
 *
 * Grammar shape: command-line = pipeline (separator pipeline)*;
 * pipeline = command ('|' command)*; command = words plus redirections.
 * This stage checks operator placement and owns deep copies of parsed words;
 * it does not execute commands or expand variables.
 */
#include "parser.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cmdline_init(cmdline_t *cl)
{
    cl->pipelines = NULL;
    cl->npipelines = 0;
}

static void command_free(command_t *c)
{
    for (size_t i = 0; i < c->argc; i++)
        word_free(&c->argv[i]);
    free(c->argv);
    for (size_t i = 0; i < c->nredirs; i++)
        word_free(&c->redirs[i].target);
    free(c->redirs);
    c->argv = NULL;
    c->redirs = NULL;
    c->argc = c->nredirs = 0;
}

void cmdline_free(cmdline_t *cl)
{
    for (size_t i = 0; i < cl->npipelines; i++) {
        pipeline_t *p = &cl->pipelines[i];
        for (size_t j = 0; j < p->ncmds; j++)
            command_free(&p->cmds[j]);
        free(p->cmds);
    }
    free(cl->pipelines);
    cmdline_init(cl);
}

static void command_add_arg(command_t *c, const word_t *w)
{
    c->argv = xrealloc(c->argv, (c->argc + 1) * sizeof(*c->argv));
    word_copy(&c->argv[c->argc], w); /* deep copy: AST owns its words */
    c->argc++;
}

static void command_add_redir(command_t *c, redir_type_t type, const word_t *w)
{
    c->redirs = xrealloc(c->redirs, (c->nredirs + 1) * sizeof(*c->redirs));
    c->redirs[c->nredirs].type = type;
    word_copy(&c->redirs[c->nredirs].target, w);
    c->nredirs++;
}

static void pipeline_add_cmd(pipeline_t *p, command_t *c)
{
    p->cmds = xrealloc(p->cmds, (p->ncmds + 1) * sizeof(*p->cmds));
    p->cmds[p->ncmds++] = *c;
}

static void cmdline_add_pipeline(cmdline_t *cl, pipeline_t *p)
{
    cl->pipelines = xrealloc(cl->pipelines,
                             (cl->npipelines + 1) * sizeof(*cl->pipelines));
    cl->pipelines[cl->npipelines++] = *p;
}

static int is_separator(token_type_t t)
{
    return t == TOK_SEMI || t == TOK_NEWLINE || t == TOK_EOF;
}

int parse_tokens(const token_list_t *tl, cmdline_t *out,
                 char *errbuf, size_t errlen)
{
    /* Consume tokens left-to-right. Each completed command is appended to a
     * pipeline, and each completed pipeline is appended to the command line. */
    size_t i = 0;
    if (errbuf && errlen)
        errbuf[0] = '\0';

    while (i < tl->ntoks && tl->toks[i].type != TOK_EOF) {
        /* Skip leading separators. */
        if (is_separator(tl->toks[i].type)) {
            i++;
            continue;
        }
        if (tl->toks[i].type == TOK_BG) {
            snprintf(errbuf, errlen, "syntax error near '&'");
            return -1;
        }

        pipeline_t pipe = { NULL, 0, 0 };
        int expect_cmd = 1;

        while (i < tl->ntoks) {
            token_type_t t = tl->toks[i].type;
            if (is_separator(t))
                break;
            if (t == TOK_BG) {
                pipe.background = 1;
                i++;
                break;
            }
            if (t == TOK_PIPE) {
                if (expect_cmd) {
                    snprintf(errbuf, errlen, "syntax error: empty pipeline segment");
                    goto fail;
                }
                expect_cmd = 1;
                i++;
                continue;
            }
            if (t == TOK_REDIR_IN || t == TOK_REDIR_OUT || t == TOK_REDIR_APPEND) {
                if (expect_cmd) {
                    snprintf(errbuf, errlen, "syntax error: redirection without command");
                    goto fail;
                }
                redir_type_t rt = t == TOK_REDIR_IN ? REDIR_IN :
                                  t == TOK_REDIR_OUT ? REDIR_OUT : REDIR_APPEND;
                i++;
                if (i >= tl->ntoks || tl->toks[i].type != TOK_WORD) {
                    snprintf(errbuf, errlen, "syntax error: missing redirection target");
                    goto fail;
                }
                command_t *cur = &pipe.cmds[pipe.ncmds - 1];
                command_add_redir(cur, rt, &tl->toks[i].word);
                i++;
                continue;
            }
            if (t == TOK_WORD) {
                if (expect_cmd) {
                    command_t c = { NULL, 0, NULL, 0 };
                    pipeline_add_cmd(&pipe, &c);
                    expect_cmd = 0;
                }
                command_t *cur = &pipe.cmds[pipe.ncmds - 1];
                command_add_arg(cur, &tl->toks[i].word);
                i++;
                continue;
            }
            i++;
        }

        if (expect_cmd && pipe.ncmds > 0) {
            snprintf(errbuf, errlen, "syntax error: empty pipeline segment");
            goto fail;
        }
        if (pipe.ncmds > 0)
            cmdline_add_pipeline(out, &pipe);
        else
            free(pipe.cmds);
        continue;

fail:
        for (size_t j = 0; j < pipe.ncmds; j++)
            command_free(&pipe.cmds[j]);
        free(pipe.cmds);
        return -1;
    }
    return 0;
}
