/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Append-only history store: v1 <unix_ts> <status> <cwd_esc> <cmd_esc>\n */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/history.h"
#include "tam-clam/shell.h"

static void entry_free(struct tc_hist_entry *e)
{
	free(e->cwd);
	free(e->cmd);
	e->cwd = NULL;
	e->cmd = NULL;
	e->cmd_len = 0;
}

void tc_hist_init(struct tc_history *h, const char *path, bool private_mode)
{
	memset(h, 0, sizeof(*h));
	h->private_mode = private_mode;
	if (!private_mode && path && *path) {
		size_t n = strlen(path);
		h->path = malloc(n + 1);
		if (h->path)
			memcpy(h->path, path, n + 1);
	}
}

void tc_hist_clear(struct tc_history *h)
{
	if (!h)
		return;
	for (size_t i = 0; i < h->count; i++)
		entry_free(&h->entries[i]);
	h->count = 0;
}

void tc_hist_finish(struct tc_history *h)
{
	if (!h)
		return;
	tc_hist_clear(h);
	free(h->entries);
	free(h->path);
	memset(h, 0, sizeof(*h));
}

static int append_entry(struct tc_history *h, int64_t ts, int status,
			const char *cwd, const char *cmd, size_t cmd_len)
{
	if (h->count == TC_HIST_MAX) {
		entry_free(&h->entries[0]);
		memmove(&h->entries[0], &h->entries[1],
			(TC_HIST_MAX - 1) * sizeof(h->entries[0]));
		h->count--;
	}
	if (h->count == h->cap) {
		size_t ncap = h->cap ? h->cap * 2 : 64;
		if (ncap > TC_HIST_MAX)
			ncap = TC_HIST_MAX;
		struct tc_hist_entry *ne = realloc(h->entries, ncap * sizeof(*ne));
		if (!ne)
			return -1;
		h->entries = ne;
		h->cap = ncap;
	}
	size_t cwd_len = cwd ? strlen(cwd) : 0;
	char *cwd_copy = malloc(cwd_len + 1);
	char *cmd_copy = malloc(cmd_len + 1);
	if (!cwd_copy || !cmd_copy) {
		free(cwd_copy);
		free(cmd_copy);
		return -1;
	}
	if (cwd_len > 0)
		memcpy(cwd_copy, cwd, cwd_len);
	cwd_copy[cwd_len] = '\0';
	if (cmd_len > 0)
		memcpy(cmd_copy, cmd, cmd_len);
	cmd_copy[cmd_len] = '\0';

	struct tc_hist_entry *e = &h->entries[h->count++];
	e->timestamp = ts;
	e->status = status;
	e->cwd = cwd_copy;
	e->cmd = cmd_copy;
	e->cmd_len = cmd_len;
	return 0;
}

/* Escape a field: esc_space=true escapes ' ' and '\t' too (for cwd token). */
static size_t escape_field(const char *src, size_t len, bool esc_space,
			   char *dst, size_t cap)
{
	size_t out = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)src[i];
		const char *rep = NULL;
		if (c == '\\')
			rep = "\\\\";
		else if (c == '\n')
			rep = "\\n";
		else if (c == '\r')
			rep = "\\r";
		else if (c == '\0')
			rep = "\\0";
		else if (esc_space && c == ' ')
			rep = "\\s";
		else if (esc_space && c == '\t')
			rep = "\\t";

		if (rep) {
			if (out + 2 > cap)
				return (size_t)-1;
			dst[out++] = rep[0];
			dst[out++] = rep[1];
		} else {
			if (out + 1 > cap)
				return (size_t)-1;
			dst[out++] = (char)c;
		}
	}
	return out;
}

static int unescape_field(const char *src, size_t len, char **out_str, size_t *out_len)
{
	char *buf = malloc(len + 1);
	if (!buf)
		return -1;
	size_t pos = 0;
	for (size_t i = 0; i < len; i++) {
		if (src[i] == '\\') {
			if (i + 1 >= len) {
				free(buf);
				return -1;
			}
			char next = src[++i];
			switch (next) {
			case '\\': buf[pos++] = '\\'; break;
			case 'n':  buf[pos++] = '\n'; break;
			case 'r':  buf[pos++] = '\r'; break;
			case 't':  buf[pos++] = '\t'; break;
			case 's':  buf[pos++] = ' ';  break;
			case '0':  buf[pos++] = '\0'; break;
			default:
				free(buf);
				return -1;
			}
		} else {
			buf[pos++] = src[i];
		}
	}
	buf[pos] = '\0';
	*out_str = buf;
	if (out_len)
		*out_len = pos;
	return 0;
}

