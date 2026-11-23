/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "check.h"
#include "tam-clam/builtins.h"
#include "tam-clam/shell.h"

static int run_test(struct tc_shell *sh, int argc, char **argv)
{
	return tc_bi_test(sh, argc, argv, 1);
}

int main(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test_test");

	/* 0 and 1 arg. */
	{
		char *a0[] = {"test", NULL};
		CHECK(run_test(&sh, 1, a0) == 1);
		char *a1[] = {"test", "", NULL};
		CHECK(run_test(&sh, 2, a1) == 1);
		char *a2[] = {"test", "x", NULL};
		CHECK(run_test(&sh, 2, a2) == 0);
		char *a3[] = {"test", "!", NULL};
		CHECK(run_test(&sh, 2, a3) == 0);
		char *a4[] = {"test", "-n", NULL};
		CHECK(run_test(&sh, 2, a4) == 0);
	}

	/* 2 args. */
	{
		char *a1[] = {"test", "!", "", NULL};
		CHECK(run_test(&sh, 3, a1) == 0);
		char *a2[] = {"test", "!", "x", NULL};
		CHECK(run_test(&sh, 3, a2) == 1);
		char *a3[] = {"test", "-z", "", NULL};
		CHECK(run_test(&sh, 3, a3) == 0);
		char *a4[] = {"test", "-z", "x", NULL};
		CHECK(run_test(&sh, 3, a4) == 1);
		char *a5[] = {"test", "-n", "x", NULL};
		CHECK(run_test(&sh, 3, a5) == 0);
		char *a6[] = {"test", "-d", "/tmp", NULL};
		CHECK(run_test(&sh, 3, a6) == 0);
	}

	/* 3 args (including POSIX disambiguation: '!' '=' '!' is binary '='). */
	{
		char *a1[] = {"test", "!", "=", "!", NULL};
		CHECK(run_test(&sh, 4, a1) == 0);
		char *a2[] = {"test", "a", "!=", "b", NULL};
		CHECK(run_test(&sh, 4, a2) == 0);
		char *a3[] = {"test", "-5", "-lt", "3", NULL};
		CHECK(run_test(&sh, 4, a3) == 0);
		char *a4[] = {"test", "10", "-eq", "10", NULL};
		CHECK(run_test(&sh, 4, a4) == 0);
		char *a5[] = {"test", "(", "x", ")", NULL};
		CHECK(run_test(&sh, 4, a5) == 0);
		char *a6[] = {"test", "x", "-a", "", NULL};
		CHECK(run_test(&sh, 4, a6) == 1);
		char *a7[] = {"test", "", "-o", "x", NULL};
		CHECK(run_test(&sh, 4, a7) == 0);
	}

	/* 4 args. */
	{
		char *a1[] = {"test", "!", "a", "=", "b", NULL};
		CHECK(run_test(&sh, 5, a1) == 0);
		char *a2[] = {"test", "(", "-z", "", ")", NULL};
		CHECK(run_test(&sh, 5, a2) == 0);
	}

	/* 5+ args with precedence (-a binds tighter than -o) and parens. */
	{
		char *a1[] = {"test", "1", "-eq", "1", "-o", "1", "-eq", "2", "-a", "2", "-eq", "3", NULL};
		CHECK(run_test(&sh, 12, a1) == 0);
		char *a2[] = {"[", "(", "1", "-eq", "2", "-o", "2", "-eq", "2", ")", "-a", "3", "-gt", "1", "]", NULL};
		CHECK(run_test(&sh, 15, a2) == 0);
	}

	/* Error cases (status 2). */
	{
		char *a1[] = {"[", "x", NULL};
		CHECK(run_test(&sh, 2, a1) == 2);
		char *a2[] = {"test", "abc", "-eq", "1", NULL};
		CHECK(run_test(&sh, 4, a2) == 2);
	}

	tc_shell_finish(&sh);
	CHECK_DONE("test_test");
}
