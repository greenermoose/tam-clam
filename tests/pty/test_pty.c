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
#include <time.h>
#include <unistd.h>

#include "../check.h"

static void sleep_ms(int ms)
{
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
	(void)nanosleep(&ts, NULL);
}

struct pty_sess {
	int master;
	pid_t pid;
	char out[32768];
	size_t out_len;
};

static int pty_start_opts(struct pty_sess *s, const char *shell_path,
			  const char *term, const char *histfile, bool priv)
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
		if (histfile)
			setenv("HISTFILE", histfile, 1);
		else
			setenv("HISTFILE", "", 1);
		if (priv)
			execl(shell_path, "tam-clam", "--private", (char *)NULL);
		else
			execl(shell_path, "tam-clam", (char *)NULL);
		_exit(127);
	}
	s->master = m;
	s->pid = pid;
	return 0;
}

static int pty_start(struct pty_sess *s, const char *shell_path, const char *term)
{
	return pty_start_opts(s, shell_path, term, NULL, false);
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

static void test_interactive_history(const char *sh)
{
	char hf[128];
	snprintf(hf, sizeof(hf), "/tmp/tam-clam-pty-hist-%ld", (long)getpid());
	unlink(hf);

	struct pty_sess s1, s2;
	CHECK(pty_start_opts(&s1, sh, "xterm-256color", hf, false) == 0);
	CHECK(pty_wait_for(&s1, "CLAM$ ", 2000));

	/* Run two distinct commands. */
	pty_clear_out(&s1);
	CHECK(pty_send(&s1, "printf 'H1:%s\\n' alpha\r", 23) == 0);
	CHECK(pty_wait_after(&s1, "H1:alpha", "CLAM$ ", 2000));

	pty_clear_out(&s1);
	CHECK(pty_send(&s1, "printf 'H2:%s\\n' omega\r", 23) == 0);
	CHECK(pty_wait_after(&s1, "H2:omega", "CLAM$ ", 2000));

	/* Prefix Up search: type "printf 'H1" then Up -> recalls H1 command. */
	pty_clear_out(&s1);
	const char *up = "printf 'H1\x1b[A\r";
	CHECK(pty_send(&s1, up, strlen(up)) == 0);
	CHECK(pty_wait_after(&s1, "H1:alpha", "CLAM$ ", 2000));

	/* Ctrl+F incremental search for "omega", Enter accepts, Enter runs. */
	pty_clear_out(&s1);
	const char *srch = "\x06omega\r\r";
	CHECK(pty_send(&s1, srch, strlen(srch)) == 0);
	CHECK(pty_wait_after(&s1, "H2:omega", "CLAM$ ", 2000));

	/* Alt+L inserts last arg ("omega") of previous command. */
	pty_clear_out(&s1);
	const char *la = "printf 'LA:%s\\n' \x1bl\r";
	CHECK(pty_send(&s1, la, strlen(la)) == 0);
	CHECK(pty_wait_after(&s1, "LA:omega", "CLAM$ ", 2000));

	/* Second concurrent shell shares the same history file via live sync. */
	CHECK(pty_start_opts(&s2, sh, "xterm-256color", hf, false) == 0);
	CHECK(pty_wait_for(&s2, "CLAM$ ", 2000));
	pty_clear_out(&s2);
	CHECK(pty_send(&s2, "history 2\r", 10) == 0);
	CHECK(pty_wait_after(&s2, "LA:%s", "CLAM$ ", 2000));

	CHECK(pty_send(&s1, "\x11", 1) == 0);
	CHECK(pty_finish(&s1) == 0);
	CHECK(pty_send(&s2, "\x11", 1) == 0);
	CHECK(pty_finish(&s2) == 0);
	unlink(hf);

	/* Private mode writes no history file. */
	struct pty_sess sp;
	CHECK(pty_start_opts(&sp, sh, "xterm-256color", hf, true) == 0);
	CHECK(pty_wait_for(&sp, "CLAM$ ", 2000));
	pty_clear_out(&sp);
	CHECK(pty_send(&sp, "printf 'PRIV:ok\\n'\r", 19) == 0);
	CHECK(pty_wait_after(&sp, "PRIV:ok", "CLAM$ ", 2000));
	CHECK(pty_send(&sp, "\x11", 1) == 0);
	CHECK(pty_finish(&sp) == 0);
	CHECK(access(hf, F_OK) < 0);
}

static void test_interactive_jobs(const char *sh)
{
	struct pty_sess s;
	CHECK(pty_start(&s, sh, "xterm-256color") == 0);
	CHECK(pty_wait_for(&s, "CLAM$ ", 2000));

	/* 1. Ctrl+P (0x10, VSUSP) stops a foreground sleep; prompt returns. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "sleep 30\r", 9) == 0);
	CHECK(pty_wait_for(&s, "sleep 30", 2000));
	sleep_ms(120);
	CHECK(pty_send(&s, "\x10", 1) == 0); /* Ctrl+P -> SIGTSTP */
	CHECK(pty_wait_after(&s, "[1]+ Stopped", "CLAM$ ", 2000));

	/* 2. jobs lists [1]+ Stopped. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "jobs\r", 5) == 0);
	CHECK(pty_wait_after(&s, "[1]+ Stopped", "CLAM$ ", 2000));

	/* 3. bg %1 resumes in background, then kill %1 + wait %1 cleans it up. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "bg %1\r", 6) == 0);
	CHECK(pty_wait_after(&s, "[1]+ sleep 30 &", "CLAM$ ", 2000));

	pty_clear_out(&s);
	CHECK(pty_send(&s, "kill %1; wait %1; printf 'J1:ok\\n'\r", 35) == 0);
	CHECK(pty_wait_after(&s, "J1:ok", "CLAM$ ", 2000));

	/* 4. Ctrl+\ (0x1C, VINTR) interrupts foreground sleep cleanly. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "sleep 30\r", 9) == 0);
	CHECK(pty_wait_for(&s, "sleep 30", 2000));
	sleep_ms(120);
	CHECK(pty_send(&s, "\x1c", 1) == 0); /* Ctrl+\ -> SIGINT */
	CHECK(pty_wait_after(&s, "sleep 30", "CLAM$ ", 2000));

	/* 5. Background job finishes and prints [1]+ Done before next prompt. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "sleep 0.1 &\r", 12) == 0);
	CHECK(pty_wait_after(&s, "[1] ", "CLAM$ ", 2000));
	sleep_ms(250);
	pty_clear_out(&s);
	CHECK(pty_send(&s, "\r", 1) == 0);
	CHECK(pty_wait_after(&s, "[1]+ Done", "CLAM$ ", 2000));

	/* 6. fg %1 brings a stopped job to foreground and finishes cleanly. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "sleep 0.4\r", 10) == 0);
	CHECK(pty_wait_for(&s, "sleep 0.4", 2000));
	sleep_ms(100);
	CHECK(pty_send(&s, "\x10", 1) == 0);
	CHECK(pty_wait_after(&s, "[1]+ Stopped", "CLAM$ ", 2000));
	pty_clear_out(&s);
	CHECK(pty_send(&s, "fg %1; printf 'FG:ok\\n'\r", 24) == 0);
	CHECK(pty_wait_after(&s, "FG:ok", "CLAM$ ", 2000));

	CHECK(pty_send(&s, "\x11", 1) == 0);
	CHECK(pty_finish(&s) == 0);
}

static void test_interactive_complete(const char *sh)
{
	struct pty_sess s;
	CHECK(pty_start(&s, sh, "xterm-256color") == 0);
	CHECK(pty_wait_for(&s, "CLAM$ ", 2000));

	/* 1. Command Tab completion: "pri<Tab>'C1:ok\n'\r" -> printf. */
	pty_clear_out(&s);
	const char *c1 = "pri\t'C1:ok\\n'\r";
	CHECK(pty_send(&s, c1, strlen(c1)) == 0);
	CHECK(pty_wait_after(&s, "C1:ok", "CLAM$ ", 2000));

	/* 2. Variable Tab completion. */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "TAM_COMP_UNI=yes\r", 17) == 0);
	CHECK(pty_wait_for(&s, "CLAM$ ", 2000));
	pty_clear_out(&s);
	const char *c2 = "printf 'C2:%s\\n' $TAM_COMP_U\t\r";
	CHECK(pty_send(&s, c2, strlen(c2)) == 0);
	CHECK(pty_wait_after(&s, "C2:yes", "CLAM$ ", 2000));

	/* 3. Declarative option completion (share/completions/tam-clam.comp). */
	pty_clear_out(&s);
	const char *c3 = "echo tam-clam --li\t\r";
	CHECK(pty_send(&s, c3, strlen(c3)) == 0);
	/* First word is "echo", so test with "tam-clam --li\t" echoed via Ctrl+K or directly: */
	CHECK(pty_wait_for(&s, "CLAM$ ", 2000));

	pty_clear_out(&s);
	const char *c4 = "tam-clam --ver\t\r";
	CHECK(pty_send(&s, c4, strlen(c4)) == 0);
	CHECK(pty_wait_after(&s, "--version", "CLAM$ ", 2000));

	CHECK(pty_send(&s, "\x11", 1) == 0);
	CHECK(pty_finish(&s) == 0);
}

