# Troubleshooting

## `make` cannot find X11 or Xft headers
Install the Xlib development package for your distribution (Debian/Ubuntu
commonly use `libx11-dev` and `libxft-dev`) and ensure the Makefile links X11
and Xft (`-lX11 -lXft`).

## `XOpenDisplay` fails
Check that an X server is running and `DISPLAY` is set correctly. Terminator
prints `terminator: cannot open X display (is DISPLAY set?)` and exits non-zero
rather than silently falling back to console mode.

## `make test` reports "Permission denied"
Some filesystems (for example Android shared storage) strip execute permission
bits. The `test` target detects this and re-runs the binary from `$TMPDIR`. If
you run the binary manually, copy it to a directory that permits execution and
`chmod +x` it.

## A pipeline hangs
Check that every child and the parent close all unused pipe ends. A reader does
not receive EOF while any process still has the write end open.

## Ctrl+C kills the application
Verify the GUI never shares the foreground command's process group and that
Ctrl+C is forwarded only to the active tab's foreground job process group.

## One tab's `cd` changes another
`chdir()` is process-wide, not thread-local. The tab stores a logical cwd and
sets cwd only in a forked child before external execution.

## Redirection breaks the GUI
Do not call `dup2()` on the GUI process's descriptors. Redirect only in a child.

## Process mode history differs between tabs
Ensure the shared-memory object is truly mapped across processes and
synchronization is initialized for process sharing (`sem_init(..., 1, 1)`). Do
not use an ordinary process-private mutex for cross-process data.

## Clipboard does not work
Check X11 selection ownership/request handling and ensure the event loop
continues processing selection events. Terminator supports `CLIPBOARD` with
`UTF8_STRING`/`XA_STRING` targets.

## Full-screen program behaves incorrectly
A basic pipe-based terminal has no PTY and does not implement terminal
escape-sequence semantics. This is a documented limitation, not a shell parser
failure.

## Script reports syntax errors
Confirm quotes and conditional blocks are closed. The interpreter validates
block structure (for example a missing `fi`) before executing the script.
