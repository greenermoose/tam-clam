/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/glob.h"
#include "tam-clam/shell.h"

static void touch_file(const char *path)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd >= 0)
		close(fd);
}

int main(void)
{
	char tmpl[] = "/tmp/tc-glob-XXXXXX";
	char *dir = mkdtemp(tmpl);
	CHECK(dir != NULL);
	if (!dir)
		CHECK_DONE("test_glob");

	char old_cwd[4096];
	CHECK(getcwd(old_cwd, sizeof(old_cwd)) != NULL);
	CHECK(chdir(dir) == 0);

	touch_file("alpha.c");
	touch_file("beta.c");
	touch_file("gamma.txt");
	touch_file(".hidden");
	CHECK(mkdir("sub", 0700) == 0);
	touch_file("sub/one.txt");
	touch_file("sub/two.txt");

	struct tc_shell sh;
	tc_shell_init(&sh, "test_glob");

	char **out = NULL;
	size_t n = 0;

	/* *.c matches alpha.c and beta.c in sorted order */
	{
		struct tc_pat p = { .text = "*.c", .quoted = NULL, .len = 3 };
		CHECK(tc_glob_expand(&sh, &p, 1, &out, &n) == TC_OK);
		CHECK(n == 2 && strcmp(out[0], "alpha.c") == 0 &&
		      strcmp(out[1], "beta.c") == 0);
	}

	/* * skips .hidden */
	{
		struct tc_pat p = { .text = "*", .quoted = NULL, .len = 1 };
		CHECK(tc_glob_expand(&sh, &p, 1, &out, &n) == TC_OK);
		CHECK(n == 4 && strcmp(out[0], "alpha.c") == 0 &&
		      strcmp(out[1], "beta.c") == 0 &&
		      strcmp(out[2], "gamma.txt") == 0 &&
		      strcmp(out[3], "sub") == 0);
	}

	/* .* matches .hidden (and . and ..) */
	{
		struct tc_pat p = { .text = ".h*", .quoted = NULL, .len = 3 };
		CHECK(tc_glob_expand(&sh, &p, 1, &out, &n) == TC_OK);
		CHECK(n == 1 && strcmp(out[0], ".hidden") == 0);
	}

	/* Trailing slash matches directories only */
	{
		struct tc_pat p = { .text = "*/", .quoted = NULL, .len = 2 };
		CHECK(tc_glob_expand(&sh, &p, 1, &out, &n) == TC_OK);
		CHECK(n == 1 && strcmp(out[0], "sub/") == 0);
	}

	/* Subdirectory glob */
	{
		struct tc_pat p = { .text = "sub/*.txt", .quoted = NULL, .len = 9 };
		CHECK(tc_glob_expand(&sh, &p, 1, &out, &n) == TC_OK);
		CHECK(n == 2 && strcmp(out[0], "sub/one.txt") == 0 &&
		      strcmp(out[1], "sub/two.txt") == 0);
	}

	/* No match returns 0 */
	{
		struct tc_pat p = { .text = "*.nomatch", .quoted = NULL, .len = 9 };
		CHECK(tc_glob_expand(&sh, &p, 1, &out, &n) == TC_OK);
		CHECK(n == 0);
	}

	/* Quoted metacharacter does not expand */
	{
		uint8_t q[3] = {1, 0, 0};
		struct tc_pat p = { .text = "*.c", .quoted = q, .len = 3 };
		CHECK(tc_glob_expand(&sh, &p, 1, &out, &n) == TC_OK);
		CHECK(n == 0);
	}

	tc_shell_finish(&sh);

	unlink("sub/one.txt");
	unlink("sub/two.txt");
	rmdir("sub");
	unlink(".hidden");
	unlink("gamma.txt");
	unlink("beta.c");
	unlink("alpha.c");
	CHECK(chdir(old_cwd) == 0);
	rmdir(dir);

	CHECK_DONE("test_glob");
}
