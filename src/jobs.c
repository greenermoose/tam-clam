/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX job control: process groups, tcsetpgrp, job table, %job_id,
 * fg/bg/jobs builtins, and prompt notifications. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tam-clam/bg.h"
#include "tam-clam/builtins.h"
#include "tam-clam/jobs.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"

static void job_free_fields(struct tc_job *j)
{
	free(j->pids);
	free(j->done);
	free(j->statuses);
	free(j->cmd);
	memset(j, 0, sizeof(*j));
}

void tc_jobs_init(struct tc_job_table *jt)
{
	memset(jt, 0, sizeof(*jt));
}

void tc_jobs_finish(struct tc_job_table *jt)
{
	if (!jt)
		return;
	for (size_t i = 0; i < jt->count; i++)
		job_free_fields(&jt->jobs[i]);
	free(jt->jobs);
	memset(jt, 0, sizeof(*jt));
}

void tc_jobs_reset_subshell(struct tc_job_table *jt)
{
	if (!jt)
		return;
	for (size_t i = 0; i < jt->count; i++)
		job_free_fields(&jt->jobs[i]);
	jt->count = 0;
}

void tc_jobs_init_terminal(struct tc_shell *sh)
{
	if (!sh || !sh->interactive || !isatty(STDIN_FILENO))
		return;
	sh->tty_fd = STDIN_FILENO;
	pid_t pid = getpid();
	if (setpgid(pid, pid) < 0 && errno != EPERM && errno != EACCES)
		return;
	sh->shell_pgid = getpgrp();
	if (tcsetpgrp(sh->tty_fd, sh->shell_pgid) < 0)
		return;
	sh->job_control = true;
	sh->opt_monitor = true;
}

static int next_free_id(const struct tc_job_table *jt)
{
	for (int id = 1; id <= TC_JOBS_MAX + 1; id++) {
		bool used = false;
		for (size_t i = 0; i < jt->count; i++) {
			if (jt->jobs[i].id == id) {
				used = true;
				break;
			}
		}
		if (!used)
			return id;
	}
	return (int)jt->count + 1;
}

static void remove_job_at(struct tc_job_table *jt, size_t idx)
{
	job_free_fields(&jt->jobs[idx]);
	if (idx + 1 < jt->count)
		memmove(&jt->jobs[idx], &jt->jobs[idx + 1],
			(jt->count - idx - 1) * sizeof(jt->jobs[0]));
	jt->count--;
}

struct tc_job *tc_jobs_add(struct tc_job_table *jt, pid_t pgid,
			   const pid_t *pids, size_t npids,
			   const char *cmd, enum tc_job_state state)
{
	if (!jt || npids == 0)
		return NULL;
	if (jt->count >= TC_JOBS_MAX) {
		for (size_t i = 0; i < jt->count; i++) {
			if (jt->jobs[i].state == TC_JOB_DONE) {
				remove_job_at(jt, i);
				break;
			}
		}
		if (jt->count >= TC_JOBS_MAX)
			return NULL;
	}
	if (jt->count == jt->cap) {
		size_t ncap = jt->cap ? jt->cap * 2 : 16;
		if (ncap > TC_JOBS_MAX)
			ncap = TC_JOBS_MAX;
		struct tc_job *nj = realloc(jt->jobs, ncap * sizeof(*nj));
		if (!nj)
			return NULL;
		jt->jobs = nj;
		jt->cap = ncap;
	}

	pid_t *pc = malloc(npids * sizeof(*pc));
	bool *dc = calloc(npids, sizeof(*dc));
	int *sc = calloc(npids, sizeof(*sc));
	const char *label = (cmd && *cmd) ? cmd : "(job)";
	size_t clen = strlen(label);
	char *cc = malloc(clen + 1);
	if (!pc || !dc || !sc || !cc) {
		free(pc);
		free(dc);
		free(sc);
		free(cc);
		return NULL;
	}
	memcpy(pc, pids, npids * sizeof(*pc));
	memcpy(cc, label, clen + 1);

	struct tc_job *j = &jt->jobs[jt->count++];
	memset(j, 0, sizeof(*j));
	j->id = next_free_id(jt);
	j->pgid = pgid;
	j->pids = pc;
	j->done = dc;
	j->statuses = sc;
	j->npids = npids;
	j->state = state;
	j->order = ++jt->next_order;
	j->cmd = cc;
	return j;
}

