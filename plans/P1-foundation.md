# P1: Foundation (0.0.x)

**Status:** in progress. 0.0.3, variables and parameter expansion, built on
2026-11-22 and waiting for Fred to try it. Next: 0.0.4, compound commands
and functions.

## Goal

A small, clean shell that runs simple scripts exactly as POSIX says, and
refuses everything it cannot run yet. It sets the module structure, the
memory model, the error model and the test harness that everything later
builds on.

## Versions

### 0.0.1: the skeleton

- Repository, license and documentation folders (`ideas/`, `arch/`,
  `plans/`, `bugs/`, `work/`), with the ideas from reading bash, dash,
  zsh, fish, mksh, Oils and busybox ash.
- Plain Makefile; `make test` and `make sanitize`.
- Modules: `arena`, `shell`, `signals`, `input`, `lex`, `parse`,
  `expand`, `exec`, `builtins`, `main`.
- The lexer recognizes every POSIX operator and quoting form, and flags
  each word with what it needs (`$`, backquotes, patterns, tilde, an
  assignment), so the parser can refuse what is not supported yet.
- The parser handles lists of simple commands. Reserved words, pipes,
  and-or lists, `&`, subshells and redirections are refused with
  "... is not supported yet; see ROADMAP.md".
- Standard input is read a byte at a time, so commands can read the
  lines after them. Script files are read in blocks, on a descriptor
  moved above 10 and closed on exec.
- PATH search in the child, with statuses 126 and 127. Files the kernel
  cannot run (ENOEXEC) run as scripts with tam-clam itself.
- An interactive prompt (`$ ` and `> `) without line editing. SIGINT
  cancels the line, and SIGQUIT and SIGTERM are ignored, as POSIX asks.

### 0.0.2: pipelines and redirections

- New nodes: pipeline (with `!`), and-or list, redirection list on a
  simple command.
- Every redirection operator: `<`, `>`, `>|`, `>>`, `<>`, `<&`, `>&`,
  with IO numbers (`2>file`) and `-` to close.
- Here-documents (`<<`, `<<-`) read after the line's newline, kept in
  memory, with a size cap. Quoted delimiters turn off expansion (moot
  until 0.0.3).
- Redirections on builtins save and restore descriptors, so a failure
  half-way leaves the shell's own descriptors as they were.
- The pipeline status is the last command's; `pipefail` waits for 0.1.3.

### 0.0.3: variables and parameter expansion

- A variable table (`src/var.c`): shell variables, exported variables,
  read-only flags, imported from `environ` at start-up. Names are checked;
  the environment is rebuilt for each `execve` only when it changed.
- Assignments before a command, and assignment-only commands.
- Words become a list of parts (literal, quoted literal, parameter) in
  the parser instead of raw text, so the expander never re-reads quotes
  and no in-band control bytes are needed (see `ideas/dash.md`).
- Parameter expansion: `$name`, `${name}`, the `-`, `=`, `?`, `+` forms
  with and without `:`, `${#name}`, the `%`, `%%`, `#`, `##` forms (with
  plain-string patterns until 0.1.0), special parameters `$? $$ $! $# $@
  $* $- $0` and positional parameters.
- `PS1` and `PS2` become variables. Tilde expansion. Logical `cd`
  (`-L`, the POSIX default) with `PWD`, `OLDPWD` and `CDPATH`, and `-P`.

### 0.0.4: compound commands and functions

- `if`, `while`, `until`, `for`, `case` (plain-string patterns until
  0.1.0), `{ ...; }`, `( ... )` subshells.
- Functions, with a recursion limit (default 1000, settable) that stops
  runaway recursion with an error instead of a crash.
- `break`, `continue` and `return`.
- Execution walks the tree with an explicit depth count; the parser has a
  nesting limit, so deeply nested input is an error, never a stack
  overflow.

## Acceptance

- `make clean && make`: no warnings with `-Wall -Wextra -Wpedantic
  -Wshadow -Wvla`.
- `make test` and `make sanitize` pass. Every script test gives the same
  output and status under tam-clam and the reference shell.
- `readelf -d build/tam-clam` lists only `libc.so.6` as NEEDED.
- Each new feature adds tests to `tests/scripts/`, and each refusal it
  replaces moves from `tests/refuse/` to `tests/scripts/`.
- Fred tries each version from a terminal. Agents run the exhaustive
  checks; Fred's checks stay short.

## Dependencies

- Uses: the C library, libtam (static: `tam_str` today).
- None added in this phase.

## Risks

- **Parser structure chosen too early.** The 0.0.3 change from raw text
  to word parts touches the lexer, parser and expander. It is planned for
  that version, while the code is small.
- **Behaviour that differs between shells.** Where POSIX leaves a choice,
  we follow dash and bash --posix when they agree, and record the choice
  in `arch/` when they do not.

## Decisions

- 2026-11-17, Fred: yes to a lint mode that warns about constructs that
  are legal but almost always mistakes. It is planned in P2 (0.1.5),
  once the whole language parses.

## Open points

- None.
