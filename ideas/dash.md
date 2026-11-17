# dash

## What it is

- **Language:** C, about 16,800 lines. **License:** BSD (from the
  4.4BSD ash), with a few GPL files.
- **Scope:** the Debian Almquist shell, a small, fast POSIX shell. It is
  `/bin/sh` on Debian and Ubuntu, and the reference shell our script
  tests prefer when it is installed.
- `src/TOUR` is the original 1989 design paper by Kenneth Almquist; still
  the best short introduction to how a shell is put together.

## Ideas worth taking

- **A recursive-descent parser** over the POSIX grammar (`parser.c`).
  adopt (P1, 0.0.1); ours is written fresh.
- **A stack allocator for per-command memory** (`memalloc.c`, `stalloc`
  and `STPUTC`). Everything a command allocates is freed by resetting one
  pointer. adopt (P1, 0.0.1) as `tc_arena`, without the `longjmp`.
- **Here-documents kept in memory** rather than temporary files. The TOUR
  explains why: temporary files were a source of bugs. adopt (P1,
  0.0.2), with a size cap.
- **Here-documents read after the newline,** from a list of pending
  documents (`heredoclist`, `parseheredoc`). adopt (P1, 0.0.2).
- **INTOFF and INTON:** critical sections in which an interrupt is held
  until the section ends. adopt the rule (signals act at safe points), not
  the macros.
- **`vfork` for simple commands** (`jobs.c`, `vforkexec`). It skips
  copying page tables, so starting commands costs less time and energy.
  consider (P2); the child must touch nothing but `execve`.
- **PATH search in the child, trying `execve` on each candidate**
  (`exec.c`, `shellexec`). No race between checking and running. adopt
  (P1, 0.0.1).
- **Smallness as a feature.** dash starts fast and is easy to audit. A
  target for tam-clam's start-up time (P6).

## Things to avoid

- **Control bytes in words** (`CTLESC`, `CTLVAR`, `CTLBACKQ` in
  `parser.h`). Bytes 0x81 to 0x88 in input need escaping everywhere, and
  bugs in that escaping have been found for decades. Words as lists of
  parts instead (0.0.3).
- **Generated code** (`mksyntax`, `mknodes`, `mkinit`, `mkbuiltins`). The
  TOUR itself says "there are undoubtedly too many of these". Plain C
  tables are enough.
- **`setjmp` exceptions** (`error.c`). We return results.
- **No interactive comforts.** dash's line editing needs libedit and is
  minimal. Ours is built in (P3).

## Where to look

- `src/TOUR`, `src/parser.c`, `src/expand.c`, `src/eval.c`,
  `src/exec.c`, `src/jobs.c`, `src/memalloc.c`, `src/input.c`.
