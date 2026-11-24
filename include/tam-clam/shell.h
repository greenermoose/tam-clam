/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The shell's state. One tc_shell is passed to every function that needs
 * it; there is no other global state except the signal flags in
 * signals.h. */
#ifndef TAM_CLAM_SHELL_H
#define TAM_CLAM_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "tam-clam/alias.h"
#include "tam-clam/arena.h"
#include "tam-clam/bg.h"
#include "tam-clam/func.h"
#include "tam-clam/hash.h"
#include "tam-clam/trap.h"
#include "tam-clam/var.h"

struct tc_input;

/* Results shared by the lexer, parser and input layers. */
enum tc_result {
	TC_OK = 0,
	TC_END,          /* no more input */
	TC_ERR_SYNTAX,   /* a syntax error, already reported */
	TC_ERR_NOMEM,    /* out of memory, or a size limit was reached */
	TC_ERR_IO,       /* reading the input failed */
	TC_ERR_INTR,     /* interrupted by SIGINT (interactive only) */
};

/* Exit statuses that POSIX fixes. */
enum {
	TC_STATUS_USAGE = 2,      /* syntax error or bad use of a builtin */
	TC_STATUS_NOEXEC = 126,   /* found but cannot be executed */
	TC_STATUS_NOTFOUND = 127, /* command not found */
};

#define TC_SELF_PATH_MAX 4096

/* Function recursion and AST execution depth limits. */
#define TC_FUNC_DEPTH_DEFAULT 1000
#define TC_FUNC_DEPTH_MAX     2000
#define TC_EXEC_DEPTH_MAX     8192
#define TC_DOT_DEPTH_MAX      128
#define TC_EVAL_DEPTH_MAX     128

/* Explicit flow control (no setjmp/longjmp). */
enum tc_flow {
	TC_FLOW_NONE = 0,
	TC_FLOW_BREAK,
	TC_FLOW_CONTINUE,
	TC_FLOW_RETURN,
};

struct tc_shell {
	const char *name;          /* $0, used in messages */
	char self_path[TC_SELF_PATH_MAX]; /* this executable, for ENOEXEC */
	bool interactive;
	int status;                /* status of the last command ($?) */
	bool exit_requested;       /* the exit builtin or fatal error ran */
	int exit_status;
	enum tc_flow flow;         /* active break / continue / return */
	unsigned flow_count;       /* remaining loop levels for break/continue */
	int return_status;         /* status for return */
	unsigned loop_depth;       /* enclosing while/until/for depth */
	unsigned func_depth;       /* active function call depth */
	unsigned func_limit;       /* default recursion limit (1000) */
	unsigned exec_depth;       /* active AST execution nesting depth */
	unsigned dot_depth;        /* active . (dot) script nesting depth */
	unsigned eval_depth;       /* active eval nesting depth */
	unsigned trap_depth;       /* active trap dispatch nesting depth */
	bool in_exit_trap;         /* true while running EXIT trap */
	bool in_command_builtin;   /* true while `command` runs a special builtin */
	bool opt_allexport;        /* set -a */
	bool opt_noclobber;        /* set -C */
	bool opt_errexit;          /* set -e */
	bool opt_noglob;           /* set -f */
	bool opt_noexec;           /* set -n */
	bool opt_nounset;          /* set -u */
	bool opt_xtrace;           /* set -x */
	bool opt_pipefail;         /* set -o pipefail */
	unsigned errexit_ignore;   /* >0 while POSIX set -e is ignored */
	unsigned subshell_depth;   /* nesting depth for PS4 indirection */
	unsigned long lineno;      /* current executing command line */
	bool lineno_disabled;      /* true after unset LINENO */
	pid_t root_pid;            /* $$: main shell PID */
	pid_t last_bg_pid;         /* $!: last background PID */
	bool cmdsub_ran;           /* true if a command sub ran in current simple cmd */
	int cmdsub_status;         /* exit status of last command substitution */
	size_t getopts_pos;        /* char offset inside current getopts arg */
	long getopts_optind;       /* last OPTIND observed by getopts */
	struct tc_vars vars;       /* shell variables and environment */
	struct tc_funcs funcs;     /* shell functions */
	struct tc_aliases aliases; /* shell aliases */
	struct tc_hash_table hash; /* cached PATH lookups */
	struct tc_traps traps;     /* signal and EXIT traps */
	struct tc_bg_table bg;     /* background child tracking */
	char **params;             /* positional parameters $1..$N */
	size_t nparams;
	struct tc_arena arena;     /* memory for the command being run */
	struct tc_input *in;       /* the input being read, for line numbers */
};

void tc_shell_init(struct tc_shell *sh, const char *name);
void tc_shell_finish(struct tc_shell *sh);

/* Replace positional parameters ($1..$N) with heap copies of argv[0..n). */
int tc_shell_set_params(struct tc_shell *sh, int n, char *const *argv);
/* Shift positional parameters left by n (n <= sh->nparams). */
void tc_shell_shift_params(struct tc_shell *sh, size_t n);

/* Report an error on standard error as "name: line N: message". line 0
 * leaves the line out. Never fails loudly: a failed write is ignored. */
void tc_error(const struct tc_shell *sh, unsigned long line, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* Write all of buf to fd, retrying short writes and EINTR. 0 or -1. */
int tc_write_all(int fd, const void *buf, size_t len);

/* Heap-allocated current working directory (NULL on failure, errno set). */
char *tc_getcwd(void);
/* True if pwd is an absolute path without . or .. that matches stat("."). */
bool tc_valid_pwd(const char *pwd);

#endif
