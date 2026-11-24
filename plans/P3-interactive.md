# P3: Interactive (0.2.x)

**Status:** complete (0.2.0–0.2.5 landed 2026-11-24).

## Goal

A shell that is a pleasure to type in, with fish's helpfulness and
Tamlinux's own ergonomic keybindings, written as our own line editor on
libtam, and with a history that can never be lost or corrupted.

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
  instead of failing. Ctrl+F incremental search, and Up/Down that search
  by prefix (as fish and zsh do). A private mode that writes no history.
- **0.2.2** Job control: process groups, `tcsetpgrp`, Ctrl+P (`VSUSP`),
  Ctrl+\ (`VINTR`), `fg`, `bg`, `jobs`, `%n` job IDs, notifications of
  finished jobs before the prompt.
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

Decided by Fred on 2026-11-23: the Tamlinux-wide keybinding standard
shared across all Tamlinux programs (compositor, terminal, shell, editor,
file manager, pager) is defined in `tamlinux/arch/keybindings.md`.
`tam-clam`'s `arch/keys.md` follows `tamlinux/arch/keybindings.md` and
defines shell-specific bindings on top of it.

- Before 0.2.0 code is written, `tamlinux/arch/keybindings.md` is agreed
  with Fred and `arch/keys.md` is drafted against it. (Drafted 2026-11-23.)
- Principles: no double-acting `Ctrl` codes; three-physical-key ceiling;
  the letter names the action where it can; keys that terminals and the
  Linux console deliver unreliably are avoided or paired with `Alt`
  fallbacks; and every action has a canonical widget name so users can
  search/run it (`Ctrl+E`) or rebind it.
- Keys always name the physical key (`Meta`, `Ctrl`, `Alt`, `Shift`, a
  letter, or a named key), never a placeholder.
- Foreground child signals use `termios` `VINTR` = `Ctrl+\` (Break, with
  `VQUIT` disabled) and `VSUSP` = `Ctrl+P` (Pause), leaving `Ctrl+C`
  (Copy) and `Ctrl+Z` (Undo) free everywhere. The four legacy ASCII
  collisions (`Ctrl+H`, `Ctrl+I`, `Ctrl+J`/`M`, `Ctrl+[`) are never
  bound separately.
- Decided by Fred on 2026-11-17: no readline-style key map is built in.
  tam-clam ships only its own map. The binding format must be able to
  express readline's keys, so that if users ask for them later, a
  readline map can be a small optional file rather than a built-in
  setting.

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

- Settled (2026-11-23): `tamlinux/arch/keybindings.md` and `arch/keys.md`.
- Settled (2026-11-24): Fred's energy rule forbids timers/polling while
  idle; clock escapes (`\t`, `\T`, `\A`) evaluate once when the prompt is
  drawn, never ticking in the background.
