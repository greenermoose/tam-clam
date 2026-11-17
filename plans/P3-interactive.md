# P3: Interactive (0.2.x)

**Status:** not started.

## Goal

A shell that is a pleasure to type in, with fish's helpfulness and
bash's keys, written as our own line editor on libtam, and with a history
that can never be lost or corrupted.

## Versions

- **0.2.0** The line editor (`src/edit/`): raw mode through `tam_tty`
  (restored on every exit path, including signals), keys through
  `tam_key`, display widths through `tam_uni`. tam-clam's own key
  bindings (below); multi-line editing; bracketed paste;
  redraw on SIGWINCH. No terminfo: ANSI and xterm sequences only, as
  libtam does. On the Linux console (`TERM=linux`) it uses only what the
  console supports.
- **0.2.1** History (`src/history/`): one record per command with a
  timestamp, exit status and working directory, appended with
  `O_APPEND` in single writes so several shells can share the file
  without locking or losing lines. A reader that skips a damaged record
  instead of failing. Ctrl+R incremental search, and Up/Down that search
  by prefix (as fish and zsh do). A private mode that writes no history.
- **0.2.2** Job control: process groups, `tcsetpgrp`, Ctrl+Z, `fg`, `bg`,
  `jobs`, `%n` job IDs, notifications of finished jobs before the prompt.
- **0.2.3** Completion of command names, files, variables and user names,
  and a small declarative format for command options (one file per
  command, like fish's `share/completions/`), loaded lazily.
- **0.2.4** Syntax highlighting as you type, from the real parser:
  unknown commands, unterminated quotes and syntax errors are shown
  before Enter. Suggestions from history shown in a dim colour and
  accepted with Right.
- **0.2.5** Prompts: `PS1` with escapes, a right prompt, and hooks before
  each prompt and each command (zsh's `precmd` and `preexec`).

## Key bindings

Decided by Fred on 2026-11-17: tam-clam has its own set of key bindings,
more ergonomic and mnemonic than readline's. There is no vi mode, and
readline's emacs bindings are not the model.

- Before 0.2.0 code is written, the key map is drafted in
  `arch/keys.md` and agreed with Fred.
- Principles for the draft: the letter names the action where it can;
  the most frequent actions get the easiest keys; keys that terminals
  and the Linux console deliver unreliably are avoided; and every action
  has a name (as zsh's widgets do), so a user can rebind it.
- Keys always name the physical key (Ctrl, Alt, Meta, a letter), never a
  placeholder.

## Acceptance

- Interactive tests in C over a pseudo-terminal (`tests/pty/`), so they
  need no Python or expect.
- History survives: two shells writing at once, a shell killed with
  SIGKILL while writing, a full disk, a truncated file.
- Fred uses it on a terminal and on the Linux console.

## Dependencies

- libtam `tam_tty`, `tam_key`, `tam_uni`. No readline, no ncurses, no
  terminfo.

## Risks

- **Highlighting cost.** Checking whether a command exists on every key
  press costs `stat` calls. Results are cached per prompt, and the check
  is skipped on slow file systems.

## Open points

- The key map itself (`arch/keys.md`), to agree with Fred before 0.2.0.
- Is there also an optional map with readline's bindings for people
  arriving from bash, or only ours?
- Fred's energy rule: no timers or background polling while the shell
  waits for input. Does that rule out showing a clock in the prompt?
