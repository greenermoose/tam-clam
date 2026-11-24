/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Live syntax highlighting (0.2.4) backed by the real parser, per-prompt
 * command-existence cache, and dimmed history suggestions. */
#ifndef TAM_CLAM_HIGHLIGHT_H
#define TAM_CLAM_HIGHLIGHT_H

#include <stdbool.h>
#include <stddef.h>

struct tc_shell;

#define TC_HL_CACHE_MAX 64

enum tc_hl_kind {
	TC_HL_NONE = 0,
	TC_HL_CMD_OK,    /* known builtin, alias, function, or PATH command */
	TC_HL_CMD_BAD,   /* unknown command */
	TC_HL_KEYWORD,   /* POSIX reserved word (if, then, for, while, ...) */
	TC_HL_STRING,    /* quoted string */
	TC_HL_VAR,       /* $var, ${...}, $((...)) */
	TC_HL_OP,        /* |, &&, ||, ;, &, redirections */
	TC_HL_COMMENT,   /* # comment */
	TC_HL_ERROR,     /* unterminated quote or real-parser syntax error */
};

struct tc_hl_cache_entry {
	char name[64];
	bool exists;
};

struct tc_hl_cache {
	struct tc_hl_cache_entry entries[TC_HL_CACHE_MAX];
	size_t count;
};

void tc_hl_cache_clear(struct tc_hl_cache *c);

/* Fill styles[0..len) with enum tc_hl_kind values for buf[0..len).
 * Uses cache to avoid repeated PATH/stat lookups per prompt, and runs a
 * silent pass of the real parser to highlight unexpected-token syntax errors. */
void tc_highlight_line(struct tc_shell *sh, struct tc_hl_cache *cache,
		       const char *buf, size_t len, unsigned char *styles);

/* Return pointer to the suffix of the most recent history entry that strictly
 * starts with buf[0..len), or NULL if none. Pointer stays valid until the
 * next history mutation. */
const char *tc_suggest_history(struct tc_shell *sh, const char *buf, size_t len);

#endif
