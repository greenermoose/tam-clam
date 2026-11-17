# The best ideas, by topic

This file gathers the ideas from every shell in this folder, grouped by
what they are for, with where each one came from and the phase that takes
it up. Start here; follow the links to the per-shell files for reasons and
source locations.

## 1. Never run the wrong thing

These come first, because a shell that runs the wrong command can destroy
data.

- **Refuse, never approximate.** A feature not built yet is a clear
  error, not a guess. tam-clam's own rule, P1 0.0.1 onward: 0.0.1 refuses
  `$x`, pipes, redirections, globs and reserved words.
- **Parse once.** Command substitutions become a subtree when the script
  is parsed, never text that is parsed again later. Learned from bash's
  double parse (`bash.md`). P2 0.1.0.
- **Words as lists of parts,** not text with control bytes inside. The
  in-band bytes of dash and bash (`CTLESC`, `CTLVAR`) have caused bugs for
  decades (`dash.md`). P1 0.0.3.
- **Nothing executable from the environment.** No function import
  (Shellshock, `bash.md`), no `ENV` file outside interactive shells,
  prompts not re-parsed as code. From the start.
- **POSIX exactly, in POSIX mode.** Where POSIX leaves a choice, follow
  dash and bash --posix when they agree, and write down the choice when
  they do not (`oils.md`, known differences). P2.
- **Read standard input a byte at a time,** so a command reads the lines
  after it, as POSIX requires (dash, bash). P1 0.0.1, done.

## 2. Errors that cannot be missed

- **Strict modes with Oils' names:** `strict_errexit` (errexit never
  silently disabled), `command_sub_errexit`, `strict_argv`,
  `strict_arith`, `strict_tilde`, `simple_word_eval`, plus `nounset` and
  `pipefail`; one switch turns on all of them (`oils.md`). P4 0.3.3.
- **No rescanning of values** in strict mode: `$x` is one word, never
  split or globbed. The rule of rc (`rc.md`) and of zsh's default
  (`zsh.md`), opt-in for us. P4 0.3.3.
- **`pipefail` and `PIPESTATUS`** (bash, fish). P2 0.1.3, P4.
- **Messages with file, line and column, and a caret** under the problem
  (fish, Oils, nushell). Line numbers from P1 0.0.1; columns and carets
  in P2.
- **An error catalog:** every message documented with an example and a
  test (`oils.md`). P2.
- **The last error kept** for inspection (`powershell.md`). consider, P2.
- **`set -x` that says where** each traced line came from, optionally on
  its own descriptor (`BASH_XTRACEFD`, `bash.md`). P2 0.1.3.

## 3. Robust by construction

- **Per-command arena memory** with a cap (dash's stack, mksh's `ATEMP`).
  P1 0.0.1, done.
- **Return results; no `setjmp` and `longjmp`** (against bash and dash).
  P1 0.0.1, done.
- **Signal handlers only set flags;** work happens at safe points (dash's
  INTOFF and INTON rule). P1 0.0.1, done.
- **Limits on everything input controls:** word length, words per
  command, nesting depth, recursion depth, here-document size. P1.
- **No recursion over lists;** a 65,536-command line runs in a loop. P1
  0.0.1, done.
- **The shell's own descriptors above 10 and close-on-exec.** P1 0.0.1,
  done.
- **Linear-time pattern matching and regular expressions,** our own code,
  so no input can make the shell hang. P2 0.1.0, P4 0.3.1.
- **One plain C table for options** (`mksh.md`) and for builtins. P2.

## 4. Tested against everyone

- **Script tests compared with a reference shell** on every run. P1
  0.0.1, done (`tests/run-scripts.sh`, dash or bash --posix).
- **A spec suite run against several shells,** recording where they
  differ (Oils' `spec/`, `oils.md`; zsh's `Test/`, `zsh.md`). P2 0.1.4.
