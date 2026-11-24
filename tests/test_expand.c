/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/expand.h"
#include "tam-clam/input.h"
#include "tam-clam/lex.h"

static int expand_to(struct tc_shell *sh, const char *src, const char *want)
{
	struct tc_input in;
	struct tc_lexer lx;
	tc_input_from_string(&in, src, strlen(src));
	if (tc_lexer_init(&lx, sh, &in) != TC_OK)
		return 0;
	struct tc_token tok;
	int r = tc_lex_next(&lx, &tok);
	tc_lexer_finish(&lx);
	if (r != TC_OK || tok.kind != TC_TOK_WORD)
		return 0;
	struct tc_word w = {tok.text, tok.len, tok.parts, tok.flags, tok.line, tok.col};
	char *got = NULL;
	int er = tc_expand_single(sh, &w, false, &got);
	return er == TC_EXP_OK && got && strcmp(got, want) == 0;
}

static int expand_fails(struct tc_shell *sh, const char *src)
{
	struct tc_input in;
	struct tc_lexer lx;
	tc_input_from_string(&in, src, strlen(src));
	if (tc_lexer_init(&lx, sh, &in) != TC_OK)
		return 0;
	struct tc_token tok;
	int r = tc_lex_next(&lx, &tok);
	tc_lexer_finish(&lx);
	if (r != TC_OK || tok.kind != TC_TOK_WORD)
		return 0;
	struct tc_word w = {tok.text, tok.len, tok.parts, tok.flags, tok.line, tok.col};
	int saved = dup(2), null = open("/dev/null", O_WRONLY);
	dup2(null, 2);
	char *got = NULL;
	int er = tc_expand_single(sh, &w, false, &got);
	dup2(saved, 2);
	close(saved);
	close(null);
	return er == TC_EXP_ERR;
}

