/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "check.h"
#include "tam-clam/input.h"
#include "tam-clam/lex.h"

struct fixture {
	struct tc_shell sh;
	struct tc_input in;
	struct tc_lexer lx;
};

static void start(struct fixture *f, const char *src)
{
	tc_shell_init(&f->sh, "test");
	tc_input_from_string(&f->in, src, strlen(src));
	tc_lexer_init(&f->lx, &f->sh, &f->in);
}

static void stop(struct fixture *f)
{
	tc_lexer_finish(&f->lx);
	tc_shell_finish(&f->sh);
}

static int word_is(struct tc_lexer *lx, const char *text, unsigned flags)
{
	struct tc_token t;
	if (tc_lex_next(lx, &t) != TC_OK || t.kind != TC_TOK_WORD)
		return 0;
	return strcmp(t.text, text) == 0 && t.flags == flags;
}

static int kind_is(struct tc_lexer *lx, enum tc_token_kind k)
{
	struct tc_token t;
	return tc_lex_next(lx, &t) == TC_OK && t.kind == k;
}

static int lex_fails(const char *src)
{
	struct fixture f;
	start(&f, src);
	/* Silence the expected message. */
	int saved = dup(2), null = open("/dev/null", O_WRONLY);
	dup2(null, 2);
	struct tc_token t;
	int r;
	do
		r = tc_lex_next(&f.lx, &t);
	while (r == TC_OK && t.kind != TC_TOK_END);
	dup2(saved, 2);
	close(saved);
	close(null);
	stop(&f);
	return r == TC_ERR_SYNTAX;
}

