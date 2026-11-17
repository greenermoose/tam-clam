# rc (Plan 9)

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** Plan 9 is MIT-licensed since 2021; Byron
  Rakitzis's Unix rc has its own permissive licence.
- **Scope:** Tom Duff's shell for Plan 9, a clean redesign of the Bourne
  shell.

## Ideas worth taking

- **No rescanning of values.** A variable's value is a list and is never
  split or globbed again when used. adopt as the meaning of our
  `simple_word_eval` strict mode (P4, 0.3.3).
- **One quoting rule:** only single quotes, with `''` for a quote inside.
  reject for the POSIX language, but it shows how simple quoting can be.
- **`if not`** instead of `else`, and a grammar small enough to fit on a
  page. reject the syntax; keep the lesson that a small grammar is a
  correct grammar.

## Things to avoid

- **Incompatibility with sh.** rc never runs sh scripts.

## Where to look

- Tom Duff, "Rc - The Plan 9 Shell"; `github.com/rakitzis/rc`.
