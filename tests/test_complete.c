/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for 0.2.3 completion (src/complete/complete.c):
 * commands, files/dirs, variables ($VAR/${VAR}), ~users, and lazy .comp files. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/complete.h"
#include "tam-clam/shell.h"

static bool has_item(const struct tc_comp_list *cl, const char *text)
{
	for (size_t i = 0; i < cl->count; i++) {
		if (strcmp(cl->items[i].text, text) == 0)
			return true;
	}
	return false;
}

static void test_commands_vars_users(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	CHECK(tc_var_set(&sh.vars, "TAM_COMP_ALPHA", "1", 0) == TC_VAR_OK);
	CHECK(tc_var_set(&sh.vars, "TAM_COMP_BETA", "2", 0) == TC_VAR_OK);
	CHECK(tc_alias_set(&sh.aliases, "mycompalias", "echo hi") == 0);

	struct tc_comp_list cl;

	/* Command completion finds builtins and aliases. */
	CHECK(tc_complete(&sh, "pri", 3, 3, &cl) == 0);
	CHECK(has_item(&cl, "printf"));
	tc_comp_list_finish(&cl);

	CHECK(tc_complete(&sh, "mycomp", 6, 6, &cl) == 0);
	CHECK(cl.count == 1 && strcmp(cl.items[0].text, "mycompalias") == 0);
	tc_comp_list_finish(&cl);

	/* Variable completion: $VAR and ${VAR. */
	CHECK(tc_complete(&sh, "echo $TAM_COMP_", 15, 15, &cl) == 0);
	CHECK(cl.count == 2);
	CHECK(has_item(&cl, "TAM_COMP_ALPHA"));
	CHECK(has_item(&cl, "TAM_COMP_BETA"));
	CHECK(tc_comp_common_prefix(&cl) == 9); /* "TAM_COMP_" */
	tc_comp_list_finish(&cl);

	CHECK(tc_complete(&sh, "echo ${TAM_COMP_A", 17, 17, &cl) == 0);
	CHECK(cl.count == 1 && strcmp(cl.items[0].text, "TAM_COMP_ALPHA}") == 0);
	tc_comp_list_finish(&cl);

	/* User-name completion (~ro -> ~root/). */
	CHECK(tc_complete(&sh, "ls ~ro", 6, 6, &cl) == 0);
	CHECK(has_item(&cl, "~root/"));
	tc_comp_list_finish(&cl);

	tc_shell_finish(&sh);
}

static void test_files_and_declarative_comp(void)
{
	char dir[128];
	snprintf(dir, sizeof(dir), "/tmp/tam-clam-comp-%ld", (long)getpid());
	(void)mkdir(dir, 0700);

	char sub[160], f1[160], f2[160], fdot[160], fsp[160], cfile[160];
	snprintf(sub, sizeof(sub), "%s/adir", dir);
	snprintf(f1, sizeof(f1), "%s/alpha.txt", dir);
	snprintf(f2, sizeof(f2), "%s/alps.txt", dir);
	snprintf(fdot, sizeof(fdot), "%s/.hidden", dir);
	snprintf(fsp, sizeof(fsp), "%s/my file.txt", dir);
	snprintf(cfile, sizeof(cfile), "%s/mytool.comp", dir);
	(void)mkdir(sub, 0700);
	close(open(f1, O_CREAT | O_WRONLY, 0600));
	close(open(f2, O_CREAT | O_WRONLY, 0600));
	close(open(fdot, O_CREAT | O_WRONLY, 0600));
	close(open(fsp, O_CREAT | O_WRONLY, 0600));

	FILE *cf = fopen(cfile, "w");
	CHECK(cf != NULL);
	if (cf) {
		fputs("# sample completion\n"
		      "opt --verbose  Verbose output\n"
		      "opt --version  Show version\n"
		      "sub build      Build project\n"
		      "sub bench      Run benchmarks\n", cf);
		fclose(cf);
	}

	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	CHECK(tc_var_set(&sh.vars, "TAM_CLAM_COMPLETIONS_DIR", dir, 0) == TC_VAR_OK);

	struct tc_comp_list cl;
	char line[256];

	/* File completion: hides dotfiles, appends '/' to dirs, escapes spaces. */
	snprintf(line, sizeof(line), "ls %s/a", dir);
	CHECK(tc_complete(&sh, line, strlen(line), strlen(line), &cl) == 0);
	CHECK(cl.count == 3);
	char expect_dir[180];
	snprintf(expect_dir, sizeof(expect_dir), "%s/adir/", dir);
	CHECK(has_item(&cl, expect_dir));
	tc_comp_list_finish(&cl);

	snprintf(line, sizeof(line), "ls %s/my", dir);
	CHECK(tc_complete(&sh, line, strlen(line), strlen(line), &cl) == 0);
	char expect_sp[180];
	snprintf(expect_sp, sizeof(expect_sp), "%s/my\\ file.txt", dir);
	CHECK(has_item(&cl, expect_sp));
	tc_comp_list_finish(&cl);

	/* Dotfile appears only when '.' prefix is typed. */
	snprintf(line, sizeof(line), "ls %s/.", dir);
	CHECK(tc_complete(&sh, line, strlen(line), strlen(line), &cl) == 0);
	CHECK(cl.count == 1);
	tc_comp_list_finish(&cl);

	/* Declarative .comp: options and subcommands. */
	CHECK(tc_complete(&sh, "mytool --v", 10, 10, &cl) == 0);
	CHECK(cl.count == 2);
	CHECK(has_item(&cl, "--verbose"));
	CHECK(has_item(&cl, "--version"));
	CHECK(cl.items[0].desc != NULL);
	tc_comp_list_finish(&cl);

	CHECK(tc_complete(&sh, "mytool b", 8, 8, &cl) == 0);
	CHECK(has_item(&cl, "build"));
	CHECK(has_item(&cl, "bench"));
	tc_comp_list_finish(&cl);

	tc_shell_finish(&sh);
	unlink(f1);
	unlink(f2);
	unlink(fdot);
	unlink(fsp);
	unlink(cfile);
	rmdir(sub);
	rmdir(dir);
}

int main(void)
{
	test_commands_vars_users();
	test_files_and_declarative_comp();
	CHECK_DONE("test_complete");
}
