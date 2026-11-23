/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell alias table and the alias / unalias builtins. */
#ifndef TAM_CLAM_ALIAS_H
#define TAM_CLAM_ALIAS_H

#include <stdbool.h>
#include <stddef.h>

struct tc_shell;

struct tc_alias_entry {
	char *name;
	char *value;
};

struct tc_aliases {
	struct tc_alias_entry *slots;
	size_t cap;
	size_t count;
};

void tc_alias_init(struct tc_aliases *al);
void tc_alias_finish(struct tc_aliases *al);
void tc_alias_clear(struct tc_aliases *al);

bool tc_alias_valid_name(const char *name);
const char *tc_alias_get(const struct tc_aliases *al, const char *name);
int tc_alias_set(struct tc_aliases *al, const char *name, const char *value);
bool tc_alias_unset(struct tc_aliases *al, const char *name);

/* Return a heap-allocated, name-sorted array of pointers to alias entries.
 * Caller frees *out. Returns 0 on success, -1 on OOM. */
int tc_alias_list(const struct tc_aliases *al,
		  const struct tc_alias_entry ***out, size_t *out_n);

int tc_bi_alias(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_unalias(struct tc_shell *sh, int argc, char **argv, unsigned long line);

#endif
