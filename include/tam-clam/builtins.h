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

#endif
