# Testing Strategy and Results

## Build checks
- `make clean && make` — builds `./terminator` with `-Wall -Wextra -Wpedantic`
  and **zero warnings**.
- `make test` — builds and runs the headless test suite.
- `make debug`, `make sanitize` — available for debugging and sanitizer runs.

## Unit / integration tests
The test harness (`tests/test_main.c`) links only the GUI-free core modules, so
it runs without X11. It covers:

- **lexer:** simple words, single quotes, empty quotes, escaped space,
  operators, open quote / trailing backslash / open `$(` (incomplete), variable
  segments.
- **parser:** pipeline, redirection, background, missing redirection target,
  empty pipeline segment.
- **expand:** variable expansion, command substitution.
- **execution:** `echo`, `true`, `false`, `$?`, unknown command, assignment,
  and explicit rejection of `bash -c` shell delegation.
- **redirection:** `<`, `>`, missing input file.
- **pipelines:** 2-stage, 3-stage, 5-stage, pipeline + redirection.
- **history:** persistence, exact match, tied substring results, no match, and
  child-to-parent visibility through process-shared history.
- **completion:** no match, one match, common prefix, sorted ambiguous choices,
  numbered selection, and preservation of text after the cursor.
- **scripts:** variables, positional parameters, quoted `$1` in tests, relative
  file checks after a tab-local `cd`, nested `if`/`elif`, `-eq`, `-gt`, `-f`,
  `-d`, `=`, exit status, and missing `fi`.
- **script invocation:** `source`, `bash script.sh`, and path forms.
- **multiwatch:** usage message, exact output format (header + 52-dash
  separators + periodic re-run), and interrupt (Ctrl+C) recovery.
- **config:** defaults, custom bindings, invalid-key fallback, and conflicting
  binding fallback.
- **jobs:** add, count, find; plus a stopped-job regression test (Ctrl+Z).
- **tab:** input editing and command execution in **both** modes, including
  first typed characters, symbols, multiline visibility, completion selection,
  text after the cursor, Insert/overwrite editing, and the prompt returning after the sleep regression.

## Observed results

```
=== Terminator core tests ===
lexer:
parser:
expand:
execution:
redirection:
pipelines:
history:
completion:
scripts:
config:
jobs:
job stop:
script invocation:
multiwatch:
multiwatch output:
multiwatch interrupt:
tab (thread mode):
tab (process mode):

=== 243 passed, 0 failed ===
```

Latest observed validation on Ubuntu in WSL: `make clean && make test && make`
completed with **243 passed, 0 failed**, then built the application without
compiler warnings. The automated suite is headless; it does not replace GUI
checks under Xvfb.

## GUI / manual tests
The GUI was exercised under a virtual X server (Xvfb on display `:1`) and driven
with `xdotool` in **both** launch modes:

```sh
Xvfb :1 -screen 0 1024x768x24 -nolisten tcp &
export DISPLAY=:1
./terminator                    # thread mode
./terminator useProcess=True    # process mode
```

Verified interactively (both modes unless noted):

- window creation, input editing, expose/resize, scrolling;
- tab new / switch / close, output in hidden tabs;
- history recall (Up) and Ctrl+R search;
- Ctrl+L clear, Ctrl+U/K/W editing, Ctrl+A/E, arrow keys;
- clipboard copy/paste round-trip (Ctrl+Shift+C / Ctrl+Shift+V);
- filename completion (Tab);
- Home / End cursor movement, Insert/overwrite cursor modes, and a cursor-only line during a running pipeline in both modes;
- Ctrl+D exit (clean shutdown);
- background jobs and the `jobs` built-in;
- **Ctrl+C** interrupting `multiWatch` and returning control to the shell;
- **Ctrl+Z** suspending a foreground command, `jobs` reporting it as stopped,
  and `bg` resuming it.

## Both modes
The tab tests run the same functional checks under thread mode and process mode.
Shared history across tabs, independent cwd, and signal isolation are
implemented and exercised. In process mode the GUI signals the foreground job's
process group directly (reported by the worker over IPC) so Ctrl+C/Ctrl+Z reach
the job rather than the worker process.

The app-level tab regression test also verifies the 16-tab cap, stable IDs for
open tabs, lowest-vacant-ID reuse, and creation-order placement. Its sequence
checks that closing IDs 1–3 while 4 and 5 remain, then opening five tabs,
produces `4, 5, 1, 2, 3, 6, 7` in the strip.

## Anti-hang policy
Tests that may block use bounded polling (`wait_for_output` polls for up to
3 seconds). Pipelines are tested specifically for EOF hangs caused by open write
descriptors.

## Environment note
On filesystems that strip execute permission (for example Android shared
storage), `make test` automatically copies the test binary to `$TMPDIR` and runs
it there. This is handled by the `test` target in the `Makefile`.
