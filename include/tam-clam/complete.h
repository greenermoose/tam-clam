/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Context-aware completion (0.2.3): commands, files/directories, shell
 * variables ($VAR / ${VAR}), user names (~user), and lazy per-command
 * declarative option/subcommand files (*.comp). */
#ifndef TAM_CLAM_COMPLETE_H
#define TAM_CLAM_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>

struct tc_shell;

#define TC_COMP_MAX 512

struct tc_comp_item {
	char *text;      /* replacement text for the active token */
	char *desc;      /* optional description (for options/subcommands), or NULL */
	bool append_sp;  /* append trailing space on unique acceptance */
};

struct tc_comp_list {
	struct tc_comp_item *items;
	size_t count;
	size_t cap;
	size_t tok_start; /* byte offset in buffer where token begins */
	size_t tok_end;   /* byte offset in buffer where token ends (cursor) */
};

void tc_comp_list_init(struct tc_comp_list *cl);
void tc_comp_list_finish(struct tc_comp_list *cl);

/* Populate cl with sorted, deduplicated completions for buf[0..len) at cursor.
 * Returns 0 on success, -1 on OOM. */
int tc_complete(struct tc_shell *sh, const char *buf, size_t len,
		size_t cursor, struct tc_comp_list *cl);

/* Length of the longest common prefix across all items in cl. */
size_t tc_comp_common_prefix(const struct tc_comp_list *cl);

#endif
