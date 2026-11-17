# es

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** its own permissive licence.
- **Scope:** "extensible shell" by Paul Haahr and Byron Rakitzis, built on
  rc, with first-class functions and lambdas.

## Ideas worth taking

- **Every shell operation is a function you can replace.** Pipes,
  redirections and command lookup call hookable functions (`%pipe`,
  `%create`, `%pathsearch`). consider (P3) for a few well-chosen hooks
  (command not found, before and after each command), not all of them.
- **Exceptions** (`throw`, `catch`) for errors. consider together with
  Oils' `try` (P4).

## Things to avoid

- **Hooking everything.** A user can replace any operation, so no
  operation can be trusted. A few named hooks give most of the value.

## Where to look

- Haahr and Rakitzis, "Es: A shell with higher-order functions" (USENIX
  1993); `github.com/wryun/es-shell`.
