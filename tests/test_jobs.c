/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for 0.2.2 job control table and %job_id resolution (src/jobs.c). */
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include "check.h"
#include "tam-clam/jobs.h"

static void test_job_table_and_resolve(void)
{
	struct tc_job_table jt;
	tc_jobs_init(&jt);

	pid_t p1[1] = {1001};
	pid_t p2[2] = {1002, 1003};
	pid_t p3[1] = {1004};

	struct tc_job *j1 = tc_jobs_add(&jt, 1001, p1, 1, "sleep 10", TC_JOB_RUNNING);
	struct tc_job *j2 = tc_jobs_add(&jt, 1002, p2, 2, "grep foo | wc -l", TC_JOB_RUNNING);
	struct tc_job *j3 = tc_jobs_add(&jt, 1004, p3, 1, "sleep 20", TC_JOB_STOPPED);
	CHECK(j1 && j1->id == 1);
	CHECK(j2 && j2->id == 2);
	CHECK(j3 && j3->id == 3);
	CHECK(jt.count == 3);

	/* Stopped job (id 3) is current (+), most recent running (id 2) is prev (-). */
	ssize_t cur = -1, prev = -1;
	tc_jobs_cur_prev(&jt, &cur, &prev);
	CHECK(cur == 2 && jt.jobs[cur].id == 3);
	CHECK(prev == 1 && jt.jobs[prev].id == 2);

	const char *err = NULL;
	CHECK(tc_jobs_resolve(&jt, "%", &err) == &jt.jobs[2]);
	CHECK(tc_jobs_resolve(&jt, "%%", &err) == &jt.jobs[2]);
	CHECK(tc_jobs_resolve(&jt, "%+", &err) == &jt.jobs[2]);
	CHECK(tc_jobs_resolve(&jt, "%-", &err) == &jt.jobs[1]);
	CHECK(tc_jobs_resolve(&jt, "%1", &err) == &jt.jobs[0]);
	CHECK(tc_jobs_resolve(&jt, "%2", &err) == &jt.jobs[1]);
	CHECK(tc_jobs_resolve(&jt, "%99", &err) == NULL);

	/* Prefix and substring matching. */
	CHECK(tc_jobs_resolve(&jt, "%grep", &err) == &jt.jobs[1]);
	CHECK(tc_jobs_resolve(&jt, "%?wc", &err) == &jt.jobs[1]);
	CHECK(tc_jobs_resolve(&jt, "%sleep", &err) == NULL); /* ambiguous */
	CHECK(err && strstr(err, "ambiguous") != NULL);

	/* Multi-process pipeline job becomes DONE only when all PIDs finish. */
	tc_jobs_update_pid(&jt, 1002, 0);
	CHECK(jt.jobs[1].state == TC_JOB_RUNNING);
	tc_jobs_update_pid(&jt, 1003, 0);
	CHECK(jt.jobs[1].state == TC_JOB_DONE);

	/* Reset clears table cleanly. */
	tc_jobs_reset_subshell(&jt);
	CHECK(jt.count == 0);
	CHECK(tc_jobs_resolve(&jt, "%+", &err) == NULL);

	tc_jobs_finish(&jt);
}

int main(void)
{
	test_job_table_and_resolve();
	CHECK_DONE("test_jobs");
}
