/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for fish-style abbreviations (src/abbr.c) and man-page
 * completion generation (tools/tam-clam-gen-comp -> src/complete/complete.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/abbr.h"
#include "tam-clam/complete.h"
#include "tam-clam/edit.h"
#include "tam-clam/exec.h"
#include "tam-clam/shell.h"
#include "tam-clam/var.h"

static int run_code(struct tc_shell *sh, const char *code)
{
	bool ran = false;
	return tc_exec_string(sh, code, strlen(code), 1, &ran);
}

static void test_abbr_table_and_builtin(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test_abbr");
	sh.silent_errors = true;

	CHECK(run_code(&sh, "abbr -a gco git checkout") == 0);
	CHECK(run_code(&sh, "abbr -a -p anywhere L '| less'") == 0);
	CHECK(run_code(&sh, "abbr -q gco") == 0);
	CHECK(run_code(&sh, "abbr -q L") == 0);
	CHECK(run_code(&sh, "abbr -q nosuch") == 1);

	const struct tc_abbr_entry *e = tc_abbr_get(&sh.abbrs, "gco");
	CHECK(e != NULL && strcmp(e->expansion, "git checkout") == 0 &&
	      e->pos == TC_ABBR_COMMAND);
	e = tc_abbr_get(&sh.abbrs, "L");
	CHECK(e != NULL && strcmp(e->expansion, "| less") == 0 &&
	      e->pos == TC_ABBR_ANYWHERE);

	/* Shorthand NAME=EXPANSION */
	CHECK(run_code(&sh, "abbr gs='git status'") == 0);
	e = tc_abbr_get(&sh.abbrs, "gs");
	CHECK(e != NULL && strcmp(e->expansion, "git status") == 0);

	/* Erase */
	CHECK(run_code(&sh, "abbr -e gs") == 0);
	CHECK(run_code(&sh, "abbr -q gs") == 1);
	CHECK(run_code(&sh, "abbr -e gs") == 1);

	tc_shell_finish(&sh);
}

static void test_abbr_expansion(void)
{
	struct tc_abbrs ab;
	tc_abbr_init(&ab);
	CHECK(tc_abbr_set(&ab, "gco", "git checkout", TC_ABBR_COMMAND) == 0);
	CHECK(tc_abbr_set(&ab, "L", "| less", TC_ABBR_ANYWHERE) == 0);

	struct tc_edit_buf buf;
	tc_ebuf_init(&buf);

	/* Command position expands */
	CHECK(tc_ebuf_set(&buf, "gco", 3) == 0);
	CHECK(tc_abbr_try_expand(&ab, &buf) == true);
	CHECK(strcmp(buf.data, "git checkout") == 0);

	/* Undo reverts expansion cleanly */
	CHECK(tc_ebuf_undo(&buf) == 1);
	CHECK(strcmp(buf.data, "gco") == 0);

	/* Non-command position does NOT expand command-only abbr */
	CHECK(tc_ebuf_set(&buf, "echo gco", 8) == 0);
	CHECK(tc_abbr_try_expand(&ab, &buf) == false);
	CHECK(strcmp(buf.data, "echo gco") == 0);

	/* After pipe or && is command position */
	CHECK(tc_ebuf_set(&buf, "true && gco", 11) == 0);
	CHECK(tc_abbr_try_expand(&ab, &buf) == true);
	CHECK(strcmp(buf.data, "true && git checkout") == 0);

	/* Anywhere abbr expands after arguments */
	CHECK(tc_ebuf_set(&buf, "ls -la L", 8) == 0);
	CHECK(tc_abbr_try_expand(&ab, &buf) == true);
	CHECK(strcmp(buf.data, "ls -la | less") == 0);

	/* Inside quotes does NOT expand */
	CHECK(tc_ebuf_set(&buf, "echo \"L", 7) == 0);
	CHECK(tc_abbr_try_expand(&ab, &buf) == false);
	CHECK(tc_ebuf_set(&buf, "'gco", 4) == 0);
	CHECK(tc_abbr_try_expand(&ab, &buf) == false);

	tc_ebuf_finish(&buf);
	tc_abbr_finish(&ab);
}

static void test_manpage_compgen(void)
{
	char tmpl[] = "/tmp/tc-compgen-XXXXXX";
	char *dir = mkdtemp(tmpl);
	CHECK(dir != NULL);
	if (!dir)
		return;

	char man_path[256];
	snprintf(man_path, sizeof(man_path), "%s/sample.1", dir);
	FILE *f = fopen(man_path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("SAMPLE(1)\n\n"
		      "NAME\n"
		      "       sample - demo command\n\n"
		      "OPTIONS\n"
		      "       -v, --verbose\n"
		      "              Explain what is being done\n\n"
		      "       -o FILE, --output=FILE  Write result to FILE\n\n"
		      "       --dry-run\n"
		      "              Do not modify anything\n",
		      f);
		fclose(f);
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "sh tools/tam-clam-gen-comp -o '%s' -f '%s' sample", dir, man_path);
	int rc = system(cmd);
	CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);

	/* Load the generated sample.comp directly through tc_complete! */
	struct tc_shell sh;
	tc_shell_init(&sh, "test_abbr");
	CHECK(tc_var_set(&sh.vars, "TAM_CLAM_COMPLETIONS_DIR", dir, 0) == TC_VAR_OK);

	struct tc_comp_list cl;
	tc_comp_list_init(&cl);
	CHECK(tc_complete(&sh, "sample --", 9, 9, &cl) == 0);
	CHECK(cl.count == 3);
	bool saw_dry = false, saw_out = false, saw_verb = false;
	for (size_t i = 0; i < cl.count; i++) {
		if (strcmp(cl.items[i].text, "--dry-run") == 0)
			saw_dry = true;
		if (strcmp(cl.items[i].text, "--output") == 0)
			saw_out = true;
		if (strcmp(cl.items[i].text, "--verbose") == 0)
			saw_verb = true;
	}
	CHECK(saw_dry && saw_out && saw_verb);
	tc_comp_list_finish(&cl);
	tc_shell_finish(&sh);

	char comp_path[256];
	snprintf(comp_path, sizeof(comp_path), "%s/sample.comp", dir);
	unlink(comp_path);
	unlink(man_path);
	rmdir(dir);
}

int main(void)
{
	test_abbr_table_and_builtin();
	test_abbr_expansion();
	test_manpage_compgen();
	CHECK_DONE("test_abbr");
}
