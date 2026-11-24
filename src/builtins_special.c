/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX special builtins (set, shift, eval, ., export, readonly, unset,
 * exec stub, times) and wait. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>

#include "tam-clam/bg.h"
#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/input.h"
#include "tam-clam/options.h"
#include "tam-clam/shell.h"
#include "tam-clam/trap.h"
#include "tam-clam/var.h"

static int special_fail(struct tc_shell *sh, int status)
{
	if (!sh->interactive && !sh->in_command_builtin) {
		sh->exit_requested = true;
		sh->exit_status = status;
	}
	return status;
}

int tc_write_shell_quoted(int fd, const char *s)
{
	if (tc_write_all(fd, "'", 1) < 0)
		return -1;
	const char *seg = s;
	for (const char *p = s; *p; p++) {
		if (*p == '\'') {
			if (p > seg && tc_write_all(fd, seg, (size_t)(p - seg)) < 0)
				return -1;
			if (tc_write_all(fd, "'\\''", 4) < 0)
				return -1;
			seg = p + 1;
		}
	}
	if (*seg && tc_write_all(fd, seg, strlen(seg)) < 0)
		return -1;
	return tc_write_all(fd, "'", 1);
}

/* ---- set -------------------------------------------------------------- */

static int print_all_vars(struct tc_shell *sh, unsigned long line)
{
	const struct tc_var_entry **list = NULL;
	size_t n = 0;
	if (tc_var_list(&sh->vars, 0, &list, &n) != TC_VAR_OK) {
		tc_error(sh, line, "set: out of memory");
		return 1;
	}
	int status = 0;
	for (size_t i = 0; i < n; i++) {
		const struct tc_var_entry *e = list[i];
		if (tc_write_all(STDOUT_FILENO, e->name, strlen(e->name)) < 0 ||
		    tc_write_all(STDOUT_FILENO, "=", 1) < 0 ||
		    tc_write_shell_quoted(STDOUT_FILENO, e->value) < 0 ||
		    tc_write_all(STDOUT_FILENO, "\n", 1) < 0) {
			tc_error(sh, line, "set: write error: %s", strerror(errno));
			status = 1;
			break;
		}
	}
	free(list);
	return status;
}

int tc_bi_set(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	if (argc == 1)
		return print_all_vars(sh, line);

	bool set_params = false;
	int i = 1;
	for (; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--") == 0) {
			set_params = true;
			i++;
			break;
		}
		if (strcmp(a, "-") == 0) {
			/* POSIX obsoleted form: set - [args...] */
			i++;
			if (i < argc)
				set_params = true;
			break;
		}
		if ((a[0] != '-' && a[0] != '+') || a[1] == '\0') {
			set_params = true;
			break;
		}
		bool enable = (a[0] == '-');
		for (const char *p = a + 1; *p; p++) {
			if (*p == 'o') {
				const char *optname = p[1] ? p + 1 : NULL;
				if (!optname && i + 1 < argc)
					optname = argv[++i];
				if (!optname) {
					if (tc_opt_print(sh, !enable, line) != 0)
						return 1;
					break;
				}
				enum tc_opt_res or = tc_opt_set_name(sh, optname, enable);
				if (or == TC_OPT_DEFERRED) {
					tc_error(sh, line,
						 "set: %co %s: option not supported yet; see ROADMAP.md",
						 enable ? '-' : '+', optname);
					return special_fail(sh, TC_STATUS_USAGE);
				}
				if (or == TC_OPT_INVALID) {
					tc_error(sh, line,
						 "set: %co %s: invalid option name",
						 enable ? '-' : '+', optname);
					return special_fail(sh, TC_STATUS_USAGE);
				}
				break;
			}
			enum tc_opt_res or = tc_opt_set_char(sh, *p, enable);
			if (or == TC_OPT_DEFERRED) {
				tc_error(sh, line,
					 "set: %c%c: option not supported yet; see ROADMAP.md",
					 enable ? '-' : '+', *p);
				return special_fail(sh, TC_STATUS_USAGE);
			}
			if (or == TC_OPT_INVALID) {
				tc_error(sh, line, "set: %c%c: invalid option",
					 enable ? '-' : '+', *p);
				return special_fail(sh, TC_STATUS_USAGE);
			}
		}
	}

	if (set_params) {
		if (tc_shell_set_params(sh, argc - i, argv + i) != TC_OK) {
			tc_error(sh, line, "set: out of memory");
			return special_fail(sh, 1);
		}
	}
	return 0;
}

