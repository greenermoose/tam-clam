# Oils (OSH and YSH)

## What it is

- **Language:** Python, translated to C++ by its own `mycpp` for the
  release build. **License:** Apache 2.0.
- **Scope:** two shells sharing one runtime: OSH, which aims to run bash
  scripts, and YSH, a new language. The project's documentation of how
  shells really behave is unmatched.

## Ideas worth taking

- **Spec tests run against every shell** (`spec/*.test.sh`, 229 files).
  Each case has a script and its expected output, and notes where other
  shells differ:

      ## compare_shells: dash bash mksh zsh ash

      #### cd with 2 or more args is allowed (strict_arg_parse disabled)
      mkdir -p foo
      cd foo bar
      ## status: 0
      ## OK bash/zsh status: 1

  `BUG`, `OK` and `N-I` (not implemented) mark the differences. adopt (P2,
  0.1.4): our spec suite uses the same idea, so we learn what each shell
  does, not just what we do.
- **Strict options** (`frontend/option_def.py`), each fixing one known
  trap without changing valid programs:
  - `strict_errexit`: errexit can never be silently disabled; code that
    POSIX would run with it disabled is an error instead;
  - `command_sub_errexit`: a failing `$(...)` fails the command;
  - `strict_argv`: an empty command name is an error;
  - `strict_arith`: `x=foo; $(( x ))` is an error, not 0;
  - `strict_arg_parse`: extra arguments are errors (`cd a b`);
  - `strict_tilde`: `~nosuchuser` is an error;
  - `simple_word_eval`: unquoted `$x` is never split or globbed;
  - `sigpipe_status_ok`: a reader that stops early does not fail a
    `pipefail` pipeline.

  adopt (P4, 0.3.3), with Oils' names, so users can read Oils'
  documentation for ours.
- **The error-handling analysis** (`doc/error-handling.md`): why `$?`,
  `if` and errexit interact badly, and what fixes each problem. adopt as
  the reference for P2 0.1.3 and P4 0.3.3.
- **An error catalog** (`doc/error-catalog.md`): every error message,
  with an example that triggers it. adopt (P2): each tam-clam message
  gets an entry and a test.
- **"Known differences" written down** (`doc/known-differences.md`).
  adopt: our `arch/` will hold the same for tam-clam.
- **Errors with exact locations:** every token knows its line and column,
  and messages underline it. adopt (P2).
- **A syntax tree that keeps everything** (comments, spacing), so tools
  like formatters can be built on the parser. consider (P2), with fish's
  `fish_indent`.

## Things to avoid

- **A second language.** YSH is well designed, but Fred chose a POSIX
  core with opt-in strict modes, not two languages.
- **A Python codebase translated to C++.** Its tooling (`mycpp`, a
  vendored Python 2.7) is far from "the C library and libtam".

## Where to look

- `spec/` (start with `spec/errexit*.test.sh`, `spec/word-split.test.sh`,
  `spec/builtin-cd.test.sh`), `frontend/option_def.py`,
  `doc/error-handling.md`, `doc/error-catalog.md`,
  `doc/known-differences.md`, `doc/warts.md`, `osh/word_parse.py`,
  `osh/cmd_parse.py`.
