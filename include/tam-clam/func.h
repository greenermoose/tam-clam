/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell function table. Each function owns its deep-cloned AST in a private
 * arena and is reference-counted so a running function can safely redefine
 * or unset itself without use-after-free. */
#ifndef TAM_CLAM_FUNC_H
#define TAM_CLAM_FUNC_H

#include <stddef.h>

#include "tam-clam/arena.h"
#include "tam-clam/ast.h"

struct tc_func {
	char *name;
	struct tc_arena arena;
	struct tc_node *body;
	size_t refcount;
};

struct tc_funcs {
	struct tc_func **slots;
	size_t cap;
	size_t count;
};

void tc_funcs_init(struct tc_funcs *ft);
void tc_funcs_finish(struct tc_funcs *ft);

/* Define or replace function name with a deep clone of body.
 * Returns 0 on success, -1 on out of memory or depth overflow. */
int tc_func_define(struct tc_funcs *ft, const char *name,
		   const struct tc_node *body);

/* Look up function by name; NULL if not defined. */
struct tc_func *tc_func_find(const struct tc_funcs *ft, const char *name);

/* Remove function by name (no-op if absent). */
void tc_func_unset(struct tc_funcs *ft, const char *name);

/* Return a heap-allocated, sorted array of function names. Caller frees *out. */
int tc_funcs_list(const struct tc_funcs *ft, const char ***out, size_t *out_n);

void tc_func_ref(struct tc_func *fn);
void tc_func_unref(struct tc_func *fn);

#endif
