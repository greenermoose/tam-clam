/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running commands: builtins in the shell, everything else in a child. */
#ifndef TAM_CLAM_EXEC_H
#define TAM_CLAM_EXEC_H

#include <stddef.h>

#include "tam-clam/ast.h"
#include "tam-clam/shell.h"

/* Maximum bytes captured from one command substitution. */
#define TC_CMDSUB_MAX ((size_t)16 << 20)

/* Run a tree. Sets and returns sh->status. */
int tc_exec(struct tc_shell *sh, const struct tc_node *n);

/* Run cmd in a subshell, capture stdout (stripping trailing newlines) into
 * sh->arena, and record sh->cmdsub_ran / sh->cmdsub_status.
 * Returns TC_OK, TC_ERR_NOMEM, or TC_ERR_IO. */
int tc_exec_cmdsub(struct tc_shell *sh, const struct tc_node *cmd,
		   unsigned long line, char **out, size_t *out_len);

#endif
