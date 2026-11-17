# mksh

## What it is

- **Language:** C, about 40,000 lines with headers, plus a 14,500-line
  test file.
  **License:** the MirOS licence (permissive).
- **Scope:** the MirBSD Korn shell, descended from pdksh. It was the
  `/bin/sh` of Android for years, so it has run on billions of devices.
  `lksh` is its legacy, more POSIX-strict build.

## Ideas worth taking

- **One regression file with every case** (`check.t`, 627 tests, run by
  `check.pl`). Each test has a name, a description, its input and its
  expected output, and every fixed bug adds one:

      name: xxx-quoted-newline-1
      description:
          Check that \<newline> works inside of ${}
      stdin:
          abc=2
          echo ${ab\
          c}
      expected-stdout:
          2

  adopt the idea (P2, spec suite); every fix in tam-clam adds a test.
- **Area allocation** (`lalloc.c`): memory is allocated in an area, and
  the temporary area `ATEMP` is freed after each command. The same idea
  as dash's stack and our arena; mksh shows it scales to a full shell.
  adopt (P1, 0.0.1).
- **Its own small output layer** (`shf.c`) instead of stdio, with
  explicit error checks. We write with `tc_write_all` for the same
  reason. adopt (P1, 0.0.1).
- **Korn shell features in a small shell:** arrays, `[[ ]]`, `typeset`,
  `print`, `$'...'`, coprocesses. Proof that the extras fit in a small
  codebase. adopt the extras (P4), as bash spells them.
- **Options in one table file** (`sh_flags.opt`). Every option, its
  letter, its default and its help in one place. adopt as a plain C table
  (P2, 0.1.3).
- **A UTF-8 mode** (`set -U`) that switches the line editor and string
  lengths to characters. adopt the result (P3, 0.2.0); ours is always on
  in a UTF-8 locale.

## Things to avoid

- **A build script instead of Make** (`Build.sh`, about 3,000 lines of
  shell that probes the system). Plain Make with POSIX interfaces needs no
  probing.
- **ksh-isms where bash differs** (`print`, `typeset` details, `$'...'`
  quirks). When the two disagree we follow bash, which more scripts are
  written for.

## Where to look

- `check.t`, `check.pl`, `lalloc.c`, `shf.c`, `lex.c`, `syn.c`,
  `eval.c`, `edit.c` (the line editor), `sh_flags.opt`, `mksh.faq`.