/* ---- shift ------------------------------------------------------------ */

static int parse_nonneg_size(const char *s, size_t *out)
{
	if (!s || *s == '\0')
		return -1;
	size_t v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		size_t d = (size_t)(*p - '0');
		if (v > (SIZE_MAX - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	*out = v;
	return 0;
}

int tc_bi_shift(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (argc - i > 1) {
		tc_error(sh, line, "shift: too many arguments");
		return special_fail(sh, 1);
	}
	size_t n = 1;
	if (i < argc && parse_nonneg_size(argv[i], &n) < 0) {
		tc_error(sh, line, "shift: %s: a non-negative integer is needed",
			 argv[i]);
		return special_fail(sh, TC_STATUS_USAGE);
	}
	if (n > sh->nparams) {
		tc_error(sh, line, "shift: can't shift that many");
		return special_fail(sh, 1);
	}
	tc_shell_shift_params(sh, n);
	return 0;
}

/* ---- eval ------------------------------------------------------------- */

int tc_bi_eval(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (i >= argc)
		return 0;

	if (sh->eval_depth >= TC_EVAL_DEPTH_MAX) {
		tc_error(sh, line, "eval: maximum nesting depth exceeded (%d)",
			 TC_EVAL_DEPTH_MAX);
		return special_fail(sh, 1);
	}

	size_t total = 0;
	for (int j = i; j < argc; j++) {
		size_t l = strlen(argv[j]);
		if (l > SIZE_MAX - total - 2) {
			tc_error(sh, line, "eval: out of memory");
			return special_fail(sh, 1);
		}
		total += l + (j > i ? 1 : 0);
	}
	if (total == 0)
		return 0;

	char *buf = malloc(total + 1);
	if (!buf) {
		tc_error(sh, line, "eval: out of memory");
		return special_fail(sh, 1);
	}
	size_t pos = 0;
	for (int j = i; j < argc; j++) {
		if (j > i)
			buf[pos++] = ' ';
		size_t l = strlen(argv[j]);
		memcpy(buf + pos, argv[j], l);
		pos += l;
	}
	buf[pos] = '\0';

	sh->eval_depth++;
	bool ran_any = false;
	int st = tc_exec_string(sh, buf, pos, line, &ran_any);
	sh->eval_depth--;
	free(buf);
	return ran_any ? st : (st != 0 ? st : 0);
}

/* ---- . (dot) ---------------------------------------------------------- */

static int open_dot_candidate(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
		close(fd);
		errno = S_ISDIR(st.st_mode) ? EISDIR : errno;
		return -1;
	}
	int high = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	if (high >= 0) {
		close(fd);
		fd = high;
	}
	return fd;
}

static int find_dot_file(struct tc_shell *sh, const char *name,
			 unsigned long line)
{
	if (strchr(name, '/')) {
		int fd = open_dot_candidate(name);
		if (fd < 0)
			tc_error(sh, line, ".: %s: %s", name, strerror(errno));
		return fd;
	}

	const char *path = tc_var_get(&sh->vars, "PATH");
	if (path) {
		size_t nlen = strlen(name);
		char cand[4096];
		for (const char *p = path;; p++) {
			const char *end = strchr(p, ':');
			size_t dlen = end ? (size_t)(end - p) : strlen(p);
			const char *dir = dlen ? p : ".";
			size_t use = dlen ? dlen : 1;
			if (use + 1 + nlen < sizeof(cand)) {
				memcpy(cand, dir, use);
				cand[use] = '/';
				memcpy(cand + use + 1, name, nlen + 1);
				int fd = open_dot_candidate(cand);
				if (fd >= 0)
					return fd;
			}
			if (!end)
				break;
			p = end;
		}
	}
	tc_error(sh, line, ".: %s: file not found", name);
	return -1;
}

int tc_bi_dot(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (i >= argc) {
		tc_error(sh, line, ".: filename argument required");
		return special_fail(sh, TC_STATUS_USAGE);
	}
	if (sh->dot_depth >= TC_DOT_DEPTH_MAX) {
		tc_error(sh, line, ".: maximum nesting depth exceeded (%d)",
			 TC_DOT_DEPTH_MAX);
		return special_fail(sh, 1);
	}

	int fd = find_dot_file(sh, argv[i], line);
	if (fd < 0)
		return special_fail(sh, 1);

	struct tc_input in;
	tc_input_from_fd(&in, fd, false);

	sh->dot_depth++;
	bool ran_any = false;
	int st = tc_exec_input(sh, &in, line, &ran_any);
	if (sh->flow == TC_FLOW_RETURN) {
		sh->flow = TC_FLOW_NONE;
		st = sh->return_status;
	}
	sh->dot_depth--;
	close(fd);
	return ran_any ? st : (st != 0 ? st : 0);
}

/* ---- export / readonly ------------------------------------------------ */

static int print_flagged_vars(struct tc_shell *sh, unsigned flag,
			      const char *cmd, unsigned long line)
{
	const struct tc_var_entry **list = NULL;
	size_t n = 0;
	if (tc_var_list(&sh->vars, flag, &list, &n) != TC_VAR_OK) {
		tc_error(sh, line, "%s: out of memory", cmd);
		return 1;
	}
	int status = 0;
	for (size_t i = 0; i < n; i++) {
		const struct tc_var_entry *e = list[i];
		if (tc_write_all(STDOUT_FILENO, cmd, strlen(cmd)) < 0 ||
		    tc_write_all(STDOUT_FILENO, " ", 1) < 0 ||
		    tc_write_all(STDOUT_FILENO, e->name, strlen(e->name)) < 0) {
			tc_error(sh, line, "%s: write error: %s", cmd, strerror(errno));
			status = 1;
			break;
		}
		if (e->value != NULL) {
			if (tc_write_all(STDOUT_FILENO, "=", 1) < 0 ||
			    tc_write_shell_quoted(STDOUT_FILENO, e->value) < 0) {
				tc_error(sh, line, "%s: write error: %s", cmd,
					 strerror(errno));
				status = 1;
				break;
			}
		}
		if (tc_write_all(STDOUT_FILENO, "\n", 1) < 0) {
			tc_error(sh, line, "%s: write error: %s", cmd, strerror(errno));
			status = 1;
			break;
		}
	}
	free(list);
	return status;
}

static int bi_var_attr(struct tc_shell *sh, int argc, char **argv,
		       unsigned long line, unsigned flag, const char *cmd)
{
	int i = 1;
	bool print_mode = false;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (strcmp(argv[i], "-p") == 0) {
			print_mode = true;
			continue;
		}
		tc_error(sh, line, "%s: %s: invalid option", cmd, argv[i]);
		return special_fail(sh, TC_STATUS_USAGE);
	}

	if (print_mode || i >= argc)
		return print_flagged_vars(sh, flag, cmd, line);

	int status = 0;
	for (; i < argc; i++) {
		const char *arg = argv[i];
		const char *eq = strchr(arg, '=');
		char *name_copy = NULL;
		const char *name = arg;
		const char *val = NULL;
		if (eq) {
			size_t nlen = (size_t)(eq - arg);
			name_copy = malloc(nlen + 1);
			if (!name_copy) {
				tc_error(sh, line, "%s: out of memory", cmd);
				return special_fail(sh, 1);
			}
			memcpy(name_copy, arg, nlen);
			name_copy[nlen] = '\0';
			name = name_copy;
			val = eq + 1;
		}
		int r = tc_var_set(&sh->vars, name, val, flag);
		if (r == TC_VAR_ERR_NAME) {
			tc_error(sh, line, "%s: `%s': not a valid identifier",
				 cmd, arg);
			status = 1;
		} else if (r == TC_VAR_ERR_READONLY) {
			tc_error(sh, line, "%s: %s: readonly variable", cmd, name);
			status = 1;
		} else if (r == TC_VAR_ERR_NOMEM) {
			tc_error(sh, line, "%s: out of memory", cmd);
			free(name_copy);
			return special_fail(sh, 1);
		}
		free(name_copy);
		if (status != 0 && !sh->interactive)
			return special_fail(sh, status);
	}
	return status;
}

