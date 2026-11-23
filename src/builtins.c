/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell builtins: exit, cd (-L/-P, PWD, OLDPWD, CDPATH), break, continue,
 * return, :, true, false. */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static char *dup_str(const char *s)
{
	size_t len = strlen(s);
	char *p = malloc(len + 1);
	if (p)
		memcpy(p, s, len + 1);
	return p;
}

static char *join_path(const char *a, size_t alen, const char *b)
{
	size_t blen = strlen(b);
	bool need_slash = (alen > 0 && a[alen - 1] != '/');
	char *out = malloc(alen + (need_slash ? 1 : 0) + blen + 1);
	if (!out)
		return NULL;
	memcpy(out, a, alen);
	size_t pos = alen;
	if (need_slash)
		out[pos++] = '/';
	memcpy(out + pos, b, blen + 1);
	return out;
}

static bool is_dot_prefix(const char *s)
{
	if (s[0] != '.')
		return false;
	if (s[1] == '\0' || s[1] == '/')
		return true;
	return s[1] == '.' && (s[2] == '\0' || s[2] == '/');
}

/* POSIX cd -L canonicalization (XCU cd steps 7-8). Verifies that each prefix
 * preceding '..' exists and is a directory. Returns heap string or NULL
 * (with errno set). */
static char *canonicalize_logical(const char *raw)
{
	size_t len = strlen(raw);
	char *out = malloc(len + 2);
	if (!out) {
		errno = ENOMEM;
		return NULL;
	}

	size_t olen = 0;
	const char *p = raw;
	/* Preserve leading "//" (two slashes only) per POSIX. */
	size_t root_len = 1;
	if (p[0] == '/' && p[1] == '/' && p[2] != '/') {
		out[olen++] = '/';
		out[olen++] = '/';
		root_len = 2;
		p += 2;
	} else {
		out[olen++] = '/';
		while (*p == '/')
			p++;
	}
	out[olen] = '\0';

	while (*p) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		const char *start = p;
		while (*p && *p != '/')
			p++;
		size_t clen = (size_t)(p - start);

		if (clen == 1 && start[0] == '.')
			continue;
		if (clen == 2 && start[0] == '.' && start[1] == '.') {
			struct stat st;
			if (stat(out, &st) < 0) {
				int e = errno;
				free(out);
				errno = e;
				return NULL;
			}
			if (!S_ISDIR(st.st_mode)) {
				free(out);
				errno = ENOTDIR;
				return NULL;
			}
			if (olen > root_len) {
				while (olen > root_len && out[olen - 1] != '/')
					olen--;
				if (olen > root_len)
					olen--;
				out[olen] = '\0';
			}
			continue;
		}
		if (olen > root_len)
			out[olen++] = '/';
		memcpy(out + olen, start, clen);
		olen += clen;
		out[olen] = '\0';
	}
	return out;
}

