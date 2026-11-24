/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Word expansion (XCU 2.6): tilde expansion, parameter expansion, command
 * substitution, arithmetic expansion, IFS field splitting, pathname
 * expansion, and quote removal. */
#ifndef TAM_CLAM_EXPAND_H
#define TAM_CLAM_EXPAND_H

#include <stdbool.h>
#include <stddef.h>

#include "tam-clam/arena.h"
#include "tam-clam/ast.h"
#include "tam-clam/pattern.h"
#include "tam-clam/shell.h"

enum tc_exp_result {
	TC_EXP_OK = 0,
	TC_EXP_ERR,         /* expansion error (${x:?msg}, readonly assign, arith), reported */
	TC_EXP_NOMEM,       /* out of memory, reported */
	TC_EXP_CMDSUB_FAIL, /* command_sub_errexit: $(...) exited non-zero (sh->cmdsub_status) */
};

/* Expand a command's words into a NULL-terminated argv array in sh->arena.
 * Handles "$@" / unquoted $@/$* multi-word expansion, IFS field splitting,
 * pathname expansion (globbing), and quote removal. */
int tc_expand_words(struct tc_shell *sh, const struct tc_word *words,
		    size_t nwords, char ***out_argv, size_t *out_argc);

/* Expand a single word to one string in sh->arena (for assignment RHS or
 * redirection targets). When is_assign is true, tilde expansion also applies
 * after unquoted ':'. */
int tc_expand_single(struct tc_shell *sh, const struct tc_word *w,
		     bool is_assign, char **out);

/* Expand a single word into a tc_pat in sh->arena preserving per-byte quoting
 * (for case patterns and [[ == ]] RHS). */
int tc_expand_pat(struct tc_shell *sh, const struct tc_word *w,
		  struct tc_pat *out);

/* Expand a single word into a POSIX ERE string in sh->arena for [[ =~ ]]:
 * quoted segments have ERE metacharacters backslash-escaped, unquoted
 * segments are preserved as raw ERE syntax. */
int tc_expand_regex(struct tc_shell *sh, const struct tc_word *w,
		    char **out, size_t *out_len);

/* Expand an unquoted here-document's parts into sh->arena. */
int tc_expand_heredoc(struct tc_shell *sh, const struct tc_wpart *parts,
		      unsigned long line, char **out, size_t *out_len);

/* Quote-removal only for here-document delimiters (no parameter or tilde
 * expansion). Returns NULL on out of memory. */
char *tc_expand_delim(struct tc_arena *a, const struct tc_word *w);

#endif
