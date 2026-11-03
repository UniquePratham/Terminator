/* executor.h — runs pipelines: fork, pipes, redirection, process groups. */
#ifndef TERMINATOR_EXECUTOR_H
#define TERMINATOR_EXECUTOR_H

#include "parser.h"
#include "shell.h"

/* Execute a parsed command line. Returns the exit status of the last
 * pipeline. */
int executor_run(shell_t *sh, cmdline_t *cl);

#endif /* TERMINATOR_EXECUTOR_H */
