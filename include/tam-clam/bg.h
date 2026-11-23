/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Background process tracking table for & and wait. */
#ifndef TAM_CLAM_BG_H
#define TAM_CLAM_BG_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct tc_shell;

#define TC_BG_MAX 4096

struct tc_bg_proc {
	pid_t pid;
	bool done;
	int status;
};

struct tc_bg_table {
	struct tc_bg_proc *procs;
	size_t len;
	size_t cap;
};

void tc_bg_init(struct tc_bg_table *bg);
void tc_bg_finish(struct tc_bg_table *bg);

/* Clear parent's background list inside a forked subshell. */
void tc_bg_reset_subshell(struct tc_bg_table *bg);

/* Record a newly started background process. Returns 0 or -1 on OOM. */
int tc_bg_add(struct tc_bg_table *bg, pid_t pid);

/* Non-blockingly reap any finished children and update matching entries. */
void tc_bg_reap_nonblock(struct tc_bg_table *bg);

/* Convert raw waitpid status into POSIX shell exit status. */
int tc_wait_status(int ws);

/* Wait for all known background children. Returns 0, or 128+sig if
 * interrupted by a trapped signal. */
int tc_bg_wait_all(struct tc_shell *sh);

/* Wait for a specific PID. Returns its exit status, 127 if unknown/already
 * waited, or 128+sig if interrupted by a trapped signal. Sets *interrupted
 * when interrupted by a trapped signal. */
int tc_bg_wait_pid(struct tc_shell *sh, pid_t pid, bool *interrupted);

#endif
