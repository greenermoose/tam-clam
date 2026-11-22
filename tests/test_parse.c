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
	CHECK(fails("|"));
	CHECK(fails("a |"));
	CHECK(fails("a &&"));
	CHECK(fails("a ||"));
	CHECK(fails("a >"));
	CHECK(fails("!"));

	/* Pipelines, !, &&/||, redirections, and here-documents. */
	start(&f, "! a | b && c || d");
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && n && n->kind == TC_NODE_AND_OR &&
	      n->u.and_or.nitems == 3);
	CHECK(n->u.and_or.items[0].op == TC_ANDOR_FIRST &&
	      n->u.and_or.items[0].cmd->kind == TC_NODE_PIPELINE &&
	      n->u.and_or.items[0].cmd->u.pipeline.bang &&
	      n->u.and_or.items[0].cmd->u.pipeline.ncmds == 2);
	CHECK(n->u.and_or.items[1].op == TC_ANDOR_AND &&
	      is_simple(n->u.and_or.items[1].cmd, 1, "c"));
	CHECK(n->u.and_or.items[2].op == TC_ANDOR_OR &&
	      is_simple(n->u.and_or.items[2].cmd, 1, "d"));
	stop(&f);

	start(&f, "a |\n\nb &&\nc");
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && n && n->kind == TC_NODE_AND_OR &&
	      n->u.and_or.nitems == 2 &&
	      n->u.and_or.items[0].cmd->kind == TC_NODE_PIPELINE &&
	      n->u.and_or.items[0].cmd->u.pipeline.ncmds == 2);
	stop(&f);

	start(&f, "> out\na 2>err <<'EOF'\nhello\nEOF\n");
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && n && n->kind == TC_NODE_SIMPLE &&
	      n->u.simple.nwords == 0 && n->u.simple.nredirs == 1 &&
	      n->u.simple.redirs[0].kind == TC_REDIR_OUT);
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 1, "a") &&
	      n->u.simple.nredirs == 2 &&
	      n->u.simple.redirs[0].fd == 2 &&
	      n->u.simple.redirs[1].kind == TC_REDIR_HEREDOC &&
	      n->u.simple.redirs[1].heredoc_quoted &&
	      strcmp(n->u.simple.redirs[1].heredoc_body, "hello\n") == 0);
	stop(&f);

	/* 0.0.3: assignments, parameter expansions, and tilde now parse. */
	start(&f, "X=1 Y=$X\nA=1 env\necho $HOME \"$HOME\" ~ ${x:-a b}\n");
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && n && n->kind == TC_NODE_SIMPLE &&
	      n->u.simple.nassigns == 2 && n->u.simple.nwords == 0 &&
	      strcmp(n->u.simple.assigns[0].name, "X") == 0 &&
	      strcmp(n->u.simple.assigns[1].name, "Y") == 0);
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && n && n->kind == TC_NODE_SIMPLE &&
	      n->u.simple.nassigns == 1 && is_simple(n, 1, "env"));
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 5, "echo"));
	stop(&f);

	/* Valid shell that 0.0.3 still refuses rather than runs wrongly. */
	CHECK(fails("a &"));
	CHECK(fails("(a)"));
	CHECK(fails("if true; then :; fi"));
	CHECK(fails("{ a; }"));
	CHECK(fails("echo `date`"));
	CHECK(fails("echo $(date)"));
	CHECK(fails("echo $((1+2))"));
	CHECK(fails("echo $'abc'"));
	CHECK(fails("echo *.c"));
	CHECK(fails("echo ${x%*}"));

	/* An error leaves the next line parseable after recovery. */
	start(&f, "a & b c\nok\n");
	CHECK(tc_parse_next(&f.p, &n) == TC_ERR_SYNTAX);
	tc_parser_recover(&f.p);
	CHECK(tc_parse_next(&f.p, &n) == TC_OK && is_simple(n, 1, "ok"));
	stop(&f);

	CHECK_DONE("test_parse");
}
