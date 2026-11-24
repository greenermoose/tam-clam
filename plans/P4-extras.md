# P4: The extras (0.3.x)

**Status:** in progress (0.3.0 landed 2026-11-24).

## Goal

The bash and zsh features people use every day, and the opt-in strict
modes that make new scripts safer. None of these may change how a POSIX
script runs.

## Versions

- **0.3.0** Indexed arrays (`a=(x y)`, `${a[@]}`, `${#a[@]}`, `a+=(z)`)
  and associative arrays (`declare -A`), with bash's syntax.
- **0.3.1** `[[ ]]` with `==` patterns, `=~` using our own small regular
  expression engine (POSIX ERE, linear time), `local` and `declare`,
  brace expansion `{a,b}` and `{1..9}`.
- **0.3.2** Process substitution `<(...)` and `>(...)` through
  `/dev/fd`, and `**` recursive globbing.
- **0.3.3** Strict modes, named after Oils' options so users can look
  them up: `strict_errexit` (errexit is never silently disabled inside
  `if`, `&&`, `||` or `!`; code that would disable it is an error),
  `command_sub_errexit` (a failing `$(...)` fails the command),
  `simple_word_eval` (no implicit field splitting or globbing of
  unquoted expansions), `strict_argv` (no empty command names),
  `nounset` and `pipefail`. One switch, `set -o strict`, turns on all of
  them.
- **0.3.4** fish-style abbreviations, and a tool that writes completion
  files from man pages, as fish's `create_manpage_completions.py` does,
  written in C or POSIX sh.

## Acceptance

- The spec suite gains cases for each feature, run against bash and zsh
  where they agree.
- With no strict option set, every P2 spec case still passes unchanged.

## Dependencies

- None added. Regular expressions are our own code, not pcre or the C
  library's `regcomp` (whose behaviour differs between glibc and musl).

## Open points

- Should `local -`, `mapfile` and `printf -v` come in with this phase?
