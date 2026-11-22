/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running commands, assignments, pipelines, and-or lists, and redirections.
 * The PATH search happens in the child, as in dash: each candidate is tried
 * with execve, so there is no window between checking a file and running it. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/expand.h"
#include "tam-clam/signals.h"

/* Used when PATH is unset. */
#define TC_DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin"
#define TC_PATH_MAX 4096

static int fatal_exp_error(struct tc_shell *sh)
{
	if (!sh->interactive) {
		sh->exit_requested = true;
		sh->exit_status = 1;
	}
	return 1;
}

/* execve, and for a file the kernel cannot run (ENOEXEC), run it as a
 * shell script with this shell, as POSIX requires. Returns only on
 * failure, with errno set. */
static void try_exec(const struct tc_shell *sh, const char *path,
		     char **argv, char **envp)
{
	execve(path, argv, envp);
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
	execve(sh->self_path, nargv, envp);
	free(nargv);
	errno = ENOEXEC;
}

/* In the child: find and run argv[0]. Returns the exit status to use
 * when nothing could be run. */
static int run_external(struct tc_shell *sh, char **argv,
			const struct tc_var_kv *overrides, size_t noverrides,
			unsigned long line)
{
	const char *name = argv[0];
	if (name[0] == '\0') {
		tc_error(sh, line, "%s: command not found", name);
		return TC_STATUS_NOTFOUND;
	}

	char **envp = noverrides > 0
			  ? tc_var_envp_override(&sh->vars, overrides, noverrides, &sh->arena)
			  : tc_var_envp(&sh->vars);
	if (!envp) {
		tc_error(sh, line, "out of memory");
		return TC_STATUS_USAGE;
	}

	if (strchr(name, '/')) {
		try_exec(sh, name, argv, envp);
		int e = errno;
		tc_error(sh, line, "%s: %s", name, strerror(e));
		return (e == ENOENT || e == ENOTDIR) ? TC_STATUS_NOTFOUND : TC_STATUS_NOEXEC;
	}

	const char *path = tc_var_lookup_override(&sh->vars, overrides, noverrides, "PATH");
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
			try_exec(sh, buf, argv, envp);
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
		return TC_STATUS_NOEXEC;
	}
	if (WIFEXITED(ws))
		return WEXITSTATUS(ws);
	if (WIFSIGNALED(ws))
		return 128 + WTERMSIG(ws);
	return TC_STATUS_NOEXEC;
}

struct fd_save {
	int orig_fd;
	int saved_fd; /* -1 if orig_fd was closed */
};

static int default_redir_fd(enum tc_redir_kind kind)
{
	switch (kind) {
	case TC_REDIR_OUT:
	case TC_REDIR_CLOBBER:
	case TC_REDIR_APPEND:
	case TC_REDIR_DUP_OUT:
		return STDOUT_FILENO;
	default:
		return STDIN_FILENO;
	}
}

