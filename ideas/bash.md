# bash

## What it is

- **Language:** C, about 83,000 lines with readline. **License:** GPL v3
  or later.
- **Scope:** the GNU shell, the de facto standard interactive shell on
  Linux, and `/bin/sh` on Arch. `bash --posix` is one of our two
  reference shells.
- The full evaluation is `arch/bash-evaluation.md`.

## Ideas worth taking

- **Its messages and statuses.** "line N: name: command not found", 126
  and 127. adopt (P1, 0.0.1).
- **The extras people use:** arrays and associative arrays, `[[ ]]`,
  `local`, process substitution, brace expansion, `+=`. adopt (P4).
- **`pipefail`** and **`$'...'`**, both now in POSIX. adopt (P2).
- **`set -x` that says where:** `PS4` with `$LINENO`, `BASH_XTRACEFD` to
  send the trace elsewhere. adopt (P2, 0.1.3).
- **`inherit_errexit`, `lastpipe`, `nullglob`, `failglob`.** Options that
  fix real problems. consider (P4).
- **`${ command; }` and `${| command; }`** (bash 5.3): command
  substitution without a fork. consider (P4).
- **`printf %q` and `${var@Q}`:** quote a value so it can be read back as
  shell input. adopt (P2, `printf`), it makes safe code generation easy.
- **`$EPOCHSECONDS`, `$EPOCHREALTIME`, `$SRANDOM`.** Cheap, useful
  variables. consider (P4).
- **readline's keys** (emacs mode defaults, `~/.inputrc` names). reject
  as the model: Fred decided on 2026-11-17 that tam-clam has its own,
  more ergonomic and mnemonic set (see `plans/P3-interactive.md`).
  readline's names for actions are still a useful checklist of what an
  editor needs.
- **`complete` and `compgen`** so bash-completion scripts work. consider
  (P3); a large interface, but thousands of completions come with it.
- **`PROMPT_COMMAND`** and **`PS0`** (printed before a command runs).
  adopt (P3, 0.2.5) alongside zsh's hooks.

## Things to avoid

- **The yacc grammar** and its 28 lexer state flags. Hand-written
  recursive descent instead.
- **Parsing command substitutions twice.**
- **Control bytes in words** (`CTLESC`, `CTLNUL`).
- **Function import from the environment** (Shellshock, 2014).
- **`longjmp` error handling and pervasive global state.**
- **`echo` with options and escapes that depend on `xpg_echo`.** Our
  `echo` follows POSIX and dash; `printf` is the portable answer.
- **Silent errexit suppression** inside functions called from `if`, `&&`
  and `||`. POSIX requires it, so it stays in POSIX mode, but the strict
  mode makes it an error (see `oils.md`).

## Where to look

- `eval.c` (`reader_loop`), `parse.y` (`read_token`, `parse_comsub`),
  `subst.c` (`expand_word_internal`, `param_expand`, `word_split`),
  `execute_cmd.c` (`execute_simple_command`, `shell_execve`), `jobs.c`
  (`make_child`, `wait_for`), `variables.c`, `builtins/*.def`.
- `POSIX` in the source tree lists what `--posix` changes.
- `tests/` has bash's own regression tests (`run-*` scripts with `.right`
  files).
