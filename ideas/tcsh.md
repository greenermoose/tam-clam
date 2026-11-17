# tcsh

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** BSD.
- **Scope:** the C shell with a line editor; still the default root shell
  on FreeBSD until 14.0.

## Ideas worth taking

- **History substitution** (`!!`, `!$`, `!-2`), which bash took from csh.
  adopt in interactive mode only (P3, 0.2.1), and off in scripts.
- **`autolist` and command spelling correction.** Early versions of what
  zsh and fish do better (see `zsh.md`).

## Things to avoid

- **The csh language.** Its grammar is irregular and its error handling
  weak; Tom Christiansen's "Csh Programming Considered Harmful" lists
  why. reject.

## Where to look

- `github.com/tcsh-org/tcsh`.
