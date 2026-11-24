/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for [[ ]], local, and full declare (0.3.1). */
#include <string.h>

#include "check.h"
#include "tam-clam/exec.h"
#include "tam-clam/shell.h"
#include "tam-clam/var.h"

static int run_code(struct tc_shell *sh, const char *code)
{
	bool ran = false;
	return tc_exec_string(sh, code, strlen(code), 1, &ran);
}

static void test_cond_ops(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	sh.silent_errors = true;

	CHECK(run_code(&sh, "x='hello world'; [[ $x == hello* ]]") == 0);
	CHECK(run_code(&sh, "x='hello world'; [[ $x == 'hello*' ]]") == 1);
	CHECK(run_code(&sh, "[[ abc < abd && abd > abc ]]") == 0);
	CHECK(run_code(&sh, "[[ ! (1 -eq 2 || 3 -lt 2) ]]") == 0);
	CHECK(run_code(&sh, "a=10; b=20; [[ a+5 -eq 15 && b -gt a ]]") == 0);
	CHECK(run_code(&sh, "unset u; [[ -v a && ! -v u ]]") == 0);
	CHECK(run_code(&sh, "set -f; [[ -o noglob && ! -o errexit ]]") == 0);

	/* Regex + BASH_REMATCH. */
	CHECK(run_code(&sh, "[[ 2026-11-24 =~ ^([0-9]{4})-([0-9]{2})-([0-9]{2})$ ]]") == 0);
	const struct tc_var_entry *bm = tc_var_find(&sh.vars, "BASH_REMATCH");
	CHECK(bm != NULL && (bm->flags & TC_VAR_ARRAY));
	const char *v = NULL;
	CHECK(tc_array_get(bm->arr, 0, &v) == TC_ARR_OK && v && strcmp(v, "2026-11-24") == 0);
	CHECK(tc_array_get(bm->arr, 1, &v) == TC_ARR_OK && v && strcmp(v, "2026") == 0);
	CHECK(tc_array_get(bm->arr, 2, &v) == TC_ARR_OK && v && strcmp(v, "11") == 0);
	CHECK(tc_array_get(bm->arr, 3, &v) == TC_ARR_OK && v && strcmp(v, "24") == 0);

	/* Invalid regex returns status 2. */
	CHECK(run_code(&sh, "[[ abc =~ [unclosed ]]") == 2);

	tc_shell_finish(&sh);
}

static void test_local_and_declare(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	sh.silent_errors = true;

	/* local outside function fails with status 1. */
	CHECK(run_code(&sh, "local x=1") == 1);

	/* Dynamic local scoping + declare -g. */
	CHECK(run_code(&sh,
		"x=outer; g=orig\n"
		"f() { local x=inner; declare y=localonly; declare -g g=changed; inner_seen=\"$x:$y\"; }\n"
		"f\n") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "x"), "outer") == 0);
	CHECK(tc_var_get(&sh.vars, "y") == NULL);
	CHECK(strcmp(tc_var_get(&sh.vars, "g"), "changed") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "inner_seen"), "inner:localonly") == 0);

	/* declare -i, -l, -u. */
	CHECK(run_code(&sh,
		"declare -i n=2+3; n+=4*2\n"
		"declare -l lo=HeLLo\n"
		"declare -u up=HeLLo\n") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "n"), "13") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "lo"), "hello") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "up"), "HELLO") == 0);

	tc_shell_finish(&sh);
}

int main(void)
{
	test_cond_ops();
	test_local_and_declare();
	CHECK_DONE("test_cond");
}
