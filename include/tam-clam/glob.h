/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pathname expansion (globbing), XCU 2.6.6 / 2.13.3. */
#ifndef TAM_CLAM_GLOB_H
#define TAM_CLAM_GLOB_H

#include <stddef.h>

#include "tam-clam/pattern.h"
#include "tam-clam/shell.h"

#define TC_GLOB_DEPTH_MAX 256

/* Expand pat against the filesystem into a sorted array of strings in
 * sh->arena. Returns TC_OK on success (*out_n == 0 if no paths matched),
 * or TC_ERR_NOMEM on allocation or limit failure. */
int tc_glob_expand(struct tc_shell *sh, const struct tc_pat *pat,
		   unsigned long line, char ***out, size_t *out_n);

#endif
