/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interactive C tests over a pseudo-terminal (plans/P3-interactive.md).
 * Uses POSIX posix_openpt/grantpt/unlockpt/ptsname; needs no Python/expect. */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "../check.h"

struct pty_sess {
	int master;
	pid_t pid;
	char out[32768];
	size_t out_len;
};

static int pty_start(struct pty_sess *s, const char *shell_path, const char *term)
{
	memset(s, 0, sizeof(*s));
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0)
		return -1;
	if (grantpt(m) < 0 || unlockpt(m) < 0) {
		close(m);
		return -1;
	}
	const char *sname = ptsname(m);
	if (!sname) {
		close(m);
		return -1;
	}
	struct winsize ws = {.ws_row = 24, .ws_col = 80};
	(void)ioctl(m, TIOCSWINSZ, &ws);

	pid_t pid = fork();
	if (pid < 0) {
		close(m);
		return -1;
	}
	if (pid == 0) {
		close(m);
		if (setsid() < 0)
			_exit(126);
		int sl = open(sname, O_RDWR);
		if (sl < 0)
			_exit(126);
		dup2(sl, STDIN_FILENO);
		dup2(sl, STDOUT_FILENO);
		dup2(sl, STDERR_FILENO);
		if (sl > 2)
			close(sl);
		setenv("PS1", "CLAM$ ", 1);
		setenv("PS2", "CLAM> ", 1);
		setenv("TERM", term ? term : "xterm-256color", 1);
		execl(shell_path, "tam-clam", (char *)NULL);
		_exit(127);
	}
	s->master = m;
	s->pid = pid;
	return 0;
}

