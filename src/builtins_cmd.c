/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Regular POSIX command-discovery and option-parsing builtins:
 * getopts, command, type. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/alias.h"
#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/func.h"
#include "tam-clam/hash.h"
#include "tam-clam/parse.h"
#include "tam-clam/shell.h"
#include "tam-clam/var.h"

/* ---- getopts ---------------------------------------------------------- */

static long parse_optind(const char *s)
{
	if (!s || *s == '\0')
		return 1;
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (end == s || *end != '\0' || errno == ERANGE || v < 1)
		return 1;
	return v;
}

static int set_optind(struct tc_shell *sh, long v, unsigned long line)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", v);
	sh->getopts_optind = v;
	if (tc_var_set(&sh->vars, "OPTIND", buf, 0) != TC_VAR_OK) {
		tc_error(sh, line, "getopts: cannot set OPTIND");
		return -1;
	}
	return 0;
}

int tc_bi_getopts(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (argc - i < 2) {
		tc_error(sh, line, "getopts: usage: getopts optstring name [arg...]");
		return TC_STATUS_USAGE;
	}

	const char *optstr = argv[i];
	const char *varname = argv[i + 1];
	if (!tc_var_valid_name(varname, strlen(varname))) {
		tc_error(sh, line, "getopts: `%s': not a valid identifier", varname);
		return TC_STATUS_USAGE;
	}

	bool silent = (optstr[0] == ':');
	const char *spec = silent ? optstr + 1 : optstr;

	char *const *args;
	size_t nargs;
	if (argc - i > 2) {
		args = argv + i + 2;
		nargs = (size_t)(argc - i - 2);
	} else {
		args = sh->params;
		nargs = sh->nparams;
	}

	long optind = parse_optind(tc_var_get(&sh->vars, "OPTIND"));
	if (optind != sh->getopts_optind || optind == 1) {
		if (optind != sh->getopts_optind)
			sh->getopts_pos = 0;
		sh->getopts_optind = optind;
	}

	if (optind < 1 || (size_t)optind > nargs) {
		sh->getopts_pos = 0;
		(void)tc_var_set(&sh->vars, varname, "?",
				 sh->opt_allexport ? TC_VAR_EXPORT : 0);
		(void)tc_var_unset(&sh->vars, "OPTARG");
		return 1;
	}

	const char *cur = args[(size_t)optind - 1];
	if (sh->getopts_pos == 0) {
		if (cur[0] != '-' || cur[1] == '\0') {
			(void)tc_var_set(&sh->vars, varname, "?",
					 sh->opt_allexport ? TC_VAR_EXPORT : 0);
			(void)tc_var_unset(&sh->vars, "OPTARG");
			return 1;
		}
		if (strcmp(cur, "--") == 0) {
			if (set_optind(sh, optind + 1, line) < 0)
				return 1;
			sh->getopts_pos = 0;
			(void)tc_var_set(&sh->vars, varname, "?",
					 sh->opt_allexport ? TC_VAR_EXPORT : 0);
			(void)tc_var_unset(&sh->vars, "OPTARG");
			return 1;
		}
		sh->getopts_pos = 1;
	}

	char opt = cur[sh->getopts_pos++];
	char opt_s[2] = {opt, '\0'};
	unsigned vfl = sh->opt_allexport ? TC_VAR_EXPORT : 0;

	const char *found = (opt != ':') ? strchr(spec, opt) : NULL;
	if (!found) {
		if (cur[sh->getopts_pos] == '\0') {
			sh->getopts_pos = 0;
			optind++;
		}
		if (set_optind(sh, optind, line) < 0)
			return 1;
		if (tc_var_set(&sh->vars, varname, "?", vfl) != TC_VAR_OK) {
			tc_error(sh, line, "getopts: cannot set %s", varname);
			return 1;
		}
		if (silent) {
			(void)tc_var_set(&sh->vars, "OPTARG", opt_s, 0);
		} else {
			tc_error(sh, line, "illegal option -- %c", opt);
			(void)tc_var_unset(&sh->vars, "OPTARG");
		}
		return 0;
	}

	if (found[1] == ':') {
		const char *optarg = NULL;
		if (cur[sh->getopts_pos] != '\0') {
			optarg = cur + sh->getopts_pos;
			sh->getopts_pos = 0;
			optind++;
		} else if ((size_t)optind < nargs) {
			optarg = args[(size_t)optind];
			sh->getopts_pos = 0;
			optind += 2;
		} else {
			sh->getopts_pos = 0;
			optind++;
			if (set_optind(sh, optind, line) < 0)
				return 1;
			if (silent) {
				if (tc_var_set(&sh->vars, varname, ":", vfl) != TC_VAR_OK)
					return 1;
				(void)tc_var_set(&sh->vars, "OPTARG", opt_s, 0);
			} else {
				tc_error(sh, line, "option requires an argument -- %c", opt);
				if (tc_var_set(&sh->vars, varname, "?", vfl) != TC_VAR_OK)
					return 1;
				(void)tc_var_unset(&sh->vars, "OPTARG");
			}
			return 0;
		}
		if (set_optind(sh, optind, line) < 0)
			return 1;
		if (tc_var_set(&sh->vars, varname, opt_s, vfl) != TC_VAR_OK) {
			tc_error(sh, line, "getopts: cannot set %s", varname);
			return 1;
		}
		(void)tc_var_set(&sh->vars, "OPTARG", optarg, 0);
		return 0;
	}

	if (cur[sh->getopts_pos] == '\0') {
		sh->getopts_pos = 0;
		optind++;
	}
	if (set_optind(sh, optind, line) < 0)
		return 1;
	if (tc_var_set(&sh->vars, varname, opt_s, vfl) != TC_VAR_OK) {
		tc_error(sh, line, "getopts: cannot set %s", varname);
		return 1;
	}
	(void)tc_var_unset(&sh->vars, "OPTARG");
	return 0;
}

