/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Quote removal, XCU 2.6.7. The lexer has already checked that quotes
 * are balanced and that no other expansion is needed. */
#include "tam-clam/expand.h"

static int dquote_escapable(char c)
{
	return c == '$' || c == '`' || c == '"' || c == '\\' || c == '\n';
}

char *tc_expand_word(struct tc_arena *a, const struct tc_word *w)
{
	/* Removing quotes never makes a word longer. */
	char *out = tc_arena_alloc(a, w->len + 1);
	if (!out)
		return NULL;
	const char *s = w->text, *end = w->text + w->len;
	char *o = out;
	while (s < end) {
		char c = *s++;
		if (c == '\\') {
			if (s < end)
				*o++ = *s++;
		} else if (c == '\'') {
			while (s < end && *s != '\'')
				*o++ = *s++;
			if (s < end)
				s++;
		} else if (c == '"') {
			while (s < end && *s != '"') {
				if (*s == '\\' && s + 1 < end && dquote_escapable(s[1]))
					s++;
				*o++ = *s++;
			}
			if (s < end)
				s++;
		} else {
			*o++ = c;
		}
	}
	*o = '\0';
	return out;
}
