/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX shell options: -a/-C/-e/-f/-n/-u/-x and -o pipefail. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/options.h"
#include "tam-clam/shell.h"

static bool is_deferred_char(char c)
{
	return c == 'v' || c == 'm' || c == 'b' || c == 'h';
}

static bool is_deferred_name(const char *name)
{
	return strcmp(name, "verbose") == 0 ||
	       strcmp(name, "monitor") == 0 ||
	       strcmp(name, "notify") == 0 ||
	       strcmp(name, "ignoreeof") == 0 ||
	       strcmp(name, "vi") == 0 ||
	       strcmp(name, "emacs") == 0;
}

enum tc_opt_res tc_opt_set_char(struct tc_shell *sh, char c, bool enable)
{
	switch (c) {
	case 'a': sh->opt_allexport = enable; return TC_OPT_OK;
	case 'C': sh->opt_noclobber = enable; return TC_OPT_OK;
	case 'e': sh->opt_errexit = enable;   return TC_OPT_OK;
	case 'f': sh->opt_noglob = enable;    return TC_OPT_OK;
	case 'n': sh->opt_noexec = enable;    return TC_OPT_OK;
	case 'u': sh->opt_nounset = enable;   return TC_OPT_OK;
	case 'x': sh->opt_xtrace = enable;    return TC_OPT_OK;
	default:
		return is_deferred_char(c) ? TC_OPT_DEFERRED : TC_OPT_INVALID;
	}
}

enum tc_opt_res tc_opt_set_name(struct tc_shell *sh, const char *name,
				bool enable)
{
	if (strcmp(name, "allexport") == 0) {
		sh->opt_allexport = enable;
		return TC_OPT_OK;
	}
	if (strcmp(name, "errexit") == 0) {
		sh->opt_errexit = enable;
		return TC_OPT_OK;
	}
	if (strcmp(name, "noclobber") == 0) {
		sh->opt_noclobber = enable;
		return TC_OPT_OK;
	}
	if (strcmp(name, "noexec") == 0) {
		sh->opt_noexec = enable;
		return TC_OPT_OK;
	}
	if (strcmp(name, "noglob") == 0) {
		sh->opt_noglob = enable;
		return TC_OPT_OK;
	}
	if (strcmp(name, "nounset") == 0) {
		sh->opt_nounset = enable;
		return TC_OPT_OK;
	}
	if (strcmp(name, "pipefail") == 0) {
		sh->opt_pipefail = enable;
		return TC_OPT_OK;
	}
	if (strcmp(name, "xtrace") == 0) {
		sh->opt_xtrace = enable;
		return TC_OPT_OK;
	}
	return is_deferred_name(name) ? TC_OPT_DEFERRED : TC_OPT_INVALID;
}

int tc_opt_print(const struct tc_shell *sh, bool reusable, unsigned long line)
{
	static const char *const names[] = {
		"allexport", "errexit", "noclobber", "noexec",
		"noglob", "nounset", "pipefail", "xtrace",
	};
	const bool vals[] = {
		sh->opt_allexport, sh->opt_errexit, sh->opt_noclobber,
		sh->opt_noexec, sh->opt_noglob, sh->opt_nounset,
		sh->opt_pipefail, sh->opt_xtrace,
	};
	char buf[128];
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		int n;
		if (reusable)
			n = snprintf(buf, sizeof(buf), "set %co %s\n",
				     vals[i] ? '-' : '+', names[i]);
		else
			n = snprintf(buf, sizeof(buf), "%-16s%s\n",
				     names[i], vals[i] ? "on" : "off");
		if (n < 0 || tc_write_all(STDOUT_FILENO, buf, (size_t)n) < 0) {
			tc_error(sh, line, "set: write error: %s", strerror(errno));
			return 1;
		}
	}
	return 0;
}

size_t tc_opt_format_flags(const struct tc_shell *sh, char *buf, size_t cap)
{
	if (cap == 0)
		return 0;
	size_t n = 0;
	if (sh->opt_allexport && n + 1 < cap) buf[n++] = 'a';
	if (sh->opt_noclobber && n + 1 < cap) buf[n++] = 'C';
	if (sh->opt_errexit   && n + 1 < cap) buf[n++] = 'e';
	if (sh->opt_noglob    && n + 1 < cap) buf[n++] = 'f';
	if (sh->interactive   && n + 1 < cap) buf[n++] = 'i';
	if (sh->opt_noexec    && n + 1 < cap) buf[n++] = 'n';
	if (sh->opt_nounset   && n + 1 < cap) buf[n++] = 'u';
	if (sh->opt_xtrace    && n + 1 < cap) buf[n++] = 'x';
	buf[n] = '\0';
	return n;
}
