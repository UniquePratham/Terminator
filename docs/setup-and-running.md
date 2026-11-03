# Setup and Running Guide

This document describes how to set up, build, run, and test **Terminator** on a
normal Linux desktop and in Ubuntu on WSL.

---

## 1. Requirements

| Component | Needed for | Notes |
|---|---|---|
| Linux (or a Linux-like environment) | everything | POSIX process/thread APIs are used |
| C11 compiler | build | `gcc` or `clang` |
| GNU Make | build | `make` |
| Xlib and Xft development headers | GUI text and font rendering | packages `libx11-dev`, `libxft-dev` |
| POSIX threads | build | `-lpthread` |
| An X server | running the GUI | a real desktop, or `Xvfb` for headless |

### Install on Debian / Ubuntu

```sh
sudo apt update
sudo apt install build-essential make libx11-dev libxft-dev
```

### Install on Fedora / RHEL

```sh
sudo dnf install gcc make libX11-devel libXft-devel
```

### Install on Arch

```sh
sudo pacman -S base-devel libx11 libxft
```

### Optional tools (only for headless GUI testing)

```sh
sudo apt install xvfb xdotool
```

- `Xvfb` — a virtual X server (no physical display required).
- `xdotool` — synthesise keystrokes/clicks into the window.
---

## 2. Build

From the project root:

```sh
make
```

This produces the executable `./terminator`. The build uses
`-std=c11 -Wall -Wextra -Wpedantic -O2 -g` and completes with **zero warnings**.

Other targets:

```sh
make debug      # -O0 -g3, for debugging
make sanitize   # AddressSanitizer + UndefinedBehaviorSanitizer
make clean      # remove build artefacts
```

---

## 3. Run

Terminator has two launch modes. Both share the same shell engine; they differ
only in how each tab's shell is isolated.

### Thread mode (default)

```sh
./terminator
```

Each tab runs its shell on a dedicated worker **thread** inside one process.
This is the lighter mode and the default.

### Process mode

```sh
./terminator useProcess=True
```

Each tab runs its shell in a separate **worker process**, communicating with the
GUI over a length-framed IPC pipe. History is shared between processes via
`mmap` + a process-shared semaphore.

### Command-line options

| Argument | Effect |
|---|---|
| *(none)* | thread mode |
| `useProcess=True` | process mode |
| `--help`, `-h` | print usage and exit |
| anything else | error, exit code 2 |

If no X display is available, Terminator prints
`terminator: cannot open X display (is DISPLAY set?)` and exits non-zero.

---

## 4. Using the terminal

Once the window is open you get a prompt. Type a command and press **Enter**.

### Built-ins

| Built-in | Description |
|---|---|
| `clear` | clear the active tab's visible output and scrollback |
| `cd [dir]` | change directory (per-tab, isolated) |
| `exit [n]` | leave the shell / close the tab |
| `history` | show recent history (most recent 1000 of up to 10000) |
| `jobs` | list background and stopped jobs |
| `fg [%n]` | bring a job to the foreground |
| `bg [%n]` | resume a stopped job in the background |
| `source file` | run a script in the current shell |
| `multiWatch 'cmd1' 'cmd2' ...` | run several commands periodically, side by side |

### Keyboard shortcuts

| Key | Action |
|---|---|
| Left / Right | move cursor |
| Home / End | move to start / end of input |
| Insert | toggle insert (thin cursor) / overwrite (block cursor) mode |
| Ctrl+A / Ctrl+E | start / end of line |
| Up / Down | history recall |
| Ctrl+U / Ctrl+K | delete to start / end of line |
| Ctrl+W | delete previous word |
| Ctrl+L | clear screen |
| Ctrl+D | exit (on empty line) |
| Ctrl+C | interrupt foreground command |
| Ctrl+Z | suspend foreground command |
| Tab | filename completion |
| Ctrl+R | search history |
| Shift+PageUp / Shift+PageDown | scroll back / forward |
| Ctrl+Shift+C / Ctrl+Shift+V | copy / paste |
| Ctrl+Shift+T | new tab |
| Ctrl+Shift+W | close tab |
| Ctrl+PageUp / Ctrl+PageDown | previous / next tab |

