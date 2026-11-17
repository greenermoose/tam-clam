/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Builtins run inside the shell. Each gets argv and returns its status. */
#ifndef TAM_CLAM_BUILTINS_H
#define TAM_CLAM_BUILTINS_H

#include "tam-clam/shell.h"

typedef int (*tc_builtin_fn)(struct tc_shell *sh, int argc, char **argv,
			     unsigned long line);

/* NULL when name is not a builtin. */
tc_builtin_fn tc_builtin_find(const char *name);

#endif
