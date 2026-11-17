/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The builtins of 0.0.1: exit, cd, :, true and false. */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/builtins.h"

static int bi_true(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	(void)sh, (void)argc, (void)argv, (void)line;
	return 0;
}

static int bi_false(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	(void)sh, (void)argc, (void)argv, (void)line;
	return 1;
}

/* A decimal number with an optional sign, taken modulo 256 as bash and
 * mksh do. -1 when it is not a number. */
static int parse_exit_status(const char *s)
{
	const char *p = s;
	int neg = 0;
	if (*p == '+' || *p == '-')
		neg = (*p++ == '-');
	if (*p == '\0')
		return -1;
	unsigned long v = 0;
	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		if (v > (ULONG_MAX - 9) / 10)
			return -1;
		v = v * 10 + (unsigned long)(*p - '0');
	}
	v &= 255;
	return (int)(neg ? (256 - v) & 255 : v);
}

static int bi_exit(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int status = sh->status;
	if (argc > 2) {
		tc_error(sh, line, "exit: too many arguments");
		return 1;
	}
	if (argc == 2) {
		status = parse_exit_status(argv[1]);
		if (status < 0) {
			tc_error(sh, line, "exit: %s: a number from 0 to 255 is needed",
				 argv[1]);
			status = TC_STATUS_USAGE;
		}
	}
	sh->exit_requested = true;
	sh->exit_status = status;
	return status;
}

/* getcwd into a growing buffer. NULL on failure, with errno set. */
static char *current_dir(void)
{
	size_t cap = 256;
	for (;;) {
		char *buf = malloc(cap);
		if (!buf)
			return NULL;
		if (getcwd(buf, cap))
			return buf;
		int e = errno;
		free(buf);
		if (e != ERANGE || cap > ((size_t)1 << 20)) {
			errno = e;
			return NULL;
		}
		cap *= 2;
	}
}

/* Physical cd for now; logical cd (-L, the POSIX default) and CDPATH come
 * with shell variables in 0.0.3. */
static int bi_cd(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (argc - i > 1) {
		tc_error(sh, line, "cd: too many arguments");
		return 1;
	}
	const char *dir = i < argc ? argv[i] : NULL;
	int print = 0;
	if (dir && dir[0] == '-' && dir[1] != '\0') {
		tc_error(sh, line, "cd: %s: options are not supported yet", dir);
		return TC_STATUS_USAGE;
	}
	if (!dir) {
		dir = getenv("HOME");
		if (!dir || !*dir) {
			tc_error(sh, line, "cd: HOME is not set");
			return 1;
		}
	} else if (strcmp(dir, "-") == 0) {
		dir = getenv("OLDPWD");
		if (!dir || !*dir) {
			tc_error(sh, line, "cd: OLDPWD is not set");
			return 1;
		}
		print = 1;
	}

	char *old = current_dir();
	if (chdir(dir) < 0) {
		tc_error(sh, line, "cd: %s: %s", dir, strerror(errno));
		free(old);
		return 1;
	}
	char *now = current_dir();
	int status = 0;
	if (old && setenv("OLDPWD", old, 1) < 0)
		status = 1;
	if (now && setenv("PWD", now, 1) < 0)
		status = 1;
	if (!now) {
		tc_error(sh, line, "cd: cannot tell the new directory: %s", strerror(errno));
		status = 1;
	} else if (print) {
		size_t len = strlen(now);
		now[len] = '\n';
		if (tc_write_all(STDOUT_FILENO, now, len + 1) < 0) {
			tc_error(sh, line, "cd: write error: %s", strerror(errno));
			status = 1;
		}
		now[len] = '\0';
	}
	if (status && now && old)
		tc_error(sh, line, "cd: cannot set PWD or OLDPWD");
	free(old);
	free(now);
	return status;
}

static const struct {
	const char *name;
	tc_builtin_fn fn;
} builtins[] = {
	{":", bi_true},
	{"cd", bi_cd},
	{"exit", bi_exit},
	{"false", bi_false},
	{"true", bi_true},
};

tc_builtin_fn tc_builtin_find(const char *name)
{
	for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
		if (strcmp(name, builtins[i].name) == 0)
			return builtins[i].fn;
	return NULL;
}
