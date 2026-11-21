/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Token recognition, XCU 2.3. Words keep their quotes. Backslash-newline
 * is removed everywhere except inside single quotes and comments. */
#include <string.h>

#include <tam.h>

#include "tam-clam/input.h"
#include "tam-clam/lex.h"

#define NO_PEEK (-10)

int tc_lexer_init(struct tc_lexer *lx, struct tc_shell *sh, struct tc_input *in)
{
	lx->sh = sh;
	lx->in = in;
	lx->peek = NO_PEEK;
	lx->word = tam_str_new("");
	return lx->word ? TC_OK : TC_ERR_NOMEM;
}

void tc_lexer_finish(struct tc_lexer *lx)
{
	tam_str_free(lx->word);
	lx->word = NULL;
}

/* A byte, TC_IN_EOF, or another negative TC_IN_* code. */
static int getch(struct tc_lexer *lx)
{
	if (lx->peek != NO_PEEK) {
		int c = lx->peek;
		lx->peek = NO_PEEK;
		return c;
	}
	return tc_input_getc(lx->in);
}

static void ungetch(struct tc_lexer *lx, int c)
{
	lx->peek = c;
}

/* Like getch, but removes backslash-newline pairs. */
static int getch_cont(struct tc_lexer *lx)
{
	for (;;) {
		int c = getch(lx);
		if (c != '\\')
			return c;
		int d = getch(lx);
		if (d == '\n')
			continue;
		ungetch(lx, d);
		return '\\';
	}
}

/* Map an input failure to a result, reporting it. */
static int input_failure(struct tc_lexer *lx, int c)
{
	if (c == TC_IN_INTR)
		return TC_ERR_INTR;
	tc_error(lx->sh, 0, "read error: %s", strerror(lx->in->err));
	return TC_ERR_IO;
}