int tc_bi_export(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	return bi_var_attr(sh, argc, argv, line, TC_VAR_EXPORT, "export");
}

int tc_bi_readonly(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	return bi_var_attr(sh, argc, argv, line, TC_VAR_READONLY, "readonly");
}

/* ---- unset ------------------------------------------------------------ */

int tc_bi_unset(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool do_vars = false;
	bool do_funcs = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'v') {
				do_vars = true;
			} else if (*p == 'f') {
				do_funcs = true;
			} else {
				tc_error(sh, line, "unset: -%c: invalid option", *p);
				return special_fail(sh, TC_STATUS_USAGE);
			}
		}
	}
	if (do_vars && do_funcs) {
		tc_error(sh, line, "unset: cannot use -v and -f together");
		return special_fail(sh, TC_STATUS_USAGE);
	}
	if (!do_vars && !do_funcs)
		do_vars = true;

	int status = 0;
	for (; i < argc; i++) {
		const char *name = argv[i];
		if (!tc_var_valid_name(name, strlen(name))) {
			tc_error(sh, line, "unset: `%s': not a valid identifier", name);
			status = 1;
			continue;
		}
		if (do_funcs) {
			tc_func_unset(&sh->funcs, name);
		} else {
			if (strcmp(name, "LINENO") == 0)
				sh->lineno_disabled = true;
			int r = tc_var_unset(&sh->vars, name);
			if (r == TC_VAR_ERR_READONLY) {
				tc_error(sh, line, "unset: %s: readonly variable", name);
				status = 1;
			}
		}
	}
	return status;
}