The application allows at most **16 open tabs**. Both the plus button and
Ctrl+Shift+T use the same app-level limit; an attempt to exceed it prints a
notice in the active tab. Each open tab keeps its ID until it closes. New tabs
reuse the lowest vacant ID and appear at the end of the strip. For example,
after tabs 1, 2, and 3 close while 4 and 5 remain, five new tabs appear with IDs
1, 2, 3, 6, and 7, after 4 and 5.

Bindings are configurable via `~/.terminatorrc` (created with defaults on first
run).

### `multiWatch` output format

```
"cmd", <unix_timestamp>:
----------------------------------------------------
<command output>
----------------------------------------------------
```

Each command's block is reprinted every interval (default 2 s). Press **Ctrl+C**
to stop.

---

## 5. Test

```sh
make test
```

This builds and runs the headless test suite (`tests/run_tests`), which links
only the GUI-free core modules and therefore needs **no X server**. Expected
output ends with:

```
=== 243 passed, 0 failed ===
```

The suite covers the lexer, parser, expansion, execution, redirection,
pipelines, history, completion, scripts, config, jobs (including Ctrl+Z stop
handling), `multiWatch` (output format and interrupt), and tab behaviour in
**both** thread and process modes.

---

## 6. Headless GUI testing (optional)

If you have no physical display, you can still run and drive the GUI with a
virtual X server.

```sh
# 1. Start a virtual display on :1
Xvfb :1 -screen 0 1024x768x24 -nolisten tcp &

# 2. Point the app at it and launch
export DISPLAY=:1
./terminator &                 # or: ./terminator useProcess=True &

# 3. Find the window and send keystrokes
WID=$(xdotool search --name "Terminator" | head -1)
xdotool windowfocus "$WID"
xdotool type --window "$WID" --delay 30 "echo hello"
xdotool key  --window "$WID" Return

# 4. Screenshot (optional)
scrot -o shot.png
```

> There is no window manager under bare `Xvfb`, so `xdotool windowactivate`
> warns; use `windowfocus` plus `type --window` / `key --window` instead.

---

## 7. Build from any project directory

Extract or copy the project to any directory on a Linux system with the listed
dependencies. From the project root (the directory containing `Makefile`):

```sh
make clean && make
make test
./terminator
./terminator useProcess=True
```

The application does not depend on the project being in a specific directory.
Its history and configuration files are stored in the current user's home
directory; relative file paths and the initial working directory come from the
directory where it is launched. For GUI checks without a desktop, install
`xvfb` and `xdotool`, start Xvfb, set `DISPLAY`, and follow the headless GUI
procedure above.

---

## 8. Verified results

The following were actually run and observed in the current workspace:

| Check | Command | Result |
|---|---|---|
| Clean build | `make clean && make` | 0 warnings, 0 errors |
| Test suite | `make test` | **243 passed, 0 failed** |
| Thread mode GUI | `./terminator` on Xvfb | window, command, and plus-button tab smoke passed |
| Process mode GUI | `./terminator useProcess=True` on Xvfb | window, command, and plus-button tab smoke passed |
| Ctrl+C recovery | `multiWatch` then Ctrl+C | shell responsive again (both modes) |
| Ctrl+Z suspend | `sleep 60` then Ctrl+Z | `jobs` shows `Stopped`, `bg` resumes (both modes) |
| Tabs | Ctrl+Shift+T / Ctrl+PageUp/Down / Ctrl+Shift+W | new / switch / close work |
| History | Up arrow, Ctrl+R | recall and search work |
| Clipboard | Ctrl+Shift+C / Ctrl+Shift+V | round-trip works |
| Completion | Tab | expands filenames |
| Exit | Ctrl+D | app exits cleanly |

---

## 9. Troubleshooting

See [`troubleshooting.md`](troubleshooting.md) for common problems (missing X11
headers, `XOpenDisplay` failure, permission errors, pipeline hangs, clipboard
issues, and more).
