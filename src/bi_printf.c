/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX.1-2024 `printf` builtin (plus %q safe shell quoting). */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tam.h>

#include "tam-clam/builtins.h"
#include "tam-clam/shell.h"

struct pf_state {
	struct tc_shell *sh;
	unsigned long line;
	char **argv;
	int argc;
	int argi;
	int status;
	bool stop;
	bool write_err;
	tam_str_t out;
};

static bool pf_emit(struct pf_state *st, const char *s, size_t len)
{
	if (len == 0)
		return true;
	tam_str_t next = tam_str_cat_len(st->out, s, len);
	if (!next) {
		tc_error(st->sh, st->line, "printf: out of memory");
		st->status = 1;
		st->stop = true;
		return false;
	}
	st->out = next;
	if (tam_str_len(st->out) >= 4096) {
		if (tc_write_all(STDOUT_FILENO, st->out, tam_str_len(st->out)) < 0) {
			tc_error(st->sh, st->line, "printf: write error: %s",
				 strerror(errno));
			st->status = 1;
			st->stop = true;
			st->write_err = true;
			return false;
		}
		tam_str_clear(st->out);
	}
	return true;
}

static bool pf_emit_char(struct pf_state *st, char c)
{
	return pf_emit(st, &c, 1);
}

static bool pf_emit_pad(struct pf_state *st, char c, size_t count)
{
	char buf[64];
	memset(buf, c, sizeof(buf));
	while (count > 0) {
		size_t chunk = count < sizeof(buf) ? count : sizeof(buf);
		if (!pf_emit(st, buf, chunk))
			return false;
		count -= chunk;
	}
	return true;
}

static const char *pf_next_arg(struct pf_state *st)
{
	if (st->argi < st->argc)
		return st->argv[st->argi++];
	return "";
}

static int parse_escape(const char **pp, bool in_b)
{
	const char *p = *pp;
	unsigned char c = (unsigned char)*p;
	if (c == '\0')
		return '\\';
	p++;
	switch (c) {
	case '\\': *pp = p; return '\\';
	case '"':  *pp = p; return '"';
	case '\'': *pp = p; return '\'';
	case 'a':  *pp = p; return '\a';
	case 'b':  *pp = p; return '\b';
	case 'f':  *pp = p; return '\f';
	case 'n':  *pp = p; return '\n';
	case 'r':  *pp = p; return '\r';
	case 't':  *pp = p; return '\t';
	case 'v':  *pp = p; return '\v';
	case 'c':
		if (in_b) {
			*pp = p;
			return -2; /* stop output */
		}
		*pp = p;
		return 'c';
	case '0': case '1': case '2': case '3':
	case '4': case '5': case '6': case '7': {
		unsigned val = 0;
		int maxd = 3;
		if (in_b && c == '0') {
			/* %b: \0ddd takes up to 3 octal digits after the 0. */
			for (int d = 0; d < 3 && *p >= '0' && *p <= '7'; d++)
				val = (val << 3) | (unsigned)(*p++ - '0');
		} else {
			val = (unsigned)(c - '0');
			for (int d = 1; d < maxd && *p >= '0' && *p <= '7'; d++)
				val = (val << 3) | (unsigned)(*p++ - '0');
		}
		*pp = p;
		return (int)(val & 0xff);
	}
	default:
		*pp = p;
		return (int)c;
	}
}

static int64_t parse_signed_arg(struct pf_state *st, const char *s)
{
	if (!s || *s == '\0')
		return 0;
	if ((s[0] == '\'' || s[0] == '"') && s[1] != '\0')
		return (unsigned char)s[1];
	char *end = NULL;
	errno = 0;
	long long v = strtoll(s, &end, 0);
	if (end == s || *end != '\0' || errno == ERANGE) {
		tc_error(st->sh, st->line, "printf: %s: invalid number", s);
		st->status = 1;
	}
	return (int64_t)v;
}

