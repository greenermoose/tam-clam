# Data structures

What tam-clam is built from today (0.0.1), and what each version adds.

## The shell: `struct tc_shell` (`include/tam-clam/shell.h`)

One per process, created in `main` and passed to every function that
needs shell state.

- `name`: `$0`, used at the start of every message.
- `self_path`: this executable, read from `/proc/self/exe`, used to run
  scripts the kernel cannot run (ENOEXEC).
- `interactive`: standard input and standard error are terminals and
  neither `-c` nor a script was given.
- `status`: the last command's status, `$?`.
- `exit_requested`, `exit_status`: set by `exit`; the main loop stops.
- `arena`: memory for the command being run.
- `in`: the input being read.

0.0.3 adds the variable table and positional parameters; 0.0.4 the
function table and the call depth; 0.1.1 the trap table; 0.2.2 the job
table.

## The arena: `struct tc_arena` (`src/arena.c`)

A linked list of blocks (16 KiB, or larger for a large request). Every
allocation is aligned for any type. `tc_arena_reset` frees all blocks but
the first after each command, so a shell running a long script reuses one
block and does not grow. The arena has a cap, `TC_ARENA_MAX` (64 MiB);
past it, allocation returns `NULL` and the command fails with "out of
memory".

The idea is dash's stack allocator and mksh's `ATEMP` area (see
`ideas/dash.md`, `ideas/mksh.md`), without their `longjmp` unwinding.
Long-lived data (variables, functions, history) will use `malloc` and be
freed explicitly; only per-command data lives in the arena.

## Input: `struct tc_input` (`src/input.c`)

A string (`-c`), a script file, or standard input. Standard input is read
one byte at a time (`bytewise`), as POSIX requires, so a command can read
the lines that follow it. Script files are read in 4 KiB blocks. The input
counts lines, and writes `$ ` (PS1) and `> ` (PS2) prompts when the shell
is interactive.

## Tokens: `struct tc_token` (`src/lex.c`)

- `kind`: a word, a newline, the end of input, or one of the 17 POSIX
  operators (XCU 2.10.1).
- `text`, `len`: a word's raw text, quotes kept, in the arena.
- `flags`: what the word contains outside single quotes: quoting, `$`,
  backquotes, unquoted pattern characters, a leading `~`, a leading
  `NAME=`. The parser uses them to refuse what is not supported yet.
- `line`: the line the token starts on.

The lexer has one byte of lookahead (`peek`) and builds each word in a
reused `tam_str` buffer before copying it to the arena.

**Next (0.0.3):** words become a list of parts instead of raw text:

    struct tc_wpart {
        enum { LITERAL, QUOTED, PARAM, COMMAND_SUB, ARITH } kind;
        ...                     /* text, or a parameter and its operator */
        struct tc_wpart *next;
    };

so quoting is decided once, by the lexer, and the expander never re-reads
quotes. dash and bash mark quoting with control bytes inside the word's
text instead (`CTLESC`, `CTLVAR`), which forces every later stage to
escape and unescape those bytes; we avoid that.

## The syntax tree: `struct tc_node` (`include/tam-clam/ast.h`)

- `TC_NODE_SIMPLE`: an array of words.
- `TC_NODE_SEQ`: `first ; rest`, a right-leaning chain built without
  recursion and run with a loop, so a line of 65,536 commands cannot
  overflow the stack.

0.0.2 adds pipelines, and-or lists and redirections; 0.0.4 the compound
commands and function definitions. Nodes live in the arena and are freed
with it.

## Execution (`src/exec.c`)

`tc_exec` walks the tree. A simple command's words are expanded into an
`argv` array in the arena. Builtins (`src/builtins.c`, a sorted table)
run in the shell. Other commands run in a child: the PATH search happens
there, trying `execve` on each candidate, so there is no gap between
checking a file and running it. A file the kernel refuses with ENOEXEC
runs as a script under tam-clam. The parent waits with `waitpid`,
retrying on `EINTR`. A signal death becomes status 128 plus the signal.
