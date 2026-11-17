# Dependency ledger

Every library tam-clam uses is listed here, with why it is needed and which
version removes it. The goal is the C library and libtam, and nothing else
(`arch/principles.md`). Update this file in the same change as any
dependency change.

## Direct dependencies (0.0.1)

`readelf -d build/tam-clam` lists only `libc.so.6` as NEEDED.

- **The C library** (glibc on Arch; musl must work too)
  - **Why:** system calls and standard C.
  - **Planned exit:** none; it is the goal. We use POSIX.1-2008
    interfaces only (`-D_POSIX_C_SOURCE=200809L`), so the shell builds on
    musl and on Void Linux.
- **libtam** (static, `$(LIBTAM_PREFIX)/lib/libtam.a`)
  - **Why:** `tam_str` builds words in the lexer. From P3, `tam_tty`
    (raw mode, size, bracketed paste), `tam_key` (key decoding) and
    `tam_uni` (UTF-8 and display width) carry the line editor; `tam_path`
    gives XDG paths for history and configuration.
  - **Planned exit:** none; it is the goal.

## Build-time only

- **make and a C11 compiler.** Plain Make; no other build system.
- **A POSIX sh** runs `tests/run-scripts.sh`. **bash** or **dash** is the
  reference shell the script tests compare against; tests still run
  without one (`TC_REF=none`).
- **ASan and UBSan** from the compiler, for `make sanitize`.
- **clang's libFuzzer** (P2, optional) for fuzzing.

## What other shells use that we do not

- **readline** (bash) and **libedit** (dash, optionally): replaced by our
  own line editor on libtam (P3).
- **ncurses and terminfo** (zsh, bash through readline, fish): libtam
  speaks ANSI and xterm sequences directly and asks the terminal what it
  supports, as zsh's newer `termquery.c` does.
- **pcre** (zsh's module) and the C library's `regcomp`: our own
  linear-time matcher for patterns (P2) and `=~` (P4).
- **gettext**: messages are in English for now.
- **Dynamic modules** (zsh's `zmodload`, bash's `enable -f`): rejected.
  Loadable code locked to one build is how Hyprland's plugins break; it
  does not fit a shell that must always start.