static uint64_t parse_unsigned_arg(struct pf_state *st, const char *s)
{
	if (!s || *s == '\0')
		return 0;
	if ((s[0] == '\'' || s[0] == '"') && s[1] != '\0')
		return (unsigned char)s[1];
	char *end = NULL;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 0);
	if (end == s || *end != '\0' || errno == ERANGE) {
		tc_error(st->sh, st->line, "printf: %s: invalid number", s);
		st->status = 1;
	}
	return (uint64_t)v;
}

static double parse_double_arg(struct pf_state *st, const char *s)
{
	if (!s || *s == '\0')
		return 0.0;
	if ((s[0] == '\'' || s[0] == '"') && s[1] != '\0')
		return (double)(unsigned char)s[1];
	char *end = NULL;
	errno = 0;
	double v = strtod(s, &end);
	if (end == s || *end != '\0' || errno == ERANGE) {
		tc_error(st->sh, st->line, "printf: %s: invalid number", s);
		st->status = 1;
	}
	return v;
}

static bool emit_padded_bytes(struct pf_state *st, const char *data, size_t len,
			      int width, bool left_align)
{
	size_t pad = (width > 0 && (size_t)width > len) ? (size_t)width - len : 0;
	if (!left_align && pad > 0 && !pf_emit_pad(st, ' ', pad))
		return false;
	if (!pf_emit(st, data, len))
		return false;
	if (left_align && pad > 0 && !pf_emit_pad(st, ' ', pad))
		return false;
	return true;
}

static bool emit_shell_quoted(struct pf_state *st, const char *s)
{
	if (!pf_emit_char(st, '\''))
		return false;
	for (const char *p = s; *p; p++) {
		if (*p == '\'') {
			if (!pf_emit(st, "'\\''", 4))
				return false;
		} else {
			if (!pf_emit_char(st, *p))
				return false;
		}
	}
	return pf_emit_char(st, '\'');
}

static bool emit_b_string(struct pf_state *st, const char *s, int width,
			  int prec, bool has_prec, bool left_align)
{
	tam_str_t tmp = tam_str_new("");
	if (!tmp) {
		tc_error(st->sh, st->line, "printf: out of memory");
		st->status = 1;
		st->stop = true;
		return false;
	}
	bool stop_after = false;
	for (const char *p = s; *p; ) {
		char ch;
		if (*p == '\\') {
			p++;
			int esc = parse_escape(&p, true);
			if (esc == -2) {
				stop_after = true;
				break;
			}
			ch = (char)esc;
		} else {
			ch = *p++;
		}
		tam_str_t next = tam_str_cat_len(tmp, &ch, 1);
		if (!next) {
			tam_str_free(tmp);
			tc_error(st->sh, st->line, "printf: out of memory");
			st->status = 1;
			st->stop = true;
			return false;
		}
		tmp = next;
	}
	size_t len = tam_str_len(tmp);
	if (has_prec && prec >= 0 && (size_t)prec < len)
		len = (size_t)prec;
	bool ok = emit_padded_bytes(st, tmp, len, width, left_align);
	tam_str_free(tmp);
	if (stop_after)
		st->stop = true;
	return ok;
}

