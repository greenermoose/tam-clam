/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX job control (0.2.2): process groups, tcsetpgrp, job table,
 * %job_id resolution, fg/bg/jobs builtins, and prompt notifications. */
#ifndef TAM_CLAM_JOBS_H
#define TAM_CLAM_JOBS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct tc_shell;

#define TC_JOBS_MAX 256

enum tc_job_state {
	TC_JOB_RUNNING = 0,
	TC_JOB_STOPPED,
	TC_JOB_DONE,
};

struct tc_job {
	int id;                /* 1-based smallest unused job number */
	pid_t pgid;            /* process group ID */
	pid_t *pids;           /* processes in this job */
	bool *done;            /* per-process exit flag */
	int *statuses;         /* per-process wait status */
	size_t npids;
	enum tc_job_state state;
	int stop_sig;          /* signal that stopped the job */
	bool notified;         /* user already notified of stop/done */
	char *cmd;             /* display string for jobs / notifications */
	unsigned long order;   /* monotonic counter for + / - ordering */
};

struct tc_job_table {
	struct tc_job *jobs;
	size_t count;
	size_t cap;
	unsigned long next_order;
};

void tc_jobs_init(struct tc_job_table *jt);
void tc_jobs_finish(struct tc_job_table *jt);
void tc_jobs_reset_subshell(struct tc_job_table *jt);

/* Initialize interactive terminal job control (setpgid + tcsetpgrp + -m). */
void tc_jobs_init_terminal(struct tc_shell *sh);

/* Add a job with pgid and npids processes. Returns pointer to job or NULL. */
struct tc_job *tc_jobs_add(struct tc_job_table *jt, pid_t pgid,
			   const pid_t *pids, size_t npids,
			   const char *cmd, enum tc_job_state state);

/* Update a process state from waitpid status (works for exit, stop, cont). */
void tc_jobs_update_pid(struct tc_job_table *jt, pid_t pid, int ws);

/* Non-blockingly reap exited/stopped/continued children, updating jobs + bg. */
void tc_jobs_reap_nonblock(struct tc_shell *sh);

/* Print Done/Stopped notifications before the next interactive prompt and
 * remove notified Done jobs. */
void tc_jobs_notify(struct tc_shell *sh);

/* Wait for a foreground job (giving/reclaiming terminal if job_control).
 * If stopped by Ctrl+P / SIGTSTP, keeps it in sh->jobs as STOPPED and
 * returns 128 + stop_sig. Otherwise removes it and returns exit status. */
int tc_jobs_wait_fg(struct tc_shell *sh, struct tc_job *job, bool pipefail);

/* Resolve a %job_id specification (%, %%, %+, %-, %n, %prefix, %?substr).
 * On failure sets *err_msg to a static description and returns NULL. */
struct tc_job *tc_jobs_resolve(struct tc_job_table *jt, const char *spec,
			       const char **err_msg);

/* Determine current (+) and previous (-) job indices in jt->jobs, or -1. */
void tc_jobs_cur_prev(const struct tc_job_table *jt, ssize_t *cur, ssize_t *prev);

/* Wait for a specific job (used by `wait %n`). */
int tc_jobs_wait_job(struct tc_shell *sh, struct tc_job *job, bool *interrupted);

/* Builtins: jobs [-l | -p], fg [%id], bg [%id ...]. */
int tc_bi_jobs(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_fg(struct tc_shell *sh, int argc, char **argv, unsigned long line);
int tc_bi_bg(struct tc_shell *sh, int argc, char **argv, unsigned long line);

#endif
