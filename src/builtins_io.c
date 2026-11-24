/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Regular POSIX I/O, environment, and process builtins:
 * echo, pwd, umask, read, kill, ulimit. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"
#include "tam-clam/trap.h"
#include "tam-clam/var.h"

/* ---- echo ------------------------------------------------------------- */

int tc_bi_echo(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	bool newline = true;
	if (i < argc && strcmp(argv[i], "-n") == 0) {
		newline = false;
		i++;
	}
	for (; i < argc; i++) {
		if (i > (newline ? 1 : 2)) {
			if (tc_write_all(STDOUT_FILENO, " ", 1) < 0)
				goto werr;
		}
		if (tc_write_all(STDOUT_FILENO, argv[i], strlen(argv[i])) < 0)
			goto werr;
	}
	if (newline && tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
		goto werr;
	return 0;

werr:
	tc_error(sh, line, "echo: write error: %s", strerror(errno));
	return 1;
}

/* ---- pwd -------------------------------------------------------------- */

int tc_bi_pwd(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool physical = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'L') {
				physical = false;
			} else if (*p == 'P') {
				physical = true;
			} else {
				tc_error(sh, line, "pwd: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}

	const char *pwd = NULL;
	char *owned = NULL;
	if (!physical) {
		const char *env_pwd = tc_var_get(&sh->vars, "PWD");
		if (tc_valid_pwd(env_pwd))
			pwd = env_pwd;
	}
	if (!pwd) {
		owned = tc_getcwd();
		if (!owned) {
			tc_error(sh, line, "pwd: %s", strerror(errno));
			return 1;
		}
		pwd = owned;
	}

	int status = 0;
	if (tc_write_all(STDOUT_FILENO, pwd, strlen(pwd)) < 0 ||
	    tc_write_all(STDOUT_FILENO, "\n", 1) < 0) {
		tc_error(sh, line, "pwd: write error: %s", strerror(errno));
		status = 1;
	}
	free(owned);
	return status;
}

/* ---- umask ------------------------------------------------------------ */

static int parse_octal_mask(const char *s, mode_t *out)
{
	if (!s || *s == '\0')
		return -1;
	unsigned v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '7')
			return -1;
		v = (v << 3) | (unsigned)(*p - '0');
		if (v > 0777)
			return -1;
	}
	*out = (mode_t)v;
	return 0;
}

static int apply_symbolic_umask(const char *spec, mode_t cur_mask, mode_t *out)
{
	/* Work on permission bits (~mask & 0777), then invert back. */
	mode_t perms = (~cur_mask) & 0777;
	const char *p = spec;
	for (;;) {
		mode_t who = 0;
		while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
			if (*p == 'u') who |= 0700;
			else if (*p == 'g') who |= 0070;
			else if (*p == 'o') who |= 0007;
			else who |= 0777;
			p++;
		}
		if (who == 0)
			who = 0777;
		if (*p != '+' && *p != '-' && *p != '=')
			return -1;
		while (*p == '+' || *p == '-' || *p == '=') {
			char op = *p++;
			mode_t bits = 0;
			for (; *p && *p != ',' && *p != '+' && *p != '-' && *p != '='; p++) {
				switch (*p) {
				case 'r': bits |= 0444; break;
				case 'w': bits |= 0222; break;
				case 'x': bits |= 0111; break;
				case 'X':
					if (perms & 0111)
						bits |= 0111;
					break;
				case 'u': {
					mode_t u = (perms >> 6) & 7;
					bits |= (u << 6) | (u << 3) | u;
					break;
				}
				case 'g': {
					mode_t g = (perms >> 3) & 7;
					bits |= (g << 6) | (g << 3) | g;
					break;
				}
				case 'o': {
					mode_t o = perms & 7;
					bits |= (o << 6) | (o << 3) | o;
					break;
				}
				case 's':
				case 't':
					break;
				default:
					return -1;
				}
			}
			bits &= who;
			if (op == '+')
				perms |= bits;
			else if (op == '-')
				perms &= ~bits;
			else
				perms = (perms & ~who) | bits;
		}
		if (*p == '\0')
			break;
		if (*p != ',')
			return -1;
		p++;
	}
	*out = (~perms) & 0777;
	return 0;
}

