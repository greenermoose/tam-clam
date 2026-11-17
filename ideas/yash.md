# yash

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** GPL v2 or later.
- **Scope:** "yet another shell", written by Magicant to follow POSIX as
  exactly as possible, with a good interactive mode.

## Ideas worth taking

- **A reference for POSIX's corner cases.** yash's `posixly-correct`
  mode and its documentation explain what POSIX requires, case by case.
  adopt as a reference shell in the spec suite (P2) when it is installed.
- **Command-line prediction** from history, like fish's suggestions.
  adopt the idea (P3, 0.2.4; see `fish.md`).
- **A careful manual** that marks each feature as POSIX or extension.
  adopt for tam-clam's own documentation (P2).

## Things to avoid

- Nothing specific recorded yet.

## Where to look

- `github.com/magicant/yash`; the manual's POSIX notes. The newer Rust
  rewrite, yash-rs, is also worth reading.
