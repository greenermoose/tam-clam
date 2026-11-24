/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Builtins run inside the shell. Each gets argv and returns its status. */
#ifndef TAM_CLAM_BUILTINS_H
#define TAM_CLAM_BUILTINS_H

#include <stdbool.h>

#include "tam-clam/shell.h"

typedef int (*tc_builtin_fn)(struct tc_shell *sh, int argc, char **argv,
			     unsigned long line);

struct tc_builtin {
	const char *name;
	tc_builtin_fn fn;
	bool special; /* POSIX special builtin: prefix assignments persist */
};

/* NULL when name is not a builtin. */
const struct tc_builtin *tc_builtin_find(const char *name);
/* Iterate builtins by index; NULL when idx is past the end. */
const struct tc_builtin *tc_builtin_at(size_t idx);

/* Special and process-control builtins implemented in builtins_special.c. */
int tc_bi_set(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_shift(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_eval(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_dot(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_export(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_readonly(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_unset(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_exec_stub(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_times(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_wait(struct tc_shell *sh, int argc, char **argv, unsigned long line);

/* Regular POSIX builtins (0.1.2). */
int tc_bi_test(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_printf(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_echo(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_pwd(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_umask(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_read(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_kill(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_ulimit(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_getopts(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_command(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_type(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_bind(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_history(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_jobs(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_fg(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_bg(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_declare(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_local(struct tc_shell *sh, int argc, char **argv, unsigned long line);

/* Execute a declaration builtin (declare/typeset/readonly/export) that has
 * compound array assignment arguments (decl_assign). */
int tc_exec_decl_builtin(struct tc_shell *sh, const char *cmd,
			 const struct tc_word *words, size_t nwords,
			 unsigned long line);

/* Write s in POSIX single-quoted form to fd. 0 or -1. */
int tc_write_shell_quoted(int fd, const char *s);
/* Write an indexed or associative array value as ([k]='v' ...). 0 or -1. */
int tc_write_array_value(int fd, const struct tc_var_entry *e);

#endif
