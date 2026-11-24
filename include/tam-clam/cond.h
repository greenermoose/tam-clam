/* SPDX-License-Identifier: GPL-3.0-or-later */
/* [[ ... ]] conditional compound command evaluator. */
#ifndef TAM_CLAM_COND_H
#define TAM_CLAM_COND_H

#include "tam-clam/ast.h"
#include "tam-clam/shell.h"

/* Evaluate a TC_NODE_COND node. Returns 0 (true), 1 (false), or 2 (error). */
int tc_exec_cond(struct tc_shell *sh, const struct tc_node *node);

#endif
