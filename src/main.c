/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tam-clam: the Tamlinux shell.
 *
 *   tam-clam -c 'commands' [name [args...]]
 *   tam-clam script [args...]
 *   tam-clam [-s] [args...]     read commands from standard input
 */
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/edit.h"
#include "tam-clam/exec.h"
#include "tam-clam/history.h"
#include "tam-clam/input.h"
#include "tam-clam/lint.h"
#include "tam-clam/options.h"
#include "tam-clam/parse.h"
#include "tam-clam/prompt.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"
#include "tam.h"

#ifndef TC_VERSION
#define TC_VERSION "unknown"
#endif

static void usage(int fd)
{
	static const char text[] =
		"usage: tam-clam -c COMMANDS [NAME [ARGS...]]\n"
		"       tam-clam SCRIPT [ARGS...]\n"
		"       tam-clam [--private] [--norc] [-s] [ARGS...]\n"
		"       tam-clam --lint FILE...\n"
		"       tam-clam -v | --version\n";
	(void)tc_write_all(fd, text, sizeof(text) - 1);
}

/* Read and run every command from in. Returns the shell's exit status. */
static int run(struct tc_shell *sh, struct tc_input *in)
{
	struct tc_parser p;
	sh->in = in;
	in->sh = sh;
	if (tc_parser_init(&p, sh, in) != TC_OK) {
		tc_error(sh, 0, "out of memory");
		return TC_STATUS_USAGE;
	}
	for (;;) {
		struct tc_node *node;
		if (sh->interactive)
			tc_jobs_notify(sh);
		tc_sigint_seen = 0;
		int r = tc_parse_next(&p, &node);
		if (r == TC_END)
			break;
		if (r == TC_OK) {
			const char *pre_pwd = tc_var_get(&sh->vars, "PWD");
			char *cwd_copy = pre_pwd ? strdup(pre_pwd) : NULL;
			if (node && !(sh->opt_noexec && !sh->interactive)) {
				if (sh->interactive) {
					const char *raw_cmd =
						(sh->editor && sh->editor->last_cmd)
							? sh->editor->last_cmd
							: "";
					tc_prompt_run_preexec(sh, raw_cmd);
				}
				tc_exec(sh, node);
			}
			if (sh->hist && sh->editor && sh->editor->last_cmd) {
				int st = sh->exit_requested ? sh->exit_status : sh->status;
				(void)tc_hist_add(sh->hist, sh->editor->last_cmd,
						  sh->editor->last_cmd_len, st,
						  cwd_copy ? cwd_copy : "");
				tc_editor_clear_last_cmd(sh->editor);
			}
			free(cwd_copy);
			tc_arena_reset(&sh->arena);
			tc_trap_run_pending(sh);
			tc_jobs_reap_nonblock(sh);
			if (sh->exit_requested)
				break;
			continue;
		}
		tc_arena_reset(&sh->arena);
		if (r == TC_ERR_INTR) {
			if (sh->editor)
				tc_editor_clear_last_cmd(sh->editor);
			(void)tc_write_all(STDERR_FILENO, "\n", 1);
			tc_parser_reset(&p);
			sh->status = 130;
			tc_trap_run_pending(sh);
			if (sh->exit_requested)
				break;
			continue;
		}
		sh->status = TC_STATUS_USAGE;
		if (sh->hist && sh->editor && sh->editor->last_cmd) {
			const char *pwd = tc_var_get(&sh->vars, "PWD");
			(void)tc_hist_add(sh->hist, sh->editor->last_cmd,
					  sh->editor->last_cmd_len, sh->status,
					  pwd ? pwd : "");
			tc_editor_clear_last_cmd(sh->editor);
		}
		/* A non-interactive shell stops at a syntax error (XCU 2.8.1). */
		if (!sh->interactive || r == TC_ERR_IO)
			break;
		tc_parser_recover(&p);
	}
	tc_parser_finish(&p);
	tc_input_finish(in);
	return sh->exit_requested ? sh->exit_status : sh->status;
}

static int open_script(struct tc_shell *sh, const char *path, int *out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		int e = errno;
		tc_error(sh, 0, "%s: %s", path, strerror(e));
		return e == ENOENT ? TC_STATUS_NOTFOUND : TC_STATUS_NOEXEC;
	}
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		tc_error(sh, 0, "%s: %s", path, strerror(EISDIR));
		close(fd);
		return TC_STATUS_NOEXEC;
	}
	/* Keep the script's descriptor out of the range scripts redirect. */
	int high = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	if (high >= 0) {
		close(fd);
		fd = high;
	}
	*out = fd;
	return 0;
}