static bool job_all_done(const struct tc_job *j)
{
	for (size_t i = 0; i < j->npids; i++) {
		if (!j->done[i])
			return false;
	}
	return true;
}

static int job_exit_status(const struct tc_job *j, bool pipefail)
{
	int st = 0;
	for (size_t i = 0; i < j->npids; i++) {
		if (pipefail) {
			if (j->statuses[i] != 0)
				st = j->statuses[i];
		} else {
			st = j->statuses[i];
		}
	}
	return st;
}

void tc_jobs_update_pid(struct tc_job_table *jt, pid_t pid, int ws)
{
	if (!jt)
		return;
	for (size_t i = 0; i < jt->count; i++) {
		struct tc_job *j = &jt->jobs[i];
		for (size_t k = 0; k < j->npids; k++) {
			if (j->pids[k] != pid)
				continue;
			if (WIFSTOPPED(ws)) {
				j->state = TC_JOB_STOPPED;
				j->stop_sig = WSTOPSIG(ws);
				j->notified = false;
				j->order = ++jt->next_order;
			} else if (WIFCONTINUED(ws)) {
				j->state = TC_JOB_RUNNING;
				j->stop_sig = 0;
				j->notified = false;
			} else {
				j->done[k] = true;
				j->statuses[k] = tc_wait_status(ws);
				if (job_all_done(j)) {
					j->state = TC_JOB_DONE;
					j->notified = false;
				}
			}
			return;
		}
	}
}

void tc_jobs_reap_nonblock(struct tc_shell *sh)
{
	if (!sh)
		return;
	for (;;) {
		int ws = 0;
		pid_t r = waitpid(-1, &ws, WNOHANG | WUNTRACED | WCONTINUED);
		if (r <= 0)
			break;
		tc_jobs_update_pid(&sh->jobs, r, ws);
		if (WIFEXITED(ws) || WIFSIGNALED(ws)) {
			/* Keep legacy bg table in sync for `wait <pid>`. */
			for (size_t i = 0; i < sh->bg.len; i++) {
				if (sh->bg.procs[i].pid == r) {
					sh->bg.procs[i].done = true;
					sh->bg.procs[i].status = tc_wait_status(ws);
					break;
				}
			}
		}
	}
}

/* POSIX current (+) and previous (-) job selection: stopped jobs outrank
 * running jobs; within each class, highest order wins. */
static bool job_ranks_higher(const struct tc_job *a, const struct tc_job *b)
{
	bool a_stop = (a->state == TC_JOB_STOPPED);
	bool b_stop = (b->state == TC_JOB_STOPPED);
	if (a_stop != b_stop)
		return a_stop;
	return a->order > b->order;
}

void tc_jobs_cur_prev(const struct tc_job_table *jt, ssize_t *cur, ssize_t *prev)
{
	*cur = -1;
	*prev = -1;
	if (!jt)
		return;
	for (size_t i = 0; i < jt->count; i++) {
		if (*cur < 0 || job_ranks_higher(&jt->jobs[i], &jt->jobs[*cur])) {
			*prev = *cur;
			*cur = (ssize_t)i;
		} else if (*prev < 0 ||
			   job_ranks_higher(&jt->jobs[i], &jt->jobs[*prev])) {
			*prev = (ssize_t)i;
		}
	}
}

static const char *job_state_str(const struct tc_job *j)
{
	switch (j->state) {
	case TC_JOB_RUNNING: return "Running";
	case TC_JOB_STOPPED: return "Stopped";
	case TC_JOB_DONE:    return "Done";
	}
	return "Unknown";
}

