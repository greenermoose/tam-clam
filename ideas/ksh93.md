# ksh93

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** an Eclipse Public License.
- **Scope:** David Korn's KornShell, the origin of much of what bash and
  zsh later took: `[[ ]]`, `$(...)`, arrays, `typeset`, coprocesses.
  ksh93u+m is the maintained line.

## Ideas worth taking

- **`${ command; }` no-fork substitution** started here. consider (P4),
  as bash 5.3 and zsh now spell it.
- **`getopts` with long options and a self-documenting usage string.**
  One string describes the options and produces `--help` and a man page.
  consider (P2, `getopts` extension; outside POSIX mode only).
- **Floating-point arithmetic** in `$(( ))`. consider (P4); POSIX needs
  integers only, and floating point has rounding traps.
- **Compound variables and discipline functions** (`x.get`, `x.set`
  run when a variable is read or set). reject for now; powerful but rarely
  used, and they make every variable access a possible function call.

## Things to avoid

- **The sfio and AST libraries** under it. A large private runtime is the
  opposite of our dependency goal.
- **Behaviour that changed between ksh88, ksh93 and its forks.** Scripts
  "for ksh" mean different things; we follow POSIX and bash.

## Where to look

- The ksh93u+m repository (`github.com/ksh93/ksh`), `src/cmd/ksh93/`.
