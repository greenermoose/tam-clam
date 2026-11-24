/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX shell options shared by main.c (CLI flags), set builtin, and $-. */
#ifndef TAM_CLAM_OPTIONS_H
#define TAM_CLAM_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

struct tc_shell;

enum tc_opt_res {
	TC_OPT_OK = 0,
	TC_OPT_DEFERRED, /* valid POSIX option not implemented yet */
	TC_OPT_INVALID,  /* unknown option */
};

/* Set or clear a single-letter option (-a, -C, -e, -f, -n, -u, -x). */
enum tc_opt_res tc_opt_set_char(struct tc_shell *sh, char c, bool enable);

/* Set or clear a named option (-o allexport, errexit, noclobber, noexec,
 * noglob, nounset, pipefail, xtrace). */
enum tc_opt_res tc_opt_set_name(struct tc_shell *sh, const char *name,
				bool enable);

/* Print all options to stdout: reusable == false for `set -o`,
 * reusable == true for `set +o`. Returns 0 or 1 on write error. */
int tc_opt_print(const struct tc_shell *sh, bool reusable, unsigned long line);

/* Format active single-letter flags for `$-` into buf[0..cap).
 * Returns the number of characters written (excluding NUL). */
size_t tc_opt_format_flags(const struct tc_shell *sh, char *buf, size_t cap);

#endif