static bool format_numeric(struct pf_state *st, const char *flags, int width,
			   bool has_width, int prec, bool has_prec, char conv)
{
	char spec[32];
	size_t pos = 0;
	spec[pos++] = '%';
	for (const char *f = flags; *f && pos < sizeof(spec) - 10; f++)
		spec[pos++] = *f;
	if (has_width) {
		spec[pos++] = '*';
	}
	if (has_prec) {
		spec[pos++] = '.';
		spec[pos++] = '*';
	}

	const char *arg = pf_next_arg(st);
	char sbuf[128];
	char *heap = NULL;
	int n = -1;

	if (conv == 'd' || conv == 'i') {
		int64_t v = parse_signed_arg(st, arg);
		spec[pos++] = 'l';
		spec[pos++] = 'l';
		spec[pos++] = 'd';
		spec[pos] = '\0';
		if (has_width && has_prec)
			n = snprintf(sbuf, sizeof(sbuf), spec, width, prec, (long long)v);
		else if (has_width)
			n = snprintf(sbuf, sizeof(sbuf), spec, width, (long long)v);
		else if (has_prec)
			n = snprintf(sbuf, sizeof(sbuf), spec, prec, (long long)v);
		else
			n = snprintf(sbuf, sizeof(sbuf), spec, (long long)v);
		if (n >= (int)sizeof(sbuf)) {
			heap = malloc((size_t)n + 1);
			if (!heap)
				goto nomem;
			if (has_width && has_prec)
				n = snprintf(heap, (size_t)n + 1, spec, width, prec, (long long)v);
			else if (has_width)
				n = snprintf(heap, (size_t)n + 1, spec, width, (long long)v);
			else if (has_prec)
				n = snprintf(heap, (size_t)n + 1, spec, prec, (long long)v);
			else
				n = snprintf(heap, (size_t)n + 1, spec, (long long)v);
		}
	} else if (conv == 'u' || conv == 'o' || conv == 'x' || conv == 'X') {
		uint64_t v = parse_unsigned_arg(st, arg);
		spec[pos++] = 'l';
		spec[pos++] = 'l';
		spec[pos++] = conv;
		spec[pos] = '\0';
		if (has_width && has_prec)
			n = snprintf(sbuf, sizeof(sbuf), spec, width, prec, (unsigned long long)v);
		else if (has_width)
			n = snprintf(sbuf, sizeof(sbuf), spec, width, (unsigned long long)v);
		else if (has_prec)
			n = snprintf(sbuf, sizeof(sbuf), spec, prec, (unsigned long long)v);
		else
			n = snprintf(sbuf, sizeof(sbuf), spec, (unsigned long long)v);
		if (n >= (int)sizeof(sbuf)) {
			heap = malloc((size_t)n + 1);
			if (!heap)
				goto nomem;
			if (has_width && has_prec)
				n = snprintf(heap, (size_t)n + 1, spec, width, prec, (unsigned long long)v);
			else if (has_width)
				n = snprintf(heap, (size_t)n + 1, spec, width, (unsigned long long)v);
			else if (has_prec)
				n = snprintf(heap, (size_t)n + 1, spec, prec, (unsigned long long)v);
			else
				n = snprintf(heap, (size_t)n + 1, spec, (unsigned long long)v);
		}
	} else {
		double v = parse_double_arg(st, arg);
		spec[pos++] = conv;
		spec[pos] = '\0';
		if (has_width && has_prec)
			n = snprintf(sbuf, sizeof(sbuf), spec, width, prec, v);
		else if (has_width)
			n = snprintf(sbuf, sizeof(sbuf), spec, width, v);
		else if (has_prec)
			n = snprintf(sbuf, sizeof(sbuf), spec, prec, v);
		else
			n = snprintf(sbuf, sizeof(sbuf), spec, v);
		if (n >= (int)sizeof(sbuf)) {
			heap = malloc((size_t)n + 1);
			if (!heap)
				goto nomem;
			if (has_width && has_prec)
				n = snprintf(heap, (size_t)n + 1, spec, width, prec, v);
			else if (has_width)
				n = snprintf(heap, (size_t)n + 1, spec, width, v);
			else if (has_prec)
				n = snprintf(heap, (size_t)n + 1, spec, prec, v);
			else
				n = snprintf(heap, (size_t)n + 1, spec, v);
		}
	}

	bool ok = true;
	if (n > 0)
		ok = pf_emit(st, heap ? heap : sbuf, (size_t)n);
	free(heap);
	return ok;

nomem:
	tc_error(st->sh, st->line, "printf: out of memory");
	st->status = 1;
	st->stop = true;
	return false;
}