static void load_interactive_rc(struct tc_shell *sh)
{
	if (getuid() != geteuid() || getgid() != getegid())
		return;
	char *path = tam_xdg_path(TAM_XDG_CONFIG, "tam-clam", "rc", false);
	if (!path)
		return;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno != ENOENT)
			tc_error(sh, 0, "%s: %s", path, strerror(errno));
		free(path);
		return;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		tc_error(sh, 0, "%s: %s", path, strerror(errno));
	} else if (!S_ISREG(st.st_mode)) {
		tc_error(sh, 0, "%s: startup file must be a regular file", path);
	} else {
		int script_fd = fcntl(fd, F_DUPFD_CLOEXEC, 10);
		if (script_fd < 0) {
			tc_error(sh, 0, "%s: %s", path, strerror(errno));
		} else {
			close(fd);
			fd = script_fd;
			struct tc_input in;
			tc_input_from_fd(&in, fd, false);
			sh->dot_depth++;
			int status = tc_exec_input(sh, &in, 0, NULL);
			sh->dot_depth--;
			if (sh->flow == TC_FLOW_RETURN)
				sh->flow = TC_FLOW_NONE;
			else if (status == TC_STATUS_USAGE && !sh->exit_requested)
				tc_error(sh, 0, "%s: startup returned status %d", path, status);
			sh->status = 0;
		}
	}
	close(fd);
	free(path);
}