- **Every fixed bug adds a test** (mksh's `check.t`, `mksh.md`). Always.
- **Sanitizer builds** (`make sanitize`). P1 0.0.1, done.
- **Fuzzing** the lexer, parser and expander. P2 0.1.4.
- **Interactive tests over a pseudo-terminal,** written in C (fish's
  `tests/pexpects/`, `fish.md`). P3.

## 5. Pleasant to type in

- **Syntax highlighting as you type, from the real parser** (fish). P3
  0.2.4.
- **Suggestions from history,** accepted with Right, only when they would
  still work (fish; yash's prediction). P3 0.2.4.
- **Up and Down search by what is typed** (fish, zsh). P3 0.2.1.
- **Abbreviations** that expand in place, so history keeps the real
  command (fish). P4 0.3.4.
- **readline's emacs keys** (bash); **named editor actions** users can
  bind (zsh's widgets). P3 0.2.0.
- **A right prompt** and **`precmd` and `preexec` hooks** (zsh), with
  bash's `PROMPT_COMMAND` and `PS0`. P3 0.2.5.
- **Good defaults with no configuration file** (fish). P3.
- **A directory jump list** (elvish's location mode; zsh's `cd -<TAB>`).
  consider, P3.
- **History substitution `!!` and `!$`,** interactive only (tcsh, bash).
  P3 0.2.1.
- **Spelling correction** (zsh, tcsh). consider, P3.

## 6. History that is never lost

- **Shared between open shells,** with timestamps, durations, exit
  status and directory (zsh's extended history, fish's records). P3
  0.2.1.
- **Single `O_APPEND` writes** of whole records, so concurrent shells
  never interleave and no lock file is needed (our design, against zsh's
  lock file). P3 0.2.1.
- **A reader that skips a damaged record** instead of failing. P3.
- **A private mode** with no history (fish). P3 0.2.1.

## 7. Completion

- **One small declarative file per command,** loaded only when needed
  (fish's `share/completions/`). P3 0.2.3.
- **Completion files generated from man pages** (fish's
  `create_manpage_completions.py`). P4 0.3.4.
- **Enough of bash's `complete -F`** to reuse bash-completion scripts.
  consider, P3.
- **Not zsh's `zstyle` system:** powerful but too hard to understand
  (`zsh.md`).

## 8. The extras, without breaking POSIX

- **bash's spellings** for arrays, associative arrays, `[[ ]]`, `local`,
  `declare`, brace expansion, process substitution, `+=` (bash; ksh93 and
  mksh show they fit a small shell). P4.
- **`$'...'` and `pipefail`,** which POSIX.1-2024 adopted. P2.
- **`**` recursive globbing** (zsh, bash's `globstar`); glob qualifiers
  are a consideration (zsh). P4 0.3.2.
- **No-fork command substitution** `${ cmd; }` and `${| cmd; }` (ksh93,
  bash 5.3, zsh). consider, P4.
- **`printf %q` and `${var@Q}`** for safe quoting (bash). P2.
- **Run-time options, never build-time ones** (against busybox's
  `ENABLE_ASH_BASH_COMPAT`). Always.
- **zsh-style emulation scoped to a function** (`emulate -L`), as the
  model for turning strict modes on locally. P4.

## 9. Small, fast and frugal

- **Start-up as fast as dash's** (`dash.md`). Measured in P6.
- **`vfork` or `posix_spawn`** for simple commands (dash, busybox, fish).
  consider, P2.
- **Builtins avoid forks;** PATH lookups are cached (`hash`). P2.
- **No threads, no timers, no polling** while waiting for input; cached,
  bounded checks for highlighting (against fish's background threads).
  P3.
- **A line editor in a few thousand lines** (busybox's `lineedit.c`). P3.
- **No dynamic modules** (against zsh's `zmodload`). Always.

## 10. Rejected, and why

- **A new, non-POSIX language** (fish, elvish, nushell, rc, es, xonsh,
  YSH): Fred chose a POSIX core.
- **Structured data in pipes** (nushell, PowerShell, elvish): every
  external command speaks bytes.
- **Universal variables** (fish): need file watching; energy and
  complexity.
- **Arrays from 1 and no splitting by default** (zsh): break POSIX
  scripts.
- **Large private runtimes** (ksh93's sfio and AST, .NET, Python, Go):
  against "the C library and libtam".
- **The csh language** (tcsh).
- **yacc and generated tables** (bash, dash).

## Ideas of our own

- **Refusals name the plan.** "pipelines are not supported yet; see
  ROADMAP.md". Done.
- **A syntax check mode,** `tam-clam -n script`, as POSIX's `set -n`
  allows, with column-precise messages: a lint pass for free. P2 0.1.3.
- **Lint warnings for legal but risky code** (unquoted `$x` in `[ ]`,
  `cd` without a check in a script) in the same parser that runs the
  code. Open point for Fred, P1.
- **A formatter** built on a lossless syntax tree (fish's `fish_indent`,
  Oils). consider, P2.
- **The Linux console as a first-class terminal:** the editor and
  highlighting work on `TERM=linux` without terminfo. P3.
