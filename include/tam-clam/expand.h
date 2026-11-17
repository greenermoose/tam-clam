/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Word expansion (XCU 2.6). In 0.0.1 this is quote removal only; the
 * parser refuses words that would need more. */
#ifndef TAM_CLAM_EXPAND_H
#define TAM_CLAM_EXPAND_H

#include "tam-clam/ast.h"
#include "tam-clam/arena.h"

/* The word with its quotes removed, NUL-terminated, in the arena. NULL when
 * out of memory. */
char *tc_expand_word(struct tc_arena *a, const struct tc_word *w);

#endif