int main(void)
{
	struct fixture f;

	start(&f, "echo  hello\tworld\n");
	CHECK(word_is(&f.lx, "echo", 0));
	CHECK(word_is(&f.lx, "hello", 0));
	CHECK(word_is(&f.lx, "world", 0));
	CHECK(kind_is(&f.lx, TC_TOK_NEWLINE));
	CHECK(kind_is(&f.lx, TC_TOK_END));
	CHECK(kind_is(&f.lx, TC_TOK_END));
	stop(&f);

	/* Quotes stay in the word; flags say so. */
	start(&f, "'a b' \"c d\" e\\ f");
	CHECK(word_is(&f.lx, "'a b'", TC_WORD_QUOTED));
	CHECK(word_is(&f.lx, "\"c d\"", TC_WORD_QUOTED));
	CHECK(word_is(&f.lx, "e\\ f", TC_WORD_QUOTED));
	stop(&f);

	/* Operators, longest match first; they also end words. */
	start(&f, "a;b;;c&&d||e|f&<<-x<<y<&z<>w>>v>&u>|t>s<r()");
	CHECK(word_is(&f.lx, "a", 0));
	CHECK(kind_is(&f.lx, TC_TOK_SEMI));
	CHECK(word_is(&f.lx, "b", 0));
	CHECK(kind_is(&f.lx, TC_TOK_DSEMI));
	CHECK(word_is(&f.lx, "c", 0));
	CHECK(kind_is(&f.lx, TC_TOK_AND_IF));
	CHECK(word_is(&f.lx, "d", 0));
	CHECK(kind_is(&f.lx, TC_TOK_OR_IF));
	CHECK(word_is(&f.lx, "e", 0));
	CHECK(kind_is(&f.lx, TC_TOK_PIPE));
	CHECK(word_is(&f.lx, "f", 0));
	CHECK(kind_is(&f.lx, TC_TOK_AMP));
	CHECK(kind_is(&f.lx, TC_TOK_DLESSDASH));
	CHECK(word_is(&f.lx, "x", 0));
	CHECK(kind_is(&f.lx, TC_TOK_DLESS));
	CHECK(word_is(&f.lx, "y", 0));
	CHECK(kind_is(&f.lx, TC_TOK_LESSAND));
	CHECK(word_is(&f.lx, "z", 0));
	CHECK(kind_is(&f.lx, TC_TOK_LESSGREAT));
	CHECK(word_is(&f.lx, "w", 0));
	CHECK(kind_is(&f.lx, TC_TOK_DGREAT));
	CHECK(word_is(&f.lx, "v", 0));
	CHECK(kind_is(&f.lx, TC_TOK_GREATAND));
	CHECK(word_is(&f.lx, "u", 0));
	CHECK(kind_is(&f.lx, TC_TOK_CLOBBER));
	CHECK(word_is(&f.lx, "t", 0));
	CHECK(kind_is(&f.lx, TC_TOK_GREAT));
	CHECK(word_is(&f.lx, "s", 0));
	CHECK(kind_is(&f.lx, TC_TOK_LESS));
	CHECK(word_is(&f.lx, "r", 0));
	CHECK(kind_is(&f.lx, TC_TOK_LPAREN));
	CHECK(kind_is(&f.lx, TC_TOK_RPAREN));
	CHECK(kind_is(&f.lx, TC_TOK_END));
	stop(&f);

	/* Backslash-newline joins lines, even inside operators and "...",
	 * but not inside '...'. */
	start(&f, "ec\\\nho &\\\n& \"a\\\nb\" 'c\\\nd'");
	CHECK(word_is(&f.lx, "echo", 0));
	CHECK(kind_is(&f.lx, TC_TOK_AND_IF));
	CHECK(word_is(&f.lx, "\"ab\"", TC_WORD_QUOTED));
	CHECK(word_is(&f.lx, "'c\\\nd'", TC_WORD_QUOTED));
	stop(&f);

	/* Comments start only at the beginning of a word. */
	start(&f, "a#b # c d\ne");
	CHECK(word_is(&f.lx, "a#b", 0));
	CHECK(kind_is(&f.lx, TC_TOK_NEWLINE));
	CHECK(word_is(&f.lx, "e", 0));
	stop(&f);

	/* Flags for what expansion will need. */
	start(&f, "$x \"$y\" '$z' \\$w `a` *.c a? [ab] ~ a~ X=1 =1 'X'=1 x1_=");
	CHECK(word_is(&f.lx, "$x", TC_WORD_DOLLAR));
	CHECK(word_is(&f.lx, "\"$y\"", TC_WORD_QUOTED | TC_WORD_DOLLAR));
	CHECK(word_is(&f.lx, "'$z'", TC_WORD_QUOTED));
	CHECK(word_is(&f.lx, "\\$w", TC_WORD_QUOTED));
	CHECK(word_is(&f.lx, "`a`", TC_WORD_BACKQ));
	CHECK(word_is(&f.lx, "*.c", TC_WORD_GLOB));
	CHECK(word_is(&f.lx, "a?", TC_WORD_GLOB));
	CHECK(word_is(&f.lx, "[ab]", TC_WORD_GLOB));
	CHECK(word_is(&f.lx, "~", TC_WORD_TILDE));
	CHECK(word_is(&f.lx, "a~", 0));
	CHECK(word_is(&f.lx, "X=1", TC_WORD_ASSIGN));
	CHECK(word_is(&f.lx, "=1", 0));
	CHECK(word_is(&f.lx, "'X'=1", TC_WORD_QUOTED));
	CHECK(word_is(&f.lx, "x1_=", TC_WORD_ASSIGN));
	stop(&f);

	/* Line numbers. */
	start(&f, "a\n\nb");
	{
		struct tc_token t;
		CHECK(tc_lex_next(&f.lx, &t) == TC_OK && t.line == 1);
		CHECK(tc_lex_next(&f.lx, &t) == TC_OK && t.kind == TC_TOK_NEWLINE && t.line == 1);
		CHECK(tc_lex_next(&f.lx, &t) == TC_OK && t.kind == TC_TOK_NEWLINE && t.line == 2);
		CHECK(tc_lex_next(&f.lx, &t) == TC_OK && t.line == 3);
	}
	stop(&f);

	/* IO numbers: unquoted all-digit words immediately before < or >. */
	start(&f, "2>err 10<&0 '2'>file 2 >file");
	{
		struct tc_token t;
		CHECK(tc_lex_next(&f.lx, &t) == TC_OK && t.kind == TC_TOK_IO_NUMBER &&
		      strcmp(t.text, "2") == 0);
		CHECK(kind_is(&f.lx, TC_TOK_GREAT));
		CHECK(word_is(&f.lx, "err", 0));
		CHECK(tc_lex_next(&f.lx, &t) == TC_OK && t.kind == TC_TOK_IO_NUMBER &&
		      strcmp(t.text, "10") == 0);
		CHECK(kind_is(&f.lx, TC_TOK_LESSAND));
		CHECK(word_is(&f.lx, "0", 0));
		CHECK(word_is(&f.lx, "'2'", TC_WORD_QUOTED));
		CHECK(kind_is(&f.lx, TC_TOK_GREAT));
		CHECK(word_is(&f.lx, "file", 0));
		CHECK(word_is(&f.lx, "2", 0));
		CHECK(kind_is(&f.lx, TC_TOK_GREAT));
		CHECK(word_is(&f.lx, "file", 0));
	}
	stop(&f);

	/* Here-document body reading. */
	start(&f, "\t\thello\n\t\tworld\n\tEOF\nafter\n");
	{
		const char *body = NULL;
		size_t blen = 0;
		CHECK(tc_lex_read_heredoc(&f.lx, "EOF", true, true, 1, &body, &blen) == TC_OK);
		CHECK(blen == 12 && strcmp(body, "hello\nworld\n") == 0);
		CHECK(word_is(&f.lx, "after", 0));
	}
	stop(&f);

	/* Errors. */
	CHECK(lex_fails("echo 'abc"));
	CHECK(lex_fails("echo \"abc"));
	CHECK(lex_fails("echo \"abc\\"));
	{
		static const char nul[] = "echo a\0b";
		struct fixture g;
		tc_shell_init(&g.sh, "test");
		tc_input_from_string(&g.in, nul, sizeof(nul) - 1);
		tc_lexer_init(&g.lx, &g.sh, &g.in);
		int saved = dup(2), null = open("/dev/null", O_WRONLY);
		dup2(null, 2);
		struct tc_token t;
		CHECK(tc_lex_next(&g.lx, &t) == TC_OK);
		CHECK(tc_lex_next(&g.lx, &t) == TC_ERR_SYNTAX);
		dup2(saved, 2);
		close(saved);
		close(null);
		stop(&g);
	}

	CHECK_DONE("test_lex");
}