static int parse_dup_fd(const char *s, int *out)
{
	if (*s == '\0')
		return -1;
	unsigned long v = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		unsigned d = (unsigned)(*s - '0');
		if (v > ((unsigned long)INT_MAX - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	*out = (int)v;
	return 0;
}

static int apply_heredoc(struct tc_shell *sh, const struct tc_redir *r,
			 int target_fd, pid_t *writer_pid)
{
	const char *body = r->heredoc_body;
	size_t blen = r->heredoc_len;
	if (!r->heredoc_quoted) {
		char *exp = NULL;
		size_t elen = 0;
		int er = tc_expand_heredoc(sh, r->heredoc_parts, r->line, &exp, &elen);
		if (er != TC_EXP_OK) {
			if (er == TC_EXP_ERR)
				(void)fatal_exp_error(sh);
			return -1;
		}
		body = exp;
		blen = elen;
	}

	int pfd[2];
	if (pipe(pfd) < 0) {
		tc_error(sh, r->line, "cannot create pipe: %s", strerror(errno));
		return -1;
	}
	if (blen > 0) {
		if (blen <= 4096) {
			if (tc_write_all(pfd[1], body, blen) < 0) {
				tc_error(sh, r->line, "here-document write error: %s",
					 strerror(errno));
				close(pfd[0]);
				close(pfd[1]);
				return -1;
			}
			close(pfd[1]);
		} else {
			pid_t pid = fork();
			if (pid < 0) {
				tc_error(sh, r->line, "cannot write here-document: %s",
					 strerror(errno));
				close(pfd[0]);
				close(pfd[1]);
				return -1;
			}
			if (pid == 0) {
				tc_signals_child();
				close(pfd[0]);
				(void)tc_write_all(pfd[1], body, blen);
				close(pfd[1]);
				_exit(0);
			}
			close(pfd[1]);
			*writer_pid = pid;
		}
	} else {
		close(pfd[1]);
	}
	if (pfd[0] != target_fd) {
		if (dup2(pfd[0], target_fd) < 0) {
			tc_error(sh, r->line, "%d: %s", target_fd, strerror(errno));
			close(pfd[0]);
			return -1;
		}
		close(pfd[0]);
	}
	return 0;
}

static int apply_one_redir(struct tc_shell *sh, const struct tc_redir *r,
			   pid_t *writer_pid)
{
	int target_fd = r->fd >= 0 ? r->fd : default_redir_fd(r->kind);

	if (r->kind == TC_REDIR_HEREDOC)
		return apply_heredoc(sh, r, target_fd, writer_pid);

	char *word = NULL;
	int er = tc_expand_single(sh, &r->target, false, &word);
	if (er != TC_EXP_OK) {
		if (er == TC_EXP_ERR)
			(void)fatal_exp_error(sh);
		return -1;
	}

	if (r->kind == TC_REDIR_DUP_IN || r->kind == TC_REDIR_DUP_OUT) {
		if (strcmp(word, "-") == 0) {
			close(target_fd);
			return 0;
		}
		int src_fd;
		if (parse_dup_fd(word, &src_fd) < 0) {
			tc_error(sh, r->line, "%s: ambiguous redirect", word);
			return -1;
		}
		if (src_fd == target_fd) {
			if (fcntl(src_fd, F_GETFD) < 0) {
				tc_error(sh, r->line, "%d: %s", src_fd, strerror(errno));
				return -1;
			}
			return 0;
		}
		if (dup2(src_fd, target_fd) < 0) {
			tc_error(sh, r->line, "%d: %s", src_fd, strerror(errno));
			return -1;
		}
		return 0;
	}

	int flags;
	switch (r->kind) {
	case TC_REDIR_IN:      flags = O_RDONLY; break;
	case TC_REDIR_OUT:
	case TC_REDIR_CLOBBER: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
	case TC_REDIR_APPEND:  flags = O_WRONLY | O_CREAT | O_APPEND; break;
	case TC_REDIR_RDWR:    flags = O_RDWR | O_CREAT; break;
	default:               return -1;
	}

	int fd = open(word, flags, 0666);
	if (fd < 0) {
		tc_error(sh, r->line, "%s: %s", word, strerror(errno));
		return -1;
	}
	if (fd != target_fd) {
		if (dup2(fd, target_fd) < 0) {
			tc_error(sh, r->line, "%d: %s", target_fd, strerror(errno));
			close(fd);
			return -1;
		}
		close(fd);
	}
	return 0;
}

static void restore_fds(const struct fd_save *saves, size_t n)
{
	while (n > 0) {
		n--;
		if (saves[n].saved_fd >= 0) {
			(void)dup2(saves[n].saved_fd, saves[n].orig_fd);
			close(saves[n].saved_fd);
		} else {
			close(saves[n].orig_fd);
		}
	}
}

static int apply_redirs(struct tc_shell *sh, const struct tc_redir *redirs,
			size_t nredirs, struct fd_save **out_saves,
			size_t *out_nsaves, pid_t **out_writers,
			size_t *out_nwriters)
{
	if (out_saves) {
		*out_saves = NULL;
		*out_nsaves = 0;
	}
	*out_writers = NULL;
	*out_nwriters = 0;
	if (nredirs == 0)
		return 0;

	struct fd_save *saves = NULL;
	if (out_saves) {
		saves = tc_arena_alloc(&sh->arena, nredirs * sizeof(*saves));
		if (!saves) {
			tc_error(sh, redirs[0].line, "out of memory");
			return -1;
		}
	}
	pid_t *writers = tc_arena_alloc(&sh->arena, nredirs * sizeof(*writers));
	if (!writers) {
		tc_error(sh, redirs[0].line, "out of memory");
		return -1;
	}
	size_t nsaves = 0, nwriters = 0;

	for (size_t i = 0; i < nredirs; i++) {
		if (saves) {
			int tfd = redirs[i].fd >= 0 ? redirs[i].fd
						    : default_redir_fd(redirs[i].kind);
			int sfd = fcntl(tfd, F_DUPFD_CLOEXEC, 10);
			if (sfd < 0 && errno != EBADF) {
				tc_error(sh, redirs[i].line, "dup: %s", strerror(errno));
				restore_fds(saves, nsaves);
				return -1;
			}
			saves[nsaves].orig_fd = tfd;
			saves[nsaves].saved_fd = sfd;
			nsaves++;
		}
		pid_t wpid = -1;
		if (apply_one_redir(sh, &redirs[i], &wpid) < 0) {
			if (saves)
				restore_fds(saves, nsaves);
			for (size_t j = 0; j < nwriters; j++)
				wait_child(writers[j]);
			return -1;
		}
		if (wpid > 0)
			writers[nwriters++] = wpid;
	}
	if (out_saves) {
		*out_saves = saves;
		*out_nsaves = nsaves;
	}
	*out_writers = writers;
	*out_nwriters = nwriters;
	return 0;
}

/* Apply assignments directly to sh->vars (left-to-right). */
static int apply_assigns_persist(struct tc_shell *sh,
				 const struct tc_assign *assigns, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char *val = NULL;
		int er = tc_expand_single(sh, &assigns[i].value, true, &val);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
		int sr = tc_var_set(&sh->vars, assigns[i].name, val, 0);
		if (sr == TC_VAR_ERR_READONLY) {
			tc_error(sh, assigns[i].line, "%s: readonly variable",
				 assigns[i].name);
			return fatal_exp_error(sh);
		}
		if (sr != TC_VAR_OK) {
			tc_error(sh, assigns[i].line, "out of memory");
			return TC_STATUS_USAGE;
		}
	}
	return 0;
}

/* Expand prefix assignments into tc_var_kv array in sh->arena, checking
 * readonly status for each name. */
static int expand_assigns_kv(struct tc_shell *sh,
			     const struct tc_assign *assigns, size_t n,
			     struct tc_var_kv **out_kvs)
{
	*out_kvs = NULL;
	if (n == 0)
		return 0;
	struct tc_var_kv *kvs = tc_arena_alloc(&sh->arena, n * sizeof(*kvs));
	if (!kvs) {
		tc_error(sh, assigns[0].line, "out of memory");
		return TC_STATUS_USAGE;
	}
	for (size_t i = 0; i < n; i++) {
		if (tc_var_is_readonly(&sh->vars, assigns[i].name)) {
			tc_error(sh, assigns[i].line, "%s: readonly variable",
				 assigns[i].name);
			return fatal_exp_error(sh);
		}
		char *val = NULL;
		int er = tc_expand_single(sh, &assigns[i].value, true, &val);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
		kvs[i].name = assigns[i].name;
		kvs[i].value = val;
	}
	*out_kvs = kvs;
	return 0;
}

static int exec_simple_child(struct tc_shell *sh, const struct tc_node *n)
{
	size_t na = n->u.simple.nassigns;
	size_t nw = n->u.simple.nwords;
	char **argv = NULL;
	size_t argc = 0;

	if (nw > 0) {
		int er = tc_expand_words(sh, n->u.simple.words, nw, &argv, &argc);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? 1 : TC_STATUS_USAGE;
	}

	if (argc == 0) {
		if (na > 0) {
			int ar = apply_assigns_persist(sh, n->u.simple.assigns, na);
			if (ar != 0)
				return ar;
		}
		pid_t *writers = NULL;
		size_t nwriters = 0;
		if (apply_redirs(sh, n->u.simple.redirs, n->u.simple.nredirs,
				 NULL, &(size_t){0}, &writers, &nwriters) < 0)
			return 1;
		for (size_t i = 0; i < nwriters; i++)
			wait_child(writers[i]);
		return 0;
	}

	struct tc_var_kv *kvs = NULL;
	if (na > 0) {
		int ar = expand_assigns_kv(sh, n->u.simple.assigns, na, &kvs);
		if (ar != 0)
			return ar;
	}

	pid_t *writers = NULL;
	size_t nwriters = 0;
	if (apply_redirs(sh, n->u.simple.redirs, n->u.simple.nredirs,
			 NULL, &(size_t){0}, &writers, &nwriters) < 0)
		return 1;

	const struct tc_builtin *bi = tc_builtin_find(argv[0]);
	int status;
	if (bi) {
		struct tc_var_save *vsaves = NULL;
		size_t nvsaves = 0;
		if (na > 0) {
			if (bi->special) {
				for (size_t i = 0; i < na; i++)
					(void)tc_var_set(&sh->vars, kvs[i].name, kvs[i].value, 0);
			} else {
				vsaves = tc_arena_alloc(&sh->arena, na * sizeof(*vsaves));
				if (!vsaves ||
				    tc_var_push_overrides(&sh->vars, kvs, na, vsaves, &nvsaves) != TC_VAR_OK)
					return 1;
			}
		}
		status = bi->fn(sh, (int)argc, argv, n->line);
		if (vsaves)
			tc_var_pop_overrides(&sh->vars, vsaves, nvsaves);
	} else {
		status = run_external(sh, argv, kvs, na, n->line);
	}
	for (size_t i = 0; i < nwriters; i++)
		wait_child(writers[i]);
	return status;
}

static int exec_simple(struct tc_shell *sh, const struct tc_node *n)
{
	size_t na = n->u.simple.nassigns;
	size_t nw = n->u.simple.nwords;
	size_t nr = n->u.simple.nredirs;
	char **argv = NULL;
	size_t argc = 0;

	if (nw > 0) {
		int er = tc_expand_words(sh, n->u.simple.words, nw, &argv, &argc);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
	}

	/* Assignment-only and/or redirection-only command (or command word
	 * expanded to nothing): assignments persist in the shell. */
	if (argc == 0) {
		if (na > 0) {
			int ar = apply_assigns_persist(sh, n->u.simple.assigns, na);
			if (ar != 0)
				return ar;
		}
		struct fd_save *saves = NULL;
		size_t nsaves = 0;
		pid_t *writers = NULL;
		size_t nwriters = 0;
		if (apply_redirs(sh, n->u.simple.redirs, nr,
				 &saves, &nsaves, &writers, &nwriters) < 0)
			return 1;
		restore_fds(saves, nsaves);
		for (size_t i = 0; i < nwriters; i++)
			wait_child(writers[i]);
		return 0;
	}

	struct tc_var_kv *kvs = NULL;
	if (na > 0) {
		int ar = expand_assigns_kv(sh, n->u.simple.assigns, na, &kvs);
		if (ar != 0)
			return ar;
	}

	const struct tc_builtin *bi = tc_builtin_find(argv[0]);
	if (bi) {
		struct fd_save *saves = NULL;
		size_t nsaves = 0;
		pid_t *writers = NULL;
		size_t nwriters = 0;
		if (apply_redirs(sh, n->u.simple.redirs, nr,
				 &saves, &nsaves, &writers, &nwriters) < 0)
			return 1;

		struct tc_var_save *vsaves = NULL;
		size_t nvsaves = 0;
		int status = 0;
		if (na > 0) {
			if (bi->special) {
				for (size_t i = 0; i < na; i++) {
					int sr = tc_var_set(&sh->vars, kvs[i].name, kvs[i].value, 0);
					if (sr != TC_VAR_OK) {
						tc_error(sh, n->line, "out of memory");
						restore_fds(saves, nsaves);
						for (size_t j = 0; j < nwriters; j++)
							wait_child(writers[j]);
						return TC_STATUS_USAGE;
					}
				}
			} else {
				vsaves = tc_arena_alloc(&sh->arena, na * sizeof(*vsaves));
				if (!vsaves ||
				    tc_var_push_overrides(&sh->vars, kvs, na, vsaves, &nvsaves) != TC_VAR_OK) {
					tc_error(sh, n->line, "out of memory");
					restore_fds(saves, nsaves);
					for (size_t j = 0; j < nwriters; j++)
						wait_child(writers[j]);
					return TC_STATUS_USAGE;
				}
			}
		}

		status = bi->fn(sh, (int)argc, argv, n->line);
		if (vsaves)
			tc_var_pop_overrides(&sh->vars, vsaves, nvsaves);
		restore_fds(saves, nsaves);
		for (size_t i = 0; i < nwriters; i++)
			wait_child(writers[i]);
		return status;
	}

	pid_t pid = fork();
	if (pid < 0) {
		tc_error(sh, n->line, "cannot start %s: %s", argv[0], strerror(errno));
		return TC_STATUS_USAGE;
	}
	if (pid == 0) {
		tc_signals_child();
		pid_t *writers = NULL;
		size_t nwriters = 0;
		if (apply_redirs(sh, n->u.simple.redirs, nr,
				 NULL, &(size_t){0}, &writers, &nwriters) < 0)
			_exit(1);
		_exit(run_external(sh, argv, kvs, na, n->line));
	}
	return wait_child(pid);
}

static int exec_pipeline(struct tc_shell *sh, const struct tc_node *n)
{
	size_t ncmds = n->u.pipeline.ncmds;
	if (ncmds == 1) {
		int status = exec_simple(sh, n->u.pipeline.cmds[0]);
		return n->u.pipeline.bang ? (status == 0 ? 1 : 0) : status;
	}

	pid_t *pids = tc_arena_alloc(&sh->arena, ncmds * sizeof(*pids));
	if (!pids) {
		tc_error(sh, n->line, "out of memory");
		return TC_STATUS_USAGE;
	}

	int in_fd = -1;
	size_t forked = 0;
	for (size_t i = 0; i < ncmds; i++) {
		int pfd[2] = {-1, -1};
		if (i + 1 < ncmds && pipe(pfd) < 0) {
			tc_error(sh, n->line, "cannot create pipe: %s", strerror(errno));
			if (in_fd >= 0)
				close(in_fd);
			for (size_t j = 0; j < forked; j++)
				wait_child(pids[j]);
			return TC_STATUS_USAGE;
		}
		pid_t pid = fork();
		if (pid < 0) {
			tc_error(sh, n->line, "cannot fork: %s", strerror(errno));
			if (in_fd >= 0)
				close(in_fd);
			if (pfd[0] >= 0)
				close(pfd[0]);
			if (pfd[1] >= 0)
				close(pfd[1]);
			for (size_t j = 0; j < forked; j++)
				wait_child(pids[j]);
			return TC_STATUS_USAGE;
		}
		if (pid == 0) {
			tc_signals_child();
			if (in_fd >= 0) {
				if (in_fd != STDIN_FILENO) {
					if (dup2(in_fd, STDIN_FILENO) < 0)
						_exit(1);
					close(in_fd);
				}
			}
			if (i + 1 < ncmds) {
				close(pfd[0]);
				if (pfd[1] != STDOUT_FILENO) {
					if (dup2(pfd[1], STDOUT_FILENO) < 0)
						_exit(1);
					close(pfd[1]);
				}
			}
			_exit(exec_simple_child(sh, n->u.pipeline.cmds[i]));
		}
		if (in_fd >= 0)
			close(in_fd);
		if (i + 1 < ncmds) {
			close(pfd[1]);
			in_fd = pfd[0];
		} else {
			in_fd = -1;
		}
		pids[forked++] = pid;
	}

	int status = 0;
	for (size_t i = 0; i < forked; i++)
		status = wait_child(pids[i]);
	return n->u.pipeline.bang ? (status == 0 ? 1 : 0) : status;
}

static int exec_one(struct tc_shell *sh, const struct tc_node *n)
{
	switch (n->kind) {
	case TC_NODE_SIMPLE:
		return exec_simple(sh, n);
	case TC_NODE_PIPELINE:
		return exec_pipeline(sh, n);
	case TC_NODE_AND_OR:
		for (size_t i = 0; i < n->u.and_or.nitems; i++) {
			const struct tc_andor_item *it = &n->u.and_or.items[i];
			if (it->op == TC_ANDOR_AND && sh->status != 0)
				continue;
			if (it->op == TC_ANDOR_OR && sh->status == 0)
				continue;
			sh->status = exec_one(sh, it->cmd);
			if (sh->exit_requested)
				break;
		}
		return sh->status;
	default:
		return TC_STATUS_USAGE;
	}
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
		sh->status = exec_one(sh, cur);
		if (sh->exit_requested)
			break;
	}
	return sh->status;
}
