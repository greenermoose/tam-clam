/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tam-clam: the Tamlinux shell.
 *
 *   tam-clam -c 'commands' [name [args...]]
 *   tam-clam script [args...]
 *   tam-clam [-s]               read commands from standard input
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/exec.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"

#ifndef TC_VERSION
#define TC_VERSION "unknown"
#endif

static void usage(int fd)
{
	static const char text[] =
		"usage: tam-clam -c COMMANDS [NAME [ARGS...]]\n"
		"       tam-clam SCRIPT [ARGS...]\n"
		"       tam-clam [-s]\n"
		"       tam-clam --version\n";
	(void)tc_write_all(fd, text, sizeof(text) - 1);
}

/* Read and run every command from in. Returns the shell's exit status. */
static int run(struct tc_shell *sh, struct tc_input *in)
{
	struct tc_parser p;
	sh->in = in;
	if (tc_parser_init(&p, sh, in) != TC_OK) {
		tc_error(sh, 0, "out of memory");
		return TC_STATUS_USAGE;
	}
	for (;;) {
		struct tc_node *node;
		tc_sigint_seen = 0;
		int r = tc_parse_next(&p, &node);
		if (r == TC_END)
			break;
		if (r == TC_OK) {
			if (node)
				tc_exec(sh, node);
			tc_arena_reset(&sh->arena);
			if (sh->exit_requested)
				break;
			continue;
		}
		tc_arena_reset(&sh->arena);
		if (r == TC_ERR_INTR) {
			(void)tc_write_all(STDERR_FILENO, "\n", 1);
			tc_parser_reset(&p);
			sh->status = 130;
			continue;
		}
		sh->status = TC_STATUS_USAGE;
		/* A non-interactive shell stops at a syntax error (XCU 2.8.1). */
		if (!sh->interactive || r == TC_ERR_IO)
			break;
		tc_parser_recover(&p);
	}
	tc_parser_finish(&p);
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

int main(int argc, char **argv)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "tam-clam");

	int i = 1;
	const char *command = NULL;
	int read_stdin = 0;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--") == 0) {
			i++;
			break;
		} else if (strcmp(a, "--version") == 0) {
			static const char v[] = "tam-clam " TC_VERSION "\n";
			return tc_write_all(STDOUT_FILENO, v, sizeof(v) - 1) < 0 ? 1 : 0;
		} else if (strcmp(a, "--help") == 0) {
			usage(STDOUT_FILENO);
			return 0;
		} else if (strcmp(a, "-c") == 0) {
			if (++i >= argc) {
				tc_error(&sh, 0, "-c: a command string is needed");
				return TC_STATUS_USAGE;
			}
			command = argv[i];
			i++;
			break;
		} else if (strcmp(a, "-s") == 0) {
			read_stdin = 1;
			i++;
			break;
		} else {
			tc_error(&sh, 0, "%s: option not supported yet", a);
			usage(STDERR_FILENO);
			return TC_STATUS_USAGE;
		}
	}

	struct tc_input in;
	int status;
	if (command) {
		/* The word after the command string becomes $0. */
		if (i < argc)
			sh.name = argv[i];
		tc_input_from_string(&in, command, strlen(command));
		status = run(&sh, &in);
	} else if (!read_stdin && i < argc) {
		int fd;
		if ((status = open_script(&sh, argv[i], &fd)) != 0)
			return status;
		sh.name = argv[i];
		tc_input_from_fd(&in, fd, false);
		status = run(&sh, &in);
		close(fd);
	} else {
		tc_input_from_fd(&in, STDIN_FILENO, true);
		sh.interactive = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
		if (sh.interactive) {
			in.prompt = true;
			if (tc_signals_interactive() < 0)
				tc_error(&sh, 0, "cannot set up signals: %s", strerror(errno));
		}
		status = run(&sh, &in);
	}
	tc_shell_finish(&sh);
	return status;
}