static int pty_send(struct pty_sess *s, const char *data, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(s->master, data + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static bool pty_wait_after(struct pty_sess *s, const char *first,
			   const char *second, int timeout_ms)
{
	int elapsed = 0;
	while (elapsed < timeout_ms) {
		char *p = s->out_len > 0 ? strstr(s->out, first) : NULL;
		if (p && (!second || strstr(p + strlen(first), second)))
			return true;
		struct pollfd pfd = {.fd = s->master, .events = POLLIN};
		int r = poll(&pfd, 1, 20);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (r == 0) {
			elapsed += 20;
			continue;
		}
		char buf[1024];
		ssize_t n = read(s->master, buf, sizeof(buf));
		if (n <= 0)
			break;
		if (s->out_len + (size_t)n < sizeof(s->out)) {
			memcpy(s->out + s->out_len, buf, (size_t)n);
			s->out_len += (size_t)n;
			s->out[s->out_len] = '\0';
		}
	}
	char *p = s->out_len > 0 ? strstr(s->out, first) : NULL;
	return p && (!second || strstr(p + strlen(first), second));
}

static bool pty_wait_for(struct pty_sess *s, const char *needle, int timeout_ms)
{
	return pty_wait_after(s, needle, NULL, timeout_ms);
}

static void pty_clear_out(struct pty_sess *s)
{
	s->out_len = 0;
	s->out[0] = '\0';
}

static int pty_finish(struct pty_sess *s)
{
	int status = -1;
	bool reaped = false;
	int waited = 0;
	while (waited < 2000) {
		pid_t r = waitpid(s->pid, &status, WNOHANG);
		if (r == s->pid) {
			reaped = true;
			break;
		}
		struct pollfd pfd = {.fd = s->master, .events = POLLIN};
		if (poll(&pfd, 1, 20) > 0) {
			char drain[256];
			if (read(s->master, drain, sizeof(drain)) <= 0) {
				if (waitpid(s->pid, &status, 0) == s->pid)
					reaped = true;
				break;
			}
		}
		waited += 20;
	}
	if (!reaped) {
		kill(s->pid, SIGKILL);
		(void)waitpid(s->pid, &status, 0);
	}
	close(s->master);
	return (reaped && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

static void test_interactive_xterm(const char *sh)
{
	struct pty_sess s;
	CHECK(pty_start(&s, sh, "xterm-256color") == 0);
	CHECK(pty_wait_for(&s, "CLAM$ ", 2000));

	/* 1. Basic command. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "printf 'R1:%s\\n' hello-pty\r", 27) == 0);
	CHECK(pty_wait_after(&s, "R1:hello-pty", "CLAM$ ", 2000));

	/* 2. UTF-8 + wide CJK + Left + Backspace. */
	pty_clear_out(&s);
	const char *u = "printf 'R2:%s\\n' \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E!\x1b[D\x7f\r";
	CHECK(pty_send(&s, u, strlen(u)) == 0);
	CHECK(pty_wait_after(&s, "R2:\xE6\x97\xA5\xE6\x9C\xAC!", "CLAM$ ", 2000));

	/* 3. Shift+Left x2, Ctrl+C (Copy), End, Ctrl+V (Paste) -> "abab". */
	pty_clear_out(&s);
	const char *cp = "printf 'R3:%s\\n' ab\x1b[1;2D\x1b[1;2D\x03\x1b[F\x16\r";
	CHECK(pty_send(&s, cp, strlen(cp)) == 0);
	CHECK(pty_wait_after(&s, "R3:abab", "CLAM$ ", 2000));

	/* 4. Ctrl+X (Cut) then Ctrl+Z (Undo) keeps original command. */
	pty_clear_out(&s);
	const char *uz = "printf 'R4:%s\\n' undo-ok\x18\x1a\r";
	CHECK(pty_send(&s, uz, strlen(uz)) == 0);
	CHECK(pty_wait_after(&s, "R4:undo-ok", "CLAM$ ", 2000));

	/* 5. Alt+Enter multi-line editing. */
	pty_clear_out(&s);
	const char *ml = "printf 'R5A\\n'\x1b\rprintf 'R5B\\n'\r";
	CHECK(pty_send(&s, ml, strlen(ml)) == 0);
	CHECK(pty_wait_after(&s, "R5A", "R5B", 2000));
	CHECK(pty_wait_after(&s, "R5B", "CLAM$ ", 2000));

	/* 6. Bracketed paste inserts newline literally without executing until Enter. */
	pty_clear_out(&s);
	const char *bp = "\x1b[200~printf 'R6A\\n'\nprintf 'R6B\\n'\x1b[201~\r";
	CHECK(pty_send(&s, bp, strlen(bp)) == 0);
	CHECK(pty_wait_after(&s, "R6A", "R6B", 2000));
	CHECK(pty_wait_after(&s, "R6B", "CLAM$ ", 2000));

	/* 7. Ctrl+K clears buffer; typing fresh command runs only the fresh one. */
	pty_clear_out(&s);
	const char *kl = "echo SHOULD_NOT_RUN\x0b" "printf 'R7:ok\\n'\r";
	CHECK(pty_send(&s, kl, strlen(kl)) == 0);
	CHECK(pty_wait_after(&s, "R7:ok", "CLAM$ ", 2000));

	/* 8. SIGWINCH resize + bind builtin. */
	struct winsize ws = {.ws_row = 40, .ws_col = 120};
	(void)ioctl(s.master, TIOCSWINSZ, &ws);
	pty_clear_out(&s);
	const char *bd = "bind Ctrl+T kill-line; bind\r";
	CHECK(pty_send(&s, bd, strlen(bd)) == 0);
	CHECK(pty_wait_after(&s, "bind Ctrl+T kill-line\r\n", "CLAM$ ", 2000));

	/* 9. Ctrl+Q on empty prompt exits cleanly and restores termios. */
	CHECK(pty_send(&s, "\x11", 1) == 0);
	int rc = pty_finish(&s);
	CHECK(rc == 0);
}

static void test_interactive_linux_vt(const char *sh)
{
	struct pty_sess s;
	CHECK(pty_start(&s, sh, "linux") == 0);
	CHECK(pty_wait_for(&s, "CLAM$ ", 2000));

	/* Alt+M mark toggle + Alt+, word-left + Ctrl+C + End + Ctrl+V on TERM=linux. */
	pty_clear_out(&s);
	const char *cmd = "printf 'VT:%s\\n' xy\x1bm\x1b,\x03\x1b[4~\x16\r";
	CHECK(pty_send(&s, cmd, strlen(cmd)) == 0);
	CHECK(pty_wait_after(&s, "VT:xyxy", "CLAM$ ", 2000));

	/* Ctrl+Q exits cleanly. */
	CHECK(pty_send(&s, "\x11", 1) == 0);
	CHECK(pty_finish(&s) == 0);
}

int main(int argc, char **argv)
{
	const char *sh = argc > 1 ? argv[1] : "build/tam-clam";
	test_interactive_xterm(sh);
	test_interactive_linux_vt(sh);
	CHECK_DONE("test_pty");
}
