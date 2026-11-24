/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Linear-time POSIX Extended Regular Expression (ERE) engine using a
 * Thompson NFA / PikeVM simulation. No external libraries, no exponential
 * backtracking, bounded compilation and execution. */
#ifndef TAM_CLAM_REGEX_H
#define TAM_CLAM_REGEX_H

#include <stddef.h>

#define TC_RE_GROUPS_MAX 32
#define TC_RE_DEPTH_MAX  64
#define TC_RE_REPEAT_MAX 255
#define TC_RE_INSN_MAX   8192

enum tc_re_result {
	TC_RE_NOMATCH    =  0,
	TC_RE_MATCH      =  1,
	TC_RE_ERR_SYNTAX = -1,
	TC_RE_ERR_LIMIT  = -2,
	TC_RE_ERR_NOMEM  = -3,
};

struct tc_re_match {
	size_t start; /* SIZE_MAX if group did not participate in match */
	size_t end;
};

/* Compile POSIX ERE pattern[0..pat_len) and search for the leftmost match
 * in str[0..str_len). On TC_RE_MATCH, fills groups[0..*out_ngroups) (clamped
 * to ngroups), where group 0 is the entire match and 1..k are parenthesized
 * subexpressions. */
int tc_re_exec(const char *pattern, size_t pat_len,
	       const char *str, size_t str_len,
	       struct tc_re_match *groups, size_t ngroups,
	       size_t *out_ngroups);

#endif
