/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fish-style interactive abbreviations (0.3.4).
 * Abbreviations expand in place in the line editor on Space or Enter so
 * history records the real command and Ctrl+Z undoes the expansion. */
#ifndef TAM_CLAM_ABBR_H
#define TAM_CLAM_ABBR_H

#include <stdbool.h>
#include <stddef.h>

struct tc_shell;
struct tc_edit_buf;

enum tc_abbr_pos {
	TC_ABBR_COMMAND = 0,  /* expand only at command position (default) */
	TC_ABBR_ANYWHERE = 1, /* expand at any unquoted token position */
};

struct tc_abbr_entry {
	char *name;
	char *expansion;
	enum tc_abbr_pos pos;
};

struct tc_abbrs {
	struct tc_abbr_entry *slots;
	size_t cap;
	size_t count;
};

void tc_abbr_init(struct tc_abbrs *ab);
void tc_abbr_finish(struct tc_abbrs *ab);

const struct tc_abbr_entry *tc_abbr_get(const struct tc_abbrs *ab, const char *name);
int tc_abbr_set(struct tc_abbrs *ab, const char *name, const char *expansion,
		enum tc_abbr_pos pos);
bool tc_abbr_erase(struct tc_abbrs *ab, const char *name);

/* Inspect the unquoted token immediately preceding buf->cursor. If it matches
 * an abbreviation valid for that position, replace it in place (recording undo)
 * and return true. */
bool tc_abbr_try_expand(const struct tc_abbrs *ab, struct tc_edit_buf *buf);

/* Builtin: abbr [-a|-e|-q|-l|-s] [-p command|anywhere] [NAME [EXPANSION...]]. */
int tc_bi_abbr(struct tc_shell *sh, int argc, char **argv, unsigned long line);

#endif
