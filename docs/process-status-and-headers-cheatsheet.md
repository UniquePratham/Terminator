# Process Status and Header Files Cheatsheet

This guide explains the process-status macros Terminator uses and the headers included by its source code, public project headers, and test harness.

## 1. Process status and signals

### What happens when a command runs

Terminator starts external commands as child processes. The operating system reports a child's state through `wait()` or `waitpid()`. The integer `status` returned through `waitpid()` is encoded: use the `WIF...` macros to determine what happened, then use the matching `W...` macro to read the exit code or signal.

```c
int status = 0;
pid_t result = waitpid(pid, &status, 0);

if (result > 0) {
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        int shell_status = 128 + signal_number;
    }
}
```

The code must test `WIFEXITED` before using `WEXITSTATUS`, and test `WIFSIGNALED` before using `WTERMSIG`. Those value macros only make sense for their matching condition.

### Status macros

| Name | Meaning | Terminator's use |
|---|---|---|
| `WIFEXITED(status)` | The child finished normally, by returning from `main()` or calling `exit()`. | Tests whether a regular exit code is available. |
| `WEXITSTATUS(status)` | The child's exit code; use only after `WIFEXITED(status)` is true. | Records success (`0`) or a command's failure code. |
| `WIFSIGNALED(status)` | The child was terminated by a signal. | Detects termination such as Ctrl+C. |
| `WTERMSIG(status)` | The signal number that terminated the child; use only after `WIFSIGNALED(status)` is true. | Terminator maps it to the shell convention `128 + signal_number`. |
| `WIFSTOPPED(status)` | The child was stopped but still exists. | Keeps a stopped job available for `fg` or `bg`. |
| `WSTOPSIG(status)` | The signal that stopped the child; use only after `WIFSTOPPED(status)` is true. | Records the stop reason for a foreground command. |
| `WIFCONTINUED(status)` | A previously stopped child has resumed. | Marks a background job as running again. |

`W...` names are macros from the operating system's `<sys/wait.h>` header. They are not definitions written by Terminator.

### `waitpid()` options and results

| Name/result | Meaning | Where it helps |
|---|---|---|
| `waitpid(pid, &status, options)` | Ask the operating system for a child process's state. | Used by the executor and job manager to collect child status. |
| `WNOHANG` | Return immediately if there is no new child status yet. | Lets the GUI/event loop check children without blocking. |
| `WUNTRACED` | Also report children that have stopped. | Supports Ctrl+Z and stopped jobs. |
| `WCONTINUED` | Also report children that resumed. | Lets the job table notice a resumed job. |
| Return value `> 0` | A child state was collected; the value is the child's PID. | Decode the accompanying `status`. |
| Return value `0` | With `WNOHANG`, no state change is ready. | Keep the child marked as running and check again later. |
| Return value `-1` | An error occurred; inspect `errno`. | Code commonly retries when `errno == EINTR`. |
| `EINTR` | A system call was interrupted by a signal. | Retry `waitpid()` when the wait itself was interrupted. |

### Signal names

Signal constants come from `<signal.h>`. They are identifiers passed to functions such as `kill()` or installed with `sigaction()`; they are different from the `WIF...` status-decoding macros.

| Signal | Typical meaning in Terminator |
|---|---|
| `SIGINT` | Interrupt, commonly Ctrl+C. |
| `SIGTERM` | Ask a process to terminate; used while cleaning up a worker. |
| `SIGSTOP` | Stop a process immediately; used in tests and job-control scenarios. |
| `SIGTSTP` | Interactive stop, commonly Ctrl+Z. |
| `SIGCONT` | Resume a stopped process. |
| `SIGPIPE` | Can occur when writing to a pipe whose reader has exited; the GUI ignores it and handles write errors. |

### Where status handling appears

- [`src/executor.c`](../src/executor.c): waits for pipeline children, handles stopped processes, gets normal exit codes, and uses `WTERMSIG` when a command is killed by a signal.
- [`src/jobs.c`](../src/jobs.c): periodically reaps background jobs and updates their running, stopped, or done state.
- [`src/shell.c`](../src/shell.c): brings a background job to the foreground and waits for it to finish or stop.
- [`src/multiwatch.c`](../src/multiwatch.c): waits for monitored commands and recognizes interruption/termination signals.
- [`src/tab.c`](../src/tab.c): stops and reaps the per-tab worker process during cleanup.
- [`src/gui.c`](../src/gui.c): sends user-requested signals such as Ctrl+C and Ctrl+Z to the active tab's job.

## 2. Terminator's own headers

Files in `include/` are project headers. They define Terminator's types and function interfaces. Their implementations generally live in the matching `.c` file under `src/`.

