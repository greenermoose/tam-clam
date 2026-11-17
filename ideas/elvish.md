# elvish

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Go. **License:** BSD 2-clause.
- **Scope:** a modern shell with structured values in pipelines,
  namespaces, exceptions for failed commands, and an inventive line
  editor.

## Ideas worth taking

- **Navigation modes in the editor:** a directory-history jump ("location
  mode", like zoxide built in), a file navigator, and history as a
  searchable list. consider (P3); a directory jump list is cheap and
  useful.
- **A non-zero exit is an exception** unless handled. The same goal as
  our `strict_errexit` (P4), reached by a new language.
- **Value pipelines** next to byte pipelines. reject for the POSIX core
  (see `nushell.md`).

## Things to avoid

- **A Go runtime** and a non-POSIX language.

## Where to look

- `github.com/elves/elvish`, `pkg/edit/`.
