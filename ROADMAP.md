# tam-clam roadmap

`tam-clam` is Tamlinux's POSIX.1-2024 shell with interactive line editing,
written in C17 on top of `libtam`.

- **Current version:** `0.3.10` (branch `dev`)
- **Progress:** 4 of 6 phases done (P1–P4 done; P5 next)
- **Versioning:** Semantic versioning; during 0.x, minor marks a phase and
  patch marks milestones or fixes within it.

## Phase summary

| Phase | Versions | Goal | Plan | Status |
| --- | --- | --- | --- | --- |
| **P1** | 0.0.x | Foundation: simple commands, pipelines, expansions, compound commands, functions | [P1-foundation.md](plans/P1-foundation.md) | Done |
| **P2** | 0.1.x | Full POSIX.1-2024 language, builtins, spec tests, fuzzing, and `--lint` mode | [P2-posix-language.md](plans/P2-posix-language.md) | Done |
| **P3** | 0.2.x | Interactive line editor, history, job control, completion, highlighting, prompts | [P3-interactive.md](plans/P3-interactive.md) | Done (2026-11-24) |
| **P4** | 0.3.x | Arrays, `[[ ]]`, strict modes, abbreviations, rc/help, and `libtam` integration | [P4-extras.md](plans/P4-extras.md) | Done (2026-11-26) |
| **P5** | 0.4.x | Dogfooding as Fred's interactive shell with daily-use bug fixes | [P5-dogfood.md](plans/P5-dogfood.md) | Planned |
| **P6** | 1.0.0 | Fit to serve as `/bin/sh` (system scripts and full spec suite) | [P6-bin-sh.md](plans/P6-bin-sh.md) | Planned |

## Decided

Fred decided these on 2026-11-17:

- The language is a POSIX sh core that runs any POSIX script exactly, plus
  the bash and zsh features people use (arrays, `[[ ]]`, `local`, history,
  completion). Safer modes are opt-in. `tam-clam` can become `/bin/sh` one
  day.
- The only dependencies are the C library and `libtam`.
- `tam-clam` has a lint mode that warns about legal but risky code.
- The language target is POSIX.1-2024.
- The line editor has `tam-clam`'s own ergonomic, mnemonic key bindings
  (no vi mode; readline is not the model).

## Phases and milestones

### Phase P1: Foundation (0.0.x) — [P1-foundation.md](plans/P1-foundation.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.0.1** | Skeleton: simple commands, quoting, `;`/newline lists, comments, PATH search, exit statuses, core builtins, `-c`/script/stdin | Done |
| **0.0.2** | Pipelines, `&&`/`||`, `!`, and redirections including here-documents | Done |
| **0.0.3** | Shell variables, environment, parameter expansion, special/positional parameters, tilde expansion, logical `cd` | Done |
| **0.0.4** | Compound commands (`if`, `while`, `until`, `for`, `case`, `{ }`, subshells) and functions | Done |

### Phase P2: The POSIX language (0.1.x) — [P2-posix-language.md](plans/P2-posix-language.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.1.0** | Command substitution, `$'...'` strings, arithmetic expansion, field splitting, and globbing | Done |
| **0.1.1** | Background commands, `wait`, `trap`, and POSIX special builtins | Done |
| **0.1.2** | POSIX regular builtins (`read`, `printf`, `echo`, `test`, `getopts`, `command`, `type`, `umask`, `alias`) | Done |
| **0.1.3** | `set -e`, `set -u`, `set -o pipefail`, and `set -x` with line numbers | Done |
| **0.1.4** | POSIX spec tests (against dash, bash --posix, mksh, ash) and parser/expander fuzzing | Done |
| **0.1.5** | Lint mode (`tam-clam --lint script`) warning on legal but risky shell code | Done |

### Phase P3: Interactive (0.2.x) — [P3-interactive.md](plans/P3-interactive.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.2.0** | Line editor on `libtam` (`tam_tty`, `tam_key`, `tam_uni`): UTF-8, wide chars, bracketed paste | Done |
| **0.2.1** | Append-only shared history with timestamps, prefix search, `Ctrl+F` search, `Alt+L`, `--private` | Done (2026-11-24) |
| **0.2.2** | POSIX job control (`fg`, `bg`, `jobs`, `Ctrl+P`/`Ctrl+\`, process groups, `%n`) | Done (2026-11-24) |
| **0.2.3** | `Tab`/`Shift+Tab` completion for commands, files, variables, users, and `.comp` files | Done (2026-11-24) |
| **0.2.4** | Live syntax highlighting from real parser and dimmed history suggestions | Done (2026-11-24) |
| **0.2.5** | Prompts: `PS1` escapes, right prompt, and `precmd`/`preexec` hooks | Done (2026-11-24) |

### Phase P4: The extras (0.3.x) — [P4-extras.md](plans/P4-extras.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.3.0** | Indexed and associative arrays | Done (2026-11-24) |
| **0.3.1** | `[[ ]]`, linear-time POSIX ERE `=~`, `local`/`declare`, and brace expansion | Done (2026-11-24) |
| **0.3.2** | Process substitution `<(...)`/`>(...)` and `**` recursive globbing | Done (2026-11-24) |
| **0.3.3** | Strict modes (Oils-named strict options and `set -o strict`) | Done (2026-11-24) |
| **0.3.4** | Fish-style abbreviations (`abbr`) and man-page completion generator (`tools/tam-clam-gen-comp`) | Done (2026-11-24) |
| **0.3.5** | Add `-v` as synonym for `--version` | Done (2026-11-24) |
| **0.3.6** | One-handed chord composer, command menu, and deepest-two-level cwd in default `PS1` | Done (2026-11-25) |
| **0.3.7** | Interactive startup configuration loading and guidance | Done (2026-11-26) |
| **0.3.8** | Load interactive run commands from `~/.rc` | Done (2026-11-26) |
| **0.3.9** | Add `help` builtin with usage guide and keybindings | Done (2026-11-26) |
| **0.3.10** | Move line editor onto `libtam`; fix history, pipes, and temp file handling | Done (2026-11-26) |

### Phase P5: Dogfood (0.4.x) — [P5-dogfood.md](plans/P5-dogfood.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.4.0** | Packaged for Tamlinux; Fred tries `tam-clam` as his interactive shell (bash stays login shell) | Planned |
| **0.4.x** | Fixes from daily use, recorded in `bugs/` | Planned |

### Phase P6: `/bin/sh` (1.0.0) — [P6-bin-sh.md](plans/P6-bin-sh.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **1.0.0** | Fit to serve as `/bin/sh`: runs system scripts and passes full spec suite (adoption only on Fred's word) | Planned |

## How a milestone is done

A milestone is done when its acceptance checks in the phase plan pass and
Fred has tried the build. When a milestone lands, bump `VERSION`, update
the phase plan's status, and update this roadmap in the same change.
