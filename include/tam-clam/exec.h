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

/* Spawn a process substitution <(...) (is_out == false) or >(...) (is_out == true),
 * register its fd/pid in sh->procsubs, and return "/dev/fd/N" in sh->arena. */
int tc_exec_procsub(struct tc_shell *sh, const struct tc_node *cmd,
		    bool is_out, unsigned long line, char **out_path);

/* Close parent fds and wait for process substitutions opened since mark. */
void tc_procsub_cleanup(struct tc_shell *sh, size_t mark);

/* Find an executable regular file in PATH (or default POSIX PATH when
 * default_path is true). Returns a heap-allocated path or NULL. */
char *tc_exec_find_in_path(struct tc_shell *sh, const char *name,
			   bool default_path);

/* Execute a simple command bypassing shell functions (used by `command`).
 * When default_path is true, uses the default POSIX PATH. */
int tc_exec_command_builtin(struct tc_shell *sh, int argc, char **argv,
			    bool default_path, unsigned long line);

/* Apply a single assignment (scalar, subscripted, or compound array) into
 * sh->vars. Returns 0 on success or an exit status on error. */
int tc_exec_apply_assign(struct tc_shell *sh, const struct tc_assign *a,
			 unsigned extra_flags);

#endif
