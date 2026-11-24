/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Append-only command history (0.2.1): single-write O_APPEND records with
 * timestamp, exit status, and working directory; damage-tolerant reader;
 * live multi-shell sync; prefix/substring search; and private mode. */
#ifndef TAM_CLAM_HISTORY_H
#define TAM_CLAM_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct tc_shell;

#define TC_HIST_MAX      10000
#define TC_HIST_REC_MAX  131072

struct tc_hist_entry {
	int64_t timestamp;
	int status;
	char *cwd;
	char *cmd;
	size_t cmd_len;
};

struct tc_history {
	struct tc_hist_entry *entries;
	size_t count;
	size_t cap;
	char *path;        /* NULL in private mode or if no path available */
	off_t file_offset; /* bytes already read from path */
	bool private_mode;
};

void tc_hist_init(struct tc_history *h, const char *path, bool private_mode);
void tc_hist_finish(struct tc_history *h);
void tc_hist_clear(struct tc_history *h);

/* Load existing history file from offset 0 (skips damaged records). 0 or -1. */
int tc_hist_load(struct tc_history *h);
/* Incrementally read any records appended by other shells since last sync. */
int tc_hist_sync(struct tc_history *h);

/* Record a command in memory and (unless private_mode / leading space) append
 * atomically to h->path via a single O_APPEND write. Returns 0 on success,
 * -1 on allocation or disk write error (in-memory history stays intact). */
int tc_hist_add(struct tc_history *h, const char *cmd, size_t cmd_len,
		int status, const char *cwd);

/* Prefix search backward (step = -1) or forward (step = +1) starting from
 * start_idx (inclusive). Returns matching index, or -1 if none. */
ssize_t tc_hist_find_prefix(const struct tc_history *h, const char *prefix,
			    size_t prefix_len, ssize_t start_idx, int step);

/* Case-insensitive substring search backward (step = -1) or forward (+1)
 * starting from start_idx (inclusive). Returns matching index, or -1. */
ssize_t tc_hist_find_substr(const struct tc_history *h, const char *query,
			    ssize_t start_idx, int step);

/* Extract the last shell word from cmd[0..len) into a heap string.
 * Returns NULL if no word is found or on OOM. Caller frees. */
char *tc_hist_last_arg(const char *cmd, size_t len, size_t *out_len);

/* Builtin: history [N | -c]. */
int tc_bi_history(struct tc_shell *sh, int argc, char **argv, unsigned long line);

#endif