| Project header | Main responsibility | Main implementation |
|---|---|---|
| [`app.h`](../include/app.h) | Application lifecycle, tab collection, active-tab selection, and shared services. | [`app.c`](../src/app.c) |
| [`completion.h`](../include/completion.h) | Completion result structure and filename/command completion interface. | [`completion.c`](../src/completion.c) |
| [`config.h`](../include/config.h) | Configuration types and key-binding settings. | [`config.c`](../src/config.c) |
| [`executor.h`](../include/executor.h) | Interface for executing parsed commands and pipelines. | [`executor.c`](../src/executor.c) |
| [`expand.h`](../include/expand.h) | Word-expansion data and functions, including variable and wildcard expansion. | [`expand.c`](../src/expand.c) |
| [`gui.h`](../include/gui.h) | Interface for starting the X11 GUI event loop. | [`gui.c`](../src/gui.c) |
| [`history.h`](../include/history.h) | Shared command-history creation, lookup, search, and saving. | [`history.c`](../src/history.c) |
| [`ipc.h`](../include/ipc.h) | Process-mode message types and functions for sending and receiving framed messages. | [`ipc.c`](../src/ipc.c) |
| [`jobs.h`](../include/jobs.h) | Job records and functions for tracking, signaling, and reaping child processes. | [`jobs.c`](../src/jobs.c) |
| [`lexer.h`](../include/lexer.h) | Word, segment, and token structures used to turn input text into tokens. | [`lexer.c`](../src/lexer.c) |
| [`multiwatch.h`](../include/multiwatch.h) | Interface for the `multiWatch` built-in. | [`multiwatch.c`](../src/multiwatch.c) |
| [`parser.h`](../include/parser.h) | Command, pipeline, and redirection structures, plus the token-to-command parser interface. | [`parser.c`](../src/parser.c) |
| [`scripts.h`](../include/scripts.h) | Functions for validating and interpreting the project's supported script subset. | [`scripts.c`](../src/scripts.c) |
| [`shell.h`](../include/shell.h) | Shell state and operations: variables, cwd, status, output, built-ins, and command execution. | [`shell.c`](../src/shell.c) |
| [`tab.h`](../include/tab.h) | Public tab operations: editing input, moving the cursor, output, completion, history, and command submission. | [`tab.c`](../src/tab.c) |
| [`util.h`](../include/util.h) | Shared allocation, string, buffer, and reliable-write helpers. | [`util.c`](../src/util.c) |

### How the project interfaces fit together

```text
GUI input
   -> lexer.h      splits text into tokens
   -> parser.h     groups tokens into commands and pipelines
   -> expand.h     expands words and arguments
   -> executor.h   runs built-ins or starts child processes
   -> shell.h      holds shell state and reports output/status
```

`app.h` and `tab.h` connect this command engine to the window and tabs. `jobs.h` tracks child commands, while `ipc.h` carries messages between the GUI and worker processes in process mode.

## 3. System and library headers used directly

These headers are supplied by the C library, the Linux/POSIX system, or X11 development libraries. They are not project files.

### C standard library

| Header | What it provides in this project |
|---|---|
| `<ctype.h>` | Character tests and conversions used by the lexer and script parser. |
| `<errno.h>` | Error codes from failed system calls; used to handle errors and retry interrupted calls. |
| `<limits.h>` | System-defined numeric limits. |
| `<stdarg.h>` | Variable-argument handling for formatted helper functions. |
| `<stddef.h>` | Basic types and values such as `size_t` and `NULL`. |
| `<stdint.h>` | Fixed-width integer types such as `uint32_t`, used by IPC. |
| `<stdio.h>` | File and formatted input/output, including `FILE`, `fopen`, `fgets`, `snprintf`, and `fprintf`. |
| `<stdlib.h>` | General utilities such as memory allocation, conversions, and process exit. |
| `<string.h>` | String and memory functions such as `strlen`, `strcmp`, `memcpy`, and `strstr`. |

### POSIX/Linux headers

| Header | What it provides in this project |
|---|---|
| `<dirent.h>` | Reading directory entries for filename completion. |
| `<fcntl.h>` | Opening files and setting descriptor flags for redirection and pipes. |
| `<glob.h>` | Expanding wildcard path patterns such as `*.c`. |
| `<poll.h>` | Waiting for file descriptors to become ready without blocking the GUI. |
| `<pthread.h>` | Worker threads, mutexes, and thread synchronization. |
| `<pwd.h>` | User-account information, including the home-directory fallback. |
| `<semaphore.h>` | Semaphores used to coordinate shared history. |
| `<signal.h>` | Signal constants and functions such as `SIGINT`, `sigaction`, and `kill`. |
| `<sys/mman.h>` | Memory mapping used by shared history storage. |
| `<sys/stat.h>` | File metadata and checks for regular files/directories. |
| `<sys/types.h>` | System types, including process IDs such as `pid_t`. |
| `<sys/utsname.h>` | Host/system name information used in the prompt. |
| `<sys/wait.h>` | `waitpid()` and status macros such as `WIFEXITED` and `WTERMSIG`. |
| `<time.h>` | Clocks, timestamps, and intervals used by `multiWatch` and cursor timing. |
| `<unistd.h>` | POSIX process and file-descriptor calls such as `fork`, `exec`, `pipe`, `read`, `write`, `close`, and `chdir`. |

### X11 GUI headers

| Header | What it provides in this project |
|---|---|
| `<X11/Xlib.h>` | Core X11 display, window, event, and drawing APIs. |
| `<X11/Xatom.h>` | X11 atoms used for clipboard properties and window-manager protocols. |
| `<X11/Xutil.h>` | X11 helper utilities for windows and events. |
| `<X11/XKBlib.h>` | Keyboard-layout and keyboard event support. |
| `<X11/Xft/Xft.h>` | Antialiased font loading and text drawing. |
| `<X11/keysym.h>` | Symbolic key names such as Home, End, and the arrow keys. |

## 4. A quick explanation for a viva

> “Headers declare interfaces, types, and constants so source files can use them. Our headers under `include/` define Terminator's own modules. Headers in angle brackets come from the system or libraries. For process status, `<sys/wait.h>` provides macros that decode the value returned by `waitpid()`; for example, `WIFSIGNALED` checks whether a process died from a signal, and `WTERMSIG` tells us which signal it was.”
