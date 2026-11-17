# xonsh

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Python. **License:** BSD 2-clause.
- **Scope:** a Python interpreter that also accepts shell commands.

## Ideas worth taking

- **Little for tam-clam.** It shows how far users will go to script in a
  real language; our answer is good POSIX scripting with strict modes.

## Things to avoid

- **A heavy interpreter as the shell.** Start-up time and memory are the
  opposite of our goals. reject.
- **Guessing whether a line is Python or shell.** Ambiguous parsing is
  exactly what a bullet-proof shell must not do.

## Where to look

- `github.com/xonsh/xonsh`.