static int add(struct tc_lexer *lx, int c)
{
	if (tam_str_len(lx->word) >= TC_WORD_MAX) {
		tc_error(lx->sh, lx->in->line, "word longer than %zu bytes", TC_WORD_MAX);
		return TC_ERR_NOMEM;
	}
	char ch = (char)c;
	tam_str_t w = tam_str_cat_len(lx->word, &ch, 1);
	if (!w) {
		tc_error(lx->sh, lx->in->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	lx->word = w;
	return TC_OK;
}

static int nul_byte(struct tc_lexer *lx)
{
	tc_error(lx->sh, lx->in->line, "the input contains a NUL byte");
	return TC_ERR_SYNTAX;
}

static int is_operator_start(int c)
{
	return c == '&' || c == '|' || c == ';' || c == '<' || c == '>' ||
	       c == '(' || c == ')';
}

static int is_name_char(int c, int first)
{
	if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		return 1;
	return !first && c >= '0' && c <= '9';
}

/* Next character continues the operator if it is `want`. */
static int follows(struct tc_lexer *lx, int want, int *err)
{
	int c = getch_cont(lx);
	if (c < TC_IN_EOF) {
		*err = input_failure(lx, c);
		return 0;
	}
	if (c == want)
		return 1;
	ungetch(lx, c);
	return 0;
}

static int lex_operator(struct tc_lexer *lx, int c, struct tc_token *tok)
{
	int err = TC_OK;
	switch (c) {
	case '&':
		tok->kind = follows(lx, '&', &err) ? TC_TOK_AND_IF : TC_TOK_AMP;
		break;
	case '|':
		tok->kind = follows(lx, '|', &err) ? TC_TOK_OR_IF : TC_TOK_PIPE;
		break;
	case ';':
		tok->kind = follows(lx, ';', &err) ? TC_TOK_DSEMI : TC_TOK_SEMI;
		break;
	case '(':
		tok->kind = TC_TOK_LPAREN;
		break;
	case ')':
		tok->kind = TC_TOK_RPAREN;
		break;
	case '<':
		if (follows(lx, '<', &err))
			tok->kind = (!err && follows(lx, '-', &err)) ? TC_TOK_DLESSDASH : TC_TOK_DLESS;
		else if (!err && follows(lx, '&', &err))
			tok->kind = TC_TOK_LESSAND;
		else if (!err && follows(lx, '>', &err))
			tok->kind = TC_TOK_LESSGREAT;
		else
			tok->kind = TC_TOK_LESS;
		break;
	default: /* '>' */
		if (follows(lx, '>', &err))
			tok->kind = TC_TOK_DGREAT;
		else if (!err && follows(lx, '&', &err))
			tok->kind = TC_TOK_GREATAND;
		else if (!err && follows(lx, '|', &err))
			tok->kind = TC_TOK_CLOBBER;
		else
			tok->kind = TC_TOK_GREAT;
		break;
	}
	return err;
}

/* Inside '...': everything up to the next ' is literal. */
static int lex_single(struct tc_lexer *lx, unsigned long start)
{
	for (;;) {
		int c = getch(lx);
		if (c == TC_IN_EOF) {
			tc_error(lx->sh, start, "unterminated quote (') at end of input");
			return TC_ERR_SYNTAX;
		}
		if (c < TC_IN_EOF)
			return input_failure(lx, c);
		if (c == 0)
			return nul_byte(lx);
		int r = add(lx, c);
		if (r != TC_OK || c == '\'')
			return r;
	}
}

/* Inside "...": backslash quotes only $ ` " \ and newline. */
static int lex_double(struct tc_lexer *lx, unsigned long start, unsigned *flags)
{
	for (;;) {
		int c = getch_cont(lx);
		if (c == TC_IN_EOF) {
			tc_error(lx->sh, start, "unterminated quote (\") at end of input");
			return TC_ERR_SYNTAX;
		}
		if (c < TC_IN_EOF)
			return input_failure(lx, c);
		if (c == 0)
			return nul_byte(lx);
		int r = add(lx, c);
		if (r != TC_OK || c == '"')
			return r;
		if (c == '$')
			*flags |= TC_WORD_DOLLAR;
		else if (c == '`')
			*flags |= TC_WORD_BACKQ;
		else if (c == '\\') {
			int d = getch(lx);
			if (d == TC_IN_EOF)
				continue; /* the closing " is missing; reported next */
			if (d < TC_IN_EOF)
				return input_failure(lx, d);
			if (d == 0)
				return nul_byte(lx);
			if ((r = add(lx, d)) != TC_OK)
				return r;
		}
	}
}

static int lex_word(struct tc_lexer *lx, int c, struct tc_token *tok)
{
	unsigned flags = 0;
	int name_so_far = 1; /* every byte so far could start an assignment */
	tam_str_clear(lx->word);

	for (;; c = getch_cont(lx)) {
		int r;
		if (c < TC_IN_EOF)
			return input_failure(lx, c);
		if (c == TC_IN_EOF || c == ' ' || c == '\t' || c == '\n' ||
		    is_operator_start(c)) {
			if (c != TC_IN_EOF)
				ungetch(lx, c);
			break;
		}
		if (c == 0)
			return nul_byte(lx);
		size_t len = tam_str_len(lx->word);
		if (c == '=' && name_so_far && len > 0 && !(flags & TC_WORD_QUOTED))
			flags |= TC_WORD_ASSIGN;
		if (!is_name_char(c, len == 0))
			name_so_far = 0;

		if ((r = add(lx, c)) != TC_OK)
			return r;
		switch (c) {
		case '\'':
			flags |= TC_WORD_QUOTED;
			r = lex_single(lx, lx->in->line);
			break;
		case '"':
			flags |= TC_WORD_QUOTED;
			r = lex_double(lx, lx->in->line, &flags);
			break;
		case '\\': {
			flags |= TC_WORD_QUOTED;
			int d = getch(lx);
			if (d < TC_IN_EOF)
				return input_failure(lx, d);
			if (d == 0)
				return nul_byte(lx);
			if (d != TC_IN_EOF)
				r = add(lx, d);
			break;
		}
		case '$':
			flags |= TC_WORD_DOLLAR;
			break;
		case '`':
			flags |= TC_WORD_BACKQ;
			break;
		case '*': case '?': case '[':
			flags |= TC_WORD_GLOB;
			break;
		case '~':
			if (len == 0)
				flags |= TC_WORD_TILDE;
			break;
		}
		if (r != TC_OK)
			return r;
	}

	size_t len = tam_str_len(lx->word);
	enum tc_token_kind kind = TC_TOK_WORD;
	if ((c == '<' || c == '>') && !(flags & TC_WORD_QUOTED) && len > 0) {
		int all_digits = 1;
		for (size_t i = 0; i < len; i++) {
			if (lx->word[i] < '0' || lx->word[i] > '9') {
				all_digits = 0;
				break;
			}
		}
		if (all_digits)
			kind = TC_TOK_IO_NUMBER;
	}
	tok->kind = kind;
	tok->flags = flags;
	tok->len = len;
	tok->text = tc_arena_strndup(&lx->sh->arena, lx->word, len);
	if (!tok->text) {
		tc_error(lx->sh, tok->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	return TC_OK;
}

int tc_lex_next(struct tc_lexer *lx, struct tc_token *tok)
{
	tok->text = NULL;
	tok->len = 0;
	tok->flags = 0;
	for (;;) {
		int c = getch_cont(lx);
		tok->line = lx->in->line;
		if (c == ' ' || c == '\t')
			continue;
		if (c == TC_IN_EOF) {
			tok->kind = TC_TOK_END;
			return TC_OK;
		}
		if (c < TC_IN_EOF)
			return input_failure(lx, c);
		if (c == '#') {
			/* A comment runs to the end of the line. */
			do {
				c = getch(lx);
			} while (c >= 0 && c != '\n');
			if (c < TC_IN_EOF)
				return input_failure(lx, c);
			if (c == '\n')
				ungetch(lx, c);
			continue;
		}
		if (c == '\n') {
			tok->kind = TC_TOK_NEWLINE;
			tok->line--; /* the newline belongs to the line it ends */
			return TC_OK;
		}
		if (is_operator_start(c))
			return lex_operator(lx, c, tok);
		return lex_word(lx, c, tok);
	}
}

void tc_lex_skip_line(struct tc_lexer *lx)
{
	int c;
	do {
		c = getch(lx);
	} while (c >= 0 && c != '\n');
}

void tc_lexer_reset(struct tc_lexer *lx)
{
	lx->peek = NO_PEEK;
}

const char *tc_token_name(enum tc_token_kind kind)
{
	static const char *const names[] = {
		[TC_TOK_WORD] = "word", [TC_TOK_IO_NUMBER] = "io_number",
		[TC_TOK_NEWLINE] = "newline",
		[TC_TOK_END] = "end of input", [TC_TOK_SEMI] = ";",
		[TC_TOK_AMP] = "&", [TC_TOK_PIPE] = "|", [TC_TOK_AND_IF] = "&&",
		[TC_TOK_OR_IF] = "||", [TC_TOK_DSEMI] = ";;", [TC_TOK_LPAREN] = "(",
		[TC_TOK_RPAREN] = ")", [TC_TOK_LESS] = "<", [TC_TOK_GREAT] = ">",
		[TC_TOK_DLESS] = "<<", [TC_TOK_DGREAT] = ">>",
		[TC_TOK_LESSAND] = "<&", [TC_TOK_GREATAND] = ">&",
		[TC_TOK_LESSGREAT] = "<>", [TC_TOK_DLESSDASH] = "<<-",
		[TC_TOK_CLOBBER] = ">|",
	};
	if ((size_t)kind < sizeof(names) / sizeof(names[0]) && names[kind])
		return names[kind];
	return "?";
}

static int hd_append(struct tc_lexer *lx, tam_str_t *buf, char ch)
{
	if (tam_str_len(*buf) >= TC_HEREDOC_MAX) {
		tc_error(lx->sh, lx->in->line,
			 "here-document longer than %zu bytes", TC_HEREDOC_MAX);
		return TC_ERR_NOMEM;
	}
	tam_str_t next = tam_str_cat_len(*buf, &ch, 1);
	if (!next) {
		tc_error(lx->sh, lx->in->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	*buf = next;
	return TC_OK;
}

int tc_lex_read_heredoc(struct tc_lexer *lx, const char *delim, bool strip_tabs,
			bool quoted, unsigned long start_line,
			const char **out_body, size_t *out_len)
{
	*out_body = "";
	*out_len = 0;
	tam_str_t body = tam_str_new("");
	tam_str_t line = tam_str_new("");
	if (!body || !line) {
		tam_str_free(body);
		tam_str_free(line);
		tc_error(lx->sh, start_line, "out of memory");
		return TC_ERR_NOMEM;
	}

	int r = TC_OK;
	for (;;) {
		tam_str_clear(line);
		unsigned long line_no = lx->in->line;
		int eof = 0;
		for (;;) {
			int c = quoted ? getch(lx) : getch_cont(lx);
			if (c == TC_IN_EOF) {
				eof = 1;
				break;
			}
			if (c < TC_IN_EOF) {
				r = input_failure(lx, c);
				goto out;
			}
			if (c == 0) {
				r = nul_byte(lx);
				goto out;
			}
			if (c == '\n')
				break;
			if (tam_str_len(line) >= TC_HEREDOC_MAX) {
				tc_error(lx->sh, line_no,
					 "here-document longer than %zu bytes",
					 TC_HEREDOC_MAX);
				r = TC_ERR_NOMEM;
				goto out;
			}
			char ch = (char)c;
			tam_str_t next = tam_str_cat_len(line, &ch, 1);
			if (!next) {
				tc_error(lx->sh, line_no, "out of memory");
				r = TC_ERR_NOMEM;
				goto out;
			}
			line = next;
		}

		const char *s = line;
		size_t slen = tam_str_len(line);
		if (strip_tabs) {
			while (slen > 0 && *s == '\t') {
				s++;
				slen--;
			}
		}

		if (!eof && strcmp(s, delim) == 0)
			break;

		if (eof && slen == 0) {
			tc_error(lx->sh, line_no,
				 "warning: here-document at line %lu delimited by end-of-file (wanted `%s')",
				 start_line, delim);
			break;
		}

		if (!quoted) {
			for (size_t i = 0; i < slen; i++) {
				if (s[i] == '\\' && i + 1 < slen &&
				    (s[i + 1] == '$' || s[i + 1] == '`' || s[i + 1] == '\\')) {
					if ((r = hd_append(lx, &body, s[++i])) != TC_OK)
						goto out;
					continue;
				}
				if (s[i] == '$' || s[i] == '`') {
					tc_error(lx->sh, line_no,
						 "expansions inside here-documents are not supported yet; see ROADMAP.md");
					r = TC_ERR_SYNTAX;
					goto out;
				}
				if ((r = hd_append(lx, &body, s[i])) != TC_OK)
					goto out;
			}
		} else {
			for (size_t i = 0; i < slen; i++) {
				if ((r = hd_append(lx, &body, s[i])) != TC_OK)
					goto out;
			}
		}

		if (eof) {
			tc_error(lx->sh, line_no,
				 "warning: here-document at line %lu delimited by end-of-file (wanted `%s')",
				 start_line, delim);
			break;
		}
		if ((r = hd_append(lx, &body, '\n')) != TC_OK)
			goto out;
	}

	size_t blen = tam_str_len(body);
	char *copy = tc_arena_strndup(&lx->sh->arena, body, blen);
	if (!copy) {
		tc_error(lx->sh, start_line, "out of memory");
		r = TC_ERR_NOMEM;
		goto out;
	}
	*out_body = copy;
	*out_len = blen;

out:
	tam_str_free(body);
	tam_str_free(line);
	return r;
}