int main(int argc, char **argv)
{
	(void)setlocale(LC_CTYPE, "");
	struct tc_shell sh;
	tc_shell_init(&sh, "tam-clam");

	int i = 1;
	const char *command = NULL;
	bool want_c = false;
	bool lint_mode = false;
	bool no_rc = false;
	int read_stdin = 0;
	for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') &&
	       argv[i][1] != '\0';
	     i++) {
		const char *a = argv[i];
		if (strcmp(a, "--") == 0) {
			i++;
			break;
		}
		if (strcmp(a, "--lint") == 0) {
			lint_mode = true;
			continue;
		}
		if (strcmp(a, "--private") == 0) {
			sh.private_mode = true;
			continue;
		}
		if (strcmp(a, "--norc") == 0) {
			no_rc = true;
			continue;
		}
		if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
			static const char v[] = "tam-clam " TC_VERSION "\n";
			int wr = tc_write_all(STDOUT_FILENO, v, sizeof(v) - 1) < 0 ? 1 : 0;
			tc_shell_finish(&sh);
			return wr;
		}
		if (strcmp(a, "--help") == 0) {
			usage(STDOUT_FILENO);
			tc_shell_finish(&sh);
			return 0;
		}
		bool enable = (a[0] == '-');
		bool stop_opts = false;
		for (const char *p = a + 1; *p; p++) {
			if (enable && *p == 'c') {
				want_c = true;
				stop_opts = true;
			} else if (enable && *p == 's') {
				read_stdin = 1;
			} else if (*p == 'o') {
				const char *optname = p[1] ? p + 1 : NULL;
				if (!optname && i + 1 < argc)
					optname = argv[++i];
				if (!optname) {
					if (tc_opt_print(&sh, !enable, 0) != 0) {
						tc_shell_finish(&sh);
						return 1;
					}
					break;
				}
				enum tc_opt_res or = tc_opt_set_name(&sh, optname, enable);
				if (or == TC_OPT_DEFERRED) {
					tc_error(&sh, 0, "%co %s: option not supported yet; see ROADMAP.md",
						 enable ? '-' : '+', optname);
					tc_shell_finish(&sh);
					return TC_STATUS_USAGE;
				}
				if (or == TC_OPT_INVALID) {
					tc_error(&sh, 0, "%co %s: invalid option name",
						 enable ? '-' : '+', optname);
					usage(STDERR_FILENO);
					tc_shell_finish(&sh);
					return TC_STATUS_USAGE;
				}
				break;
			} else {
				enum tc_opt_res or = tc_opt_set_char(&sh, *p, enable);
				if (or == TC_OPT_DEFERRED) {
					tc_error(&sh, 0, "%c%c: option not supported yet; see ROADMAP.md",
						 enable ? '-' : '+', *p);
					tc_shell_finish(&sh);
					return TC_STATUS_USAGE;
				}
				if (or == TC_OPT_INVALID) {
					tc_error(&sh, 0, "%c%c: invalid option",
						 enable ? '-' : '+', *p);
					usage(STDERR_FILENO);
					tc_shell_finish(&sh);
					return TC_STATUS_USAGE;
				}
			}
		}
		if (stop_opts) {
			i++;
			break;
		}
	}
	if (lint_mode) {
		if (i >= argc) {
			tc_error(&sh, 0, "--lint: at least one file is required");
			usage(STDERR_FILENO);
			tc_shell_finish(&sh);
			return TC_STATUS_USAGE;
		}
		int max_status = 0;
		for (; i < argc; i++) {
			int rc = tc_lint_file(&sh, argv[i]);
			if (rc > max_status)
				max_status = rc;
		}
		tc_shell_finish(&sh);
		return max_status;
	}
	if (want_c) {
		if (i >= argc) {
			tc_error(&sh, 0, "-c: a command string is needed");
			tc_shell_finish(&sh);
			return TC_STATUS_USAGE;
		}
		command = argv[i++];
	}

	struct tc_input in;
	int status;
	if (command) {
		/* The word after the command string becomes $0; the rest $1..$N. */
		if (i < argc) {
			sh.name = argv[i++];
			if (i < argc &&
			    tc_shell_set_params(&sh, argc - i, argv + i) != TC_OK) {
				tc_error(&sh, 0, "out of memory");
				tc_shell_finish(&sh);
				return TC_STATUS_USAGE;
			}
		}
		tc_input_from_string(&in, command, strlen(command));
		status = run(&sh, &in);
	} else if (!read_stdin && i < argc) {
		int fd;
		if ((status = open_script(&sh, argv[i], &fd)) != 0) {
			tc_shell_finish(&sh);
			return status;
		}
		sh.name = argv[i++];
		if (i < argc &&
		    tc_shell_set_params(&sh, argc - i, argv + i) != TC_OK) {
			tc_error(&sh, 0, "out of memory");
			close(fd);
			tc_shell_finish(&sh);
			return TC_STATUS_USAGE;
		}
		tc_input_from_fd(&in, fd, false);
		status = run(&sh, &in);
		close(fd);
	} else {
		if (i < argc &&
		    tc_shell_set_params(&sh, argc - i, argv + i) != TC_OK) {
			tc_error(&sh, 0, "out of memory");
			tc_shell_finish(&sh);
			return TC_STATUS_USAGE;
		}
		tc_input_from_fd(&in, STDIN_FILENO, true);
		sh.interactive = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
		struct tc_editor ed;
		struct tc_history hist;
		bool have_ed = false;
		bool have_hist = false;
		if (sh.interactive) {
			in.prompt = true;
			tc_traps_finish(&sh.traps);
			tc_traps_init(&sh.traps, true);
			if (tc_signals_interactive() < 0)
				tc_error(&sh, 0, "cannot set up signals: %s", strerror(errno));
			tc_jobs_init_terminal(&sh);
			const char *hf = tc_var_get(&sh.vars, "HISTFILE");
			char *xdg_hf = NULL;
			bool priv = sh.private_mode || (hf != NULL && hf[0] == '\0');
			if (!priv && !hf) {
				xdg_hf = tam_xdg_path(TAM_XDG_STATE, "tam-clam",
						      "history", true);
				hf = xdg_hf;
			}
			tc_hist_init(&hist, priv ? NULL : hf, priv);
			free(xdg_hf);
			if (!priv)
				(void)tc_hist_load(&hist);
			sh.hist = &hist;
			have_hist = true;

			const char *term = getenv("TERM");
			if (!term || strcmp(term, "dumb") != 0) {
				tc_editor_init(&ed, STDIN_FILENO, STDERR_FILENO);
				tc_editor_configure_child_termios(&ed);
				sh.editor = &ed;
				in.ed = &ed;
				have_ed = true;
			}
			if (!no_rc)
				load_interactive_rc(&sh);
		}
		status = sh.exit_requested ? sh.exit_status : run(&sh, &in);
		if (have_ed) {
			tc_editor_finish(&ed);
			sh.editor = NULL;
		}
		if (have_hist) {
			tc_hist_finish(&hist);
			sh.hist = NULL;
		}
	}
	tc_trap_run_exit(&sh);
	status = sh.exit_requested ? sh.exit_status : status;
	tc_shell_finish(&sh);
	return status;
}
