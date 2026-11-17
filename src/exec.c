/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running simple commands. The PATH search happens in the child, as in
 * dash: each candidate is tried with execve, so there is no window between
 * checking a file and running it. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/expand.h"
#include "tam-clam/signals.h"

extern char **environ;

/* Used when PATH is unset. */
#define TC_DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin"
#define TC_PATH_MAX 4096

/* execve, and for a file the kernel cannot run (ENOEXEC), run it as a
 * shell script with this shell, as POSIX requires. Returns only on
 * failure, with errno set. */
static void try_exec(const struct tc_shell *sh, const char *path, char **argv)
{
	execve(path, argv, environ);
	if (errno != ENOEXEC || sh->self_path[0] == '\0')
		return;
	size_t argc = 0;
	while (argv[argc])
		argc++;
	char **nargv = malloc((argc + 2) * sizeof(*nargv));
	if (!nargv) {
		errno = ENOMEM;
		return;
	}
	nargv[0] = argv[0];
	nargv[1] = (char *)path;
	for (size_t i = 1; i <= argc; i++)
		nargv[i + 1] = argv[i];
	execve(sh->self_path, nargv, environ);
	free(nargv);
	errno = ENOEXEC;
}

/* In the child: find and run argv[0]. Returns the exit status to use
 * when nothing could be run. */
static int run_external(const struct tc_shell *sh, char **argv, unsigned long line)
{
	const char *name = argv[0];
	if (name[0] == '\0') {
		tc_error(sh, line, "%s: command not found", name);
		return TC_STATUS_NOTFOUND;
	}
	if (strchr(name, '/')) {
		try_exec(sh, name, argv);
		int e = errno;
		tc_error(sh, line, "%s: %s", name, strerror(e));
		return (e == ENOENT || e == ENOTDIR) ? TC_STATUS_NOTFOUND : TC_STATUS_NOEXEC;
	}

	const char *path = getenv("PATH");
	if (!path)
		path = TC_DEFAULT_PATH;
	size_t nlen = strlen(name);
	int denied = 0, other = 0;
	char buf[TC_PATH_MAX];
	for (const char *p = path;; p++) {
		const char *end = strchr(p, ':');
		size_t dlen = end ? (size_t)(end - p) : strlen(p);
		/* An empty entry means the current directory. */
		const char *dir = dlen ? p : ".";
		size_t use = dlen ? dlen : 1;
		if (use + 1 + nlen < sizeof(buf)) {
			memcpy(buf, dir, use);
			buf[use] = '/';
			memcpy(buf + use + 1, name, nlen + 1);
			try_exec(sh, buf, argv);
			if (errno == EACCES)
				denied = 1;
			else if (errno != ENOENT && errno != ENOTDIR && errno != ELOOP &&
				 errno != ENAMETOOLONG) {
				other = errno;
				break;
			}
		}
		if (!end)
			break;
		p = end;
	}
	if (other) {
		tc_error(sh, line, "%s: %s", buf, strerror(other));
		return TC_STATUS_NOEXEC;
	}
	if (denied) {
		tc_error(sh, line, "%s: %s", name, strerror(EACCES));
		return TC_STATUS_NOEXEC;
	}
	tc_error(sh, line, "%s: command not found", name);
	return TC_STATUS_NOTFOUND;
}

static int wait_child(pid_t pid)
{
	int ws;
	for (;;) {
		pid_t r = waitpid(pid, &ws, 0);
		if (r == pid)
			break;
		if (r < 0 && errno == EINTR)
			continue;
		return TC_STATUS_NOEXEC; /* ECHILD: cannot happen without a bug */
	}
	if (WIFEXITED(ws))
		return WEXITSTATUS(ws);
	if (WIFSIGNALED(ws))
		return 128 + WTERMSIG(ws);
	return TC_STATUS_NOEXEC;
}

static int exec_simple(struct tc_shell *sh, const struct tc_node *n)
{
	size_t nw = n->u.simple.nwords;
	char **argv = tc_arena_alloc(&sh->arena, (nw + 1) * sizeof(*argv));
	if (!argv)
		goto nomem;
	for (size_t i = 0; i < nw; i++) {
		argv[i] = tc_expand_word(&sh->arena, &n->u.simple.words[i]);
		if (!argv[i])
			goto nomem;
	}
	argv[nw] = NULL;

	tc_builtin_fn bi = tc_builtin_find(argv[0]);
	if (bi)
		return bi(sh, (int)nw, argv, n->line);

	pid_t pid = fork();
	if (pid < 0) {
		tc_error(sh, n->line, "cannot start %s: %s", argv[0], strerror(errno));
		return TC_STATUS_USAGE;
	}
	if (pid == 0) {
		tc_signals_child();
		_exit(run_external(sh, argv, n->line));
	}
	return wait_child(pid);

nomem:
	tc_error(sh, n->line, "out of memory");
	return TC_STATUS_USAGE;
}

int tc_exec(struct tc_shell *sh, const struct tc_node *n)
{
	/* A sequence is a right-leaning chain; walk it without recursion. */
	while (n) {
		const struct tc_node *cur = n;
		n = NULL;
		if (cur->kind == TC_NODE_SEQ) {
			n = cur->u.seq.rest;
			cur = cur->u.seq.first;
		}
		sh->status = exec_simple(sh, cur);
		if (sh->exit_requested)
			break;
	}
	return sh->status;
}
