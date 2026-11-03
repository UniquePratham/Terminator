# Terminator task demonstrations

Use these short demonstrations in the X11 window. The shell understands the
project's documented subset; it does not invoke `/bin/bash` to parse commands
or scripts. Open a new tab with the **+** button or `Ctrl+Shift+T` where noted.

## Task 1 — X11 GUI and tabs

1. Type `echo first-tab` and press Enter.
2. Click **+**, type `echo second-tab`, and press Enter.
3. Click tab 1, then tab 2. Each tab keeps its own input and output; tab 1's
   output continues to update while tab 2 is visible.
4. To demonstrate stable labels and vacant ID reuse: create tabs until IDs 1–5
   are open, close tabs 1, 2, and 3, then create five more. The strip stays in
   creation order and shows `4, 5, 1, 2, 3, 6, 7`; the existing tabs keep IDs 4
   and 5, and each new tab gets the lowest free ID. The application allows at
   most 16 open tabs.

## Task 2 — Built-in and external commands

```text
cd .
pwd
false
echo $?
terminator_command_that_does_not_exist
```

`cd` is a shell built-in; `pwd` and `false` are external programs. `$?` prints
the last exit status, and the unknown command reports an error while leaving
the tab usable.

## Task 3 — Multiline input, quoting, and substitution

Type `echo "first line` and press Enter. At the `>` continuation prompt, type
`second line"` and press Enter. Then try:

```text
echo '$HOME' "$HOME"
echo "folder=$(basename $(pwd))"
```

Single quotes preserve `$HOME`; double quotes expand it. Nested command
substitutions run from the inside out. A trailing backslash can also continue
a command onto the next line.

## Task 4 — Input and output redirection

```text
printf 'pear\napple\n' > /tmp/terminator-input.txt
sort < /tmp/terminator-input.txt > /tmp/terminator-output.txt
cat < /tmp/terminator-output.txt
```

This writes a fixture, reads it through stdin, redirects stdout to a second
file, then displays the result. A missing input path should report an error
without running the command.

## Task 5 — Pipelines

```text
printf 'pear\napple\npear\n' | sort | uniq -c
```

Three child commands are connected with two pipes.

## Task 6 — `multiWatch`

```text
multiWatch 'date +%T' 'ls | wc -l'
```

The commands run concurrently and print timestamped output blocks again at the
configured interval. Press **Ctrl+C**; the shell prompt should return.

## Task 7 — User-configurable hotkeys

Edit `~/.terminatorrc` in a text editor outside Terminator and add:

```text
bind clear-screen = Ctrl+Shift+L
```

Restart Terminator and press **Ctrl+Shift+L**. The change persists across
restarts. Invalid key names and conflicting assignments are reported and keep
the default binding. **Home** and **End** also move to the start and end of the
current input line.

## Task 8 — Signals and jobs

1. Run `sleep 30`.
2. Press **Ctrl+Z**, then run `jobs`.
3. Run `bg %1`, then `jobs` again.
4. Run `fg %1` and press **Ctrl+C**.

The foreground job stops, resumes in the background, then is interrupted when
brought back to the foreground. Terminator remains open.

## Task 9 — Shared searchable history

Run these in one tab, then press **Ctrl+R** in another tab and search for
`terminator-history-demo`:

```text
echo terminator-history-demo first
echo terminator-history-demo second
```

The search prints every command tied for the best substring score; an exact
match takes priority. The `history` built-in displays recent commands.

## Task 10 — Filename completion

Create two files, then start typing a matching path and press Tab:

```text
touch terminator-complete-a.txt terminator-complete-b.txt
cat terminator-complete-
```

The menu lists the ambiguous candidates. Press **1** to insert the first
alphabetical match, then press Enter. Text to the right of the cursor is kept
when completion replaces a word.

## Task 11 — The project's script interpreter

Create a small script and run it using the supported `bash script.sh` spelling:

```text
printf 'if [ -f "$1" ]; then\necho file_exists\nelse\necho missing\nfi\n' > check.sh
bash check.sh /etc/hosts
```

The script runs in Terminator's own interpreter. `source check.sh` runs a
script in the current shell context so assignments and `cd` can persist.

## Two execution modes

Start the same demonstrations once with `./terminator` (thread mode) and once
with `./terminator useProcess=True` (process mode). Both launch modes have the
same command interface and share command history between their tabs.

## Similar regression checks

Run `make test` for the matching automated checks, including both tab worker
modes. The suite also covers a nested quoted substitution, a five-stage
pipeline, missing-file redirection, tied history results, process-shared history
from a child, stop/resume handling, script `-f`/`-d`/`-eq`/`-gt`/`=` conditions,
ambiguous completion selection, and preservation of text after the cursor.
