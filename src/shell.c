/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/input.h"
#include "tam-clam/shell.h"

extern char **environ;

static bool valid_initial_pwd(const char *pwd)
{
	if (!pwd || pwd[0] != '/')
		return false;
	/* Reject . or .. path components. */
	const char *p = pwd;
	while (*p) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		const char *start = p;
		while (*p && *p != '/')
			p++;
		size_t len = (size_t)(p - start);
		if ((len == 1 && start[0] == '.') ||
		    (len == 2 && start[0] == '.' && start[1] == '.'))
			return false;
	}
	struct stat st_pwd, st_dot;
	if (stat(pwd, &st_pwd) < 0 || stat(".", &st_dot) < 0)
		return false;
	return st_pwd.st_dev == st_dot.st_dev && st_pwd.st_ino == st_dot.st_ino;
}

static char *shell_getcwd(void)
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
		if (e != ERANGE || cap > ((size_t)1 << 20))
			return NULL;
		cap *= 2;
	}
}

void tc_shell_init(struct tc_shell *sh, const char *name)
{
	memset(sh, 0, sizeof(*sh));
	sh->name = name;
	sh->root_pid = getpid();
	sh->func_limit = TC_FUNC_DEPTH_DEFAULT;
	tc_arena_init(&sh->arena, TC_ARENA_MAX);
	tc_var_init(&sh->vars);
	tc_funcs_init(&sh->funcs);
	(void)tc_var_import_environ(&sh->vars, environ);

	if (!tc_var_get(&sh->vars, "IFS"))
		(void)tc_var_set(&sh->vars, "IFS", " \t\n", 0);
	if (!tc_var_get(&sh->vars, "PS1"))
		(void)tc_var_set(&sh->vars, "PS1", geteuid() == 0 ? "# " : "$ ", 0);
	if (!tc_var_get(&sh->vars, "PS2"))
		(void)tc_var_set(&sh->vars, "PS2", "> ", 0);

	const char *pwd = tc_var_get(&sh->vars, "PWD");
	if (!valid_initial_pwd(pwd)) {
		char *cwd = shell_getcwd();
		if (cwd) {
			(void)tc_var_set(&sh->vars, "PWD", cwd, TC_VAR_EXPORT);
			free(cwd);
		}
	}

	ssize_t n = readlink("/proc/self/exe", sh->self_path, sizeof(sh->self_path) - 1);
	sh->self_path[n > 0 ? n : 0] = '\0';
}

static void free_params(struct tc_shell *sh)
{
	if (!sh->params)
		return;
	for (size_t i = 0; i < sh->nparams; i++)
		free(sh->params[i]);
	free(sh->params);
	sh->params = NULL;
	sh->nparams = 0;
}

int tc_shell_set_params(struct tc_shell *sh, int n, char *const *argv)
{
	if (n <= 0) {
		free_params(sh);
		return TC_OK;
	}
	char **copy = calloc((size_t)n, sizeof(*copy));
	if (!copy)
		return TC_ERR_NOMEM;
	for (int i = 0; i < n; i++) {
		size_t len = strlen(argv[i]);
		copy[i] = malloc(len + 1);
		if (!copy[i]) {
			for (int j = 0; j < i; j++)
				free(copy[j]);
			free(copy);
			return TC_ERR_NOMEM;
		}
		memcpy(copy[i], argv[i], len + 1);
	}
	free_params(sh);
	sh->params = copy;
	sh->nparams = (size_t)n;
	return TC_OK;
}

void tc_shell_finish(struct tc_shell *sh)
{
	free_params(sh);
	tc_funcs_finish(&sh->funcs);
	tc_var_finish(&sh->vars);
	tc_arena_finish(&sh->arena);
}

int tc_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

void tc_error(const struct tc_shell *sh, unsigned long line, const char *fmt, ...)
{
	char buf[1024];
	int n;
	if (line && !sh->interactive)
		n = snprintf(buf, sizeof(buf), "%s: line %lu: ", sh->name, line);
	else
		n = snprintf(buf, sizeof(buf), "%s: ", sh->name);
	if (n < 0)
		return;
	size_t used = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;

	va_list ap;
	va_start(ap, fmt);
	n = vsnprintf(buf + used, sizeof(buf) - used, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	used += (size_t)n < sizeof(buf) - used ? (size_t)n : sizeof(buf) - used - 1;
	/* A message too long for the buffer is cut, but still ends a line. */
	if (used == sizeof(buf) - 1)
		used--;
	buf[used++] = '\n';
	(void)tc_write_all(STDERR_FILENO, buf, used);
}