static void test_interactive_highlight_and_suggest(const char *sh)
{
	struct pty_sess s;
	CHECK(pty_start(&s, sh, "xterm-256color") == 0);
	CHECK(pty_wait_for(&s, "CLAM$ ", 2000));

	/* 1. Seed history with a distinctive multi-word command. */
	pty_clear_out(&s);
	const char *seed = "printf 'SG:%s-%s\\n' one two\r";
	CHECK(pty_send(&s, seed, strlen(seed)) == 0);
	CHECK(pty_wait_after(&s, "SG:one-two", "CLAM$ ", 2000));

	/* 2. Typing "printf 'SG" renders green SGR (\x1b[32m) and dim ghost (\x1b[2m);
	 *    Right (\x1b[C) accepts full suggestion! */
	pty_clear_out(&s);
	CHECK(pty_send(&s, "printf 'SG", 10) == 0);
	CHECK(pty_wait_after(&s, "\x1b[32m", "\x1b[2m", 2000));
	pty_clear_out(&s);
	CHECK(pty_send(&s, "\x1b[C\r", 4) == 0);
	CHECK(pty_wait_after(&s, "SG:one-two", "CLAM$ ", 2000));

	/* 3. Ctrl+Right (\x1b[1;5C) accepts one word of suggestion. */
	pty_clear_out(&s);
	const char *wacc = "printf 'SG:%s-%s\\n'\x1b[1;5C three\r";
	CHECK(pty_send(&s, wacc, strlen(wacc)) == 0);
	CHECK(pty_wait_after(&s, "SG:one-three", "CLAM$ ", 2000));

	/* 4. Unknown command highlights in bold red (\x1b[1;31m) before Enter. */
	pty_clear_out(&s);
	const char *bad = "nosuchcmd_xyz_999";
	CHECK(pty_send(&s, bad, strlen(bad)) == 0);
	CHECK(pty_wait_for(&s, "\x1b[1;31m", 2000));
	CHECK(pty_send(&s, "\x0b", 1) == 0); /* Ctrl+K clears buffer */

	CHECK(pty_send(&s, "\x11", 1) == 0);
	CHECK(pty_finish(&s) == 0);
}

int main(int argc, char **argv)
{
	const char *sh = argc > 1 ? argv[1] : "build/tam-clam";
	test_interactive_xterm(sh);
	test_interactive_linux_vt(sh);
	test_interactive_history(sh);
	test_interactive_jobs(sh);
	test_interactive_complete(sh);
	test_interactive_highlight_and_suggest(sh);
	CHECK_DONE("test_pty");
}
