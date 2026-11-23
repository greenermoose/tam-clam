/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Linear-time POSIX shell pattern matcher (XCU 2.13.1), shared by pathname
 * expansion, case statements, and ${x#pat}/${x%pat} parameter trimming. */
#ifndef TAM_CLAM_PATTERN_H
#define TAM_CLAM_PATTERN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A shell pattern with per-byte quoting metadata (no in-band escape bytes).
 * quoted[i] == 1 means text[i] is quoted and matches only itself literally;
 * quoted[i] == 0 means text[i] is an active pattern character.
 * quoted may be NULL to treat all bytes as unquoted. */
struct tc_pat {
	const char *text;
	const uint8_t *quoted;
	size_t len;
};

/* Return true if pat matches the entire string str[0..slen). */
bool tc_pat_match(const struct tc_pat *pat, const char *str, size_t slen);

/* Return true if pat contains an unquoted '*', '?', or closed '[...]'. */
bool tc_pat_has_meta(const struct tc_pat *pat);

/* Return the length of the shortest (longest=false) or longest (longest=true)
 * prefix of str[0..slen) matched by pat, or 0 if no prefix matches. */
size_t tc_pat_trim_prefix(const struct tc_pat *pat, const char *str,
			  size_t slen, bool longest);

/* Return the length of the shortest (longest=false) or longest (longest=true)
 * suffix of str[0..slen) matched by pat, or 0 if no suffix matches. */
size_t tc_pat_trim_suffix(const struct tc_pat *pat, const char *str,
			  size_t slen, bool longest);

#endif
