# nushell

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Rust. **License:** MIT.
- **Scope:** a shell where commands pass tables and records instead of
  bytes, with built-in commands for JSON, CSV, YAML and SQL-like queries.

## Ideas worth taking

- **Structured output for our own builtins** (for example a `jobs` or
  `history` that can print one record per line in a stable format).
  consider (P3); a line format like tam-pane's IPC, not JSON.
- **Errors with spans and labels** pointing into the command line. adopt
  the idea (P2), with fish and Oils.

## Things to avoid

- **Structured data as the core of the language.** Every external
  command still speaks bytes; nushell has to convert at every boundary.
  reject for tam-clam's core.
- **Breaking changes between releases.** A shell's language must stay
  stable.

## Where to look

- `github.com/nushell/nushell`.