int main(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "mysh");
	tc_var_set(&sh.vars, "HOME", "/home/tester", TC_VAR_EXPORT);
	tc_var_set(&sh.vars, "X", "hello", 0);
	tc_var_set(&sh.vars, "EMPTY", "", 0);
	tc_var_set(&sh.vars, "PATHLIKE", "foo/bar/baz.txt", 0);
	sh.status = 42;
	char *args[] = {"one", "two three", "four"};
	CHECK(tc_shell_set_params(&sh, 3, args) == TC_OK);

	/* Quote removal. */
	CHECK(expand_to(&sh, "plain", "plain"));
	CHECK(expand_to(&sh, "'a b'", "a b"));
	CHECK(expand_to(&sh, "'a\\b'", "a\\b"));
	CHECK(expand_to(&sh, "\"a b\"", "a b"));
	CHECK(expand_to(&sh, "\"a\\$b\"", "a$b"));
	CHECK(expand_to(&sh, "\"a\\\\b\"", "a\\b"));
	CHECK(expand_to(&sh, "\"a\\\"b\"", "a\"b"));
	CHECK(expand_to(&sh, "\"a\\xb\"", "a\\xb"));
	CHECK(expand_to(&sh, "a\\ b", "a b"));
	CHECK(expand_to(&sh, "\\\\", "\\"));
	CHECK(expand_to(&sh, "x'y'\"z\"w", "xyzw"));
	CHECK(expand_to(&sh, "''", ""));
	CHECK(expand_to(&sh, "\"'\"", "'"));
	CHECK(expand_to(&sh, "'\"'", "\""));

	/* Basic & braced parameter expansion. */
	CHECK(expand_to(&sh, "$X", "hello"));
	CHECK(expand_to(&sh, "${X}world", "helloworld"));
	CHECK(expand_to(&sh, "$UNSET", ""));
	CHECK(expand_to(&sh, "${#X}", "5"));
	CHECK(expand_to(&sh, "${#UNSET}", "0"));

	/* Default, assign, alt, error operators. */
	CHECK(expand_to(&sh, "${UNSET:-def}", "def"));
	CHECK(expand_to(&sh, "${EMPTY:-def}", "def"));
	CHECK(expand_to(&sh, "${EMPTY-def}", ""));
	CHECK(expand_to(&sh, "${X:-def}", "hello"));
	CHECK(expand_to(&sh, "${X:+alt}", "alt"));
	CHECK(expand_to(&sh, "${EMPTY:+alt}", ""));
	CHECK(expand_to(&sh, "${EMPTY+alt}", "alt"));
	CHECK(expand_to(&sh, "${NEWVAR:=assigned}", "assigned"));
	CHECK(strcmp(tc_var_get(&sh.vars, "NEWVAR"), "assigned") == 0);
	CHECK(expand_fails(&sh, "${UNSET:?missing}"));

	/* Prefix/suffix plain-string trimming. */
	CHECK(expand_to(&sh, "${PATHLIKE%.txt}", "foo/bar/baz"));
	CHECK(expand_to(&sh, "${PATHLIKE%%.txt}", "foo/bar/baz"));
	CHECK(expand_to(&sh, "${PATHLIKE#foo/}", "bar/baz.txt"));
	CHECK(expand_to(&sh, "${PATHLIKE##foo/}", "bar/baz.txt"));
	CHECK(expand_to(&sh, "${PATHLIKE#nope}", "foo/bar/baz.txt"));

	/* Special and positional parameters. */
	CHECK(expand_to(&sh, "$?", "42"));
	CHECK(expand_to(&sh, "$#", "3"));
	CHECK(expand_to(&sh, "${#}", "3"));
	CHECK(expand_to(&sh, "$0", "mysh"));
	CHECK(expand_to(&sh, "$1", "one"));
	CHECK(expand_to(&sh, "${2}", "two three"));
	CHECK(expand_to(&sh, "$4", ""));
	CHECK(expand_to(&sh, "$!", ""));
	CHECK(expand_to(&sh, "$-", ""));
	CHECK(expand_to(&sh, "\"$*\"", "one two three four"));

	/* Tilde expansion. */
	CHECK(expand_to(&sh, "~", "/home/tester"));
	CHECK(expand_to(&sh, "~/docs", "/home/tester/docs"));
	CHECK(expand_to(&sh, "'~'", "~"));

	/* Multi-word "$@" and empty unquoted elision. */
	{
		struct tc_input in;
		struct tc_lexer lx;
		const char *line = "$UNSET \"pre$@post\" \"\"";
		tc_input_from_string(&in, line, strlen(line));
		CHECK(tc_lexer_init(&lx, &sh, &in) == TC_OK);
		struct tc_word ws[3];
		for (int i = 0; i < 3; i++) {
			struct tc_token t;
			CHECK(tc_lex_next(&lx, &t) == TC_OK && t.kind == TC_TOK_WORD);
			ws[i] = (struct tc_word){t.text, t.len, t.parts, t.flags, t.line, t.col};
		}
		tc_lexer_finish(&lx);
		char **argv = NULL;
		size_t argc = 0;
		CHECK(tc_expand_words(&sh, ws, 3, &argv, &argc) == TC_EXP_OK);
		CHECK(argc == 4);
		CHECK(strcmp(argv[0], "preone") == 0);
		CHECK(strcmp(argv[1], "two three") == 0);
		CHECK(strcmp(argv[2], "fourpost") == 0);
		CHECK(strcmp(argv[3], "") == 0);
	}

	/* "$@" with 0 positional parameters produces 0 words. */
	CHECK(tc_shell_set_params(&sh, 0, NULL) == TC_OK);
	{
		struct tc_input in;
		struct tc_lexer lx;
		const char *line = "\"$@\"";
		tc_input_from_string(&in, line, strlen(line));
		CHECK(tc_lexer_init(&lx, &sh, &in) == TC_OK);
		struct tc_token t;
		CHECK(tc_lex_next(&lx, &t) == TC_OK && t.kind == TC_TOK_WORD);
		tc_lexer_finish(&lx);
		struct tc_word w = {t.text, t.len, t.parts, t.flags, t.line, t.col};
		char **argv = NULL;
		size_t argc = 0;
		CHECK(tc_expand_words(&sh, &w, 1, &argv, &argc) == TC_EXP_OK);
		CHECK(argc == 0);
	}

	/* The arena: a reset keeps one block; the cap is enforced. */
	tc_arena_reset(&sh.arena);
	CHECK(tc_arena_alloc(&sh.arena, 100) != NULL);
	struct tc_arena small;
	tc_arena_init(&small, 1000);
	CHECK(tc_arena_alloc(&small, 2000) == NULL);
	tc_arena_finish(&small);

	tc_shell_finish(&sh);
	CHECK_DONE("test_expand");
}
