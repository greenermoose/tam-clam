/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"

struct fixture {
	struct tc_shell sh;
	struct tc_input in;
	struct tc_parser p;
	int saved_stderr;
};

static void start(struct fixture *f, const char *src)
{
	tc_shell_init(&f->sh, "test");
	tc_input_from_string(&f->in, src, strlen(src));
	tc_parser_init(&f->p, &f->sh, &f->in);
	/* Expected error messages go to /dev/null. */
	f->saved_stderr = dup(2);
	int null = open("/dev/null", O_WRONLY);
	dup2(null, 2);
	close(null);
}

static void stop(struct fixture *f)
{
	dup2(f->saved_stderr, 2);
	close(f->saved_stderr);
	tc_parser_finish(&f->p);
	tc_shell_finish(&f->sh);
}

static int is_simple(const struct tc_node *n, size_t nwords, const char *first)
{
	return n && n->kind == TC_NODE_SIMPLE && n->u.simple.nwords == nwords &&
	       strcmp(n->u.simple.words[0].text, first) == 0;
}

static int fails(const char *src)
{
	struct fixture f;
	struct tc_node *n;
	start(&f, src);
	int r = tc_parse_next(&f.p, &n);
	stop(&f);
	return r == TC_ERR_SYNTAX;
}

int main(void)
{
	struct fixture f;
	struct tc_node *n;

	start(&f, "echo a b\n\nls;\ntrue ; false;x");
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 3, "echo"));
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && n == NULL);
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 1, "ls"));
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && n && n->kind == TC_NODE_SEQ);
	CHECK(n && is_simple(n->u.seq.first, 1, "true"));
	CHECK(n && n->u.seq.rest->kind == TC_NODE_SEQ);
	CHECK(n && is_simple(n->u.seq.rest->u.seq.first, 1, "false"));
	CHECK(n && is_simple(n->u.seq.rest->u.seq.rest, 1, "x"));
	CHECK(tc_parse_next(&f.p, &n) == TC_END);
	CHECK(tc_parse_next(&f.p, &n) == TC_END);
	stop(&f);

	/* Reserved words count only unquoted and first; words after a
	 * command name may contain =. */
	start(&f, "'if' x\necho if X=1\n");
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 2, "'if'"));
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 3, "echo"));
	stop(&f);

	/* Syntax errors. */
	CHECK(fails(";"));
	CHECK(fails("a;;"));
	CHECK(fails("a ; ;"));
	CHECK(fails(")"));

	/* Valid shell that 0.0.1 refuses rather than runs wrongly. */
	CHECK(fails("a | b"));
	CHECK(fails("a && b"));
	CHECK(fails("a || b"));
	CHECK(fails("a &"));
	CHECK(fails("a > f"));
	CHECK(fails("a 2>f"));
	CHECK(fails("(a)"));
	CHECK(fails("if true; then :; fi"));
	CHECK(fails("{ a; }"));
	CHECK(fails("! a"));
	CHECK(fails("echo $HOME"));
	CHECK(fails("echo \"$HOME\""));
	CHECK(fails("echo `date`"));
	CHECK(fails("echo *.c"));
	CHECK(fails("echo ~"));
	CHECK(fails("X=1 env"));

	/* An error leaves the next line parseable after recovery. */
	start(&f, "a | b c\nok\n");
	CHECK(tc_parse_next(&f.p, &n) == TC_ERR_SYNTAX);
	tc_parser_recover(&f.p);
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 1, "ok"));
	stop(&f);

	CHECK_DONE("test_parse");
}
