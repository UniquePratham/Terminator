# System Call and API Map

This table lists the calls actually present in the source.

| Feature | APIs | Where | Why |
|---|---|---|---|
| X11 GUI | `XOpenDisplay`, `XCreateSimpleWindow`, `XSelectInput`, `XStoreName`, `XMapWindow`, `XCreateGC`, `XLoadQueryFont`, `XNextEvent`, `XDrawString`, `XLookupString`, `XFlush` | `src/gui.c` | Window, event handling, keyboard decoding and drawing |
| Clipboard | `XInternAtom`, `XSetSelectionOwner`, `XConvertSelection`, `XChangeProperty`, `XGetWindowProperty` | `src/gui.c` | Copy/paste via X selections |
| Responsive multiplexing | `ConnectionNumber`, `poll`, `read`, `write` | `src/gui.c`, `src/tab.c`, `src/ipc.c` | Wait for GUI and worker output without busy-waiting |
| Child execution | `fork`, `execvp`, `waitpid`, `_exit` | `src/executor.c`, `src/shell.c`, `src/multiwatch.c` | Launch and reap external programs |
| Pipe creation | `pipe`, `dup2`, `close` | `src/executor.c`, `src/multiwatch.c`, `src/shell.c`, `src/tab.c` | Connect command output to the next command's input |
| Redirection | `open`, `dup2`, `close` | `src/executor.c` | Connect files to stdin/stdout in the child |
| Jobs/signals | `setpgid`, `kill`, `waitpid`, `signal` | `src/executor.c`, `src/jobs.c`, `src/tab.c`, `src/main.c` | Process groups, signal forwarding, stopped/continued state |
| Thread mode | `pthread_create`, `pthread_join`, `pthread_mutex_init/lock/unlock/destroy` | `src/tab.c`, `src/history.c` | Per-tab shell workers and synchronized shared history |
| Process mode | `mmap`, process-shared `sem_init`/`sem_wait`/`sem_post`, `pipe`, `fork` | `src/history.c`, `src/tab.c` | Shared history and GUI/worker communication |
| Completion | `opendir`, `readdir`, `closedir`, `stat` | `src/completion.c` | Enumerate candidate filenames |
| Globbing | `glob`, `globfree` | `src/expand.c` | Expand `*`, `?`, `[...]` patterns |
| Persistence | `fopen`, `fgets`, `fprintf`, `fclose` | `src/history.c`, `src/config.c` | Read/write bounded history and configuration |
| Time | `time` | `src/multiwatch.c` | multiWatch timestamps and scheduling |
| Temp files (tests) | `mkstemp`, `mkdtemp`, `unlink`, `rmdir` | `tests/test_main.c` | Fixtures for redirection/completion/script tests |

All return values are checked. The mapping above matches calls actually present
in the source.
