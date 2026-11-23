/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running commands: builtins in the shell, everything else in a child. */
#ifndef TAM_CLAM_EXEC_H
#define TAM_CLAM_EXEC_H

#include <stddef.h>

#include "tam-clam/ast.h"
#include "tam-clam/input.h"
#include "tam-clam/shell.h"

/* Maximum bytes captured from one command substitution. */
#define TC_CMDSUB_MAX ((size_t)16 << 20)

/* Run a tree. Sets and returns sh->status. */
int tc_exec(struct tc_shell *sh, const struct tc_node *n);

/* Parse and execute all commands from input in the current shell using
 * arena marks so enclosing ASTs remain intact. Sets *ran_any if at least
 * one command ran. Returns exit status (or TC_STATUS_USAGE on syntax error). */
int tc_exec_input(struct tc_shell *sh, struct tc_input *in,
		  unsigned long call_line, bool *ran_any);

/* Convenience wrapper around tc_exec_input for a string (eval, traps). */
int tc_exec_string(struct tc_shell *sh, const char *src, size_t len,
		   unsigned long call_line, bool *ran_any);

/* Run cmd in a subshell, capture stdout (stripping trailing newlines) into
 * sh->arena, and record sh->cmdsub_ran / sh->cmdsub_status.
 * Returns TC_OK, TC_ERR_NOMEM, or TC_ERR_IO. */
int tc_exec_cmdsub(struct tc_shell *sh, const struct tc_node *cmd,
		   unsigned long line, char **out, size_t *out_len);

#endif
