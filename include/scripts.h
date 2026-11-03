/* scripts.h — the required Bash-script subset interpreter. */
#ifndef TERMINATOR_SCRIPTS_H
#define TERMINATOR_SCRIPTS_H

#include "shell.h"

/* Validate a script's block structure (if/fi, quotes) without executing.
 * Returns 0 if valid, -1 with a message in errbuf otherwise. */
int scripts_validate(const char *path, char *errbuf, size_t errlen);

/* Run a script file in a fresh script context with positional parameters.
 * Returns the script's exit status. */
int scripts_run_file(shell_t *sh, const char *path, char *const *args, int nargs);

/* `source file [args...]`: run in the current shell context so assignments
 * and cd persist. */
int scripts_source(shell_t *sh, const char *path, char *const *args, int nargs);

#endif /* TERMINATOR_SCRIPTS_H */
