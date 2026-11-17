/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running commands: builtins in the shell, everything else in a child. */
#ifndef TAM_CLAM_EXEC_H
#define TAM_CLAM_EXEC_H

#include "tam-clam/ast.h"
#include "tam-clam/shell.h"

/* Run a tree. Sets and returns sh->status. */
int tc_exec(struct tc_shell *sh, const struct tc_node *n);

#endif
