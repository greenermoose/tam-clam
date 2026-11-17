# fish

## What it is

- **Language:** Rust since fish 4.0 (it was C++). **License:** GPL v2
  only for most of it, so we take ideas from fish, never code (see
  `UPSTREAM.md`).
- **Scope:** the "friendly interactive shell". Not POSIX: its own,
  cleaner language. Its interactive design is the best of any shell.

## Ideas worth taking

- **Autosuggestions:** as you type, the most likely completion from
  history is shown in a dim colour; Right accepts it (`src/reader/`,
  `autosuggest_validate_from_history`). Suggestions are only offered when
  they would still work, for example when a path they name exists.
  adopt (P3, 0.2.4).
- **Syntax highlighting as you type, from the real parser**
  (`src/highlight/highlight.rs`, `highlight_shell`): unknown commands in
  red before you press Enter, paths that exist underlined
  (`file_tester.rs`). adopt (P3, 0.2.4); our highlighter runs our own
  parser, so it can never disagree with what will run.
- **Errors that show the line with a caret** under the problem
  (`ParseError::describe`, `src/parse_constants.rs`). adopt (P2).
- **Abbreviations** (`src/abbrs.rs`): `gco` expands to `git checkout` in
  the line as you type space, so history shows the real command. adopt
  (P4, 0.3.4); better than aliases for interactive use.
- **Completions generated from man pages**
  (`share/tools/create_manpage_completions.py`), plus about 1,100
  hand-written completion files in `share/completions/`, one per
  command, loaded only when needed. adopt the design (P3, 0.2.3 and P4,
  0.3.4).
- **Up and Down search history by what is already typed.** adopt (P3,
  0.2.1).
- **`$status` and `$pipestatus`.** Every command's status in a pipeline.
  adopt `PIPESTATUS` with bash's name (P4).
- **A private mode** (`fish --private`) that keeps no history. adopt (P3,
  0.2.1).
- **`fish_indent`**, a formatter for scripts (`src/bin/fish_indent.rs`).
  consider (P2, from our parser; it needs the parser to keep comments
  and layout).
- **Tests as scripts with expected output in comments**
  (`tests/checks/*.fish` with `# CHECK:` lines, run by `littlecheck.py`),
  and interactive tests over a pseudo-terminal (`tests/pexpects/`).
  adopt both ideas; our pseudo-terminal tests are in C (P3).
- **Sensible defaults with no configuration file.** A new user gets
  colours, completions and suggestions on first start. adopt (P3).
- **`posix_spawn` where it can** (`src/fork_exec/spawn.rs`). consider
  (P2), with dash's `vfork` idea.

## Things to avoid

- **A non-POSIX language.** fish cannot run sh scripts, so it is never
  `/bin/sh`. Fred chose a POSIX core.
- **Universal variables** shared live between all sessions through a
  file (`env_universal_common.rs`). Useful, but it needs file watching
  and a background notifier. reject for now, on energy and complexity.
- **Background threads in the shell** (`src/threads/`, `src/reader/iothreads.rs`)
  for highlighting and completion. They make signals and `fork` harder.
  Our checks are cached and bounded instead.
- **A web configuration tool** (`fish_config`, `share/tools/web_config/`).
  reject; it needs Python and a browser.

## Where to look

- `src/reader/reader.rs`, `src/highlight/`, `src/history/`,
  `src/abbrs.rs`, `src/parse_constants.rs`, `share/completions/`,
  `share/tools/create_manpage_completions.py`, `tests/checks/`,
  `tests/pexpects/`.
