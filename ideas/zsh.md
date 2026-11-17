# zsh

## What it is

- **Language:** C. **License:** zsh's own MIT-style licence, with some
  shipped shell functions under the GPL.
- **Scope:** the Z shell, the most feature-rich interactive shell.
  Default on macOS. Its line editor is ZLE (`Src/Zle/`), and its
  completion system lives in shell functions under `Completion/`.

## Ideas worth taking

- **`emulate sh`, `emulate ksh`.** One shell that changes its rules for
  compatibility, and `emulate -L` to do it for one function only. adopt
  the idea for our strict modes: `set -o strict` applies to the current
  function only when set with `local -` (P4).
- **Glob qualifiers** (`*(.)` regular files, `*(/)` directories,
  `*(om[1,3])` the three newest) and **`**/` recursive globbing**
  (`Src/glob.c`). consider qualifiers (P4); adopt `**` (P4, 0.3.2).
- **A right prompt** (`RPROMPT`, `Src/Zle/zle_refresh.c`). adopt (P3,
  0.2.5).
- **`precmd` and `preexec` hooks.** Run code before each prompt and
  before each command. adopt (P3, 0.2.5).
- **Shared history** (`SHARE_HISTORY`, `INC_APPEND_HISTORY`, extended
  history with timestamps and durations, `Src/hist.c`). Commands from
  every open shell are available in every other. adopt the result (P3,
  0.2.1), with single `O_APPEND` writes instead of zsh's lock file.
- **Spelling correction** (`CORRECT`: "correct 'gti' to 'git'?").
  consider (P3); fish's highlighting may make it unnecessary.
- **`auto_cd`, `auto_pushd`, `cd -<TAB>` through the directory stack.**
  consider (P3).
- **ZLE widgets:** every editor action is a named function, and users can
  bind keys to their own shell functions. adopt (P3, 0.2.0); our editor
  actions get names from the start.
- **Asking the terminal what it supports** (`Src/Zle/termquery.c`,
  2025), instead of trusting terminfo. adopt (P3); it matches libtam's
  no-terminfo design.
- **`${| ... }` and `${ ... }` no-fork substitution** (`Test/D10nofork.ztst`).
  consider (P4), with bash 5.3's identical syntax.
- **A test suite by topic** (`Test/A01grammar.ztst` ...) with a small
  format for input and expected output. adopt the idea (P2, spec suite).

## Things to avoid

- **Arrays that start at 1** and **no word splitting of `$var` by
  default.** Both break POSIX and bash scripts; zsh only behaves in
  `emulate sh`. Our default is POSIX; no splitting is an opt-in strict
  mode.
- **Parameter expansion flags** (`${(f)var}`, `${(s.:.)PATH}`) and other
  dense syntax nobody can read later.
- **Loadable modules** (`zmodload`, `Src/Modules/`). Code loaded at run
  time can break when the shell is updated, and a shell must always
  start.
- **The completion system's size and configuration by `zstyle`.**
  Powerful but famously hard to understand. Ours is declarative and small
  (P3, 0.2.3).
- **`zcompile` wordcode files.** A cache of parsed code adds a second
  format to keep correct; parsing is fast enough.

## Where to look

- `Src/hist.c`, `Src/glob.c`, `Src/Zle/zle_main.c`,
  `Src/Zle/zle_refresh.c`, `Src/Zle/termquery.c`, `Src/builtin.c`
  (`bin_emulate`), `Test/`.
