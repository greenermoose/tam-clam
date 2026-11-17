# Design principles

tam-clam is built fresh, from the ground up. We learn from bash, zsh,
fish, dash, mksh, Oils and the rest, and take their best code, but we do
not inherit their structure. These rules decide how code is written here.

## Bullet-proof first

A shell runs everything else, often as root and often unattended. A bug
that would be a nuisance in another program can delete files there. So:

- **Never run a command other than the one written.** A feature that is
  not built yet is refused with a clear message, never approximated.
  0.0.1 refuses `$x` rather than passing `$x` through literally.
- **Every failure is checked and reported.** Every allocation, every
  system call. `EINTR` is retried, short reads and writes are finished,
  and a failed write to standard output is an error, not silence.
- **Memory never runs out silently.** Allocation returns `NULL`; it never
  aborts the shell. The arena for one command has a cap
  (`TC_ARENA_MAX`), and a command that reaches it fails alone.
- **Input has limits.** Word length (`TC_WORD_MAX`), words per command
  (`TC_WORDS_MAX`), and from 0.0.4 nesting depth and function recursion.
  A hostile or broken script gets an error, never a crash or a hang.
- **No stack overflow from input.** Lists are built and run without
  recursion. Nested constructs recurse only to a fixed depth.
- **No undefined behaviour.** The tests run under ASan and UBSan
  (`make sanitize`). The parser and expander get fuzzing in P2.
- **No `setjmp` and `longjmp`.** bash and dash unwind errors with
  `longjmp`, which skips cleanup and makes every function a possible exit
  point. Our functions return a result, and callers handle it.
- **Signal handlers only set flags** (`src/signals.c`). The shell acts on
  them at safe points: between commands and when a read is interrupted.
- **Nothing executable comes from the environment.** No function import
  through environment variables (the 2014 Shellshock hole in bash), no
  `ENV` file in non-interactive shells, and prompts are not re-parsed as
  code.
- **Descriptors stay tidy.** The shell's own files (scripts, history)
  sit above descriptor 10 and close on exec, so commands never inherit
  them and redirections never collide with them.
- **History is never lost** (P3). Appends are single `O_APPEND` writes;
  a damaged record is skipped, never fatal.

## POSIX first

- The language core is POSIX.1-2024 XCU chapter 2. Where it leaves a
  choice, we follow dash and bash --posix when they agree, and record
  the choice when they do not.
- Extras (arrays, `[[ ]]`, strict modes) never change what a POSIX
  script does.
- Every behaviour has a script test, run under tam-clam and a reference
  shell (`tests/run-scripts.sh`). From P2, the spec suite runs against
  several shells.

## Our own structure

- Modules, names and data flow are designed for tam-clam. Other shells'
  layouts are references, not templates.
- Code taken from another shell is rewritten into our modules and our
  style, and recorded in `UPSTREAM.md` with the reason it earned its
  place. Mind the licence (see `UPSTREAM.md`).

## Small modules, explicit state

- One job per file. No source file over about 800 lines.
- One `tc_shell` is passed to every function that needs shell state.
  The only global variables are the signal flags in `src/signals.c`.
- Pure logic (lexer, parser, expansion, later pattern matching and
  arithmetic) runs without forking and is unit tested.
- Names start with `tc_`. Headers live in `include/tam-clam/`.

## Every dependency earns its place

- `arch/dependencies.md` lists each library, why it is needed, and the
  version that removes it.
- The goal is the C library and libtam, nothing else. libtam is linked
  statically, so `readelf -d build/tam-clam` lists only `libc.so.6`.
- Small jobs never pull in a library: our own pattern matcher, regular
  expressions, arithmetic and line editor.
- Plain Make only. Generated code only where it removes real work; none
  so far.

## Energy is a feature

- An idle shell waiting for input uses no CPU: no timers, no polling, no
  background threads.
- Builtins avoid forks; PATH lookups are cached from 0.1.2 (`hash`).
- Features that cost work on every key press (highlighting,
  suggestions) cache their results and can be turned off.

## Helpful errors

- Every message names the shell or script and the line: `script: line 3:
  nosuch: command not found`. From P2, syntax errors also give the
  column and show the line with a caret under the problem, as fish and
  Oils do.
- Each message says what to do where it can ("is not supported yet; see
  ROADMAP.md").
