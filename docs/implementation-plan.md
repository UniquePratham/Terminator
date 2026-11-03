# Implementation Plan and Feature Matrix

## Milestones

1. **Foundation:** Makefile, utilities, bounded buffers, diagnostics, lifecycle.
2. **Shell core:** lexer, parser, AST, syntax errors, headless harness.
3. **Execution:** simple commands, built-ins, cwd isolation, `$?`, redirection, pipelines.
4. **Expansion and multiline:** variable expansion, substitutions, globbing, continuation.
5. **Scripts:** variable assignments, arguments, conditionals, tests, source.
6. **History and completion:** persistent shared history, Ctrl+R, file completion, config.
7. **GUI:** Xlib window, input editing, scrollback, tabs, poll-driven output.
8. **Modes:** thread mode then process mode with shared history and framed IPC.
9. **Jobs and multiWatch:** process groups, stop/resume, background jobs, concurrent periodic commands.
10. **Hardening and docs:** tests, sanitizers, troubleshooting, final documentation.

The feature matrix below tracks the assignment's 11 tasks and the required two
execution modes. “Implemented” means the source has the feature and the cited
automated checks pass; GUI interaction is also exercised under Xvfb. Full-screen
programs remain outside the pipe-based terminal's scope.

## Feature matrix

| Feature | Status | Test / evidence |
|---|---|---|
| 1. X11 GUI and tabs | implemented | Xvfb GUI checks in both modes; regression checks cover stable IDs, lowest-vacant reuse, creation order, and the 16-tab cap |
| 2. Built-in and external commands | implemented | execution and script-invocation tests; external launch uses `fork` + `execvp` |
| 3. Multiline input and quoting | implemented | lexer, expansion, and continuation tests; `$()` and backticks |
| 4. `<` and `>` redirection | implemented | input/output, missing-input, combined redirection tests |
| 5. Pipelines | implemented | 2/3/5-stage and pipeline-plus-redirection tests |
| 6. `multiWatch` | implemented | format, repeated output, interrupt, and recovery tests |
| 7. Configurable hotkeys | implemented | default, custom, invalid-key, and conflict regression tests |
| 8. Signals and jobs | implemented | background table and stop/resume tests; GUI signal forwarding tested |
| 9. Shared searchable history | implemented | exact/tied substring/no-match, persistence, and process-shared write tests |
| 10. Filename completion | implemented | no/one/prefix/ambiguous selection tests; text-after-cursor regression |
| 11. Script interpreter subset | implemented | variables, arguments, `$?`, nested conditions/operators, source/invocation, malformed `fi` tests |
| Two execution modes | implemented | tab tests in thread and process modes; Xvfb launch checks |
| Automated suite | implemented | `make test` → **243 passed, 0 failed** |
| Input cursor modes | implemented | Insert toggles thin insert cursor and block overwrite cursor |
| Submission documentation | implemented | README, walkthrough, design, syscall map, test and demo guides |

## Known gaps

- Full-screen interactive programs (for example `vim`) are not guaranteed to
  work: the terminal is pipe-based and does not implement a PTY or terminal
  escape-sequence semantics. This is a documented scope limitation.
- Only `<` and `>` redirection are required and implemented; `2>&1` and other
  descriptor-duplication forms are out of scope.
- This is the specified shell/script subset, not all of Bash. Loops, functions,
  heredocs, and many less common expansion rules are outside the requirement.
- The command spelling `bash script.sh` is handled by this project's script
  interpreter. Commands and scripts are never sent to `/bin/bash`; `execvp()`
  launches external utilities.
- Sanitizer targets are available; the current acceptance run recorded below
  uses the standard warning-enabled build and test suite.
