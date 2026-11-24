/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running commands, assignments, pipelines, and-or lists, redirections,
 * compound commands, and shell functions.
 * The PATH search happens in the child, as in dash: each candidate is tried
 * with execve, so there is no window between checking a file and running it. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tam-clam/arith.h"
#include "tam-clam/bg.h"
#include "tam-clam/builtins.h"
#include "tam-clam/edit.h"
#include "tam-clam/exec.h"
#include "tam-clam/expand.h"
#include "tam-clam/jobs.h"
#include "tam-clam/lex.h"
#include "tam-clam/parse.h"
#include "tam-clam/pattern.h"
#include "tam-clam/signals.h"
#include "tam-clam/trap.h"

/* Used when PATH is unset. */
#define TC_DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin"
#define TC_PATH_MAX 4096

static int exec_one(struct tc_shell *sh, const struct tc_node *n);

static bool flow_stop(const struct tc_shell *sh)
{
	return sh->exit_requested || sh->flow != TC_FLOW_NONE;
}

static void maybe_errexit(struct tc_shell *sh, int status)
{
	if (status != 0 && sh->opt_errexit && sh->errexit_ignore == 0 &&
	    !sh->exit_requested && sh->flow == TC_FLOW_NONE) {
		sh->exit_requested = true;
		sh->exit_status = status;
	}
}

static bool xtrace_needs_quote(const char *s)
{
	if (!s || *s == '\0')
		return true;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		unsigned char c = *p;
		bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			    (c >= '0' && c <= '9') || c == '_' || c == '/' ||
			    c == '.' || c == '-' || c == '+' || c == ':' ||
			    c == '@' || c == ',' || c == '=' || c == '%';
		if (!safe)
			return true;
	}
	return false;
}

static void write_xtrace_word(int fd, const char *s)
{
	if (xtrace_needs_quote(s))
		(void)tc_write_shell_quoted(fd, s ? s : "");
	else
		(void)tc_write_all(fd, s, strlen(s));
}

static void trace_command(struct tc_shell *sh, const struct tc_var_kv *kvs,
			  size_t na, char *const *argv, size_t argc,
			  unsigned long line)
{
	if (!sh->opt_xtrace || (na == 0 && argc == 0))
		return;

	bool saved_xtrace = sh->opt_xtrace;
	bool saved_nounset = sh->opt_nounset;
	bool saved_errexit = sh->opt_errexit;
	bool saved_cmdsub_ran = sh->cmdsub_ran;
	int saved_cmdsub_status = sh->cmdsub_status;
	unsigned long saved_lineno = sh->lineno;
	struct tc_arena_mark mark = tc_arena_mark(&sh->arena);

	sh->opt_xtrace = false;
	sh->opt_nounset = false;
	sh->opt_errexit = false;
	if (line > 0)
		sh->lineno = line;

	const char *ps4 = tc_var_get(&sh->vars, "PS4");
	const char *prefix = "";
	size_t plen = 0;
	if (ps4 && *ps4) {
		struct tc_wpart *parts = NULL;
		char *exp = NULL;
		size_t elen = 0;
		if (tc_lex_expand_parts(sh, ps4, strlen(ps4), line, &parts) == TC_OK &&
		    tc_expand_heredoc(sh, parts, line, &exp, &elen) == TC_EXP_OK) {
			prefix = exp;
			plen = elen;
		} else {
			prefix = ps4;
			plen = strlen(ps4);
		}
	}

	if (plen > 0) {
		unsigned depth = sh->eval_depth + sh->dot_depth;
		if (depth > 64)
			depth = 64;
		for (unsigned d = 0; d < depth; d++)
			(void)tc_write_all(STDERR_FILENO, prefix, 1);
		(void)tc_write_all(STDERR_FILENO, prefix, plen);
	}

	bool first = true;
	for (size_t i = 0; i < na; i++) {
		if (!first)
			(void)tc_write_all(STDERR_FILENO, " ", 1);
		first = false;
		(void)tc_write_all(STDERR_FILENO, kvs[i].name, strlen(kvs[i].name));
		(void)tc_write_all(STDERR_FILENO, "=", 1);
		write_xtrace_word(STDERR_FILENO, kvs[i].value);
	}
	for (size_t i = 0; i < argc; i++) {
		if (!first)
			(void)tc_write_all(STDERR_FILENO, " ", 1);
		first = false;
		write_xtrace_word(STDERR_FILENO, argv[i]);
	}
	(void)tc_write_all(STDERR_FILENO, "\n", 1);

	tc_arena_rewind(&sh->arena, mark);
	sh->lineno = saved_lineno;
	sh->cmdsub_ran = saved_cmdsub_ran;
	sh->cmdsub_status = saved_cmdsub_status;
	sh->opt_errexit = saved_errexit;
	sh->opt_nounset = saved_nounset;
	sh->opt_xtrace = saved_xtrace;
}

static int fatal_exp_error(struct tc_shell *sh)
{
	if (!sh->interactive) {
		sh->exit_requested = true;
		sh->exit_status = 1;
	}
	return 1;
}

