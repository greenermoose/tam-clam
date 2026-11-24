/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Brace expansion ({a,b}, {1..9}, {a..z}, nested braces) operating on
 * tc_wpart lists before tilde, parameter, and pathname expansion. */
#ifndef TAM_CLAM_BRACE_H
#define TAM_CLAM_BRACE_H

#include <stddef.h>

#include "tam-clam/ast.h"
#include "tam-clam/shell.h"

#define TC_BRACE_DEPTH_MAX 32
#define TC_BRACE_WORDS_MAX 4096

/* Expand unquoted brace expressions in parts into *out_words[0..*out_nwords),
 * allocated in sh->arena. Returns TC_OK, TC_ERR_NOMEM (or limit exceeded). */
int tc_brace_expand(struct tc_shell *sh, const struct tc_wpart *parts,
		    unsigned long line,
		    struct tc_wpart ***out_words, size_t *out_nwords);

#endif
