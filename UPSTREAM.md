# Code taken from other projects

tam-clam is written fresh. When another shell has code that works well,
we read it, then write it into tam-clam's own modules in C and in our own
style. This file lists every such piece: where it came from, what changed,
and why it earned its place. Ideas taken without code go in `ideas/`
instead.

When you take something new, add a row here in the same change, and add
the project's notice to `COPYRIGHT`.

Licenses matter when taking code. tam-clam is GPL-3.0-or-later. Code
under a permissive licence can come in with its notice kept: dash (BSD),
zsh (its own MIT-style licence, though some shipped functions are GPL),
mksh (the MirOS licence) and Oils (Apache 2.0). bash is GPL v3, which
matches. fish is GPL v2 only and busybox is GPL v2 only, and GPL v2-only
code cannot be combined with GPL v3 code, so from those two we take ideas
only, never code. Always check the file's own header as well.

**Reference clones.** These are kept outside this repository and are
updated with `tools/pull-references` (see `ideas/sources`). The commits
below are the ones read on 2026-11-17.

- bash: `~/Code/bash`, `https://git.savannah.gnu.org/git/bash.git`, branch
  `master`, commit `9c465866` (bash 5.3 patch 20). The pull on 2026-11-17
  failed with an HTTP 500 from Savannah; the clone is unchanged.
- dash: `~/Code/dash`, commit `0a896bf`.
- zsh: `~/Code/zsh`, commit `b13bbf2`.
- fish: `~/Code/fish`, commit `f0d874a` (fish 4, written in Rust).
- mksh: `~/Code/mksh`, the GitHub mirror, commit `3417528` (2026-02-15;
  the mirror trails the upstream CVS repository).
- Oils: `~/Code/oils`, commit `d114978`.
- busybox: `~/Code/busybox`, commit `6412da2`; we read `shell/ash.c`,
  `shell/hush.c` and `libbb/lineedit.c`.

## Pieces taken

None yet.

## Compared with, not taken

- The 0.0.1 lexer and parser were written from the POSIX standard (XCU
  2.3 token recognition and 2.10 grammar). They were checked against
  dash's `src/parser.c`, but none of its code or structure was copied.
- Reading standard input one byte at a time, so that commands can read
  the lines after them, is what POSIX requires and what dash and bash do.
  `src/input.c` implements it fresh.
