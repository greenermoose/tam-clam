/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for 0.3.3 Oils-named strict modes and `set -o strict`. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/exec.h"
#include "tam-clam/options.h"
#include "tam-clam/shell.h"
#include "tam-clam/var.h"

static int run_code(struct tc_shell *sh, const char *code)
{
	bool ran = false;
	return tc_exec_string(sh, code, strlen(code), 1, &ran);
}

static void test_option_toggles(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test_strict");

	/* All strict options off by default. */
	CHECK(!sh.opt_strict_errexit);
	CHECK(!sh.opt_command_sub_errexit);
	CHECK(!sh.opt_simple_word_eval);
	CHECK(!sh.opt_strict_argv);
	bool valid = false;
	CHECK(!tc_opt_get_name(&sh, "strict", &valid) && valid);

	/* set -o strict enables all 7 options. */
	CHECK(run_code(&sh, "set -o strict") == 0);
	CHECK(sh.opt_errexit && sh.opt_nounset && sh.opt_pipefail);
	CHECK(sh.opt_strict_errexit && sh.opt_command_sub_errexit);
	CHECK(sh.opt_simple_word_eval && sh.opt_strict_argv);
	CHECK(tc_opt_get_name(&sh, "strict", &valid) && valid);
	CHECK(run_code(&sh, "[[ -o strict && -o strict_errexit && -o simple_word_eval ]]") == 0);

	/* set +o strict disables all 7 options. */
	CHECK(run_code(&sh, "set +o strict") == 0);
	CHECK(!sh.opt_errexit && !sh.opt_nounset && !sh.opt_pipefail);
	CHECK(!sh.opt_strict_errexit && !sh.opt_command_sub_errexit);
	CHECK(!sh.opt_simple_word_eval && !sh.opt_strict_argv);
	CHECK(!tc_opt_get_name(&sh, "strict", &valid) && valid);

	tc_shell_finish(&sh);
}

static void test_strict_errexit(void)
{
	/* Function in `if` condition is a runtime error under strict_errexit. */
	{
		struct tc_shell sh;
		tc_shell_init(&sh, "test_strict");
		sh.silent_errors = true;
		CHECK(run_code(&sh,
			"set -e -o strict_errexit\n"
			"f() { false; echo still; }\n"
			"if f; then echo yes; fi\n") != 0);
		CHECK(sh.exit_requested);
		tc_shell_finish(&sh);
	}

	/* Compound group in `&&` LHS is a runtime error under strict_errexit. */
	{
		struct tc_shell sh;
		tc_shell_init(&sh, "test_strict");
		sh.silent_errors = true;
		CHECK(run_code(&sh,
			"set -e -o strict_errexit\n"
			"{ true; } && echo ok\n") != 0);
		CHECK(sh.exit_requested);
		tc_shell_finish(&sh);
	}

	/* Simple commands and [[ ]] remain legal in conditions under strict_errexit. */
	{
		struct tc_shell sh;
		tc_shell_init(&sh, "test_strict");
		CHECK(run_code(&sh,
			"set -e -o strict_errexit\n"
			"x=ok\n"
			"if [[ $x == ok ]] && ! false; then res=good; fi\n") == 0);
		CHECK(!sh.exit_requested);
		CHECK(strcmp(tc_var_get(&sh.vars, "res"), "good") == 0);
		tc_shell_finish(&sh);
	}
}

static void test_command_sub_errexit(void)
{
	/* Failing $(...) fails the enclosing command. */
	{
		struct tc_shell sh;
		tc_shell_init(&sh, "test_strict");
		CHECK(run_code(&sh, "set -o command_sub_errexit; echo $(exit 7)") == 7);
		CHECK(run_code(&sh, "set -o command_sub_errexit; f() { local x=$(exit 5); }; f") == 5);
		tc_shell_finish(&sh);
	}

	/* Combined with set -e, failing $(...) exits non-interactive shell. */
	{
		struct tc_shell sh;
		tc_shell_init(&sh, "test_strict");
		CHECK(run_code(&sh, "set -e -o command_sub_errexit; echo $(false); reached=yes") != 0);
		CHECK(sh.exit_requested);
		CHECK(tc_var_get(&sh.vars, "reached") == NULL);
		tc_shell_finish(&sh);
	}
}

static void test_simple_word_eval(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test_strict");

	CHECK(run_code(&sh,
		"set -o simple_word_eval\n"
		"x='a b c'\n"
		"e=''\n"
		"g='*'\n"
		"set -- $x $e $g\n"
		"cnt=$#\n"
		"p1=$1; p2=$2; p3=$3\n") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "cnt"), "3") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "p1"), "a b c") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "p2"), "") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "p3"), "*") == 0);

	tc_shell_finish(&sh);
}

static void test_strict_argv(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test_strict");
	sh.silent_errors = true;

	/* Empty command word fails under strict_argv. */
	CHECK(run_code(&sh, "set -o strict_argv; empty=''; $empty") != 0);
	CHECK(run_code(&sh, "set -o strict_argv; \"\"") != 0);

	/* Assignment-only command remains legal. */
	CHECK(run_code(&sh, "set -o strict_argv; a=42") == 0);
	CHECK(strcmp(tc_var_get(&sh.vars, "a"), "42") == 0);

	tc_shell_finish(&sh);
}

int main(void)
{
	test_option_toggles();
	test_strict_errexit();
	test_command_sub_errexit();
	test_simple_word_eval();
	test_strict_argv();
	CHECK_DONE("test_strict");
}