static bool run_format_pass(struct pf_state *st, const char *fmt)
{
	for (const char *p = fmt; *p && !st->stop; ) {
		if (*p == '\\') {
			p++;
			int ch = parse_escape(&p, false);
			if (!pf_emit_char(st, (char)ch))
				return false;
			continue;
		}
		if (*p != '%') {
			if (!pf_emit_char(st, *p++))
				return false;
			continue;
		}
		p++;
		if (*p == '%') {
			p++;
			if (!pf_emit_char(st, '%'))
				return false;
			continue;
		}

		char flags[8];
		size_t nf = 0;
		bool left_align = false;
		while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') {
			if (*p == '-')
				left_align = true;
			if (nf < sizeof(flags) - 1 && !memchr(flags, *p, nf))
				flags[nf++] = *p;
			p++;
		}
		flags[nf] = '\0';

		int width = 0;
		bool has_width = false;
		if (*p == '*') {
			p++;
			has_width = true;
			int64_t w = parse_signed_arg(st, pf_next_arg(st));
			if (w < 0) {
				left_align = true;
				if (nf < sizeof(flags) - 1 && !memchr(flags, '-', nf)) {
					flags[nf++] = '-';
					flags[nf] = '\0';
				}
				w = (w == INT64_MIN) ? INT_MAX : -w;
			}
			width = w > INT_MAX ? INT_MAX : (int)w;
		} else if (isdigit((unsigned char)*p)) {
			has_width = true;
			long w = 0;
			while (isdigit((unsigned char)*p)) {
				if (w < INT_MAX / 10)
					w = w * 10 + (*p - '0');
				p++;
			}
			width = (int)w;
		}

		int prec = 0;
		bool has_prec = false;
		if (*p == '.') {
			p++;
			has_prec = true;
			if (*p == '*') {
				p++;
				int64_t pr = parse_signed_arg(st, pf_next_arg(st));
				if (pr < 0)
					has_prec = false;
				else
					prec = pr > INT_MAX ? INT_MAX : (int)pr;
			} else {
				long pr = 0;
				while (isdigit((unsigned char)*p)) {
					if (pr < INT_MAX / 10)
						pr = pr * 10 + (*p - '0');
					p++;
				}
				prec = (int)pr;
			}
		}

		char conv = *p;
		if (conv == '\0') {
			tc_error(st->sh, st->line, "printf: missing format character");
			st->status = 1;
			return false;
		}
		p++;

		switch (conv) {
		case 's': {
			const char *s = pf_next_arg(st);
			size_t len = strlen(s);
			if (has_prec && (size_t)prec < len)
				len = (size_t)prec;
			if (!emit_padded_bytes(st, s, len, has_width ? width : 0, left_align))
				return false;
			break;
		}
		case 'b': {
			const char *s = pf_next_arg(st);
			if (!emit_b_string(st, s, has_width ? width : 0, prec, has_prec,
					   left_align))
				return false;
			break;
		}
		case 'c': {
			const char *s = pf_next_arg(st);
			char ch = s[0];
			if (!emit_padded_bytes(st, &ch, 1, has_width ? width : 0, left_align))
				return false;
			break;
		}
		case 'q': {
			const char *s = pf_next_arg(st);
			if (!emit_shell_quoted(st, s))
				return false;
			break;
		}
		case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
		case 'a': case 'A':
			if (!format_numeric(st, flags, width, has_width, prec, has_prec, conv))
				return false;
			break;
		default:
			tc_error(st->sh, st->line, "printf: %%%c: invalid conversion", conv);
			st->status = 1;
			st->stop = true;
			return false;
		}
	}
	return true;
}

int tc_bi_printf(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (i >= argc) {
		tc_error(sh, line, "printf: usage: printf format [arguments]");
		return TC_STATUS_USAGE;
	}

	const char *fmt = argv[i++];
	tam_str_t out = tam_str_new("");
	if (!out) {
		tc_error(sh, line, "printf: out of memory");
		return 1;
	}

	struct pf_state st = {
		.sh = sh,
		.line = line,
		.argv = argv,
		.argc = argc,
		.argi = i,
		.status = 0,
		.stop = false,
		.write_err = false,
		.out = out,
	};

	do {
		int before = st.argi;
		run_format_pass(&st, fmt);
		if (st.stop || st.argi == before)
			break;
	} while (st.argi < st.argc);

	if (!st.write_err && tam_str_len(st.out) > 0) {
		if (tc_write_all(STDOUT_FILENO, st.out, tam_str_len(st.out)) < 0) {
			tc_error(sh, line, "printf: write error: %s", strerror(errno));
			st.status = 1;
		}
	}

	tam_str_free(st.out);
	return st.status;
}