static int bi_cd(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool physical = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *opt = argv[i] + 1; *opt; opt++) {
			if (*opt == 'L') {
				physical = false;
			} else if (*opt == 'P') {
				physical = true;
			} else {
				tc_error(sh, line, "cd: -%c: invalid option", *opt);
				return TC_STATUS_USAGE;
			}
		}
	}
	if (argc - i > 1) {
		tc_error(sh, line, "cd: too many arguments");
		return 1;
	}

	const char *dir = i < argc ? argv[i] : NULL;
	bool print = false;
	bool try_cdpath = false;
	if (!dir) {
		dir = tc_var_get(&sh->vars, "HOME");
		if (!dir || !*dir) {
			tc_error(sh, line, "cd: HOME is not set");
			return 1;
		}
	} else if (strcmp(dir, "-") == 0) {
		dir = tc_var_get(&sh->vars, "OLDPWD");
		if (!dir || !*dir) {
			tc_error(sh, line, "cd: OLDPWD is not set");
			return 1;
		}
		print = true;
	} else if (dir[0] != '\0' && dir[0] != '/' && !is_dot_prefix(dir)) {
		try_cdpath = true;
	}

	char *cdpath_hit = NULL;
	if (try_cdpath) {
		const char *cdpath = tc_var_get(&sh->vars, "CDPATH");
		if (cdpath && *cdpath) {
			for (const char *p = cdpath;; p++) {
				const char *end = strchr(p, ':');
				size_t dlen = end ? (size_t)(end - p) : strlen(p);
				char *cand = dlen ? join_path(p, dlen, dir) : dup_str(dir);
				if (!cand) {
					tc_error(sh, line, "cd: out of memory");
					return 1;
				}
				struct stat st;
				if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
					cdpath_hit = cand;
					if (dlen > 0)
						print = true;
					break;
				}
				free(cand);
				if (!end)
					break;
				p = end;
			}
		}
	}

	const char *target = cdpath_hit ? cdpath_hit : dir;
	const char *pwd_var = tc_var_get(&sh->vars, "PWD");
	char *old = (pwd_var && pwd_var[0] == '/') ? dup_str(pwd_var) : current_dir();
	char *now = NULL;

	if (target[0] == '\0') {
		now = old ? dup_str(old) : current_dir();
	} else if (physical) {
		if (chdir(target) < 0) {
			tc_error(sh, line, "cd: %s: %s", dir, strerror(errno));
			free(cdpath_hit);
			free(old);
			return 1;
		}
		now = current_dir();
		if (!now) {
			tc_error(sh, line, "cd: cannot tell the new directory: %s",
				 strerror(errno));
			free(cdpath_hit);
			free(old);
			return 1;
		}
	} else {
		char *curpath = NULL;
		if (target[0] == '/') {
			curpath = dup_str(target);
		} else if (old) {
			curpath = join_path(old, strlen(old), target);
		} else {
			curpath = dup_str(target);
		}
		if (!curpath) {
			tc_error(sh, line, "cd: out of memory");
			free(cdpath_hit);
			free(old);
			return 1;
		}
		if (curpath[0] == '/') {
			now = canonicalize_logical(curpath);
			free(curpath);
			if (!now) {
				tc_error(sh, line, "cd: %s: %s", dir, strerror(errno));
				free(cdpath_hit);
				free(old);
				return 1;
			}
			if (chdir(now) < 0) {
				tc_error(sh, line, "cd: %s: %s", dir, strerror(errno));
				free(now);
				free(cdpath_hit);
				free(old);
				return 1;
			}
		} else {
			if (chdir(curpath) < 0) {
				tc_error(sh, line, "cd: %s: %s", dir, strerror(errno));
				free(curpath);
				free(cdpath_hit);
				free(old);
				return 1;
			}
			free(curpath);
			now = current_dir();
		}
	}

	int status = 0;
	if (old && tc_var_set(&sh->vars, "OLDPWD", old, TC_VAR_EXPORT) != TC_VAR_OK)
		status = 1;
	if (now && tc_var_set(&sh->vars, "PWD", now, TC_VAR_EXPORT) != TC_VAR_OK)
		status = 1;
	if (status)
		tc_error(sh, line, "cd: cannot set PWD or OLDPWD");

	if (now && print) {
		size_t len = strlen(now);
		char *linebuf = malloc(len + 1);
		if (!linebuf) {
			tc_error(sh, line, "cd: out of memory");
			status = 1;
		} else {
			memcpy(linebuf, now, len);
			linebuf[len] = '\n';
			if (tc_write_all(STDOUT_FILENO, linebuf, len + 1) < 0) {
				tc_error(sh, line, "cd: write error: %s", strerror(errno));
				status = 1;
			}
			free(linebuf);
		}
	}

	free(cdpath_hit);
	free(old);
	free(now);
	return status;
}

static int parse_positive_count(const char *s, unsigned *out)
{
	if (!s || *s == '\0')
		return -1;
	unsigned long v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		unsigned d = (unsigned)(*p - '0');
		if (v > ((unsigned long)UINT_MAX - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	if (v == 0)
		return -1;
	*out = (unsigned)v;
	return 0;
}

static int bi_loop_flow(struct tc_shell *sh, int argc, char **argv,
			unsigned long line, enum tc_flow flow)
{
	const char *cmd = argv[0];
	unsigned n = 1;
	if (argc > 2) {
		tc_error(sh, line, "%s: too many arguments", cmd);
		return 1;
	}
	if (argc == 2 && parse_positive_count(argv[1], &n) < 0) {
		tc_error(sh, line, "%s: %s: a positive integer is needed",
			 cmd, argv[1]);
		if (!sh->interactive) {
			sh->exit_requested = true;
			sh->exit_status = 1;
		}
		return 1;
	}
	if (sh->loop_depth == 0) {
		tc_error(sh, line, "%s: only meaningful in a loop", cmd);
		return 0;
	}
	if (n > sh->loop_depth)
		n = sh->loop_depth;
	sh->flow = flow;
	sh->flow_count = n;
	return 0;
}

static int bi_break(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	return bi_loop_flow(sh, argc, argv, line, TC_FLOW_BREAK);
}

static int bi_continue(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	return bi_loop_flow(sh, argc, argv, line, TC_FLOW_CONTINUE);
}

static int bi_return(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int status = sh->status;
	if (argc > 2) {
		tc_error(sh, line, "return: too many arguments");
		return 1;
	}
	if (argc == 2) {
		status = parse_exit_status(argv[1]);
		if (status < 0) {
			tc_error(sh, line, "return: %s: a number from 0 to 255 is needed",
				 argv[1]);
			status = TC_STATUS_USAGE;
		}
	}
	if (sh->func_depth == 0) {
		tc_error(sh, line, "return: can only return from a function");
		return 1;
	}
	sh->flow = TC_FLOW_RETURN;
	sh->return_status = status;
	return status;
}

static const struct tc_builtin builtins[] = {
	{":", bi_true, true},
	{"break", bi_break, true},
	{"cd", bi_cd, false},
	{"continue", bi_continue, true},
	{"exit", bi_exit, true},
	{"false", bi_false, false},
	{"return", bi_return, true},
	{"true", bi_true, false},
};

const struct tc_builtin *tc_builtin_find(const char *name)
{
	for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
		if (strcmp(name, builtins[i].name) == 0)
			return &builtins[i];
	return NULL;
}
