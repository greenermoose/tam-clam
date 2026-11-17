# busybox ash

## What it is

- **Language:** C; `shell/ash.c` is 15,370 lines in one file.
  **License:** busybox as a whole is GPL v2 only, so we take ideas, never
  code (see `UPSTREAM.md`). `ash.c` keeps the original BSD notice.
- **Scope:** the shell of busybox, derived from dash, used on embedded
  systems, Alpine Linux and initramfs images. `hush.c` (13,063 lines) is
  a second, independent shell in the same tree.

## Ideas worth taking

- **Bash features as build-time options** (`ENABLE_ASH_BASH_COMPAT` and
  152 `BASH_*` switches): a dash base with `[[`, `${var/x/y}`, `$'...'`
  and arrays-lite added one by one. It confirms our plan: a POSIX core,
  then the extras, each separable. adopt the order (P2, then P4); as
  run-time options, not build-time ones.
- **A complete line editor in 3,100 lines** (`libbb/lineedit.c`): history,
  tab completion, UTF-8, vi mode. A size target for ours. adopt the size
  goal (P3, 0.2.0).
- **Tests as a script plus its expected output** (`shell/ash_test/ash-*/`
  with `.tests` and `.right` files). The same shape as our
  `tests/scripts/` (`.sh` and `.out`). adopt (P1, 0.0.1).
- **Works without an MMU** (`vfork` only). Shows that a shell can avoid
  `fork` almost everywhere. consider, with dash's `vfork` idea (P2).

## Things to avoid

- **One 15,000-line file.** Our modules stay under about 800 lines.
- **Features chosen at build time.** Scripts then behave differently on
  two busybox builds. tam-clam always has the same features; strict
  modes are run-time options.

## Where to look

- `shell/ash.c`, `shell/hush.c`, `shell/math.c` (arithmetic),
  `libbb/lineedit.c`, `shell/ash_test/`.