/* ---- command & type --------------------------------------------------- */

static int write_str_nl(const char *s)
{
	if (tc_write_all(STDOUT_FILENO, s, strlen(s)) < 0 ||
	    tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
		return -1;
	return 0;
}

static bool is_exec_file(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && !S_ISDIR(st.st_mode) &&
	       access(path, X_OK) == 0;
}

/* Describe one command name for `command -v` (verbose=false) or
 * `command -V` / `type` (verbose=true). Returns 0 if found, 1 if not found,
 * -1 on write error. */
static int describe_command(struct tc_shell *sh, const char *name,
			    bool default_path, bool verbose,
			    bool report_notfound, const char *caller,
			    unsigned long line)
{
	const char *aval = tc_alias_get(&sh->aliases, name);
	if (aval) {
		if (verbose) {
			if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, " is aliased to ", 15) < 0 ||
			    tc_write_shell_quoted(STDOUT_FILENO, aval) < 0 ||
			    tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
				return -1;
		} else {
			if (tc_write_all(STDOUT_FILENO, "alias ", 6) < 0 ||
			    tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, "=", 1) < 0 ||
			    tc_write_shell_quoted(STDOUT_FILENO, aval) < 0 ||
			    tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
				return -1;
		}
		return 0;
	}

	if (tc_is_reserved_word(name)) {
		if (verbose) {
			if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, " is a shell keyword\n", 20) < 0)
				return -1;
		} else {
			if (write_str_nl(name) < 0)
				return -1;
		}
		return 0;
	}

	const struct tc_builtin *bi = tc_builtin_find(name);
	if (bi && bi->special) {
		if (verbose) {
			if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, " is a special shell builtin\n", 28) < 0)
				return -1;
		} else {
			if (write_str_nl(name) < 0)
				return -1;
		}
		return 0;
	}

	if (tc_func_find(&sh->funcs, name)) {
		if (verbose) {
			if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, " is a shell function\n", 21) < 0)
				return -1;
		} else {
			if (write_str_nl(name) < 0)
				return -1;
		}
		return 0;
	}

	if (bi) {
		if (verbose) {
			if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, " is a shell builtin\n", 20) < 0)
				return -1;
		} else {
			if (write_str_nl(name) < 0)
				return -1;
		}
		return 0;
	}

	if (strchr(name, '/')) {
		if (is_exec_file(name)) {
			if (verbose) {
				if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
				    tc_write_all(STDOUT_FILENO, " is ", 4) < 0 ||
				    write_str_nl(name) < 0)
					return -1;
			} else {
				if (write_str_nl(name) < 0)
					return -1;
			}
			return 0;
		}
	} else {
		const char *cached = NULL;
		if (!default_path) {
			tc_hash_sync(&sh->hash, sh->vars.path_gen);
			cached = tc_hash_get(&sh->hash, name, false);
		}
		char *found = NULL;
		const char *path = cached;
		if (!path) {
			found = tc_exec_find_in_path(sh, name, default_path);
			path = found;
		}
		if (path) {
			int w = 0;
			if (verbose) {
				if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
				    tc_write_all(STDOUT_FILENO, " is ", 4) < 0 ||
				    write_str_nl(path) < 0)
					w = -1;
			} else {
				w = write_str_nl(path);
			}
			free(found);
			return w;
		}
	}

	if (report_notfound)
		tc_error(sh, line, "%s: %s: not found", caller, name);
	return 1;
}

int tc_bi_command(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool default_path = false;
	bool opt_v = false;
	bool opt_V = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'p') {
				default_path = true;
			} else if (*p == 'v') {
				opt_v = true;
				opt_V = false;
			} else if (*p == 'V') {
				opt_V = true;
				opt_v = false;
			} else {
				tc_error(sh, line, "command: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}

	if (i >= argc)
		return 0;

	if (opt_v || opt_V) {
		int status = 0;
		for (; i < argc; i++) {
			int r = describe_command(sh, argv[i], default_path, opt_V,
						 opt_V, "command", line);
			if (r < 0) {
				tc_error(sh, line, "command: write error: %s",
					 strerror(errno));
				return 1;
			}
			if (r > 0)
				status = 1;
		}
		return status;
	}

	return tc_exec_command_builtin(sh, argc - i, argv + i, default_path, line);
}

int tc_bi_type(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	int status = 0;
	for (; i < argc; i++) {
		int r = describe_command(sh, argv[i], false, true, true, "type", line);
		if (r < 0) {
			tc_error(sh, line, "type: write error: %s", strerror(errno));
			return 1;
		}
		if (r > 0)
			status = 1;
	}
	return status;
}
