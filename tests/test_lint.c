/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for column-precise locations and static lint mode (src/lint.c). */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/input.h"
#include "tam-clam/lex.h"
#include "tam-clam/lint.h"
#include "tam-clam/shell.h"

static void test_columns(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");

	const char *src = "  echo hello\n\t[ $x = a ]\n";
	struct tc_input in;
	tc_input_from_string(&in, src, strlen(src));
	struct tc_lexer lx;
	CHECK(tc_lexer_init(&lx, &sh, &in) == TC_OK);

	struct tc_token tok;
	CHECK(tc_lex_next(&lx, &tok) == TC_OK);
	CHECK(tok.kind == TC_TOK_WORD && tok.line == 1 && tok.col == 3);
	CHECK(tc_lex_next(&lx, &tok) == TC_OK);
	CHECK(tok.kind == TC_TOK_WORD && tok.line == 1 && tok.col == 8);
	CHECK(tc_lex_next(&lx, &tok) == TC_OK);
	CHECK(tok.kind == TC_TOK_NEWLINE && tok.line == 1);
	CHECK(tc_lex_next(&lx, &tok) == TC_OK);
	CHECK(tok.kind == TC_TOK_WORD && tok.line == 2 && tok.col == 2); /* '[' */
	CHECK(tc_lex_next(&lx, &tok) == TC_OK);
	CHECK(tok.kind == TC_TOK_WORD && tok.line == 2 && tok.col == 4); /* '$x' */

	tc_lexer_finish(&lx);
	tc_input_finish(&in);
	tc_shell_finish(&sh);
}

static int run_lint_capture(const char *src, char *buf, size_t cap,
			    size_t *out_warnings)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	int pfd[2];
	if (pipe(pfd) < 0) {
		tc_shell_finish(&sh);
		return -1;
	}
	int saved_err = dup(STDERR_FILENO);
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, STDERR_FILENO);
		close(devnull);
	}

	int rc = tc_lint_string(&sh, "t.sh", src, strlen(src), pfd[1], out_warnings);
	close(pfd[1]);
	if (saved_err >= 0) {
		dup2(saved_err, STDERR_FILENO);
		close(saved_err);
	}

	size_t off = 0;
	for (;;) {
		ssize_t n = read(pfd[0], buf + off, cap - 1 - off);
		if (n <= 0)
			break;
		off += (size_t)n;
		if (off + 1 >= cap)
			break;
	}
	buf[off] = '\0';
	close(pfd[0]);
	tc_shell_finish(&sh);
	return rc;
}

static void test_lint_checks(void)
{
	char out[4096];
	size_t w = 0;

	/* Clean script -> status 0, 0 warnings */
	CHECK(run_lint_capture(
		"x=hello\n"
		"if [ \"$x\" = hello ]; then\n"
		"    cd /tmp || exit 1\n"
		"    printf '%s\\n' \"$@\"\n"
		"fi\n",
		out, sizeof(out), &w) == 0);
	CHECK(w == 0);

	/* unquoted-test and column accuracy */
	CHECK(run_lint_capture("x=1\n[ $x = 1 ]\n", out, sizeof(out), &w) == 1);
	CHECK(w == 1);
	CHECK(strstr(out, "t.sh:2:3: warning [unquoted-test]:") != NULL);

	/* unquoted-case */
	CHECK(run_lint_capture("x=1\ncase $x in\n1) : ;;\nesac\n",
			       out, sizeof(out), &w) == 1);
	CHECK(w == 1);
	CHECK(strstr(out, "t.sh:2:6: warning [unquoted-case]:") != NULL);

	/* unchecked-cd vs checked cd / set -e */
	CHECK(run_lint_capture("  cd /tmp\n", out, sizeof(out), &w) == 1);
	CHECK(w == 1);
	CHECK(strstr(out, "t.sh:1:3: warning [unchecked-cd]:") != NULL);

	CHECK(run_lint_capture("set -e\ncd /tmp\n", out, sizeof(out), &w) == 0);
	CHECK(run_lint_capture("cd /tmp || exit 1\n", out, sizeof(out), &w) == 0);
	CHECK(run_lint_capture("if cd /tmp; then :; fi\n", out, sizeof(out), &w) == 0);
	CHECK(run_lint_capture("cd /tmp\nif [ \"$?\" -ne 0 ]; then exit 1; fi\n",
			       out, sizeof(out), &w) == 0);

	/* unquoted-star */
	CHECK(run_lint_capture("echo $*\n", out, sizeof(out), &w) == 1);
	CHECK(w == 1);
	CHECK(strstr(out, "t.sh:1:6: warning [unquoted-star]:") != NULL);

	CHECK(run_lint_capture("for a in \"$*\"; do echo \"$a\"; done\n",
			       out, sizeof(out), &w) == 1);
	CHECK(w == 1);
	CHECK(strstr(out, "[unquoted-star]") != NULL);

	/* split-cmdsub */
	CHECK(run_lint_capture("for f in $(ls *.c); do echo \"$f\"; done\n",
			       out, sizeof(out), &w) == 1);
	CHECK(w == 1);
	CHECK(strstr(out, "t.sh:1:10: warning [split-cmdsub]:") != NULL);

	/* unset-var vs defined / default / env */
	CHECK(run_lint_capture("echo \"$NEVER_SET_VAR\"\n", out, sizeof(out), &w) == 1);
	CHECK(w == 1);
	CHECK(strstr(out, "t.sh:1:6: warning [unset-var]:") != NULL);

	CHECK(run_lint_capture(
		"echo \"${OK_DEFAULT:-fallback}\"\n"
		"echo \"$HOME\" \"$PATH\" \"$LINENO\"\n"
		"read -r line_val\n"
		"echo \"$line_val\"\n",
		out, sizeof(out), &w) == 0);

	/* Suppression: inline and preceding comment line */
	CHECK(run_lint_capture(
		"x=1\n"
		"[ $x = 1 ] # lint: allow unquoted-test\n"
		"# lint: allow unchecked-cd\n"
		"cd /tmp\n",
		out, sizeof(out), &w) == 0);
	CHECK(w == 0);

	/* Syntax error -> status 2 */
	CHECK(run_lint_capture("if true; then echo oops\n", out, sizeof(out), &w) == 2);
}

int main(void)
{
	test_columns();
	test_lint_checks();
	CHECK_DONE("test_lint");
}