static bool parse_i64(const char *s, size_t len, int64_t *out)
{
	if (len == 0)
		return false;
	size_t i = 0;
	bool neg = false;
	if (s[0] == '-') {
		neg = true;
		i = 1;
	}
	if (i >= len)
		return false;
	uint64_t v = 0;
	for (; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		unsigned d = (unsigned)(s[i] - '0');
		if (v > (UINT64_MAX - d) / 10)
			return false;
		v = v * 10 + d;
	}
	if (neg) {
		if (v > (uint64_t)INT64_MAX + 1)
			return false;
		*out = (v == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)v;
	} else {
		if (v > (uint64_t)INT64_MAX)
			return false;
		*out = (int64_t)v;
	}
	return true;
}

/* Parse one complete line (without trailing '\n'). Returns true if valid. */
static bool parse_record_line(struct tc_history *h, const char *line, size_t len)
{
	if (len < 6 || memcmp(line, "v1 ", 3) != 0)
		return false;
	const char *p = line + 3;
	const char *end = line + len;

	/* Field 1: timestamp */
	const char *sp1 = memchr(p, ' ', (size_t)(end - p));
	if (!sp1)
		return false;
	int64_t ts = 0;
	if (!parse_i64(p, (size_t)(sp1 - p), &ts))
		return false;
	p = sp1 + 1;

	/* Field 2: status */
	const char *sp2 = memchr(p, ' ', (size_t)(end - p));
	if (!sp2)
		return false;
	int64_t st = 0;
	if (!parse_i64(p, (size_t)(sp2 - p), &st) || st < -1 || st > 255)
		return false;
	p = sp2 + 1;

	/* Field 3: escaped cwd ("-" if empty) */
	const char *sp3 = memchr(p, ' ', (size_t)(end - p));
	if (!sp3)
		return false;
	char *cwd = NULL;
	size_t cwd_len = 0;
	if (sp3 - p == 1 && p[0] == '-') {
		cwd = malloc(1);
		if (!cwd)
			return false;
		cwd[0] = '\0';
	} else if (unescape_field(p, (size_t)(sp3 - p), &cwd, &cwd_len) < 0) {
		return false;
	}
	p = sp3 + 1;

	/* Field 4: escaped command (rest of line, non-empty) */
	if (p >= end) {
		free(cwd);
		return false;
	}
	char *cmd = NULL;
	size_t cmd_len = 0;
	if (unescape_field(p, (size_t)(end - p), &cmd, &cmd_len) < 0) {
		free(cwd);
		return false;
	}
	int rc = append_entry(h, ts, (int)st, cwd, cmd, cmd_len);
	free(cwd);
	free(cmd);
	return rc == 0;
}

