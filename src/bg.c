/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Background process tracking table for & and wait. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "tam-clam/bg.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"

void tc_bg_init(struct tc_bg_table *bg)
{
	bg->procs = NULL;
	bg->len = 0;
	bg->cap = 0;
}

void tc_bg_finish(struct tc_bg_table *bg)
{
	free(bg->procs);
	bg->procs = NULL;
	bg->len = 0;
	bg->cap = 0;
}

void tc_bg_reset_subshell(struct tc_bg_table *bg)
{
	bg->len = 0;
}

int tc_wait_status(int ws)
{
	if (WIFEXITED(ws))
		return WEXITSTATUS(ws);
	if (WIFSIGNALED(ws))
		return 128 + WTERMSIG(ws);
	return TC_STATUS_NOEXEC;
}

static void record_done(struct tc_bg_table *bg, pid_t pid, int status)
{
	for (size_t i = 0; i < bg->len; i++) {
		if (bg->procs[i].pid == pid) {
			bg->procs[i].done = true;
			bg->procs[i].status = status;
			return;
		}
	}
}

static void remove_at(struct tc_bg_table *bg, size_t idx)
{
	if (idx + 1 < bg->len)
		memmove(&bg->procs[idx], &bg->procs[idx + 1],
			(bg->len - idx - 1) * sizeof(*bg->procs));
	bg->len--;
}

int tc_bg_add(struct tc_bg_table *bg, pid_t pid)
{
	if (bg->len >= TC_BG_MAX) {
		/* Evict oldest already-finished entry to stay within cap. */
		for (size_t i = 0; i < bg->len; i++) {
			if (bg->procs[i].done) {
				remove_at(bg, i);
				break;
			}
		}
		if (bg->len >= TC_BG_MAX)
			remove_at(bg, 0);
	}
	if (bg->len == bg->cap) {
		size_t ncap = bg->cap ? bg->cap * 2 : 16;
		if (ncap > TC_BG_MAX)
			ncap = TC_BG_MAX;
		struct tc_bg_proc *np = realloc(bg->procs, ncap * sizeof(*np));
		if (!np)
			return -1;
		bg->procs = np;
		bg->cap = ncap;
	}
	bg->procs[bg->len].pid = pid;
	bg->procs[bg->len].done = false;
	bg->procs[bg->len].status = 0;
	bg->len++;
	return 0;
}

void tc_bg_reap_nonblock(struct tc_bg_table *bg)
{
	for (;;) {
		int ws;
		pid_t r = waitpid(-1, &ws, WNOHANG);
		if (r <= 0)
			break;
		record_done(bg, r, tc_wait_status(ws));
	}
}

static int pending_trapped_sig(const struct tc_shell *sh)
{
	if (!tc_sig_any)
		return 0;
	for (int sig = 1; sig < TC_NSIG; sig++) {
		if (tc_sig_pending[sig] && tc_trap_is_caught(&sh->traps, sig))
			return sig;
	}
	return 0;
}

int tc_bg_wait_all(struct tc_shell *sh)
{
	for (;;) {
		tc_bg_reap_nonblock(&sh->bg);
		bool any_running = false;
		for (size_t i = 0; i < sh->bg.len; i++) {
			if (!sh->bg.procs[i].done) {
				any_running = true;
				break;
			}
		}
		if (!any_running) {
			sh->bg.len = 0;
			return 0;
		}
		int ws;
		pid_t r = waitpid(-1, &ws, 0);
		if (r > 0) {
			record_done(&sh->bg, r, tc_wait_status(ws));
			continue;
		}
		if (r < 0 && errno == EINTR) {
			int sig = pending_trapped_sig(sh);
			if (sig > 0)
				return 128 + sig;
			if (sh->interactive && tc_sigint_seen)
				return 128 + SIGINT;
			continue;
		}
		/* ECHILD: no remaining children. */
		sh->bg.len = 0;
		return 0;
	}
}

int tc_bg_wait_pid(struct tc_shell *sh, pid_t pid, bool *interrupted)
{
	*interrupted = false;
	tc_bg_reap_nonblock(&sh->bg);

	size_t idx = sh->bg.len;
	for (size_t i = 0; i < sh->bg.len; i++) {
		if (sh->bg.procs[i].pid == pid) {
			idx = i;
			break;
		}
	}
	if (idx == sh->bg.len)
		return TC_STATUS_NOTFOUND;

	if (sh->bg.procs[idx].done) {
		int st = sh->bg.procs[idx].status;
		remove_at(&sh->bg, idx);
		return st;
	}

	for (;;) {
		int ws;
		pid_t r = waitpid(pid, &ws, 0);
		if (r == pid) {
			int st = tc_wait_status(ws);
			/* Re-find in case table shifted. */
			for (size_t i = 0; i < sh->bg.len; i++) {
				if (sh->bg.procs[i].pid == pid) {
					remove_at(&sh->bg, i);
					break;
				}
			}
			return st;
		}
		if (r < 0 && errno == EINTR) {
			int sig = pending_trapped_sig(sh);
			if (sig > 0) {
				*interrupted = true;
				return 128 + sig;
			}
			if (sh->interactive && tc_sigint_seen) {
				*interrupted = true;
				return 128 + SIGINT;
			}
			continue;
		}
		/* ECHILD: process already reaped or not a child. */
		for (size_t i = 0; i < sh->bg.len; i++) {
			if (sh->bg.procs[i].pid == pid) {
				int st = sh->bg.procs[i].done
					     ? sh->bg.procs[i].status
					     : TC_STATUS_NOTFOUND;
				remove_at(&sh->bg, i);
				return st;
			}
		}
		return TC_STATUS_NOTFOUND;
	}
}
