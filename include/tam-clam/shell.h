/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The shell's state. One tc_shell is passed to every function that needs
 * it; there is no other global state except the signal flags in
 * signals.h. */
#ifndef TAM_CLAM_SHELL_H
#define TAM_CLAM_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "tam-clam/arena.h"
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

struct tc_shell {
	const char *name;          /* $0, used in messages */
	char self_path[TC_SELF_PATH_MAX]; /* this executable, for ENOEXEC */
	bool interactive;
	int status;                /* status of the last command ($?) */
	bool exit_requested;       /* the exit builtin or fatal error ran */
	int exit_status;
	pid_t root_pid;            /* $$: main shell PID */
	pid_t last_bg_pid;         /* $!: 0 until background jobs in 0.1.1 */
	struct tc_vars vars;       /* shell variables and environment */
	char **params;             /* positional parameters $1..$N */
	size_t nparams;
	struct tc_arena arena;     /* memory for the command being run */
	struct tc_input *in;       /* the input being read, for line numbers */
};

void tc_shell_init(struct tc_shell *sh, const char *name);
void tc_shell_finish(struct tc_shell *sh);

/* Replace positional parameters ($1..$N) with heap copies of argv[0..n). */
int tc_shell_set_params(struct tc_shell *sh, int n, char *const *argv);

/* Report an error on standard error as "name: line N: message". line 0
 * leaves the line out. Never fails loudly: a failed write is ignored. */
void tc_error(const struct tc_shell *sh, unsigned long line, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* Write all of buf to fd, retrying short writes and EINTR. 0 or -1. */
int tc_write_all(int fd, const void *buf, size_t len);

#endif
