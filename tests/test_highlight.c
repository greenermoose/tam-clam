/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for 0.2.4 syntax highlighting and dimmed history suggestions. */
#include <string.h>

#include "check.h"
#include "tam-clam/highlight.h"
#include "tam-clam/history.h"
#include "tam-clam/shell.h"

static void test_highlighting(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	struct tc_hl_cache cache;
	tc_hl_cache_clear(&cache);

	unsigned char st[256];

	/* Known builtin + string + variable + operator + unknown command + comment. */
	const char *s1 = "echo 'hi' $HOME | nosuchcmd_xyz # note";
	tc_highlight_line(&sh, &cache, s1, strlen(s1), st);
	CHECK(st[0] == TC_HL_CMD_OK);     /* echo */
	CHECK(st[5] == TC_HL_STRING);     /* 'hi' */
	CHECK(st[10] == TC_HL_VAR);       /* $HOME */
	CHECK(st[16] == TC_HL_OP);        /* | */
	CHECK(st[18] == TC_HL_CMD_BAD);   /* nosuchcmd_xyz */
	CHECK(st[32] == TC_HL_COMMENT);   /* # note */
	CHECK(cache.count == 2);          /* echo + nosuchcmd_xyz cached */

	/* Re-highlighting uses cache without adding duplicates. */
	tc_highlight_line(&sh, &cache, s1, strlen(s1), st);
	CHECK(cache.count == 2);

	/* Reserved words. */
	const char *s2 = "if true; then echo ok; fi";
	tc_highlight_line(&sh, &cache, s2, strlen(s2), st);
	CHECK(st[0] == TC_HL_KEYWORD);    /* if */
	CHECK(st[3] == TC_HL_CMD_OK);     /* true */
	CHECK(st[9] == TC_HL_KEYWORD);    /* then */
	CHECK(st[14] == TC_HL_CMD_OK);    /* echo */

	/* Unterminated quote highlighted as TC_HL_ERROR. */
	const char *s3 = "echo \"unclosed";
	tc_highlight_line(&sh, &cache, s3, strlen(s3), st);
	CHECK(st[0] == TC_HL_CMD_OK);
	CHECK(st[5] == TC_HL_ERROR);
	CHECK(st[10] == TC_HL_ERROR);

	/* Real parser catches unexpected token syntax error before Enter. */
	const char *s4 = "echo )";
	tc_highlight_line(&sh, &cache, s4, strlen(s4), st);
	CHECK(st[0] == TC_HL_CMD_OK);
	CHECK(st[5] == TC_HL_ERROR);

	/* Incomplete compound command (EOF only) is NOT flagged as error. */
	const char *s5 = "if true; then";
	tc_highlight_line(&sh, &cache, s5, strlen(s5), st);
	CHECK(st[0] == TC_HL_KEYWORD);
	CHECK(st[9] == TC_HL_KEYWORD);

	tc_shell_finish(&sh);
}

static void test_history_suggestions(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	struct tc_history h;
	tc_hist_init(&h, NULL, true);
	sh.hist = &h;

	CHECK(tc_hist_add(&h, "git status", 10, 0, "/") == 0);
	CHECK(tc_hist_add(&h, "git diff --stat", 15, 0, "/") == 0);

	/* Most recent matching prefix wins. */
	const char *g1 = tc_suggest_history(&sh, "git ", 4);
	CHECK(g1 != NULL && strcmp(g1, "diff --stat") == 0);

	const char *g2 = tc_suggest_history(&sh, "git s", 5);
	CHECK(g2 != NULL && strcmp(g2, "tatus") == 0);

	/* Exact match has no suffix -> NULL. */
	CHECK(tc_suggest_history(&sh, "git status", 10) == NULL);
	/* No match -> NULL. */
	CHECK(tc_suggest_history(&sh, "nosuch", 6) == NULL);

	tc_hist_finish(&h);
	sh.hist = NULL;
	tc_shell_finish(&sh);
}

int main(void)
{
	test_highlighting();
	test_history_suggestions();
	CHECK_DONE("test_highlight");
}