static void format_job_line(const struct tc_job *j, char mark, bool show_pid,
			    int fd)
{
	char buf[512];
	int n;
	if (show_pid) {
		n = snprintf(buf, sizeof(buf), "[%d]%c %ld %-10s %s\n",
			     j->id, mark, (long)j->pgid, job_state_str(j), j->cmd);
	} else {
		n = snprintf(buf, sizeof(buf), "[%d]%c %-10s %s\n",
			     j->id, mark, job_state_str(j), j->cmd);
	}
	if (n > 0)
		(void)tc_write_all(fd, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

void tc_jobs_notify(struct tc_shell *sh)
{
	if (!sh || !sh->interactive)
		return;
	tc_jobs_reap_nonblock(sh);
	ssize_t cur = -1, prev = -1;
	tc_jobs_cur_prev(&sh->jobs, &cur, &prev);

	for (size_t i = 0; i < sh->jobs.count; ) {
		struct tc_job *j = &sh->jobs.jobs[i];
		if (!j->notified && (j->state == TC_JOB_DONE || j->state == TC_JOB_STOPPED)) {
			char mark = ((ssize_t)i == cur) ? '+' :
				    ((ssize_t)i == prev) ? '-' : ' ';
			format_job_line(j, mark, false, STDERR_FILENO);
			j->notified = true;
		}
		if (j->state == TC_JOB_DONE && j->notified) {
			remove_job_at(&sh->jobs, i);
			tc_jobs_cur_prev(&sh->jobs, &cur, &prev);
		} else {
			i++;
		}
	}
}

int tc_jobs_wait_fg(struct tc_shell *sh, struct tc_job *job, bool pipefail)
{
	if (!sh || !job)
		return 0;
	pid_t pgid = job->pgid;
	int job_id = job->id;

	if (sh->job_control && sh->tty_fd >= 0)
		(void)tcsetpgrp(sh->tty_fd, pgid);

	bool stopped = false;
	int stop_sig = 0;
	bool sigint_term = false;

	for (;;) {
		/* Re-locate job by id in case table changed. */
		struct tc_job *cur_job = NULL;
		for (size_t i = 0; i < sh->jobs.count; i++) {
			if (sh->jobs.jobs[i].id == job_id) {
				cur_job = &sh->jobs.jobs[i];
				break;
			}
		}
		if (!cur_job || job_all_done(cur_job))
			break;

		int ws = 0;
		pid_t r = waitpid(-pgid, &ws, WUNTRACED);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		tc_jobs_update_pid(&sh->jobs, r, ws);
		if (WIFSTOPPED(ws)) {
			stopped = true;
			stop_sig = WSTOPSIG(ws);
			break;
		}
		if (WIFSIGNALED(ws) && WTERMSIG(ws) == SIGINT)
			sigint_term = true;
	}

	if (sh->job_control && sh->tty_fd >= 0)
		(void)tcsetpgrp(sh->tty_fd, sh->shell_pgid);

	/* Re-locate job to report stop or remove completed foreground job. */
	size_t idx = sh->jobs.count;
	for (size_t i = 0; i < sh->jobs.count; i++) {
		if (sh->jobs.jobs[i].id == job_id) {
			idx = i;
			break;
		}
	}

	if (stopped) {
		if (idx < sh->jobs.count) {
			struct tc_job *j = &sh->jobs.jobs[idx];
			j->state = TC_JOB_STOPPED;
			j->stop_sig = stop_sig;
			j->notified = true;
			if (sh->interactive) {
				(void)tc_write_all(STDERR_FILENO, "\n", 1);
				format_job_line(j, '+', false, STDERR_FILENO);
			}
		}
		return 128 + stop_sig;
	}

	if (sigint_term && sh->interactive)
		(void)tc_write_all(STDERR_FILENO, "\n", 1);

	int status = 0;
	if (idx < sh->jobs.count) {
		status = job_exit_status(&sh->jobs.jobs[idx], pipefail);
		remove_job_at(&sh->jobs, idx);
	}
	return status;
}

struct tc_job *tc_jobs_resolve(struct tc_job_table *jt, const char *spec,
			       const char **err_msg)
{
	if (err_msg)
		*err_msg = "no such job";
	if (!jt || !spec || spec[0] != '%')
		return NULL;

	const char *s = spec + 1;
	ssize_t cur = -1, prev = -1;
	tc_jobs_cur_prev(jt, &cur, &prev);

	if (s[0] == '\0' || strcmp(s, "%") == 0 || strcmp(s, "+") == 0) {
		if (cur < 0) {
			if (err_msg)
				*err_msg = "no current job";
			return NULL;
		}
		return &jt->jobs[cur];
	}
	if (strcmp(s, "-") == 0) {
		if (prev < 0) {
			if (err_msg)
				*err_msg = "no previous job";
			return NULL;
		}
		return &jt->jobs[prev];
	}

	/* %n numeric job ID */
	char *end = NULL;
	long num = strtol(s, &end, 10);
	if (end && *end == '\0' && num > 0) {
		for (size_t i = 0; i < jt->count; i++) {
			if (jt->jobs[i].id == (int)num)
				return &jt->jobs[i];
		}
		return NULL;
	}

	/* %?substr or %prefix */
	bool is_substr = (s[0] == '?');
	const char *pat = is_substr ? s + 1 : s;
	size_t plen = strlen(pat);
	struct tc_job *hit = NULL;
	for (size_t i = 0; i < jt->count; i++) {
		bool match = is_substr ? (strstr(jt->jobs[i].cmd, pat) != NULL)
				       : (strncmp(jt->jobs[i].cmd, pat, plen) == 0);
		if (match) {
			if (hit) {
				if (err_msg)
					*err_msg = "ambiguous job spec";
				return NULL;
			}
			hit = &jt->jobs[i];
		}
	}
	return hit;
}

int tc_jobs_wait_job(struct tc_shell *sh, struct tc_job *job, bool *interrupted)
{
	*interrupted = false;
	if (!sh || !job)
		return TC_STATUS_NOTFOUND;
	pid_t pgid = job->pgid;
	int job_id = job->id;

	for (;;) {
		struct tc_job *j = NULL;
		size_t idx = sh->jobs.count;
		for (size_t i = 0; i < sh->jobs.count; i++) {
			if (sh->jobs.jobs[i].id == job_id) {
				j = &sh->jobs.jobs[i];
				idx = i;
				break;
			}
		}
		if (!j)
			return TC_STATUS_NOTFOUND;
		if (j->state == TC_JOB_DONE || job_all_done(j)) {
			int st = job_exit_status(j, sh->opt_pipefail);
			remove_job_at(&sh->jobs, idx);
			return st;
		}
		int ws = 0;
		pid_t r = waitpid(-pgid, &ws, 0);
		if (r > 0) {
			tc_jobs_update_pid(&sh->jobs, r, ws);
			continue;
		}
		if (r < 0 && errno == EINTR) {
			if (sh->interactive && tc_sigint_seen) {
				*interrupted = true;
				return 128 + SIGINT;
			}
			continue;
		}
		int st = job_exit_status(j, sh->opt_pipefail);
		remove_job_at(&sh->jobs, idx);
		return st;
	}
}

int tc_bi_jobs(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool show_pid = false;
	bool pids_only = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'l') {
				show_pid = true;
				pids_only = false;
			} else if (*p == 'p') {
				pids_only = true;
				show_pid = false;
			} else {
				tc_error(sh, line, "jobs: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}

	tc_jobs_reap_nonblock(sh);
	ssize_t cur = -1, prev = -1;
	tc_jobs_cur_prev(&sh->jobs, &cur, &prev);

	if (i < argc) {
		int status = 0;
		for (; i < argc; i++) {
			const char *err = NULL;
			struct tc_job *j = tc_jobs_resolve(&sh->jobs, argv[i], &err);
			if (!j) {
				tc_error(sh, line, "jobs: %s: %s", argv[i], err);
				status = 1;
				continue;
			}
			if (pids_only) {
				char buf[32];
				int n = snprintf(buf, sizeof(buf), "%ld\n", (long)j->pgid);
				if (n > 0)
					(void)tc_write_all(STDOUT_FILENO, buf, (size_t)n);
			} else {
				ssize_t idx = (ssize_t)(j - sh->jobs.jobs);
				char mark = (idx == cur) ? '+' : (idx == prev) ? '-' : ' ';
				format_job_line(j, mark, show_pid, STDOUT_FILENO);
			}
			if (j->state == TC_JOB_DONE)
				j->notified = true;
		}
		for (size_t k = 0; k < sh->jobs.count; ) {
			if (sh->jobs.jobs[k].state == TC_JOB_DONE && sh->jobs.jobs[k].notified)
				remove_job_at(&sh->jobs, k);
			else
				k++;
		}
		return status;
	}

	for (size_t k = 0; k < sh->jobs.count; ) {
		struct tc_job *j = &sh->jobs.jobs[k];
		if (pids_only) {
			char buf[32];
			int n = snprintf(buf, sizeof(buf), "%ld\n", (long)j->pgid);
			if (n > 0)
				(void)tc_write_all(STDOUT_FILENO, buf, (size_t)n);
		} else {
			char mark = ((ssize_t)k == cur) ? '+' :
				    ((ssize_t)k == prev) ? '-' : ' ';
			format_job_line(j, mark, show_pid, STDOUT_FILENO);
		}
		if (j->state == TC_JOB_DONE) {
			remove_job_at(&sh->jobs, k);
			tc_jobs_cur_prev(&sh->jobs, &cur, &prev);
		} else {
			k++;
		}
	}
	return 0;
}

int tc_bi_fg(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	if (!sh->opt_monitor) {
		tc_error(sh, line, "fg: no job control");
		return 1;
	}
	tc_jobs_reap_nonblock(sh);
	const char *spec = (argc >= 2) ? argv[1] : "%+";
	if (argc > 2) {
		tc_error(sh, line, "fg: too many arguments");
		return TC_STATUS_USAGE;
	}
	const char *err = NULL;
	struct tc_job *j = tc_jobs_resolve(&sh->jobs, spec, &err);
	if (!j) {
		tc_error(sh, line, "fg: %s: %s", spec, err);
		return 1;
	}
	char msg[256];
	int n = snprintf(msg, sizeof(msg), "%s\n", j->cmd);
	if (n > 0)
		(void)tc_write_all(STDERR_FILENO, msg, (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg) - 1);

	if (j->state == TC_JOB_STOPPED) {
		j->state = TC_JOB_RUNNING;
		j->stop_sig = 0;
		(void)kill(-j->pgid, SIGCONT);
	}
	return tc_jobs_wait_fg(sh, j, sh->opt_pipefail);
}

int tc_bi_bg(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	if (!sh->opt_monitor) {
		tc_error(sh, line, "bg: no job control");
		return 1;
	}
	tc_jobs_reap_nonblock(sh);
	const char *def[1] = {"%+"};
	const char *const *specs = (argc >= 2) ? (const char *const *)(argv + 1) : def;
	int nspecs = (argc >= 2) ? (argc - 1) : 1;
	int status = 0;

	for (int i = 0; i < nspecs; i++) {
		const char *err = NULL;
		struct tc_job *j = tc_jobs_resolve(&sh->jobs, specs[i], &err);
		if (!j) {
			tc_error(sh, line, "bg: %s: %s", specs[i], err);
			status = 1;
			continue;
		}
		if (j->state == TC_JOB_STOPPED) {
			j->state = TC_JOB_RUNNING;
			j->stop_sig = 0;
			(void)kill(-j->pgid, SIGCONT);
		}
		char msg[256];
		int n = snprintf(msg, sizeof(msg), "[%d]+ %s &\n", j->id, j->cmd);
		if (n > 0)
			(void)tc_write_all(STDERR_FILENO, msg, (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg) - 1);
	}
	return status;
}
