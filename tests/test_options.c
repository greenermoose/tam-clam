/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for shell options (src/options.c) and PS4/LINENO helpers. */
#include <string.h>

#include "check.h"
#include "tam-clam/expand.h"
#include "tam-clam/lex.h"
#include "tam-clam/options.h"
#include "tam-clam/shell.h"

static void test_char_and_name_options(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");

	CHECK(tc_opt_set_char(&sh, 'e', true) == TC_OPT_OK);
	CHECK(sh.opt_errexit);
	CHECK(tc_opt_set_char(&sh, 'u', true) == TC_OPT_OK);
	CHECK(sh.opt_nounset);
	CHECK(tc_opt_set_char(&sh, 'x', true) == TC_OPT_OK);
	CHECK(sh.opt_xtrace);
	CHECK(tc_opt_set_char(&sh, 'C', true) == TC_OPT_OK);
	CHECK(sh.opt_noclobber);
	CHECK(tc_opt_set_char(&sh, 'n', true) == TC_OPT_OK);
	CHECK(sh.opt_noexec);
	CHECK(tc_opt_set_char(&sh, 'a', true) == TC_OPT_OK);
	CHECK(sh.opt_allexport);
	CHECK(tc_opt_set_char(&sh, 'f', true) == TC_OPT_OK);
	CHECK(sh.opt_noglob);

	CHECK(tc_opt_set_name(&sh, "pipefail", true) == TC_OPT_OK);
	CHECK(sh.opt_pipefail);

	char flags[32];
	size_t fn = tc_opt_format_flags(&sh, flags, sizeof(flags));
	CHECK(fn == strlen(flags));
	CHECK(strcmp(flags, "aCefnux") == 0);

	CHECK(tc_opt_set_name(&sh, "errexit", false) == TC_OPT_OK);
	CHECK(!sh.opt_errexit);
	CHECK(tc_opt_set_name(&sh, "nounset", false) == TC_OPT_OK);
	CHECK(!sh.opt_nounset);
	CHECK(tc_opt_set_name(&sh, "pipefail", false) == TC_OPT_OK);
	CHECK(!sh.opt_pipefail);
	CHECK(tc_opt_set_name(&sh, "xtrace", false) == TC_OPT_OK);
	CHECK(!sh.opt_xtrace);
	CHECK(tc_opt_set_name(&sh, "noclobber", false) == TC_OPT_OK);
	CHECK(!sh.opt_noclobber);
	CHECK(tc_opt_set_name(&sh, "noexec", false) == TC_OPT_OK);
	CHECK(!sh.opt_noexec);

	tc_opt_format_flags(&sh, flags, sizeof(flags));
	CHECK(strcmp(flags, "af") == 0);

	CHECK(tc_opt_set_char(&sh, 'v', true) == TC_OPT_DEFERRED);
	CHECK(tc_opt_set_char(&sh, 'm', true) == TC_OPT_DEFERRED);
	CHECK(tc_opt_set_char(&sh, 'Z', true) == TC_OPT_INVALID);

	CHECK(tc_opt_set_name(&sh, "verbose", true) == TC_OPT_DEFERRED);
	CHECK(tc_opt_set_name(&sh, "vi", true) == TC_OPT_DEFERRED);
	CHECK(tc_opt_set_name(&sh, "bogusopt", true) == TC_OPT_INVALID);

	tc_shell_finish(&sh);
}

static void test_ps4_and_lineno_expansion(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");

	const char *def_ps4 = tc_var_get(&sh.vars, "PS4");
	CHECK(def_ps4 != NULL);
	CHECK(strcmp(def_ps4, "+ ") == 0);

	sh.lineno = 42;
	const char *fmt = "+line $LINENO: ";
	struct tc_wpart *parts = NULL;
	CHECK(tc_lex_expand_parts(&sh, fmt, strlen(fmt), 42, &parts) == TC_OK);
	char *exp = NULL;
	size_t elen = 0;
	CHECK(tc_expand_heredoc(&sh, parts, 42, &exp, &elen) == TC_EXP_OK);
	CHECK(strcmp(exp, "+line 42: ") == 0);

	tc_shell_finish(&sh);
}

int main(void)
{
	test_char_and_name_options();
	test_ps4_and_lineno_expansion();
	CHECK_DONE("test_options");
}