static int read_from_offset(struct tc_history *h, off_t offset)
{
	if (!h || h->private_mode || !h->path)
		return 0;
	int fd = open(h->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;

	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return -1;
	}
	if (st.st_size < offset) {
		/* File was truncated externally: reload from start. */
		tc_hist_clear(h);
		offset = 0;
	}
	if (st.st_size == offset) {
		close(fd);
		return 0;
	}
	/* Read at most 8 MiB at a time to bound memory. A first load of a
	 * larger file reads its newest 8 MiB, not its oldest: history is used
	 * from the end. The partial first line is skipped below. */
	const size_t cap = 8u << 20;
	off_t start = offset;
	if (offset == 0 && (size_t)st.st_size > cap)
		start = st.st_size - (off_t)cap;
	if (lseek(fd, start, SEEK_SET) < 0) {
		close(fd);
		return -1;
	}

	size_t to_read = (size_t)(st.st_size - start);
	if (to_read > cap)
		to_read = cap;
	char *buf = malloc(to_read);
	if (!buf) {
		close(fd);
		return -1;
	}
	size_t got = 0;
	while (got < to_read) {
		ssize_t n = read(fd, buf + got, to_read - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);

	/* Parse complete newline-terminated lines only; any trailing partial
	 * line (e.g. from a crashed writer) is skipped cleanly. */
	size_t pos = 0;
	size_t consumed = 0;
	if (start > offset) {
		char *first = memchr(buf, '\n', got);
		pos = consumed = first ? (size_t)(first - buf) + 1 : got;
	}
	while (pos < got) {
		char *nl = memchr(buf + pos, '\n', got - pos);
		if (!nl)
			break;
		size_t llen = (size_t)(nl - (buf + pos));
		(void)parse_record_line(h, buf + pos, llen);
		pos = (size_t)(nl - buf) + 1;
		consumed = pos;
	}
	free(buf);
	h->file_offset = start + (off_t)consumed;
	return 0;
}

int tc_hist_load(struct tc_history *h)
{
	if (!h)
		return -1;
	tc_hist_clear(h);
	h->file_offset = 0;
	return read_from_offset(h, 0);
}

int tc_hist_sync(struct tc_history *h)
{
	if (!h || h->private_mode || !h->path)
		return 0;
	struct stat st;
	if (stat(h->path, &st) < 0)
		return 0;
	if (st.st_size == h->file_offset)
		return 0;
	return read_from_offset(h, h->file_offset);
}

static bool is_blank_or_leading_space(const char *s, size_t len)
{
	if (len == 0)
		return true;
	if (s[0] == ' ' || s[0] == '\t')
		return true;
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			return false;
	}
	return true;
}

int tc_hist_add(struct tc_history *h, const char *cmd, size_t cmd_len,
		int status, const char *cwd)
{
	if (!h || !cmd || is_blank_or_leading_space(cmd, cmd_len))
		return 0;

	int64_t ts = (int64_t)time(NULL);
	if (append_entry(h, ts, status, cwd ? cwd : "", cmd, cmd_len) < 0)
		return -1;

	if (h->private_mode || !h->path)
		return 0;

	/* Format whole record into one buffer for a single atomic O_APPEND write. */
	size_t cwd_len = cwd ? strlen(cwd) : 0;
	size_t need = 64 + cwd_len * 2 + cmd_len * 2 + 4;
	if (need > TC_HIST_REC_MAX)
		return -1;
	char *rec = malloc(need);
	if (!rec)
		return -1;

	int hdr = snprintf(rec, need, "v1 %lld %d ", (long long)ts, status);
	if (hdr < 0 || (size_t)hdr >= need) {
		free(rec);
		return -1;
	}
	size_t pos = (size_t)hdr;
	if (cwd_len == 0) {
		rec[pos++] = '-';
	} else {
		size_t n = escape_field(cwd, cwd_len, true, rec + pos, need - pos - 2);
		if (n == (size_t)-1) {
			free(rec);
			return -1;
		}
		pos += n;
	}
	rec[pos++] = ' ';
	size_t cn = escape_field(cmd, cmd_len, false, rec + pos, need - pos - 1);
	if (cn == (size_t)-1) {
		free(rec);
		return -1;
	}
	pos += cn;
	rec[pos++] = '\n';

	int fd = open(h->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0) {
		free(rec);
		return -1;
	}
	ssize_t wr;
	do {
		wr = write(fd, rec, pos);
	} while (wr < 0 && errno == EINTR);
	struct stat st;
	bool have_st = (fstat(fd, &st) == 0);
	int close_rc = close(fd);
	free(rec);

	if (wr < 0 || (size_t)wr != pos || close_rc < 0)
		return -1;
	if (have_st)
		h->file_offset = st.st_size;
	return 0;
}

ssize_t tc_hist_find_prefix(const struct tc_history *h, const char *prefix,
			    size_t prefix_len, ssize_t start_idx, int step)
{
	if (!h || h->count == 0 || (step != -1 && step != 1))
		return -1;
	if (start_idx < 0 || (size_t)start_idx >= h->count)
		return -1;
	for (ssize_t i = start_idx; i >= 0 && (size_t)i < h->count; i += step) {
		const struct tc_hist_entry *e = &h->entries[i];
		if (prefix_len == 0 ||
		    (e->cmd_len >= prefix_len && memcmp(e->cmd, prefix, prefix_len) == 0))
			return i;
	}
	return -1;
}