static unsigned effective_func_limit(const struct tc_shell *sh)
{
	unsigned limit = sh->func_limit ? sh->func_limit : TC_FUNC_DEPTH_DEFAULT;
	const char *s = tc_var_get(&sh->vars, "FUNCNEST");
	if (s && *s) {
		unsigned long v = 0;
		bool ok = true;
		for (const char *p = s; *p; p++) {
			if (*p < '0' || *p > '9') {
				ok = false;
				break;
			}
			unsigned d = (unsigned)(*p - '0');
			if (v > ((unsigned long)TC_FUNC_DEPTH_MAX - d) / 10) {
				v = TC_FUNC_DEPTH_MAX;
				break;
			}
			v = v * 10 + d;
		}
		if (ok && v > 0)
			limit = (unsigned)v;
	}
	if (limit > TC_FUNC_DEPTH_MAX)
		limit = TC_FUNC_DEPTH_MAX;
	return limit;
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

char *tc_exec_find_in_path(struct tc_shell *sh, const char *name,
			   bool default_path)
{
	if (!name || name[0] == '\0' || strchr(name, '/'))
		return NULL;
	const char *path = default_path ? TC_DEFAULT_PATH
					: tc_var_get(&sh->vars, "PATH");
	if (!path)
		path = TC_DEFAULT_PATH;
	size_t nlen = strlen(name);
	char buf[TC_PATH_MAX];
	for (const char *p = path;; p++) {
		const char *end = strchr(p, ':');
		size_t dlen = end ? (size_t)(end - p) : strlen(p);
		const char *dir = dlen ? p : ".";
		size_t use = dlen ? dlen : 1;
		if (use + 1 + nlen < sizeof(buf)) {
			memcpy(buf, dir, use);
			buf[use] = '/';
			memcpy(buf + use + 1, name, nlen + 1);
			struct stat st;
			if (stat(buf, &st) == 0 && !S_ISDIR(st.st_mode) &&
			    access(buf, X_OK) == 0) {
				size_t blen = use + 1 + nlen;
				char *out = malloc(blen + 1);
				if (out)
					memcpy(out, buf, blen + 1);
				return out;
			}
		}
		if (!end)
			break;
		p = end;
	}
	return NULL;
}

/* In the child: find and run argv[0]. Returns the exit status to use
 * when nothing could be run. */
static int run_external(struct tc_shell *sh, char **argv,
			const struct tc_var_kv *overrides, size_t noverrides,
			const char *cached_path, bool default_path,
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

	if (cached_path) {
		try_exec(sh, cached_path, argv, envp);
		if (errno != ENOENT && errno != ENOTDIR && errno != EACCES &&
		    errno != ELOOP) {
			tc_error(sh, line, "%s: %s", cached_path, strerror(errno));
			return TC_STATUS_NOEXEC;
		}
	}

	const char *path = default_path
			       ? TC_DEFAULT_PATH
			       : tc_var_lookup_override(&sh->vars, overrides, noverrides, "PATH");
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

	int fd;
	if (r->kind == TC_REDIR_OUT && sh->opt_noclobber) {
		fd = open(word, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd < 0 && errno == EEXIST) {
			struct stat st;
			if (stat(word, &st) == 0 && !S_ISREG(st.st_mode)) {
				fd = open(word, O_WRONLY | O_TRUNC);
				if (fd >= 0 &&
				    (fstat(fd, &st) < 0 || S_ISREG(st.st_mode))) {
					close(fd);
					tc_error(sh, r->line,
						 "%s: cannot overwrite existing file", word);
					return -1;
				}
			} else {
				tc_error(sh, r->line,
					 "%s: cannot overwrite existing file", word);
				return -1;
			}
		}
		if (fd < 0) {
			tc_error(sh, r->line, "%s: %s", word, strerror(errno));
			return -1;
		}
	} else {
		int flags;
		switch (r->kind) {
		case TC_REDIR_IN:      flags = O_RDONLY; break;
		case TC_REDIR_OUT:
		case TC_REDIR_CLOBBER: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
		case TC_REDIR_APPEND:  flags = O_WRONLY | O_CREAT | O_APPEND; break;
		case TC_REDIR_RDWR:    flags = O_RDWR | O_CREAT; break;
		default:               return -1;
		}

		fd = open(word, flags, 0666);
		if (fd < 0) {
			tc_error(sh, r->line, "%s: %s", word, strerror(errno));
			return -1;
		}
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

static int report_var_err(struct tc_shell *sh, unsigned long line,
			  const char *name, int sr)
{
	if (sr == TC_VAR_ERR_READONLY) {
		tc_error(sh, line, "%s: readonly variable", name);
		return fatal_exp_error(sh);
	}
	if (sr == TC_VAR_ERR_BADSUB) {
		tc_error(sh, line, "%s: bad array subscript", name);
		return 1;
	}
	if (sr == TC_VAR_ERR_TYPE) {
		tc_error(sh, line, "%s: cannot convert between indexed and associative array",
			 name);
		return 1;
	}
	tc_error(sh, line, "out of memory");
	return TC_STATUS_USAGE;
}

static int expand_sub_word(struct tc_shell *sh, const struct tc_wpart *sub,
			   unsigned long line, char **out)
{
	struct tc_word w;
	memset(&w, 0, sizeof(w));
	w.parts = (struct tc_wpart *)sub;
	w.line = line;
	return tc_expand_single(sh, &w, false, out);
}

int tc_exec_apply_assign(struct tc_shell *sh, const struct tc_assign *a,
			 unsigned extra_flags)
{
	unsigned fl = extra_flags | (sh->opt_allexport ? TC_VAR_EXPORT : 0);

	if (a->is_compound) {
		const struct tc_var_entry *e = tc_var_find(&sh->vars, a->name);
		bool is_assoc = (e && (e->flags & TC_VAR_ASSOC)) ||
				((extra_flags & TC_VAR_ASSOC) != 0);
		if (is_assoc) {
			if (!a->append) {
				int sr = tc_var_reset_assoc(&sh->vars, a->name, fl);
				if (sr != TC_VAR_OK)
					return report_var_err(sh, a->line, a->name, sr);
			} else if (!e) {
				int sr = tc_var_reset_assoc(&sh->vars, a->name, fl);
				if (sr != TC_VAR_OK)
					return report_var_err(sh, a->line, a->name, sr);
			}
			for (size_t j = 0; j < a->nitems; j++) {
				const struct tc_array_item *it = &a->items[j];
				if (!it->has_sub) {
					/* Support bash alternating key value pairs if unsubscripted. */
					char *key = NULL;
					int er = tc_expand_single(sh, &it->value, false, &key);
					if (er != TC_EXP_OK)
						return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
					char *val = "";
					if (j + 1 < a->nitems && !a->items[j + 1].has_sub) {
						j++;
						er = tc_expand_single(sh, &a->items[j].value, false, &val);
						if (er != TC_EXP_OK)
							return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
					}
					int sr = tc_var_set_elem_key(&sh->vars, a->name, key, val, false, fl);
					if (sr != TC_VAR_OK)
						return report_var_err(sh, it->line, a->name, sr);
					continue;
				}
				char *key = NULL;
				int er = expand_sub_word(sh, it->sub, it->line, &key);
				if (er != TC_EXP_OK)
					return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
				char *val = NULL;
				er = tc_expand_single(sh, &it->value, true, &val);
				if (er != TC_EXP_OK)
					return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
				int sr = tc_var_set_elem_key(&sh->vars, a->name, key, val,
							     it->append, fl);
				if (sr != TC_VAR_OK)
					return report_var_err(sh, it->line, a->name, sr);
			}
			return 0;
		}

		/* Indexed array compound assignment. */
		if (!a->append) {
			int sr = tc_var_reset_array(&sh->vars, a->name, fl);
			if (sr != TC_VAR_OK)
				return report_var_err(sh, a->line, a->name, sr);
		} else {
			int sr = tc_var_declare(&sh->vars, a->name, TC_VAR_ARRAY | fl, 0);
			if (sr != TC_VAR_OK)
				return report_var_err(sh, a->line, a->name, sr);
		}
		const struct tc_var_entry *cur = tc_var_find(&sh->vars, a->name);
		int64_t next_idx = (a->append && cur && cur->arr && cur->arr->len > 0)
					   ? tc_array_max_idx(cur->arr) + 1
					   : 0;
		for (size_t j = 0; j < a->nitems; j++) {
			const struct tc_array_item *it = &a->items[j];
			if (it->has_sub) {
				char *sub_s = NULL;
				int er = expand_sub_word(sh, it->sub, it->line, &sub_s);
				if (er != TC_EXP_OK)
					return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
				int64_t idx = 0;
				int ar = tc_arith_eval(sh, sub_s, it->line, &idx);
				if (ar != TC_ARITH_OK)
					return ar == TC_ARITH_NOMEM ? TC_STATUS_USAGE : 1;
				char *val = NULL;
				er = tc_expand_single(sh, &it->value, true, &val);
				if (er != TC_EXP_OK)
					return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
				int sr = tc_var_set_elem_idx(&sh->vars, a->name, idx, val,
							     it->append, fl);
				if (sr != TC_VAR_OK)
					return report_var_err(sh, it->line, a->name, sr);
				cur = tc_var_find(&sh->vars, a->name);
				int64_t ridx = idx;
				if (cur && cur->arr)
					(void)tc_array_resolve_idx(cur->arr, idx, &ridx);
				next_idx = ridx + 1;
			} else {
				char **ewords = NULL;
				size_t ecount = 0;
				int er = tc_expand_words(sh, &it->value, 1, &ewords, &ecount);
				if (er != TC_EXP_OK)
					return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
				for (size_t k = 0; k < ecount; k++) {
					int sr = tc_var_set_elem_idx(&sh->vars, a->name,
								     next_idx, ewords[k],
								     false, fl);
					if (sr != TC_VAR_OK)
						return report_var_err(sh, it->line, a->name, sr);
					next_idx++;
				}
			}
		}
		return 0;
	}

	if (a->has_sub) {
		const struct tc_var_entry *e = tc_var_find(&sh->vars, a->name);
		bool is_assoc = (e != NULL && (e->flags & TC_VAR_ASSOC) != 0);
		char *sub_s = NULL;
		int er = expand_sub_word(sh, a->sub, a->line, &sub_s);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
		char *val = NULL;
		er = tc_expand_single(sh, &a->value, true, &val);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
		int sr;
		if (is_assoc) {
			sr = tc_var_set_elem_key(&sh->vars, a->name, sub_s, val,
						 a->append, fl);
		} else {
			int64_t idx = 0;
			int ar = tc_arith_eval(sh, sub_s, a->line, &idx);
			if (ar != TC_ARITH_OK)
				return ar == TC_ARITH_NOMEM ? TC_STATUS_USAGE : 1;
			sr = tc_var_set_elem_idx(&sh->vars, a->name, idx, val,
						 a->append, fl);
		}
		if (sr != TC_VAR_OK)
			return report_var_err(sh, a->line, a->name, sr);
		return 0;
	}

	char *val = NULL;
	int er = tc_expand_single(sh, &a->value, true, &val);
	if (er != TC_EXP_OK)
		return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
	int sr = a->append ? tc_var_append_scalar(&sh->vars, a->name, val, fl)
			   : tc_var_set(&sh->vars, a->name, val, fl);
	if (sr != TC_VAR_OK)
		return report_var_err(sh, a->line, a->name, sr);
	return 0;
}

/* Apply assignments directly to sh->vars (left-to-right). */
static int apply_assigns_persist(struct tc_shell *sh,
				 const struct tc_assign *assigns, size_t n)
{
	struct tc_var_kv *traced = NULL;
	bool can_trace = sh->opt_xtrace && n > 0;
	for (size_t i = 0; i < n; i++) {
		if (assigns[i].is_compound || assigns[i].has_sub || assigns[i].append) {
			can_trace = false;
			break;
		}
	}
	if (can_trace)
		traced = tc_arena_alloc(&sh->arena, n * sizeof(*traced));

	for (size_t i = 0; i < n; i++) {
		int ar = tc_exec_apply_assign(sh, &assigns[i], 0);
		if (ar != 0)
			return ar;
		if (traced) {
			traced[i].name = assigns[i].name;
			const char *v = tc_var_get(&sh->vars, assigns[i].name);
			traced[i].value = v ? v : "";
		}
	}
	if (traced)
		trace_command(sh, traced, n, NULL, 0, assigns[0].line);
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
		if (assigns[i].is_compound || assigns[i].has_sub) {
			tc_error(sh, assigns[i].line,
				 "%s: cannot use array assignment as command prefix",
				 assigns[i].name);
			return 1;
		}
		if (tc_var_is_readonly(&sh->vars, assigns[i].name)) {
			tc_error(sh, assigns[i].line, "%s: readonly variable",
				 assigns[i].name);
			return fatal_exp_error(sh);
		}
		char *val = NULL;
		int er = tc_expand_single(sh, &assigns[i].value, true, &val);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
		if (assigns[i].append) {
			const char *cur = tc_var_get(&sh->vars, assigns[i].name);
			size_t cl = cur ? strlen(cur) : 0;
			size_t vl = strlen(val);
			char *joined = tc_arena_alloc(&sh->arena, cl + vl + 1);
			if (!joined) {
				tc_error(sh, assigns[i].line, "out of memory");
				return TC_STATUS_USAGE;
			}
			if (cl)
				memcpy(joined, cur, cl);
			memcpy(joined + cl, val, vl + 1);
			val = joined;
		}
		kvs[i].name = assigns[i].name;
		kvs[i].value = val;
	}
	*out_kvs = kvs;
	return 0;
}

static int exec_func(struct tc_shell *sh, struct tc_func *fn,
		     int argc, char **argv,
		     const struct tc_var_kv *kvs, size_t na,
		     const struct tc_redir *redirs, size_t nr,
		     unsigned long line)
{
	unsigned limit = effective_func_limit(sh);
	if (sh->func_depth >= limit) {
		tc_error(sh, line, "%s: maximum function nesting level exceeded (%u)",
			 argv[0], limit);
		return 1;
	}

	struct fd_save *saves = NULL;
	size_t nsaves = 0;
	pid_t *writers = NULL;
	size_t nwriters = 0;
	if (apply_redirs(sh, redirs, nr, &saves, &nsaves, &writers, &nwriters) < 0)
		return 1;

	struct tc_var_save *vsaves = NULL;
	size_t nvsaves = 0;
	if (na > 0) {
		vsaves = tc_arena_alloc(&sh->arena, na * sizeof(*vsaves));
		if (!vsaves ||
		    tc_var_push_overrides(&sh->vars, kvs, na, vsaves, &nvsaves) != TC_VAR_OK) {
			tc_error(sh, line, "out of memory");
			restore_fds(saves, nsaves);
			for (size_t j = 0; j < nwriters; j++)
				wait_child(writers[j]);
			return TC_STATUS_USAGE;
		}
	}

	char **old_params = sh->params;
	size_t old_nparams = sh->nparams;
	sh->params = NULL;
	sh->nparams = 0;
	if (argc > 1 && tc_shell_set_params(sh, argc - 1, argv + 1) != TC_OK) {
		sh->params = old_params;
		sh->nparams = old_nparams;
		tc_error(sh, line, "out of memory");
		if (vsaves)
			tc_var_pop_overrides(&sh->vars, vsaves, nvsaves);
		restore_fds(saves, nsaves);
		for (size_t j = 0; j < nwriters; j++)
			wait_child(writers[j]);
		return TC_STATUS_USAGE;
	}

	unsigned old_loop_depth = sh->loop_depth;
	sh->loop_depth = 0;
	sh->func_depth++;
	tc_func_ref(fn);

	struct tc_arena_mark mark = tc_arena_mark(&sh->arena);
	int status = exec_one(sh, fn->body);
	if (sh->flow == TC_FLOW_RETURN) {
		sh->flow = TC_FLOW_NONE;
		status = sh->return_status;
	}
	tc_arena_rewind(&sh->arena, mark);

	tc_func_unref(fn);
	sh->func_depth--;
	sh->loop_depth = old_loop_depth;

	(void)tc_shell_set_params(sh, 0, NULL);
	sh->params = old_params;
	sh->nparams = old_nparams;

	if (vsaves)
		tc_var_pop_overrides(&sh->vars, vsaves, nvsaves);
	restore_fds(saves, nsaves);
	for (size_t i = 0; i < nwriters; i++)
		wait_child(writers[i]);
	return status;
}

static int exec_builtin(struct tc_shell *sh, const struct tc_builtin *bi,
			int argc, char **argv,
			const struct tc_var_kv *kvs, size_t na,
			const struct tc_redir *redirs, size_t nr,
			unsigned long line)
{
	struct fd_save *saves = NULL;
	size_t nsaves = 0;
	pid_t *writers = NULL;
	size_t nwriters = 0;
	if (apply_redirs(sh, redirs, nr, &saves, &nsaves, &writers, &nwriters) < 0)
		return bi->special ? fatal_exp_error(sh) : 1;

	struct tc_var_save *vsaves = NULL;
	size_t nvsaves = 0;
	if (na > 0) {
		if (bi->special) {
			unsigned fl = sh->opt_allexport ? TC_VAR_EXPORT : 0;
			for (size_t i = 0; i < na; i++) {
				int sr = tc_var_set(&sh->vars, kvs[i].name, kvs[i].value, fl);
				if (sr != TC_VAR_OK) {
					tc_error(sh, line, "out of memory");
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
				tc_error(sh, line, "out of memory");
				restore_fds(saves, nsaves);
				for (size_t j = 0; j < nwriters; j++)
					wait_child(writers[j]);
				return TC_STATUS_USAGE;
			}
		}
	}

	int status = bi->fn(sh, argc, argv, line);
	if (vsaves)
		tc_var_pop_overrides(&sh->vars, vsaves, nvsaves);
	restore_fds(saves, nsaves);
	for (size_t i = 0; i < nwriters; i++)
		wait_child(writers[i]);
	return status;
}

static int exec_simple_body(struct tc_shell *sh, const struct tc_node *n)
{
	size_t na = n->u.simple.nassigns;
	size_t nw = n->u.simple.nwords;
	size_t nr = n->u.simple.nredirs;
	char **argv = NULL;
	size_t argc = 0;

	sh->cmdsub_ran = false;
	sh->cmdsub_status = 0;

	/* Fast path for declaration builtins carrying compound decl_assign args. */
	bool has_decl_assign = false;
	for (size_t i = 1; i < nw; i++) {
		if (n->u.simple.words[i].decl_assign) {
			has_decl_assign = true;
			break;
		}
	}
	if (has_decl_assign) {
		char **cmd_argv = NULL;
		size_t cmd_argc = 0;
		int er = tc_expand_words(sh, &n->u.simple.words[0], 1, &cmd_argv, &cmd_argc);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
		struct tc_var_kv *kvs = NULL;
		if (na > 0) {
			int ar = expand_assigns_kv(sh, n->u.simple.assigns, na, &kvs);
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
		const struct tc_builtin *bi = cmd_argc > 0 ? tc_builtin_find(cmd_argv[0]) : NULL;
		struct tc_var_save *vsaves = NULL;
		size_t nvsaves = 0;
		if (na > 0) {
			if (bi && bi->special) {
				unsigned fl = sh->opt_allexport ? TC_VAR_EXPORT : 0;
				for (size_t i = 0; i < na; i++)
					(void)tc_var_set(&sh->vars, kvs[i].name, kvs[i].value, fl);
			} else {
				vsaves = tc_arena_alloc(&sh->arena, na * sizeof(*vsaves));
				if (vsaves)
					(void)tc_var_push_overrides(&sh->vars, kvs, na, vsaves, &nvsaves);
			}
		}
		int status = tc_exec_decl_builtin(sh, cmd_argc > 0 ? cmd_argv[0] : "declare",
						  n->u.simple.words, nw, n->line);
		if (vsaves)
			tc_var_pop_overrides(&sh->vars, vsaves, nvsaves);
		restore_fds(saves, nsaves);
		for (size_t i = 0; i < nwriters; i++)
			wait_child(writers[i]);
		return status;
	}

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
		return sh->cmdsub_ran ? sh->cmdsub_status : 0;
	}

	struct tc_var_kv *kvs = NULL;
	if (na > 0) {
		int ar = expand_assigns_kv(sh, n->u.simple.assigns, na, &kvs);
		if (ar != 0)
			return ar;
	}

	if (sh->opt_xtrace)
		trace_command(sh, kvs, na, argv, argc, n->line);

	/* POSIX command search order (XCU 2.9.1.1):
	 * 1. Special builtins
	 * 2. Shell functions
	 * 3. Regular builtins
	 * 4. External commands */
	const struct tc_builtin *bi = tc_builtin_find(argv[0]);
	if (bi && bi->special) {
		if (strcmp(argv[0], "exec") == 0) {
			pid_t *writers = NULL;
			size_t nwriters = 0;
			if (apply_redirs(sh, n->u.simple.redirs, nr,
					 NULL, &(size_t){0}, &writers, &nwriters) < 0)
				return fatal_exp_error(sh);
			size_t ci = 1;
			if (ci < argc && strcmp(argv[ci], "--") == 0)
				ci++;
			if (ci >= argc) {
				unsigned fl = sh->opt_allexport ? TC_VAR_EXPORT : 0;
				for (size_t i = 0; i < na; i++) {
					if (tc_var_set(&sh->vars, kvs[i].name,
						       kvs[i].value, fl) != TC_VAR_OK) {
						tc_error(sh, n->line, "out of memory");
						for (size_t j = 0; j < nwriters; j++)
							wait_child(writers[j]);
						return TC_STATUS_USAGE;
					}
				}
				for (size_t j = 0; j < nwriters; j++)
					wait_child(writers[j]);
				return 0;
			}
			tc_signals_child();
			int st = run_external(sh, argv + ci, kvs, na, NULL, false, n->line);
			for (size_t j = 0; j < nwriters; j++)
				wait_child(writers[j]);
			if (!sh->interactive) {
				sh->exit_requested = true;
				sh->exit_status = st;
			}
			return st;
		}
		return exec_builtin(sh, bi, (int)argc, argv, kvs, na,
				    n->u.simple.redirs, nr, n->line);
	}

	struct tc_func *fn = tc_func_find(&sh->funcs, argv[0]);
	if (fn)
		return exec_func(sh, fn, (int)argc, argv, kvs, na,
				 n->u.simple.redirs, nr, n->line);

	if (bi)
		return exec_builtin(sh, bi, (int)argc, argv, kvs, na,
				    n->u.simple.redirs, nr, n->line);

	const char *cached_path = NULL;
	bool path_overridden = false;
	for (size_t i = 0; i < na; i++) {
		if (strcmp(kvs[i].name, "PATH") == 0) {
			path_overridden = true;
			break;
		}
	}
	if (!path_overridden && argv[0][0] != '\0' && !strchr(argv[0], '/')) {
		tc_hash_sync(&sh->hash, sh->vars.path_gen);
		cached_path = tc_hash_get(&sh->hash, argv[0], true);
		if (!cached_path) {
			char *found = tc_exec_find_in_path(sh, argv[0], false);
			if (found) {
				if (tc_hash_put(&sh->hash, argv[0], found, true) == 0)
					cached_path = tc_hash_get(&sh->hash, argv[0], false);
				free(found);
			}
		}
	}

	bool jc = sh->opt_monitor && sh->job_control;
	pid_t pid = fork();
	if (pid < 0) {
		tc_error(sh, n->line, "cannot start %s: %s", argv[0], strerror(errno));
		return TC_STATUS_USAGE;
	}
	if (pid == 0) {
		if (jc)
			(void)setpgid(0, 0);
		tc_signals_child();
		pid_t *writers = NULL;
		size_t nwriters = 0;
		if (apply_redirs(sh, n->u.simple.redirs, nr,
				 NULL, &(size_t){0}, &writers, &nwriters) < 0)
			_exit(1);
		_exit(run_external(sh, argv, kvs, na, cached_path, false, n->line));
	}
	if (jc) {
		(void)setpgid(pid, pid);
		const char *lbl = (sh->editor && sh->editor->last_cmd)
					  ? sh->editor->last_cmd
					  : argv[0];
		struct tc_job *job = tc_jobs_add(&sh->jobs, pid, &pid, 1,
						 lbl, TC_JOB_RUNNING);
		if (job)
			return tc_jobs_wait_fg(sh, job, false);
	}
	return wait_child(pid);
}

static int exec_simple(struct tc_shell *sh, const struct tc_node *n)
{
	int status = exec_simple_body(sh, n);
	maybe_errexit(sh, status);
	return status;
}

int tc_exec_command_builtin(struct tc_shell *sh, int argc, char **argv,
			    bool default_path, unsigned long line)
{
	const struct tc_builtin *bi = tc_builtin_find(argv[0]);
	if (bi) {
		if (bi->special) {
			if (strcmp(argv[0], "exec") == 0) {
				int ci = 1;
				if (ci < argc && strcmp(argv[ci], "--") == 0)
					ci++;
				if (ci >= argc)
					return 0;
				tc_signals_child();
				return run_external(sh, argv + ci, NULL, 0, NULL,
						    default_path, line);
			}
			bool prev = sh->in_command_builtin;
			sh->in_command_builtin = true;
			int st = bi->fn(sh, argc, argv, line);
			sh->in_command_builtin = prev;
			return st;
		}
		return bi->fn(sh, argc, argv, line);
	}

	const char *cached_path = NULL;
	if (!default_path && argv[0][0] != '\0' && !strchr(argv[0], '/')) {
		tc_hash_sync(&sh->hash, sh->vars.path_gen);
		cached_path = tc_hash_get(&sh->hash, argv[0], true);
		if (!cached_path) {
			char *found = tc_exec_find_in_path(sh, argv[0], false);
			if (found) {
				if (tc_hash_put(&sh->hash, argv[0], found, true) == 0)
					cached_path = tc_hash_get(&sh->hash, argv[0], false);
				free(found);
			}
		}
	}

	bool jc = sh->opt_monitor && sh->job_control;
	pid_t pid = fork();
	if (pid < 0) {
		tc_error(sh, line, "cannot start %s: %s", argv[0], strerror(errno));
		return TC_STATUS_USAGE;
	}
	if (pid == 0) {
		if (jc)
			(void)setpgid(0, 0);
		tc_signals_child();
		_exit(run_external(sh, argv, NULL, 0, cached_path, default_path, line));
	}
	if (jc) {
		(void)setpgid(pid, pid);
		const char *lbl = (sh->editor && sh->editor->last_cmd)
					  ? sh->editor->last_cmd
					  : argv[0];
		struct tc_job *job = tc_jobs_add(&sh->jobs, pid, &pid, 1,
						 lbl, TC_JOB_RUNNING);
		if (job)
			return tc_jobs_wait_fg(sh, job, false);
	}
	return wait_child(pid);
}

static int exec_pipeline(struct tc_shell *sh, const struct tc_node *n)
{
	size_t ncmds = n->u.pipeline.ncmds;
	if (ncmds == 1) {
		if (n->u.pipeline.bang)
			sh->errexit_ignore++;
		int status = exec_one(sh, n->u.pipeline.cmds[0]);
		if (n->u.pipeline.bang)
			sh->errexit_ignore--;
		return n->u.pipeline.bang ? (status == 0 ? 1 : 0) : status;
	}

	pid_t *pids = tc_arena_alloc(&sh->arena, ncmds * sizeof(*pids));
	if (!pids) {
		tc_error(sh, n->line, "out of memory");
		return TC_STATUS_USAGE;
	}

	if (n->u.pipeline.bang)
		sh->errexit_ignore++;

	bool jc = sh->opt_monitor && sh->job_control;
	pid_t pgid = 0;
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
			if (n->u.pipeline.bang)
				sh->errexit_ignore--;
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
			if (n->u.pipeline.bang)
				sh->errexit_ignore--;
			return TC_STATUS_USAGE;
		}
		if (pid == 0) {
			if (jc)
				(void)setpgid(0, pgid);
			tc_signals_child();
			tc_traps_subshell_reset(&sh->traps);
			tc_bg_reset_subshell(&sh->bg);
			tc_jobs_reset_subshell(&sh->jobs);
			sh->interactive = false;
			sh->job_control = false;
			sh->opt_monitor = false;
			sh->subshell_depth++;
			sh->loop_depth = 0;
			sh->func_depth = 0;
			sh->dot_depth = 0;
			sh->flow = TC_FLOW_NONE;
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
			int st = exec_one(sh, n->u.pipeline.cmds[i]);
			sh->status = sh->exit_requested ? sh->exit_status : st;
			tc_trap_run_exit(sh);
			_exit(sh->exit_requested ? sh->exit_status : st);
		}
		if (jc) {
			if (pgid == 0)
				pgid = pid;
			(void)setpgid(pid, pgid);
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
	if (jc && pgid > 0) {
		const char *lbl = (sh->editor && sh->editor->last_cmd)
					  ? sh->editor->last_cmd
					  : "(pipeline)";
		struct tc_job *job = tc_jobs_add(&sh->jobs, pgid, pids, forked,
						 lbl, TC_JOB_RUNNING);
		if (job) {
			status = tc_jobs_wait_fg(sh, job, sh->opt_pipefail);
		} else {
			for (size_t i = 0; i < forked; i++) {
				int st = wait_child(pids[i]);
				if (sh->opt_pipefail) {
					if (st != 0)
						status = st;
				} else {
					status = st;
				}
			}
		}
	} else {
		for (size_t i = 0; i < forked; i++) {
			int st = wait_child(pids[i]);
			if (sh->opt_pipefail) {
				if (st != 0)
					status = st;
			} else {
				status = st;
			}
		}
	}
	if (n->u.pipeline.bang) {
		sh->errexit_ignore--;
		return status == 0 ? 1 : 0;
	}
	maybe_errexit(sh, status);
	return status;
}

static int exec_redir(struct tc_shell *sh, const struct tc_node *n)
{
	struct fd_save *saves = NULL;
	size_t nsaves = 0;
	pid_t *writers = NULL;
	size_t nwriters = 0;
	if (apply_redirs(sh, n->u.redir.redirs, n->u.redir.nredirs,
			 &saves, &nsaves, &writers, &nwriters) < 0) {
		maybe_errexit(sh, 1);
		return 1;
	}
	int status = exec_one(sh, n->u.redir.child);
	restore_fds(saves, nsaves);
	for (size_t i = 0; i < nwriters; i++)
		wait_child(writers[i]);
	return status;
}

static int exec_subshell(struct tc_shell *sh, const struct tc_node *n)
{
	bool jc = sh->opt_monitor && sh->job_control;
	pid_t pid = fork();
	if (pid < 0) {
		tc_error(sh, n->line, "cannot fork subshell: %s", strerror(errno));
		return TC_STATUS_USAGE;
	}
	if (pid == 0) {
		if (jc)
			(void)setpgid(0, 0);
		tc_signals_child();
		tc_traps_subshell_reset(&sh->traps);
		tc_bg_reset_subshell(&sh->bg);
		tc_jobs_reset_subshell(&sh->jobs);
		sh->interactive = false;
		sh->job_control = false;
		sh->opt_monitor = false;
		sh->subshell_depth++;
		sh->loop_depth = 0;
		sh->func_depth = 0;
		sh->dot_depth = 0;
		sh->flow = TC_FLOW_NONE;
		int st = tc_exec(sh, n->u.group.body);
		if (sh->flow == TC_FLOW_RETURN)
			st = sh->return_status;
		sh->status = sh->exit_requested ? sh->exit_status : st;
		tc_trap_run_exit(sh);
		_exit(sh->exit_requested ? sh->exit_status : st);
	}
	int st;
	if (jc) {
		(void)setpgid(pid, pid);
		const char *lbl = (sh->editor && sh->editor->last_cmd)
					  ? sh->editor->last_cmd
					  : "(subshell)";
		struct tc_job *job = tc_jobs_add(&sh->jobs, pid, &pid, 1,
						 lbl, TC_JOB_RUNNING);
		st = job ? tc_jobs_wait_fg(sh, job, false) : wait_child(pid);
	} else {
		st = wait_child(pid);
	}
	maybe_errexit(sh, st);
	return st;
}

static int exec_bg(struct tc_shell *sh, const struct tc_node *n)
{
	bool jc = sh->opt_monitor && sh->job_control;
	pid_t pid = fork();
	if (pid < 0) {
		tc_error(sh, n->line, "cannot fork background command: %s",
			 strerror(errno));
		return TC_STATUS_USAGE;
	}
	if (pid == 0) {
		if (jc) {
			(void)setpgid(0, 0);
			tc_signals_child();
		} else {
			tc_signals_bg_child();
		}
		tc_traps_subshell_reset(&sh->traps);
		tc_bg_reset_subshell(&sh->bg);
		tc_jobs_reset_subshell(&sh->jobs);
		sh->interactive = false;
		sh->job_control = false;
		sh->opt_monitor = false;
		sh->subshell_depth++;
		sh->loop_depth = 0;
		sh->func_depth = 0;
		sh->dot_depth = 0;
		sh->flow = TC_FLOW_NONE;
		/* POSIX XCU 2.9.3.1: stdin of an async list is /dev/null
		 * before explicit redirections when job control is off. */
		if (!jc) {
			int devnull = open("/dev/null", O_RDONLY);
			if (devnull >= 0) {
				if (devnull != STDIN_FILENO) {
					(void)dup2(devnull, STDIN_FILENO);
					close(devnull);
				}
			}
		}
		int st = tc_exec(sh, n->u.bg.child);
		if (sh->flow == TC_FLOW_RETURN)
			st = sh->return_status;
		sh->status = sh->exit_requested ? sh->exit_status : st;
		tc_trap_run_exit(sh);
		_exit(sh->exit_requested ? sh->exit_status : st);
	}
	sh->last_bg_pid = pid;
	(void)tc_bg_add(&sh->bg, pid);
	if (jc) {
		(void)setpgid(pid, pid);
		const char *lbl = (sh->editor && sh->editor->last_cmd)
					  ? sh->editor->last_cmd
					  : "(background)";
		struct tc_job *job = tc_jobs_add(&sh->jobs, pid, &pid, 1,
						 lbl, TC_JOB_RUNNING);
		if (job && sh->interactive) {
			char msg[64];
			int mn = snprintf(msg, sizeof(msg), "[%d] %ld\n",
					  job->id, (long)pid);
			if (mn > 0)
				(void)tc_write_all(STDERR_FILENO, msg, (size_t)mn);
		}
	}
	return 0;
}

static int exec_if(struct tc_shell *sh, const struct tc_node *n)
{
	for (size_t i = 0; i < n->u.if_cmd.nbranches; i++) {
		sh->errexit_ignore++;
		int cstat = tc_exec(sh, n->u.if_cmd.branches[i].cond);
		sh->errexit_ignore--;
		if (flow_stop(sh))
			return cstat;
		if (cstat == 0)
			return tc_exec(sh, n->u.if_cmd.branches[i].body);
	}
	if (n->u.if_cmd.else_body)
		return tc_exec(sh, n->u.if_cmd.else_body);
	return 0;
}

/* Returns: 0 = normal, 1 = break out of this loop, 2 = continue this loop. */
static int handle_loop_flow(struct tc_shell *sh, int *last_status, int cur_status)
{
	if (sh->exit_requested || sh->flow == TC_FLOW_RETURN) {
		*last_status = cur_status;
		return 1;
	}
	if (sh->flow == TC_FLOW_BREAK) {
		if (--sh->flow_count == 0)
			sh->flow = TC_FLOW_NONE;
		*last_status = 0;
		return 1;
	}
	if (sh->flow == TC_FLOW_CONTINUE) {
		if (--sh->flow_count == 0) {
			sh->flow = TC_FLOW_NONE;
			*last_status = 0;
			return 2;
		}
		*last_status = 0;
		return 1;
	}
	return 0;
}

static int exec_loop(struct tc_shell *sh, const struct tc_node *n)
{
	sh->loop_depth++;
	struct tc_arena_mark mark = tc_arena_mark(&sh->arena);
	int last_status = 0;

	for (;;) {
		tc_arena_rewind(&sh->arena, mark);
		sh->errexit_ignore++;
		int cstat = tc_exec(sh, n->u.loop.cond);
		sh->errexit_ignore--;
		int fl = handle_loop_flow(sh, &last_status, cstat);
		if (fl == 1)
			break;
		if (fl == 2)
			continue;

		bool keep = n->u.loop.until ? (cstat != 0) : (cstat == 0);
		if (!keep)
			break;

		int bstat = tc_exec(sh, n->u.loop.body);
		fl = handle_loop_flow(sh, &last_status, bstat);
		if (fl == 1)
			break;
		if (fl == 2)
			continue;
		last_status = bstat;
	}

	tc_arena_rewind(&sh->arena, mark);
	sh->loop_depth--;
	return last_status;
}

static int exec_for(struct tc_shell *sh, const struct tc_node *n)
{
	char **items = NULL;
	size_t nitems = 0;

	if (!n->u.for_cmd.has_in) {
		nitems = sh->nparams;
		if (nitems > 0) {
			items = tc_arena_alloc(&sh->arena, nitems * sizeof(*items));
			if (!items) {
				tc_error(sh, n->line, "out of memory");
				return TC_STATUS_USAGE;
			}
			for (size_t i = 0; i < nitems; i++) {
				items[i] = tc_arena_strndup(
					&sh->arena, sh->params[i],
					strlen(sh->params[i]));
				if (!items[i]) {
					tc_error(sh, n->line, "out of memory");
					return TC_STATUS_USAGE;
				}
			}
		}
	} else if (n->u.for_cmd.nwords > 0) {
		int er = tc_expand_words(sh, n->u.for_cmd.words,
					 n->u.for_cmd.nwords, &items, &nitems);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
	}

	if (nitems == 0)
		return 0;

	sh->loop_depth++;
	struct tc_arena_mark mark = tc_arena_mark(&sh->arena);
	int last_status = 0;

	for (size_t i = 0; i < nitems; i++) {
		tc_arena_rewind(&sh->arena, mark);
		int sr = tc_var_set(&sh->vars, n->u.for_cmd.name, items[i],
				    sh->opt_allexport ? TC_VAR_EXPORT : 0);
		if (sr == TC_VAR_ERR_READONLY) {
			tc_error(sh, n->line, "%s: readonly variable",
				 n->u.for_cmd.name);
			last_status = fatal_exp_error(sh);
			break;
		}
		if (sr != TC_VAR_OK) {
			tc_error(sh, n->line, "out of memory");
			last_status = TC_STATUS_USAGE;
			break;
		}
		int bstat = tc_exec(sh, n->u.for_cmd.body);
		int fl = handle_loop_flow(sh, &last_status, bstat);
		if (fl == 1)
			break;
		if (fl == 2)
			continue;
		last_status = bstat;
	}

	tc_arena_rewind(&sh->arena, mark);
	sh->loop_depth--;
	return last_status;
}

static int exec_case(struct tc_shell *sh, const struct tc_node *n)
{
	char *subject = NULL;
	int er = tc_expand_single(sh, &n->u.case_cmd.word, false, &subject);
	if (er != TC_EXP_OK)
		return er == TC_EXP_ERR ? fatal_exp_error(sh) : TC_STATUS_USAGE;
	size_t slen = strlen(subject);

	for (size_t i = 0; i < n->u.case_cmd.nitems; i++) {
		const struct tc_case_item *it = &n->u.case_cmd.items[i];
		bool matched = false;
		for (size_t j = 0; j < it->npatterns; j++) {
			struct tc_pat pat;
			er = tc_expand_pat(sh, &it->patterns[j], &pat);
			if (er != TC_EXP_OK)
				return er == TC_EXP_ERR ? fatal_exp_error(sh)
							: TC_STATUS_USAGE;
			if (tc_pat_match(&pat, subject, slen)) {
				matched = true;
				break;
			}
		}
		if (matched)
			return it->body ? tc_exec(sh, it->body) : 0;
	}
	return 0;
}

static int exec_funcdef(struct tc_shell *sh, const struct tc_node *n)
{
	if (tc_func_define(&sh->funcs, n->u.funcdef.name, n->u.funcdef.body) < 0) {
		tc_error(sh, n->line, "out of memory");
		return TC_STATUS_USAGE;
	}
	return 0;
}

static int exec_one(struct tc_shell *sh, const struct tc_node *n)
{
	if (sh->exec_depth >= TC_EXEC_DEPTH_MAX) {
		tc_error(sh, n->line, "execution nesting depth exceeds limit (%d)",
			 TC_EXEC_DEPTH_MAX);
		return 1;
	}
	sh->exec_depth++;
	if (n->line > 0)
		sh->lineno = n->line;
	int status;

	switch (n->kind) {
	case TC_NODE_SIMPLE:
		status = exec_simple(sh, n);
		break;
	case TC_NODE_PIPELINE:
		status = exec_pipeline(sh, n);
		break;
	case TC_NODE_AND_OR:
		for (size_t i = 0; i < n->u.and_or.nitems; i++) {
			const struct tc_andor_item *it = &n->u.and_or.items[i];
			if (it->op == TC_ANDOR_AND && sh->status != 0)
				continue;
			if (it->op == TC_ANDOR_OR && sh->status == 0)
				continue;
			bool not_last = (i + 1 < n->u.and_or.nitems);
			if (not_last)
				sh->errexit_ignore++;
			sh->status = exec_one(sh, it->cmd);
			if (not_last)
				sh->errexit_ignore--;
			if (flow_stop(sh))
				break;
		}
		status = sh->status;
		break;
	case TC_NODE_REDIR:
		status = exec_redir(sh, n);
		break;
	case TC_NODE_GROUP:
		status = tc_exec(sh, n->u.group.body);
		break;
	case TC_NODE_SUBSHELL:
		status = exec_subshell(sh, n);
		break;
	case TC_NODE_IF:
		status = exec_if(sh, n);
		break;
	case TC_NODE_LOOP:
		status = exec_loop(sh, n);
		break;
	case TC_NODE_FOR:
		status = exec_for(sh, n);
		break;
	case TC_NODE_CASE:
		status = exec_case(sh, n);
		break;
	case TC_NODE_FUNCDEF:
		status = exec_funcdef(sh, n);
		break;
	case TC_NODE_BG:
		status = exec_bg(sh, n);
		break;
	default:
		status = TC_STATUS_USAGE;
		break;
	}

	sh->exec_depth--;
	return status;
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
		tc_trap_run_pending(sh);
		tc_jobs_reap_nonblock(sh);
		if (flow_stop(sh) || (sh->opt_noexec && !sh->interactive))
			break;
	}
	return sh->status;
}

int tc_exec_input(struct tc_shell *sh, struct tc_input *in,
		  unsigned long call_line, bool *ran_any)
{
	if (ran_any)
		*ran_any = false;
	struct tc_input *saved_in = sh->in;
	sh->in = in;
	in->sh = sh;

	struct tc_parser p;
	if (tc_parser_init(&p, sh, in) != TC_OK) {
		tc_error(sh, call_line, "out of memory");
		sh->in = saved_in;
		tc_input_finish(in);
		return TC_STATUS_USAGE;
	}

	int status = 0;
	for (;;) {
		struct tc_arena_mark mark = tc_arena_mark(&sh->arena);
		struct tc_node *node = NULL;
		int r = tc_parse_next(&p, &node);
		if (r == TC_END) {
			tc_arena_rewind(&sh->arena, mark);
			break;
		}
		if (r == TC_OK) {
			if (node && !(sh->opt_noexec && !sh->interactive)) {
				if (ran_any)
					*ran_any = true;
				status = tc_exec(sh, node);
			}
			tc_arena_rewind(&sh->arena, mark);
			tc_trap_run_pending(sh);
			if (flow_stop(sh))
				break;
			continue;
		}
		tc_arena_rewind(&sh->arena, mark);
		status = TC_STATUS_USAGE;
		sh->status = status;
		if (!sh->interactive) {
			sh->exit_requested = true;
			sh->exit_status = status;
		}
		break;
	}

	tc_parser_finish(&p);
	tc_input_finish(in);
	sh->in = saved_in;
	return status;
}

int tc_exec_string(struct tc_shell *sh, const char *src, size_t len,
		   unsigned long call_line, bool *ran_any)
{
	struct tc_input in;
	tc_input_from_string(&in, src, len);
	return tc_exec_input(sh, &in, call_line, ran_any);
}

int tc_exec_cmdsub(struct tc_shell *sh, const struct tc_node *cmd,
		   unsigned long line, char **out, size_t *out_len)
{
	*out = "";
	*out_len = 0;
	if (!cmd) {
		sh->cmdsub_ran = true;
		sh->cmdsub_status = 0;
		char *empty = tc_arena_strndup(&sh->arena, "", 0);
		if (!empty) {
			tc_error(sh, line, "out of memory");
			return TC_ERR_NOMEM;
		}
		*out = empty;
		return TC_OK;
	}

	int pfd[2];
	if (pipe(pfd) < 0) {
		tc_error(sh, line, "cannot create pipe: %s", strerror(errno));
		return TC_ERR_IO;
	}
	pid_t pid = fork();
	if (pid < 0) {
		tc_error(sh, line, "cannot fork: %s", strerror(errno));
		close(pfd[0]);
		close(pfd[1]);
		return TC_ERR_IO;
	}
	if (pid == 0) {
		tc_signals_child();
		tc_traps_subshell_reset(&sh->traps);
		tc_bg_reset_subshell(&sh->bg);
		sh->interactive = false;
		sh->subshell_depth++;
		sh->loop_depth = 0;
		sh->func_depth = 0;
		sh->dot_depth = 0;
		sh->flow = TC_FLOW_NONE;
		close(pfd[0]);
		if (pfd[1] != STDOUT_FILENO) {
			if (dup2(pfd[1], STDOUT_FILENO) < 0)
				_exit(1);
			close(pfd[1]);
		}
		int st = tc_exec(sh, cmd);
		if (sh->flow == TC_FLOW_RETURN)
			st = sh->return_status;
		sh->status = sh->exit_requested ? sh->exit_status : st;
		tc_trap_run_exit(sh);
		_exit(sh->exit_requested ? sh->exit_status : st);
	}

	close(pfd[1]);
	size_t cap = 256, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(pfd[0]);
		wait_child(pid);
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}

	int err = TC_OK;
	char chunk[4096];
	for (;;) {
		ssize_t n = read(pfd[0], chunk, sizeof(chunk));
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			tc_error(sh, line, "read error: %s", strerror(errno));
			err = TC_ERR_IO;
			break;
		}
		/* Strip NUL bytes (not representable in shell C strings). */
		for (ssize_t i = 0; i < n; i++) {
			if (chunk[i] == '\0')
				continue;
			if (len >= TC_CMDSUB_MAX) {
				tc_error(sh, line,
					 "command substitution exceeds %zu bytes",
					 TC_CMDSUB_MAX);
				err = TC_ERR_NOMEM;
				break;
			}
			if (len == cap) {
				size_t ncap = cap * 2;
				if (ncap > TC_CMDSUB_MAX)
					ncap = TC_CMDSUB_MAX;
				char *nb = realloc(buf, ncap);
				if (!nb) {
					tc_error(sh, line, "out of memory");
					err = TC_ERR_NOMEM;
					break;
				}
				buf = nb;
				cap = ncap;
			}
			buf[len++] = chunk[i];
		}
		if (err != TC_OK)
			break;
	}
	close(pfd[0]);
	int st = wait_child(pid);
	sh->cmdsub_ran = true;
	sh->cmdsub_status = st;

	if (err != TC_OK) {
		free(buf);
		return err;
	}

	/* POSIX XCU 2.6.3: remove all trailing <newline> characters. */
	while (len > 0 && buf[len - 1] == '\n')
		len--;

	char *copy = tc_arena_strndup(&sh->arena, buf, len);
	free(buf);
	if (!copy) {
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	*out = copy;
	*out_len = len;
	return TC_OK;
}
