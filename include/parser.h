/* parser.h — builds a compact AST from the token stream. */
#ifndef TERMINATOR_PARSER_H
#define TERMINATOR_PARSER_H

#include "lexer.h"

typedef enum {
    REDIR_IN,      /* <  */
    REDIR_OUT,     /* >  */
    REDIR_APPEND   /* >> */
} redir_type_t;

typedef struct {
    redir_type_t type;
    word_t       target;
} redir_t;

typedef struct {
    word_t  *argv;      /* argument words; argv[0] is the command name */
    size_t   argc;      /* number of argument words */
    redir_t *redirs;    /* input/output redirections attached to this command */
    size_t   nredirs;   /* number of entries in redirs[] */
} command_t;

typedef struct {
    command_t *cmds;    /* commands connected by | */
    size_t     ncmds;   /* one more than the number of pipe operators */
    int        background; /* nonzero when pipeline ends with & */
} pipeline_t;

typedef struct {
    pipeline_t *pipelines; /* sequential groups separated by ; or newline */
    size_t      npipelines;
} cmdline_t;

void cmdline_init(cmdline_t *cl);
void cmdline_free(cmdline_t *cl);

/* Parse a token list. Returns 0 on success, -1 on syntax error with a
 * message in errbuf. */
int parse_tokens(const token_list_t *tl, cmdline_t *out,
                 char *errbuf, size_t errlen);

#endif /* TERMINATOR_PARSER_H */
