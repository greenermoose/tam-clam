/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Static lint mode (`tam-clam --lint FILE...`): parses without running and
 * warns about legal but risky POSIX shell constructs. */
#ifndef TAM_CLAM_LINT_H
#define TAM_CLAM_LINT_H

#include <stddef.h>

#include "tam-clam/shell.h"

/* Lint a single file (or stdin when path is "-").
 * Returns 0 if clean, 1 if warnings were emitted, 2 on syntax or I/O error. */
int tc_lint_file(struct tc_shell *sh, const char *path);

/* Lint a source buffer directly (for unit tests). Writes warnings to out_fd
 * (or suppresses output when out_fd < 0) and sets *out_warnings if non-NULL.
 * Returns 0 if clean, 1 if warnings, 2 on syntax/OOM error. */
int tc_lint_string(struct tc_shell *sh, const char *filename,
		   const char *src, size_t len, int out_fd,
		   size_t *out_warnings);

#endif
