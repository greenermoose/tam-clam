/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for brace expansion (src/brace.c). */
#include <string.h>

#include "check.h"
#include "tam-clam/expand.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"
#include "tam-clam/shell.h"

static int expand_line(struct tc_shell *sh, const char *src,
		       char ***out_argv, size_t *out_argc)
{
	struct tc_input in;
	tc_input_from_string(&in, src, strlen(src));
	struct tc_parser p;
	if (tc_parser_init(&p, sh, &in) != TC_OK)
		return -1;
	struct tc_node *n = NULL;
	int r = tc_parse_next(&p, &n);
	tc_parser_finish(&p);
	tc_input_finish(&in);
	if (r != TC_OK || !n || n->kind != TC_NODE_SIMPLE)
		return -1;
	return tc_expand_words(sh, n->u.simple.words, n->u.simple.nwords,
			       out_argv, out_argc);
}

static void test_comma_lists(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	char **argv = NULL;
	size_t argc = 0;

	CHECK(expand_line(&sh, "pre{a,b,c}post", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 3);
	CHECK(strcmp(argv[0], "preapost") == 0);
	CHECK(strcmp(argv[1], "prebpost") == 0);
	CHECK(strcmp(argv[2], "precpost") == 0);

	/* Empty alternatives and Cartesian product. */
	tc_arena_reset(&sh.arena);
	CHECK(expand_line(&sh, "a{,b}{1,2}", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 4);
	CHECK(strcmp(argv[0], "a1") == 0);
	CHECK(strcmp(argv[1], "a2") == 0);
	CHECK(strcmp(argv[2], "ab1") == 0);
	CHECK(strcmp(argv[3], "ab2") == 0);

	/* Nested braces. */
	tc_arena_reset(&sh.arena);
	CHECK(expand_line(&sh, "{a,{b,c}}", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 3);
	CHECK(strcmp(argv[0], "a") == 0);
	CHECK(strcmp(argv[1], "b") == 0);
	CHECK(strcmp(argv[2], "c") == 0);

	tc_shell_finish(&sh);
}

static void test_sequences(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	char **argv = NULL;
	size_t argc = 0;

	CHECK(expand_line(&sh, "{1..5}", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 5);
	CHECK(strcmp(argv[0], "1") == 0);
	CHECK(strcmp(argv[4], "5") == 0);

	tc_arena_reset(&sh.arena);
	CHECK(expand_line(&sh, "{01..05..2}", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 3);
	CHECK(strcmp(argv[0], "01") == 0);
	CHECK(strcmp(argv[1], "03") == 0);
	CHECK(strcmp(argv[2], "05") == 0);

	tc_arena_reset(&sh.arena);
	CHECK(expand_line(&sh, "{5..1..2}", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 3);
	CHECK(strcmp(argv[0], "5") == 0);
	CHECK(strcmp(argv[1], "3") == 0);
	CHECK(strcmp(argv[2], "1") == 0);

	tc_arena_reset(&sh.arena);
	CHECK(expand_line(&sh, "{e..a..2}", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 3);
	CHECK(strcmp(argv[0], "e") == 0);
	CHECK(strcmp(argv[1], "c") == 0);
	CHECK(strcmp(argv[2], "a") == 0);

	tc_shell_finish(&sh);
}

static void test_quoting_and_limits(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	sh.silent_errors = true;
	char **argv = NULL;
	size_t argc = 0;

	/* Quoted braces and non-brace strings stay literal. */
	CHECK(expand_line(&sh, "\"{a,b}\" '{1..3}' {single} {1..5..x}", &argv, &argc) == TC_EXP_OK);
	CHECK(argc == 4);
	CHECK(strcmp(argv[0], "{a,b}") == 0);
	CHECK(strcmp(argv[1], "{1..3}") == 0);
	CHECK(strcmp(argv[2], "{single}") == 0);
	CHECK(strcmp(argv[3], "{1..5..x}") == 0);

	/* Excessively large sequence fails cleanly at TC_BRACE_WORDS_MAX. */
	tc_arena_reset(&sh.arena);
	CHECK(expand_line(&sh, "{1..10000}", &argv, &argc) == TC_EXP_NOMEM);

	tc_shell_finish(&sh);
}

int main(void)
{
	test_comma_lists();
	test_sequences();
	test_quoting_and_limits();
	CHECK_DONE("test_brace");
}
