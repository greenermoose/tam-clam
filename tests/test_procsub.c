/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for process substitution <(...) and >(...) (0.3.2). */
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

int main(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test_procsub");

	/* Input process substitution <(...) */
	CHECK(run_code(&sh, "out=$(cat <(printf 'hello\\nworld\\n'))") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "out"), "hello\nworld") == 0);
	CHECK(sh.procsub_count == 0);

	/* Multiple simultaneous <(...) substitutions */
	CHECK(run_code(&sh, "out=$(paste -d: <(printf 'a\\nb\\n') <(printf '1\\n2\\n'))") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "out"), "a:1\nb:2") == 0);
	CHECK(sh.procsub_count == 0);

	/* Redirection from <(...) into while read loop */
	CHECK(run_code(&sh,
		"acc=''\n"
		"while IFS= read -r line; do acc=\"$acc[$line]\"; done < <(printf 'x\\ny\\n')\n") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "acc"), "[x][y]") == 0);
	CHECK(sh.procsub_count == 0);

	/* Output process substitution >(...) */
	CHECK(run_code(&sh,
		"tmp=/tmp/tc-procsub-test-$$\n"
		"printf 'one\\ntwo\\n' | tee >(tr a-z A-Z > \"$tmp\") >/dev/null\n"
		"res=$(cat \"$tmp\")\n"
		"rm -f \"$tmp\"\n") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "res"), "ONE\nTWO") == 0);
	CHECK(sh.procsub_count == 0);

	/* Quoted <(...) stays literal */
	CHECK(run_code(&sh, "lit=$(printf '%s' '<(echo hi)')") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "lit"), "<(echo hi)") == 0);

	/* shopt -s / -u / -q globstar */
	CHECK(sh.opt_globstar == false);
	CHECK(run_code(&sh, "shopt -q globstar") == 1);
	CHECK(run_code(&sh, "shopt -s globstar && shopt -q globstar") == 0);
	CHECK(sh.opt_globstar == true);
	CHECK(run_code(&sh, "shopt -u globstar && ! shopt -q globstar") == 0);
	CHECK(sh.opt_globstar == false);

	tc_shell_finish(&sh);
	CHECK_DONE("test_procsub");
}