static int print_symbolic_umask(mode_t mask)
{
	mode_t perms = (~mask) & 0777;
	static const char labels[] = {'u', 'g', 'o'};
	static const unsigned shifts[] = {6, 3, 0};
	char buf[32];
	size_t pos = 0;
	for (int i = 0; i < 3; i++) {
		if (i > 0)
			buf[pos++] = ',';
		buf[pos++] = labels[i];
		buf[pos++] = '=';
		unsigned p = (perms >> shifts[i]) & 7;
		if (p & 4) buf[pos++] = 'r';
		if (p & 2) buf[pos++] = 'w';
		if (p & 1) buf[pos++] = 'x';
	}
	buf[pos++] = '\n';
	return tc_write_all(STDOUT_FILENO, buf, pos);
}

int tc_bi_umask(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool symbolic = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'S') {
				symbolic = true;
			} else {
				tc_error(sh, line, "umask: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}
	if (argc - i > 1) {
		tc_error(sh, line, "umask: too many arguments");
		return TC_STATUS_USAGE;
	}

	mode_t cur = umask(0);
	umask(cur);

	if (i >= argc) {
		if (symbolic) {
			if (print_symbolic_umask(cur) < 0) {
				tc_error(sh, line, "umask: write error: %s", strerror(errno));
				return 1;
			}
		} else {
			char buf[16];
			int n = snprintf(buf, sizeof(buf), "%04o\n", (unsigned)cur);
			if (n < 0 || tc_write_all(STDOUT_FILENO, buf, (size_t)n) < 0) {
				tc_error(sh, line, "umask: write error: %s", strerror(errno));
				return 1;
			}
		}
		return 0;
	}

	mode_t next = 0;
	if (argv[i][0] >= '0' && argv[i][0] <= '7') {
		if (parse_octal_mask(argv[i], &next) < 0) {
			tc_error(sh, line, "umask: %s: octal number out of range", argv[i]);
			return 1;
		}
	} else {
		if (apply_symbolic_umask(argv[i], cur, &next) < 0) {
			tc_error(sh, line, "umask: %s: invalid symbolic mode", argv[i]);
			return 1;
		}
	}
	umask(next);
	return 0;
}

/* ---- read ------------------------------------------------------------- */

static bool is_ifs_ws(const char *ifs, char c)
{
	return (c == ' ' || c == '\t' || c == '\n') && strchr(ifs, c) != NULL;
}

static bool is_ifs_nws(const char *ifs, char c)
{
	return c != '\0' && !(c == ' ' || c == '\t' || c == '\n') &&
	       strchr(ifs, c) != NULL;
}

static int read_byte(struct tc_shell *sh, unsigned char *out, int *sig_out)
{
	for (;;) {
		ssize_t n = read(STDIN_FILENO, out, 1);
		if (n == 1)
			return 1;
		if (n == 0)
			return 0;
		if (errno == EINTR) {
			if (tc_sigint_seen) {
				*sig_out = SIGINT;
				return -2;
			}
			if (tc_sig_any) {
				for (int s = 1; s < TC_NSIG; s++) {
					if (tc_sig_pending[s] &&
					    tc_trap_is_caught(&sh->traps, s)) {
						*sig_out = s;
						return -2;
					}
				}
			}
			continue;
		}
		return -1;
	}
}

static int set_read_var(struct tc_shell *sh, const char *name, const char *val,
			size_t len, unsigned long line)
{
	char *tmp = malloc(len + 1);
	if (!tmp) {
		tc_error(sh, line, "read: out of memory");
		return 1;
	}
	if (len > 0)
		memcpy(tmp, val, len);
	tmp[len] = '\0';
	int r = tc_var_set(&sh->vars, name, tmp,
			   sh->opt_allexport ? TC_VAR_EXPORT : 0);
	free(tmp);
	if (r == TC_VAR_ERR_READONLY) {
		tc_error(sh, line, "read: %s: readonly variable", name);
		return 1;
	}
	if (r == TC_VAR_ERR_NAME) {
		tc_error(sh, line, "read: `%s': not a valid identifier", name);
		return 1;
	}
	if (r != TC_VAR_OK) {
		tc_error(sh, line, "read: out of memory");
		return 1;
	}
	return 0;
}

int tc_bi_read(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool raw = false;
	char delim = '\n';
	const char *arr_name = NULL;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'r') {
				raw = true;
			} else if (*p == 'a') {
				const char *aarg = p[1] ? p + 1 : NULL;
				if (!aarg) {
					if (i + 1 >= argc) {
						tc_error(sh, line,
							 "read: -a: option requires an argument");
						return TC_STATUS_USAGE;
					}
					aarg = argv[++i];
				}
				arr_name = aarg;
				break;
			} else if (*p == 'd') {
				const char *darg = p[1] ? p + 1 : NULL;
				if (!darg) {
					if (i + 1 >= argc) {
						tc_error(sh, line,
							 "read: -d: option requires an argument");
						return TC_STATUS_USAGE;
					}
					darg = argv[++i];
				}
				delim = darg[0];
				break;
			} else {
				tc_error(sh, line, "read: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}

	char *default_vars[1] = {"REPLY"};
	char **vars = (i < argc) ? argv + i : default_vars;
	int nvars = (i < argc) ? (argc - i) : 1;

	if (arr_name) {
		if (!tc_var_valid_name(arr_name, strlen(arr_name))) {
			tc_error(sh, line, "read: `%s': not a valid identifier", arr_name);
			return TC_STATUS_USAGE;
		}
	} else {
		for (int v = 0; v < nvars; v++) {
			if (!tc_var_valid_name(vars[v], strlen(vars[v]))) {
				tc_error(sh, line, "read: `%s': not a valid identifier", vars[v]);
				return TC_STATUS_USAGE;
			}
		}
	}

	size_t cap = 128, len = 0;
	char *buf = malloc(cap);
	unsigned char *esc = malloc(cap);
	if (!buf || !esc) {
		free(buf);
		free(esc);
		tc_error(sh, line, "read: out of memory");
		return 1;
	}

	bool eof = false;
	bool backslash = false;
	int sig_intr = 0;
	for (;;) {
		unsigned char ch = 0;
		int rr = read_byte(sh, &ch, &sig_intr);
		if (rr == 0) {
			eof = true;
			break;
		}
		if (rr == -2) {
			free(buf);
			free(esc);
			return 128 + sig_intr;
		}
		if (rr < 0) {
			tc_error(sh, line, "read: %s", strerror(errno));
			free(buf);
			free(esc);
			return 1;
		}
		if (!raw && backslash) {
			backslash = false;
			if (ch == '\n')
				continue; /* line continuation */
			if (ch == '\0')
				continue;
			if (len + 1 >= cap) {
				if (cap >= ((size_t)1 << 20)) {
					tc_error(sh, line, "read: line too long");
					free(buf);
					free(esc);
					return 1;
				}
				cap *= 2;
				char *nb = realloc(buf, cap);
				unsigned char *ne = realloc(esc, cap);
				if (!nb || !ne) {
					free(nb ? nb : buf);
					free(ne ? ne : esc);
					tc_error(sh, line, "read: out of memory");
					return 1;
				}
				buf = nb;
				esc = ne;
			}
			buf[len] = (char)ch;
			esc[len] = 1;
			len++;
			continue;
		}
		if (!raw && ch == '\\') {
			backslash = true;
			continue;
		}
		if ((char)ch == delim)
			break;
		if (ch == '\0')
			continue;
		if (len + 1 >= cap) {
			if (cap >= ((size_t)1 << 20)) {
				tc_error(sh, line, "read: line too long");
				free(buf);
				free(esc);
				return 1;
			}
			cap *= 2;
			char *nb = realloc(buf, cap);
			unsigned char *ne = realloc(esc, cap);
			if (!nb || !ne) {
				free(nb ? nb : buf);
				free(ne ? ne : esc);
				tc_error(sh, line, "read: out of memory");
				return 1;
			}
			buf = nb;
			esc = ne;
		}
		buf[len] = (char)ch;
		esc[len] = 0;
		len++;
	}

	const char *ifs = tc_var_get(&sh->vars, "IFS");
	if (!ifs)
		ifs = " \t\n";

	int status = eof ? 1 : 0;
	if (arr_name) {
		unsigned fl = sh->opt_allexport ? TC_VAR_EXPORT : 0;
		int rr = tc_var_reset_array(&sh->vars, arr_name, fl);
		if (rr != TC_VAR_OK) {
			if (rr == TC_VAR_ERR_READONLY)
				tc_error(sh, line, "read: %s: readonly variable", arr_name);
			else if (rr == TC_VAR_ERR_TYPE)
				tc_error(sh, line, "read: %s: cannot convert associative to indexed array", arr_name);
			else
				tc_error(sh, line, "read: out of memory");
			free(buf);
			free(esc);
			return 2;
		}
		if (ifs[0] == '\0') {
			if (len > 0) {
				char *tmp = malloc(len + 1);
				if (!tmp) {
					tc_error(sh, line, "read: out of memory");
					status = 2;
				} else {
					memcpy(tmp, buf, len);
					tmp[len] = '\0';
					if (tc_var_append_elem(&sh->vars, arr_name, tmp, fl) != TC_VAR_OK) {
						tc_error(sh, line, "read: out of memory");
						status = 2;
					}
					free(tmp);
				}
			}
			free(buf);
			free(esc);
			return status;
		}
		size_t pos = 0;
		while (pos < len && !esc[pos] && is_ifs_ws(ifs, buf[pos]))
			pos++;
		while (pos < len) {
			size_t start = pos;
			while (pos < len && (esc[pos] || (!is_ifs_ws(ifs, buf[pos]) &&
							  !is_ifs_nws(ifs, buf[pos]))))
				pos++;
			size_t flen = pos - start;
			char *tmp = malloc(flen + 1);
			if (!tmp) {
				tc_error(sh, line, "read: out of memory");
				status = 2;
				break;
			}
			memcpy(tmp, buf + start, flen);
			tmp[flen] = '\0';
			if (tc_var_append_elem(&sh->vars, arr_name, tmp, fl) != TC_VAR_OK) {
				free(tmp);
				tc_error(sh, line, "read: out of memory");
				status = 2;
				break;
			}
			free(tmp);
			while (pos < len && !esc[pos] && is_ifs_ws(ifs, buf[pos]))
				pos++;
			if (pos < len && !esc[pos] && is_ifs_nws(ifs, buf[pos])) {
				pos++;
				while (pos < len && !esc[pos] && is_ifs_ws(ifs, buf[pos]))
					pos++;
			}
		}
		free(buf);
		free(esc);
		return status;
	}
	if (ifs[0] == '\0') {
		if (set_read_var(sh, vars[0], buf, len, line) != 0)
			status = 2;
		for (int v = 1; v < nvars; v++) {
			if (set_read_var(sh, vars[v], "", 0, line) != 0)
				status = 2;
		}
		free(buf);
		free(esc);
		return status;
	}

	size_t pos = 0;
	while (pos < len && !esc[pos] && is_ifs_ws(ifs, buf[pos]))
		pos++;

	for (int v = 0; v < nvars - 1; v++) {
		size_t start = pos;
		while (pos < len && (esc[pos] || (!is_ifs_ws(ifs, buf[pos]) &&
						  !is_ifs_nws(ifs, buf[pos]))))
			pos++;
		if (set_read_var(sh, vars[v], buf + start, pos - start, line) != 0)
			status = 2;
		while (pos < len && !esc[pos] && is_ifs_ws(ifs, buf[pos]))
			pos++;
		if (pos < len && !esc[pos] && is_ifs_nws(ifs, buf[pos])) {
			pos++;
			while (pos < len && !esc[pos] && is_ifs_ws(ifs, buf[pos]))
				pos++;
		}
	}

	/* Last variable: trim trailing unescaped IFS whitespace (and a single
	 * trailing unescaped IFS non-whitespace if no more fields follow it). */
	size_t end = len;
	while (end > pos && !esc[end - 1] && is_ifs_ws(ifs, buf[end - 1]))
		end--;
	/* Check if remainder has no internal unescaped IFS delimiter except a
	 * single trailing unescaped IFS non-whitespace delimiter. */
	if (end > pos && !esc[end - 1] && is_ifs_nws(ifs, buf[end - 1])) {
		bool other_delim = false;
		for (size_t k = pos; k < end - 1; k++) {
			if (!esc[k] && (is_ifs_ws(ifs, buf[k]) || is_ifs_nws(ifs, buf[k]))) {
				other_delim = true;
				break;
			}
		}
		if (!other_delim)
			end--;
	}
	if (set_read_var(sh, vars[nvars - 1], buf + pos, end - pos, line) != 0)
		status = 2;

	free(buf);
	free(esc);
	return status;
}

/* ---- kill ------------------------------------------------------------- */

static int parse_kill_pid(const char *s, pid_t *out)
{
	if (!s || *s == '\0')
		return -1;
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (end == s || *end != '\0' || errno == ERANGE ||
	    v < INT_MIN || v > INT_MAX)
		return -1;
	*out = (pid_t)v;
	return 0;
}

int tc_bi_kill(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	if (argc < 2) {
		tc_error(sh, line, "kill: usage: kill [-s sig | -sig] pid... | kill -l [status]");
		return TC_STATUS_USAGE;
	}

	int sig = SIGTERM;
	int i = 1;
	if (strcmp(argv[i], "-l") == 0) {
		i++;
		if (i >= argc) {
			bool first = true;
			for (int s = 1; s < TC_NSIG; s++) {
				const char *name = tc_trap_sig_name(s);
				if (!name)
					continue;
				if (!first && tc_write_all(STDOUT_FILENO, " ", 1) < 0)
					goto werr;
				first = false;
				if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0)
					goto werr;
			}
			if (tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
				goto werr;
			return 0;
		}
		if (argc - i > 1) {
			tc_error(sh, line, "kill: -l: too many arguments");
			return TC_STATUS_USAGE;
		}
		char *end = NULL;
		errno = 0;
		long v = strtol(argv[i], &end, 10);
		if (end == argv[i] || *end != '\0' || errno == ERANGE || v < 0) {
			int sn = tc_trap_parse_sig(argv[i]);
			if (sn <= 0) {
				tc_error(sh, line, "kill: %s: invalid signal", argv[i]);
				return 1;
			}
			char buf[32];
			int n = snprintf(buf, sizeof(buf), "%d\n", sn);
			if (n < 0 || tc_write_all(STDOUT_FILENO, buf, (size_t)n) < 0)
				goto werr;
			return 0;
		}
		if (v > 128)
			v -= 128;
		const char *name = (v > 0 && v < TC_NSIG) ? tc_trap_sig_name((int)v) : NULL;
		if (!name) {
			tc_error(sh, line, "kill: %s: invalid signal", argv[i]);
			return 1;
		}
		if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
		    tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
			goto werr;
		return 0;
	}

	if (strcmp(argv[i], "-s") == 0) {
		if (i + 1 >= argc) {
			tc_error(sh, line, "kill: -s: signal name required");
			return TC_STATUS_USAGE;
		}
		sig = tc_trap_parse_sig(argv[i + 1]);
		if (sig < 0) {
			tc_error(sh, line, "kill: %s: invalid signal", argv[i + 1]);
			return TC_STATUS_USAGE;
		}
		i += 2;
	} else if (argv[i][0] == '-' && argv[i][1] != '\0' &&
		   strcmp(argv[i], "--") != 0) {
		sig = tc_trap_parse_sig(argv[i] + 1);
		if (sig < 0) {
			tc_error(sh, line, "kill: %s: invalid signal", argv[i] + 1);
			return TC_STATUS_USAGE;
		}
		i++;
	}

	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (i >= argc) {
		tc_error(sh, line, "kill: process ID required");
		return TC_STATUS_USAGE;
	}

	int status = 0;
	for (; i < argc; i++) {
		if (argv[i][0] == '%') {
			const char *err = NULL;
			struct tc_job *j = tc_jobs_resolve(&sh->jobs, argv[i], &err);
			if (!j) {
				tc_error(sh, line, "kill: %s: %s", argv[i], err);
				status = 1;
				continue;
			}
			if (kill(-j->pgid, sig) < 0) {
				tc_error(sh, line, "kill: (%s): %s", argv[i], strerror(errno));
				status = 1;
			}
			continue;
		}
		pid_t pid;
		if (parse_kill_pid(argv[i], &pid) < 0) {
			tc_error(sh, line, "kill: %s: invalid process ID", argv[i]);
			status = 1;
			continue;
		}
		if (kill(pid, sig) < 0) {
			tc_error(sh, line, "kill: (%ld): %s", (long)pid, strerror(errno));
			status = 1;
		}
	}
	return status;

werr:
	tc_error(sh, line, "kill: write error: %s", strerror(errno));
	return 1;
}

/* ---- ulimit ----------------------------------------------------------- */

struct ulimit_desc {
	char flag;
	int resource;
	unsigned long scale;
	const char *label;
};

static const struct ulimit_desc ulimit_table[] = {
	{'t', RLIMIT_CPU,    1,    "cpu time (seconds)        "},
	{'f', RLIMIT_FSIZE,  512,  "file size (blocks)        "},
	{'d', RLIMIT_DATA,   1024, "data seg size (kbytes)    "},
	{'s', RLIMIT_STACK,  1024, "stack size (kbytes)       "},
	{'c', RLIMIT_CORE,   512,  "core file size (blocks)   "},
#ifdef RLIMIT_RSS
	{'m', RLIMIT_RSS,    1024, "max memory size (kbytes)  "},
#endif
#ifdef RLIMIT_MEMLOCK
	{'l', RLIMIT_MEMLOCK, 1024, "locked memory (kbytes)    "},
#endif
#ifdef RLIMIT_NPROC
	{'u', RLIMIT_NPROC,  1,    "max user processes        "},
#endif
	{'n', RLIMIT_NOFILE, 1,    "open files                "},
	{'v', RLIMIT_AS,     1024, "virtual memory (kbytes)   "},
};

static const struct ulimit_desc *find_ulimit(char flag)
{
	for (size_t i = 0; i < sizeof(ulimit_table) / sizeof(ulimit_table[0]); i++) {
		if (ulimit_table[i].flag == flag)
			return &ulimit_table[i];
	}
	return NULL;
}

static int print_one_ulimit(const struct ulimit_desc *d, bool hard, bool with_label)
{
	struct rlimit rl;
	if (getrlimit(d->resource, &rl) < 0)
		return -1;
	rlim_t val = hard ? rl.rlim_max : rl.rlim_cur;
	char buf[128];
	int n;
	if (val == RLIM_INFINITY) {
		n = with_label
			? snprintf(buf, sizeof(buf), "%s (-%c) unlimited\n", d->label, d->flag)
			: snprintf(buf, sizeof(buf), "unlimited\n");
	} else {
		unsigned long long scaled = (unsigned long long)val / d->scale;
		n = with_label
			? snprintf(buf, sizeof(buf), "%s (-%c) %llu\n", d->label, d->flag, scaled)
			: snprintf(buf, sizeof(buf), "%llu\n", scaled);
	}
	if (n < 0)
		return -1;
	return tc_write_all(STDOUT_FILENO, buf, (size_t)n);
}

int tc_bi_ulimit(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool hard = false, soft = false, all = false;
	char res_flag = 'f';
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'H') {
				hard = true;
			} else if (*p == 'S') {
				soft = true;
			} else if (*p == 'a') {
				all = true;
			} else if (find_ulimit(*p)) {
				res_flag = *p;
			} else {
				tc_error(sh, line, "ulimit: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}

	if (all) {
		if (i < argc) {
			tc_error(sh, line, "ulimit: -a cannot be combined with a limit value");
			return TC_STATUS_USAGE;
		}
		for (size_t k = 0; k < sizeof(ulimit_table) / sizeof(ulimit_table[0]); k++) {
			if (print_one_ulimit(&ulimit_table[k], hard && !soft, true) < 0) {
				tc_error(sh, line, "ulimit: %s", strerror(errno));
				return 1;
			}
		}
		return 0;
	}

	const struct ulimit_desc *d = find_ulimit(res_flag);
	if (!d) {
		tc_error(sh, line, "ulimit: -%c: unsupported resource", res_flag);
		return 1;
	}

	if (i >= argc) {
		if (print_one_ulimit(d, hard && !soft, false) < 0) {
			tc_error(sh, line, "ulimit: %s", strerror(errno));
			return 1;
		}
		return 0;
	}
	if (argc - i > 1) {
		tc_error(sh, line, "ulimit: too many arguments");
		return TC_STATUS_USAGE;
	}

	rlim_t newval;
	if (strcmp(argv[i], "unlimited") == 0) {
		newval = RLIM_INFINITY;
	} else {
		char *end = NULL;
		errno = 0;
		unsigned long long v = strtoull(argv[i], &end, 10);
		if (end == argv[i] || *end != '\0' || errno == ERANGE ||
		    argv[i][0] == '-') {
			tc_error(sh, line, "ulimit: %s: invalid limit", argv[i]);
			return TC_STATUS_USAGE;
		}
		if (v > (unsigned long long)RLIM_INFINITY / d->scale) {
			newval = RLIM_INFINITY;
		} else {
			newval = (rlim_t)(v * d->scale);
		}
	}

	struct rlimit rl;
	if (getrlimit(d->resource, &rl) < 0) {
		tc_error(sh, line, "ulimit: %s", strerror(errno));
		return 1;
	}
	if (!hard && !soft) {
		rl.rlim_cur = newval;
		rl.rlim_max = newval;
	} else {
		if (soft)
			rl.rlim_cur = newval;
		if (hard)
			rl.rlim_max = newval;
	}
	if (setrlimit(d->resource, &rl) < 0) {
		tc_error(sh, line, "ulimit: cannot modify limit: %s", strerror(errno));
		return 1;
	}
	return 0;
}
