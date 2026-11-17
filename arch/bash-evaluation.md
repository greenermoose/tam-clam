# Bash evaluation

Read from `~/Code/bash` at commit `9c465866` (bash 5.3, patch 20), on
2026-11-17. Bash is GPL v3, the same licence as tam-clam, so its code
could be taken with its notice; so far only its ideas have been.

## How bash is built

About 83,000 lines of C in the top level and `lib/readline/`, before the
rest of `lib/`. The biggest files:

- `subst.c`, 13,279 lines: every expansion.
- `parse.y`, 7,530 lines: a yacc grammar and its hand-written lexer.
- `variables.c`, 6,521 lines.
- `execute_cmd.c`, 6,409 lines.
- `jobs.c`, 5,480 lines.
- `bashline.c`, 5,023 lines: the glue to readline.
- 43 builtins in `builtins/*.def`, turned into C by `mkbuiltins`.

## The path of a command

1. `reader_loop` (`eval.c`) calls `read_command` and then
   `parse_command`, which runs the yacc parser `yyparse`.
2. The lexer, `yylex` and `read_token` (`parse.y`), reads characters
   through `shell_getc`. It decides what a token means from 28 parser
   state flags (`PST_*` in `parser.h`): whether a reserved word is
   allowed here, whether we are in a `case` pattern, in `[[ ]]`, in an
   arithmetic command, and so on.
3. Command substitutions are parsed by `parse_comsub` to find where they
   end, then kept as text and parsed again when they run
   (`command_substitute`, `subst.c`).
4. `execute_command_internal` (`execute_cmd.c`) walks the tree.
   `execute_simple_command` expands the words (`expand_words`, then
   `expand_word_internal`, `param_expand`, `word_split`, all in `subst.c`)
   and runs a builtin, a function, or `execute_disk_command`, which forks
   through `make_child` (`jobs.c`) and calls `shell_execve`.
5. Errors anywhere unwind to the top with `longjmp`; there are about 25
   `setjmp` sites.

## What to take

- **The exit statuses and messages users know.** 126 and 127, and "line
  N: name: command not found". adopt (P1, 0.0.1).
- **`$'...'` strings, `pipefail`, `[[ ]]`, arrays, `local`, process
  substitution, brace expansion.** The extras people expect, and two of
  them are now POSIX. adopt (P2 and P4).
- **`PS4` with `$LINENO` for `set -x`, and `BASH_XTRACEFD`.** Tracing that
  says where each line came from, sent to its own descriptor. adopt (P2,
  0.1.3).
- **`shopt -s inherit_errexit` and `lastpipe`.** Small options that fix
  real problems. consider (P4).
- **`${ command; }`** (bash 5.3, after ksh93 and zsh): command
  substitution without a fork, in the current shell. consider (P4); it is
  fast and avoids a subshell, but its scoping rules are new.
- **readline's default key bindings and `~/.inputrc` names.** reject as
  the model: tam-clam has its own ergonomic, mnemonic key bindings
  (Fred, 2026-11-17). readline's list of actions is a checklist for
  ours.
- **Programmable completion (`complete`, `compgen`).** The
  bash-completion project has thousands of scripts written for it.
  consider (P3): supporting enough of `complete -F` to run them would give
  completions for free, but it is a large interface.

## What not to repeat

- **A yacc grammar with the hard parts in the lexer.** The 28 state flags
  exist because a yacc grammar cannot see the context the shell language
  needs. dash and Oils show that a recursive-descent parser follows the
  POSIX grammar more directly. Our parser is hand-written.
- **Parsing command substitutions twice.** The second parse can disagree
  with the first. We parse once, into a subtree (P2, 0.1.0).
- **Control bytes inside words** (`CTLESC`, `CTLNUL`, `subst.h`). Every
  stage has to remember which bytes are escaped. Our words will be lists
  of parts (see `arch/data-structures.md`).
- **One 13,000-line file for expansion,** and global state everywhere
  (dozens of file-scope variables in each of the five biggest files). Our
  modules stay under about 800 lines and share one `tc_shell`.
- **`longjmp` for errors.** It skips cleanup; it is why so much bash state
  is global. We return results.
- **Function import from the environment.** It was the Shellshock hole
  (2014). Bash now uses `BASH_FUNC_name%%` names (`variables.c`), but the
  feature remains. We have no such feature.
- **The readline dependency** and `bashline.c`'s 5,000 lines of glue.
  Our line editor is part of the shell.
