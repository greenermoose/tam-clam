# PowerShell

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C# on .NET. **License:** MIT.
- **Scope:** Microsoft's object-pipeline shell, also on Linux.

## Ideas worth taking

- **`-WhatIf` and `-Confirm` on commands that change things.** A dry run
  as a common convention. consider for tam-clam's own builtins that
  delete or overwrite (there are none yet).
- **Errors as records** with the failing command, position and category,
  kept in a list the user can inspect after the fact. consider (P2): a
  `$TC_LAST_ERROR` with the message, file and line of the last failure.
- **Consistent verb-noun names** for builtins. reject for POSIX names,
  but our extra builtins should be named consistently.

## Things to avoid

- **.NET as a runtime** and very slow start-up.
- **Objects in pipes** (see `nushell.md`).

## Where to look

- `github.com/PowerShell/PowerShell`; the `about_CommonParameters` help
  topic.
