/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for 0.2.5 prompt escapes and safety (src/prompt.c). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "tam-clam/prompt.h"
#include "tam-clam/shell.h"

static void test_prompt_escapes(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	CHECK(tc_var_set(&sh.vars, "USER", "alice", 0) == TC_VAR_OK);
	CHECK(tc_var_set(&sh.vars, "HOME", "/home/alice", 0) == TC_VAR_OK);
	CHECK(tc_var_set(&sh.vars, "PWD", "/home/alice/proj", 0) == TC_VAR_OK);
	sh.status = 42;

	char *s = tc_prompt_expand(&sh, "\\u:\\w:\\W:\\?:\\j:\\s:\\$\\[\\e[0m\\]\\\\");
	CHECK(s != NULL);
	CHECK(strstr(s, "alice:~/proj:proj:42:0:tam-clam:") != NULL);
	CHECK(strstr(s, "\x1b[0m\\") != NULL);
	free(s);

	/* HOME itself abbreviates to ~ in both \w and \W. */
	CHECK(tc_var_set(&sh.vars, "PWD", "/home/alice", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\w|\\W");
	CHECK(s != NULL && strcmp(s, "~|~") == 0);
	free(s);

	/* Root directory / stays /. */
	CHECK(tc_var_set(&sh.vars, "PWD", "/", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\w|\\W|\\2");
	CHECK(s != NULL && strcmp(s, "/|/|/") == 0);
	free(s);

	/* \2 shows deepest two levels with "... " when truncated. */
	CHECK(tc_var_set(&sh.vars, "PWD", "/home/alice", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\2");
	CHECK(s != NULL && strcmp(s, "~") == 0);
	free(s);

	CHECK(tc_var_set(&sh.vars, "PWD", "/home/alice/proj", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\2");
	CHECK(s != NULL && strcmp(s, "~/proj") == 0);
	free(s);

	CHECK(tc_var_set(&sh.vars, "PWD", "/home/alice/Code/tamlinux", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\2 \\$ ");
	CHECK(s != NULL && strcmp(s, "... Code/tamlinux $ ") == 0);
	free(s);

	CHECK(tc_var_set(&sh.vars, "PWD", "/usr/local/bin", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\2");
	CHECK(s != NULL && strcmp(s, "... local/bin") == 0);
	free(s);

	CHECK(tc_var_set(&sh.vars, "PWD", "/usr/local", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\2");
	CHECK(s != NULL && strcmp(s, "/usr/local") == 0);
	free(s);

	CHECK(tc_var_set(&sh.vars, "PWD", "/tmp", 0) == TC_VAR_OK);
	s = tc_prompt_expand(&sh, "\\2");
	CHECK(s != NULL && strcmp(s, "/tmp") == 0);
	free(s);

	/* Default PS1 uses \2 \$ . */
	CHECK(tc_var_set(&sh.vars, "PWD", "/home/alice/Code/tamlinux", 0) == TC_VAR_OK);
	const char *def_ps1 = tc_var_get(&sh.vars, "PS1");
	s = tc_prompt_expand(&sh, def_ps1);
	CHECK(s != NULL && strcmp(s, "... Code/tamlinux $ ") == 0);
	free(s);

	/* Time escapes produce valid HH:MM:SS and HH:MM without timers. */
	s = tc_prompt_expand(&sh, "\\t|\\A");
	CHECK(s != NULL && strlen(s) == 14);
	CHECK(isdigit((unsigned char)s[0]) && s[2] == ':' && s[5] == ':' && s[8] == '|');
	free(s);

	/* Security: prompts are NEVER parsed or executed as shell code. */
	s = tc_prompt_expand(&sh, "$(echo pwned) $USER `id`");
	CHECK(s != NULL && strcmp(s, "$(echo pwned) $USER `id`") == 0);
	free(s);

	tc_shell_finish(&sh);
}

int main(void)
{
	test_prompt_escapes();
	CHECK_DONE("test_prompt");
}
