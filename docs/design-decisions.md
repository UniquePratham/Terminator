# Design Decisions

## Parser representation
Decision: use a token stream (`src/lexer.c`) and a compact AST
(`src/parser.c`) for simple commands, pipelines, redirections, background
execution, and script conditionals. Parsing is independent of X11.

**Ownership:** the parser **deep-copies** each word into the AST
(`word_copy()`), so the token list and the AST own separate memory. This avoids
a double-free that occurs if the token list and AST both free the same segment
text. `shell_exec_line()` frees the token list immediately after parsing and the
AST after execution.

## Expansion order
Quote-aware variable expansion and command substitution are performed while
building the expanded string; globbing is applied afterwards only when the word
contains an unquoted glob metacharacter. Single-quoted text is literal;
double-quoted text permits the required expansions.

Nested `$()` and escaped nested backtick substitutions are parsed recursively,
so inner commands run first. Trailing newlines are removed from substitution
output. Globbing uses `glob()` on unquoted patterns; unmatched patterns remain
unchanged with `GLOB_NOCHECK`.

## Current directory
A tab stores a logical cwd. Threads do not have independent process working
directories; `chdir()` is process-wide. The tab cwd is applied only in forked
command children, immediately before `execvp()`. The `cd` built-in updates the
tab's logical cwd using lexical path normalisation and never calls `chdir()`.

## Prompt and cursor while a command runs
The tab records whether its worker is executing the submitted command list.
While that flag is set, the GUI redraws the new output line with a blinking
cursor and omits the prompt. The worker's prompt-ready IPC notification clears
the flag and restores the prompt. This keeps the prompt from appearing while a
foreground command such as `sleep` is still running.

## Redirection
`open()`/`dup2()` are applied only in child execution paths. Parent/GUI
descriptors are never changed.

## Job control
One process group per pipeline. Ctrl+C/Ctrl+Z are sent to the active tab's
foreground process group. Children are reaped asynchronously with
`waitpid(..., WNOHANG | WUNTRACED | WCONTINUED)`.

## Shared history
Thread mode uses a pthread mutex; process mode uses shared memory plus a
process-shared semaphore. The history lock is never held while a command
executes. History is capped at 10,000 entries; the `history` built-in displays
the latest 1,000.

Thread mode stores heap strings in a ring protected by a pthread mutex. Process
mode stores fixed-size entries and ring indices in an anonymous `MAP_SHARED`
mapping protected by a process-shared semaphore. The application persists the
history file at shutdown. Ctrl+R prefers the newest exact match; otherwise it
prints every command tied for the best common-substring score when the score is
greater than two characters.

## Filename completion
Candidates are read from the path-prefix directory, or the tab's logical
current directory when no directory prefix is present. One match completes
directly; multiple matches extend to their common prefix or are displayed in
sorted order for numbered selection. Replacing a word preserves the remainder
of the input line after the cursor.

## IPC
Process mode uses a framed protocol with an explicit message type, tab id, and
payload length (`ipc_header_t`, 12 bytes). Partial reads/writes are handled by
looping helpers, and payload lengths are validated against a maximum.
Each tab has a GUI-to-worker input pipe and a worker-to-GUI output pipe; output
and foreground process-group notifications use the framed output channel.

## Script interpreter
Only the grammar required by the brief is implemented. `/bin/bash` is never
invoked to interpret a script. Conditional block structure is validated before
execution, so a missing `fi` is reported without running the script.
The supported `bash script.sh` spelling routes directly to this interpreter;
direct external shell execution such as `bash -c ...` is rejected.

## PTY scope
Without a PTY, interactive full-screen applications are not promised. No PTY
layer is implemented.

## Scrollback and limits
- History: 10,000 entries persisted, 1,000 displayed.
- History entry length: 4,096 bytes.
- Scrollback: configurable (`scrollback = N` in `~/.terminatorrc`, default
  5000).
- IPC payload: 16 MB maximum.
- multiWatch: up to 64 commands, default 2-second interval.

## Shell service ownership
A standalone shell (used by tests and by command-substitution children) creates
its own job table so it is self-contained. The application injects a shared job
table via `shell_set_jobs()`; the shell tracks whether it owns the table and
frees only what it created. This keeps the shell usable both embedded and
standalone without leaking or double-freeing.