static bool contains_ci(const char *hay, size_t hlen, const char *needle, size_t nlen)
{
	if (nlen == 0)
		return true;
	if (nlen > hlen)
		return false;
	for (size_t i = 0; i + nlen <= hlen; i++) {
		bool ok = true;
		for (size_t j = 0; j < nlen; j++) {
			if (tolower((unsigned char)hay[i + j]) !=
			    tolower((unsigned char)needle[j])) {
				ok = false;
				break;
			}
		}
		if (ok)
			return true;
	}
	return false;
}

ssize_t tc_hist_find_substr(const struct tc_history *h, const char *query,
			    ssize_t start_idx, int step)
{
	if (!h || h->count == 0 || !query || (step != -1 && step != 1))
		return -1;
	if (start_idx < 0 || (size_t)start_idx >= h->count)
		return -1;
	size_t qlen = strlen(query);
	for (ssize_t i = start_idx; i >= 0 && (size_t)i < h->count; i += step) {
		const struct tc_hist_entry *e = &h->entries[i];
		if (contains_ci(e->cmd, e->cmd_len, query, qlen))
			return i;
	}
	return -1;
}

char *tc_hist_last_arg(const char *cmd, size_t len, size_t *out_len)
{
	if (out_len)
		*out_len = 0;
	if (!cmd || len == 0)
		return NULL;
	size_t last_start = 0, last_end = 0;
	bool found = false;
	size_t i = 0;
	while (i < len) {
		while (i < len && (cmd[i] == ' ' || cmd[i] == '\t' || cmd[i] == '\n'))
			i++;
		if (i >= len)
			break;
		size_t ws = i;
		bool in_sq = false, in_dq = false;
		while (i < len) {
			char c = cmd[i];
			if (in_sq) {
				if (c == '\'')
					in_sq = false;
				i++;
			} else if (in_dq) {
				if (c == '\\' && i + 1 < len)
					i += 2;
				else {
					if (c == '"')
						in_dq = false;
					i++;
				}
			} else if (c == '\\' && i + 1 < len) {
				i += 2;
			} else if (c == '\'') {
				in_sq = true;
				i++;
			} else if (c == '"') {
				in_dq = true;
				i++;
			} else if (c == ' ' || c == '\t' || c == '\n') {
				break;
			} else {
				i++;
			}
		}
		last_start = ws;
		last_end = i;
		found = true;
	}
	if (!found || last_end <= last_start)
		return NULL;
	size_t wlen = last_end - last_start;
	char *out = malloc(wlen + 1);
	if (!out)
		return NULL;
	memcpy(out, cmd + last_start, wlen);
	out[wlen] = '\0';
	if (out_len)
		*out_len = wlen;
	return out;
}

int tc_bi_history(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	if (!sh->hist)
		return 0;
	if (argc == 2 && strcmp(argv[1], "-c") == 0) {
		tc_hist_clear(sh->hist);
		return 0;
	}
	size_t start = 0;
	if (argc == 2) {
		char *end = NULL;
		long n = strtol(argv[1], &end, 10);
		if (!end || *end != '\0' || n < 0) {
			tc_error(sh, line, "history: %s: numeric argument required", argv[1]);
			return TC_STATUS_USAGE;
		}
		if ((size_t)n < sh->hist->count)
			start = sh->hist->count - (size_t)n;
	} else if (argc > 2) {
		tc_error(sh, line, "usage: history [N | -c]");
		return TC_STATUS_USAGE;
	}
	(void)tc_hist_sync(sh->hist);
	for (size_t i = start; i < sh->hist->count; i++) {
		const struct tc_hist_entry *e = &sh->hist->entries[i];
		char hdr[32];
		int hn = snprintf(hdr, sizeof(hdr), "%5zu  ", i + 1);
		if (hn > 0 && tc_write_all(STDOUT_FILENO, hdr, (size_t)hn) < 0)
			return 1;
		if (e->cmd_len > 0 && tc_write_all(STDOUT_FILENO, e->cmd, e->cmd_len) < 0)
			return 1;
		if (tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
			return 1;
	}
	return 0;
}