/* ---- exec stub (handled in src/exec.c for permanent redirections) ----- */

int tc_bi_exec_stub(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	(void)sh, (void)argc, (void)argv, (void)line;
	return 0;
}

/* ---- times ------------------------------------------------------------ */

static void fmt_clock(char *buf, size_t cap, clock_t ticks, long clk_tck)
{
	if (clk_tck <= 0)
		clk_tck = 100;
	unsigned long long ms =
		((unsigned long long)ticks * 1000ULL + (unsigned long long)clk_tck / 2) /
		(unsigned long long)clk_tck;
	unsigned long long mins = ms / 60000ULL;
	unsigned long long secs = (ms % 60000ULL) / 1000ULL;
	unsigned long long frac = ms % 1000ULL;
	snprintf(buf, cap, "%llum%llu.%03llus", mins, secs, frac);
}

int tc_bi_times(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	(void)argc, (void)argv;
	struct tms t;
	if (times(&t) == (clock_t)-1) {
		tc_error(sh, line, "times: %s", strerror(errno));
		return 1;
	}
	long tck = sysconf(_SC_CLK_TCK);
	char u[32], s[32], cu[32], cs[32], out[160];
	fmt_clock(u, sizeof(u), t.tms_utime, tck);
	fmt_clock(s, sizeof(s), t.tms_stime, tck);
	fmt_clock(cu, sizeof(cu), t.tms_cutime, tck);
	fmt_clock(cs, sizeof(cs), t.tms_cstime, tck);
	int n = snprintf(out, sizeof(out), "%s %s\n%s %s\n", u, s, cu, cs);
	if (n < 0 || tc_write_all(STDOUT_FILENO, out, (size_t)n) < 0) {
		tc_error(sh, line, "times: write error: %s", strerror(errno));
		return 1;
	}
	return 0;
}

/* ---- wait ------------------------------------------------------------- */

static int parse_pid_arg(const char *s, pid_t *out)
{
	if (!s || *s == '\0')
		return -1;
	long long v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		int d = *p - '0';
		if (v > (INT_MAX - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	if (v <= 0)
		return -1;
	*out = (pid_t)v;
	return 0;
}

int tc_bi_wait(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (i >= argc)
		return tc_bg_wait_all(sh);

	int status = 0;
	for (; i < argc; i++) {
		if (argv[i][0] == '%') {
			tc_error(sh, line,
				 "wait: job IDs (`%s') are not supported yet; see ROADMAP.md",
				 argv[i]);
			return TC_STATUS_USAGE;
		}
		pid_t pid;
		if (parse_pid_arg(argv[i], &pid) < 0) {
			tc_error(sh, line, "wait: %s: not a valid process ID",
				 argv[i]);
			return TC_STATUS_USAGE;
		}
		bool interrupted = false;
		status = tc_bg_wait_pid(sh, pid, &interrupted);
		if (interrupted)
			return status;
	}
	return status;
}
