/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX arithmetic expansion evaluator ($((...))), XCU 2.6.4.
 * Uses 64-bit signed integers with checked overflow and explicit recursion
 * depth limit. */
#ifndef TAM_CLAM_ARITH_H
#define TAM_CLAM_ARITH_H

#include <stdint.h>

#include "tam-clam/shell.h"

#define TC_ARITH_DEPTH_MAX 128

enum tc_arith_result {
	TC_ARITH_OK = 0,
	TC_ARITH_ERR,    /* syntax error, division by zero, overflow, readonly */
	TC_ARITH_NOMEM,
};

/* Evaluate expr using sh->vars for variable reads and assignments.
 * Errors are reported via tc_error(sh, line, ...). */
int tc_arith_eval(struct tc_shell *sh, const char *expr, unsigned long line,
		  int64_t *out);

#endif
