/* multiwatch.h — the multiWatch built-in. */
#ifndef TERMINATOR_MULTIWATCH_H
#define TERMINATOR_MULTIWATCH_H

#include "shell.h"

/* Run `multiWatch ["cmd1", "cmd2", ...]`. argv[0] is "multiWatch".
 * Returns the exit status. */
int multiwatch_run(shell_t *sh, char **argv, int argc);

#endif /* TERMINATOR_MULTIWATCH_H */
