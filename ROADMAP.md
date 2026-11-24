# tam-clam roadmap

Each milestone has a version number. A phase (see `plans/`) covers a range
of versions. Versions follow semantic versioning: during 0.x, the minor
number marks a phase's big step and the patch number marks each
milestone inside it.

**Current version:** 0.2.5 (Phase P3 complete: safe `PS1`/`PS2`/`RPS1`/`PS0`
prompt escapes, right-aligned `RPS1`, and `precmd`/`PROMPT_COMMAND` +
`preexec`/`PS0` hooks, 2026-11-24; development continues on branch `dev`).

## Decided

Fred decided these on 2026-11-17:

- The language is a POSIX sh core that runs any POSIX script exactly,
  plus the bash and zsh features people use (arrays, `[[ ]]`, `local`,
  history, completion). Safer modes are opt-in. tam-clam can become
  `/bin/sh` one day.
- The only dependencies are the C library and libtam.
- tam-clam has a lint mode that warns about legal but risky code.
- The language target is POSIX.1-2024.
- The line editor has tam-clam's own key bindings, designed to be
  ergonomic and mnemonic. There is no vi mode, and readline's bindings are
  not the model.

The versions below are a proposal. Fred has not yet set when tam-clam
enters a Tamlinux release.

## Milestones

### Phase P1: foundation (0.0.x), `plans/P1-foundation.md`

- **0.0.1** The skeleton: simple commands, quoting, `;` and newline lists,
  comments, PATH search, exit statuses, the builtins `exit`, `cd`, `:`,
  `true` and `false`, and `-c`, script and standard-input modes.
  Everything else is refused with a clear message.
- **0.0.2** Pipelines, `&&` and `||`, `!`, and redirections, including
  here-documents.
- **0.0.3** Shell variables and the environment, assignments, parameter
  expansion (`$x`, `${x:-y}` and the other POSIX forms), special
  parameters, positional parameters, tilde expansion, and logical `cd`.
- **0.0.4** Compound commands (`if`, `while`, `until`, `for`, `case`,
  `{ }`, subshells) and functions.

### Phase P2: the POSIX language (0.1.x), `plans/P2-posix-language.md`

- **0.1.0** Command substitution, `$'...'` strings, arithmetic expansion,
  field splitting and pathname expansion (globbing), with our own pattern
  matcher.
- **0.1.1** Background commands, `wait`, `trap`, and the special builtins
  (`set`, `shift`, `eval`, `.`, `export`, `readonly`, `unset`, `exec`,
  `return`, `break`, `continue`, `times`).
- **0.1.2** The regular builtins POSIX lists, including `read`,
  `printf`, `echo`, `test`, `getopts`, `command`, `type`, `umask`,
  `alias`.
- **0.1.3** `set -e`, `set -u`, `set -o pipefail`, `set -x` with line
  numbers.
- **0.1.4** Passes our POSIX spec tests, run against dash, bash --posix,
  mksh and busybox ash, and survives a fuzzing campaign on the parser
  and expander.
- **0.1.5** Lint mode: `tam-clam --lint script` parses without running
  and warns about legal but risky code, such as an unquoted `$x` inside
  `[ ]` or a `cd` whose failure is not checked.

### Phase P3: interactive (0.2.x), `plans/P3-interactive.md`

- **0.2.0** Our own line editor on libtam's `tam_tty`, `tam_key` and
  `tam_uni`: our own ergonomic, mnemonic key bindings, UTF-8, wide
  characters, bracketed paste.
- **0.2.1** History: an append-only file with timestamps, safe for several
  shells at once, and incremental search.
- **0.2.2** Job control: `fg`, `bg`, `jobs`, Ctrl+P (`VSUSP`), Ctrl+\\
  (`VINTR`), process groups.
- **0.2.3** Completion: commands, files, variables, and a small
  completion language for options.
- **0.2.4** Syntax highlighting while typing and suggestions from
  history.
- **0.2.5** Prompts: `PS1` escapes, a right prompt, and hooks that run
  before each prompt and each command.

### Phase P4: the extras (0.3.x), `plans/P4-extras.md`

- **0.3.0** Indexed and associative arrays.
- **0.3.1** `[[ ]]`, `local`, brace expansion.
- **0.3.2** Process substitution, `**` recursive globbing.
- **0.3.3** The strict modes: errexit that is never silently disabled,
  failing command substitutions, no implicit word splitting.
- **0.3.4** Abbreviations and completions generated from man pages.

### Phase P5: dogfood (0.4.x), `plans/P5-dogfood.md`

- **0.4.0** Packaged in `tamlinux-packages`. Fred tries tam-clam as his
  interactive shell, with bash kept as the login shell until he decides
  otherwise.
- **0.4.x** Fixes from daily use, recorded in `bugs/`.

### Phase P6: /bin/sh (1.0.0), `plans/P6-bin-sh.md`

- **1.0.0** tam-clam is fit to be a system's `/bin/sh`: it runs the
  system's own scripts and passes the full spec suite. Whether Tamlinux
  makes it `/bin/sh`, and when, is Fred's decision, through his Test, Run
  and Release words.

## How a milestone is done

A milestone is done when its acceptance checks in the phase plan pass and
Fred has tried the build. When a milestone lands, bump `VERSION` in the
same change and update the current version here.
