/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Token recognition, XCU 2.3. Words are parsed directly into lists of
 * tc_wpart so quoting is decided once, by the lexer. Backslash-newline is
 * removed everywhere except inside single quotes and comments. */
#include <string.h>

#include <tam.h>

#include "tam-clam/input.h"
#include "tam-clam/lex.h"

#define NO_PEEK (-10)

int tc_lexer_init(struct tc_lexer *lx, struct tc_shell *sh, struct tc_input *in)
{
	lx->sh = sh;
	lx->in = in;
	if (in && !in->sh)
		in->sh = sh;
	lx->peek = NO_PEEK;
	lx->word = tam_str_new("");
	return lx->word ? TC_OK : TC_ERR_NOMEM;
}

void tc_lexer_finish(struct tc_lexer *lx)
{
	tam_str_free(lx->word);
	lx->word = NULL;
}

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

static int input_failure(struct tc_lexer *lx, int c)
{
	if (c == TC_IN_INTR)
		return TC_ERR_INTR;
	tc_error(lx->sh, 0, "read error: %s", strerror(lx->in->err));
	return TC_ERR_IO;
}

static int raw_add(struct tc_lexer *lx, int c)
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

static int is_special_param(int c)
{
	return c == '@' || c == '*' || c == '#' || c == '?' ||
	       c == '-' || c == '$' || c == '!';
}

static int is_dquote_escapable(int c)
{
	return c == '$' || c == '`' || c == '"' || c == '\\';
}

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

/* Builder for a linked list of tc_wpart in the shell's arena. */
struct wpart_buf {
	struct tc_lexer *lx;
	struct tc_wpart *head;
	struct tc_wpart **tail;
	tam_str_t lit;
	enum tc_wpart_kind lit_kind;
	bool has_lit;
};

static int wb_init(struct wpart_buf *wb, struct tc_lexer *lx)
{
	wb->lx = lx;
	wb->head = NULL;
	wb->tail = &wb->head;
	wb->lit = tam_str_new("");
	wb->lit_kind = TC_WPART_LITERAL;
	wb->has_lit = false;
	if (!wb->lit) {
		tc_error(lx->sh, lx->in->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	return TC_OK;
}

static void wb_finish(struct wpart_buf *wb)
{
	tam_str_free(wb->lit);
	wb->lit = NULL;
}

static int wb_flush(struct wpart_buf *wb)
{
	if (!wb->has_lit)
		return TC_OK;
	struct tc_arena *a = &wb->lx->sh->arena;
	struct tc_wpart *wp = tc_arena_alloc(a, sizeof(*wp));
	size_t len = tam_str_len(wb->lit);
	char *text = tc_arena_strndup(a, wb->lit, len);
	if (!wp || !text) {
		tc_error(wb->lx->sh, wb->lx->in->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	memset(wp, 0, sizeof(*wp));
	wp->kind = wb->lit_kind;
	wp->quoted = (wb->lit_kind == TC_WPART_QUOTED);
	wp->text = text;
	wp->len = len;
	*wb->tail = wp;
	wb->tail = &wp->next;
	tam_str_clear(wb->lit);
	wb->has_lit = false;
	return TC_OK;
}

static int wb_add_char(struct wpart_buf *wb, enum tc_wpart_kind kind, int c)
{
	if (wb->has_lit && wb->lit_kind != kind) {
		int r = wb_flush(wb);
		if (r != TC_OK)
			return r;
	}
	if (tam_str_len(wb->lit) >= TC_WORD_MAX) {
		tc_error(wb->lx->sh, wb->lx->in->line, "word longer than %zu bytes", TC_WORD_MAX);
		return TC_ERR_NOMEM;
	}
	char ch = (char)c;
	tam_str_t next = tam_str_cat_len(wb->lit, &ch, 1);
	if (!next) {
		tc_error(wb->lx->sh, wb->lx->in->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	wb->lit = next;
	wb->lit_kind = kind;
	wb->has_lit = true;
	return TC_OK;
}

static int wb_ensure_quoted(struct wpart_buf *wb)
{
	if (wb->has_lit && wb->lit_kind != TC_WPART_QUOTED) {
		int r = wb_flush(wb);
		if (r != TC_OK)
			return r;
	}
	wb->lit_kind = TC_WPART_QUOTED;
	wb->has_lit = true;
	return TC_OK;
}

static int wb_add_part(struct wpart_buf *wb, struct tc_wpart *wp)
{
	int r = wb_flush(wb);
	if (r != TC_OK)
		return r;
	*wb->tail = wp;
	wb->tail = &wp->next;
	return TC_OK;
}

static int lex_dollar(struct tc_lexer *lx, struct wpart_buf *wb,
		      bool in_dquote, bool record_raw, unsigned *flags,
		      int depth, unsigned long start_line);

/* Inside '...': everything up to the next ' is literal. */
static int lex_single(struct tc_lexer *lx, struct wpart_buf *wb,
		      bool record_raw, unsigned long start)
{
	int r = wb_ensure_quoted(wb);
	if (r != TC_OK)
		return r;
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
		if (record_raw && (r = raw_add(lx, c)) != TC_OK)
			return r;
		if (c == '\'')
			return TC_OK;
		if ((r = wb_add_char(wb, TC_WPART_QUOTED, c)) != TC_OK)
			return r;
	}
}

/* Inside "...": backslash quotes only $ ` " \ (and newline via getch_cont). */
static int lex_double(struct tc_lexer *lx, struct wpart_buf *wb,
		      bool record_raw, unsigned *flags, int depth,
		      unsigned long start)
{
	bool saw_content = false;
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
		int r;
		if (record_raw && (r = raw_add(lx, c)) != TC_OK)
			return r;
		if (c == '"')
			return saw_content ? TC_OK : wb_ensure_quoted(wb);
		saw_content = true;
		if (c == '$') {
			r = lex_dollar(lx, wb, true, record_raw, flags, depth, start);
			if (r != TC_OK)
				return r;
			continue;
		}
		if (c == '`') {
			*flags |= TC_WORD_BACKQ;
			if ((r = wb_add_char(wb, TC_WPART_QUOTED, c)) != TC_OK)
				return r;
			continue;
		}
		if (c == '\\') {
			int d = getch(lx);
			if (d == TC_IN_EOF)
				continue; /* closing " missing; reported next */
			if (d < TC_IN_EOF)
				return input_failure(lx, d);
			if (d == 0)
				return nul_byte(lx);
			if (record_raw && (r = raw_add(lx, d)) != TC_OK)
				return r;
			if (!is_dquote_escapable(d)) {
				if ((r = wb_add_char(wb, TC_WPART_QUOTED, '\\')) != TC_OK)
					return r;
			}
			if ((r = wb_add_char(wb, TC_WPART_QUOTED, d)) != TC_OK)
				return r;
			continue;
		}
		if ((r = wb_add_char(wb, TC_WPART_QUOTED, c)) != TC_OK)
			return r;
	}
}

static int lex_brace_arg(struct tc_lexer *lx, bool in_dquote, bool record_raw,
			 int depth, unsigned long start,
			 struct tc_wpart **out_parts, unsigned *out_flags)
{
	struct wpart_buf wb;
	int r = wb_init(&wb, lx);
	if (r != TC_OK)
		return r;
	unsigned flags = 0;
	bool at_start = true;

	for (;;) {
		int c = getch_cont(lx);
		if (c == TC_IN_EOF) {
			tc_error(lx->sh, start, "unterminated `${...}' at end of input");
			r = TC_ERR_SYNTAX;
			goto out;
		}
		if (c < TC_IN_EOF) {
			r = input_failure(lx, c);
			goto out;
		}
		if (c == 0) {
			r = nul_byte(lx);
			goto out;
		}
		if (record_raw && (r = raw_add(lx, c)) != TC_OK)
			goto out;
		if (c == '}')
			break;

		if (in_dquote) {
			if (c == '"') {
				flags |= TC_WORD_QUOTED;
				r = lex_double(lx, &wb, record_raw, &flags, depth, lx->in->line);
				if (r != TC_OK)
					goto out;
			} else if (c == '$') {
				r = lex_dollar(lx, &wb, true, record_raw, &flags, depth, start);
				if (r != TC_OK)
					goto out;
			} else if (c == '`') {
				flags |= TC_WORD_BACKQ;
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, c)) != TC_OK)
					goto out;
			} else if (c == '\\') {
				int d = getch(lx);
				if (d == TC_IN_EOF) {
					tc_error(lx->sh, start, "unterminated `${...}' at end of input");
					r = TC_ERR_SYNTAX;
					goto out;
				}
				if (d < TC_IN_EOF) {
					r = input_failure(lx, d);
					goto out;
				}
				if (d == 0) {
					r = nul_byte(lx);
					goto out;
				}
				if (record_raw && (r = raw_add(lx, d)) != TC_OK)
					goto out;
				if (!is_dquote_escapable(d) && d != '}') {
					if ((r = wb_add_char(&wb, TC_WPART_QUOTED, '\\')) != TC_OK)
						goto out;
				}
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, d)) != TC_OK)
					goto out;
			} else {
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, c)) != TC_OK)
					goto out;
			}
		} else {
			if (c == '\'') {
				flags |= TC_WORD_QUOTED;
				r = lex_single(lx, &wb, record_raw, lx->in->line);
				if (r != TC_OK)
					goto out;
			} else if (c == '"') {
				flags |= TC_WORD_QUOTED;
				r = lex_double(lx, &wb, record_raw, &flags, depth, lx->in->line);
				if (r != TC_OK)
					goto out;
			} else if (c == '\\') {
				flags |= TC_WORD_QUOTED;
				int d = getch(lx);
				if (d == TC_IN_EOF) {
					tc_error(lx->sh, start, "unterminated `${...}' at end of input");
					r = TC_ERR_SYNTAX;
					goto out;
				}
				if (d < TC_IN_EOF) {
					r = input_failure(lx, d);
					goto out;
				}
				if (d == 0) {
					r = nul_byte(lx);
					goto out;
				}
				if (record_raw && (r = raw_add(lx, d)) != TC_OK)
					goto out;
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, d)) != TC_OK)
					goto out;
			} else if (c == '$') {
				r = lex_dollar(lx, &wb, false, record_raw, &flags, depth, start);
				if (r != TC_OK)
					goto out;
			} else if (c == '`') {
				flags |= TC_WORD_BACKQ;
				if ((r = wb_add_char(&wb, TC_WPART_LITERAL, c)) != TC_OK)
					goto out;
			} else {
				if (c == '*' || c == '?' || c == '[')
					flags |= TC_WORD_GLOB;
				if (at_start && c == '~')
					flags |= TC_WORD_TILDE;
				if ((r = wb_add_char(&wb, TC_WPART_LITERAL, c)) != TC_OK)
					goto out;
			}
		}
		at_start = false;
	}

	r = wb_flush(&wb);
	if (r == TC_OK) {
		*out_parts = wb.head;
		*out_flags = flags;
	}
out:
	wb_finish(&wb);
	return r;
}

static int read_brace_name(struct tc_lexer *lx, int first, bool record_raw,
			   unsigned long start, char **out_name, size_t *out_len)
{
	tam_str_t name = tam_str_new("");
	if (!name) {
		tc_error(lx->sh, start, "out of memory");
		return TC_ERR_NOMEM;
	}
	int r = TC_OK;
	if (is_special_param(first)) {
		char ch = (char)first;
		tam_str_t next = tam_str_cat_len(name, &ch, 1);
		if (!next) {
			r = TC_ERR_NOMEM;
			tc_error(lx->sh, start, "out of memory");
			goto out;
		}
		name = next;
	} else if (first >= '0' && first <= '9') {
		int c = first;
		for (;;) {
			char ch = (char)c;
			tam_str_t next = tam_str_cat_len(name, &ch, 1);
			if (!next) {
				r = TC_ERR_NOMEM;
				tc_error(lx->sh, start, "out of memory");
				goto out;
			}
			name = next;
			c = getch_cont(lx);
			if (c < TC_IN_EOF) {
				r = input_failure(lx, c);
				goto out;
			}
			if (c >= '0' && c <= '9') {
				if (record_raw && (r = raw_add(lx, c)) != TC_OK)
					goto out;
				continue;
			}
			if (c != TC_IN_EOF)
				ungetch(lx, c);
			break;
		}
	} else if (is_name_char(first, 1)) {
		int c = first;
		for (;;) {
			char ch = (char)c;
			tam_str_t next = tam_str_cat_len(name, &ch, 1);
			if (!next) {
				r = TC_ERR_NOMEM;
				tc_error(lx->sh, start, "out of memory");
				goto out;
			}
			name = next;
			c = getch_cont(lx);
			if (c < TC_IN_EOF) {
				r = input_failure(lx, c);
				goto out;
			}
			if (is_name_char(c, 0)) {
				if (record_raw && (r = raw_add(lx, c)) != TC_OK)
					goto out;
				continue;
			}
			if (c != TC_IN_EOF)
				ungetch(lx, c);
			break;
		}
	} else {
		tc_error(lx->sh, start, "bad substitution");
		r = TC_ERR_SYNTAX;
		goto out;
	}

	size_t nlen = tam_str_len(name);
	char *copy = tc_arena_strndup(&lx->sh->arena, name, nlen);
	if (!copy) {
		tc_error(lx->sh, start, "out of memory");
		r = TC_ERR_NOMEM;
		goto out;
	}
	*out_name = copy;
	*out_len = nlen;
out:
	tam_str_free(name);
	return r;
}

static int lex_param_brace(struct tc_lexer *lx, struct wpart_buf *wb,
			   bool in_dquote, bool record_raw, int depth,
			   unsigned long start)
{
	if (depth > TC_PARAM_DEPTH_MAX) {
		tc_error(lx->sh, start, "parameter expansion nested too deeply");
		return TC_ERR_SYNTAX;
	}

	int c = getch_cont(lx);
	if (c == TC_IN_EOF) {
		tc_error(lx->sh, start, "unterminated `${...}' at end of input");
		return TC_ERR_SYNTAX;
	}
	if (c < TC_IN_EOF)
		return input_failure(lx, c);
	if (c == 0)
		return nul_byte(lx);
	if (record_raw) {
		int rr = raw_add(lx, c);
		if (rr != TC_OK)
			return rr;
	}

	char *name = NULL;
	size_t nlen = 0;
	enum tc_param_op op = TC_PARAM_PLAIN;
	bool colon = false;
	struct tc_wpart *arg = NULL;
	unsigned arg_flags = 0;
	int r;

	if (c == '#') {
		int d = getch_cont(lx);
		if (d == TC_IN_EOF) {
			tc_error(lx->sh, start, "unterminated `${...}' at end of input");
			return TC_ERR_SYNTAX;
		}
		if (d < TC_IN_EOF)
			return input_failure(lx, d);
		if (d == '}') {
			if (record_raw && (r = raw_add(lx, d)) != TC_OK)
				return r;
			name = tc_arena_strndup(&lx->sh->arena, "#", 1);
			if (!name) {
				tc_error(lx->sh, start, "out of memory");
				return TC_ERR_NOMEM;
			}
			nlen = 1;
			goto emit;
		}
		if (d == ':' || d == '-' || d == '=' || d == '?' || d == '+' ||
		    d == '%' || d == '#') {
			/* ${#-default}, ${#:-default}, etc. apply to $# */
			ungetch(lx, d);
			name = tc_arena_strndup(&lx->sh->arena, "#", 1);
			if (!name) {
				tc_error(lx->sh, start, "out of memory");
				return TC_ERR_NOMEM;
			}
			nlen = 1;
		} else {
			/* ${#parameter} string length */
			if (record_raw && (r = raw_add(lx, d)) != TC_OK)
				return r;
			r = read_brace_name(lx, d, record_raw, start, &name, &nlen);
			if (r != TC_OK)
				return r;
			int end = getch_cont(lx);
			if (end == TC_IN_EOF) {
				tc_error(lx->sh, start, "unterminated `${...}' at end of input");
				return TC_ERR_SYNTAX;
			}
			if (end < TC_IN_EOF)
				return input_failure(lx, end);
			if (end != '}') {
				tc_error(lx->sh, start, "bad substitution");
				return TC_ERR_SYNTAX;
			}
			if (record_raw && (r = raw_add(lx, end)) != TC_OK)
				return r;
			op = TC_PARAM_LEN;
			goto emit;
		}
	} else {
		r = read_brace_name(lx, c, record_raw, start, &name, &nlen);
		if (r != TC_OK)
			return r;
	}

	int op1 = getch_cont(lx);
	if (op1 == TC_IN_EOF) {
		tc_error(lx->sh, start, "unterminated `${...}' at end of input");
		return TC_ERR_SYNTAX;
	}
	if (op1 < TC_IN_EOF)
		return input_failure(lx, op1);
	if (op1 == 0)
		return nul_byte(lx);
	if (record_raw && (r = raw_add(lx, op1)) != TC_OK)
		return r;

	if (op1 == '}') {
		op = TC_PARAM_PLAIN;
		goto emit;
	}

	if (op1 == ':') {
		colon = true;
		op1 = getch_cont(lx);
		if (op1 == TC_IN_EOF) {
			tc_error(lx->sh, start, "unterminated `${...}' at end of input");
			return TC_ERR_SYNTAX;
		}
		if (op1 < TC_IN_EOF)
			return input_failure(lx, op1);
		if (record_raw && (r = raw_add(lx, op1)) != TC_OK)
			return r;
	}

	switch (op1) {
	case '-': op = TC_PARAM_USE_DEFAULT; break;
	case '=': op = TC_PARAM_ASSIGN_DEFAULT; break;
	case '?': op = TC_PARAM_ERROR; break;
	case '+': op = TC_PARAM_USE_ALT; break;
	case '%':
		if (colon) {
			tc_error(lx->sh, start, "bad substitution");
			return TC_ERR_SYNTAX;
		}
		{
			int op2 = getch_cont(lx);
			if (op2 < TC_IN_EOF)
				return input_failure(lx, op2);
			if (op2 == '%') {
				if (record_raw && (r = raw_add(lx, op2)) != TC_OK)
					return r;
				op = TC_PARAM_TRIM_SUFFIX_L;
			} else {
				if (op2 != TC_IN_EOF)
					ungetch(lx, op2);
				op = TC_PARAM_TRIM_SUFFIX_S;
			}
		}
		break;
	case '#':
		if (colon) {
			tc_error(lx->sh, start, "bad substitution");
			return TC_ERR_SYNTAX;
		}
		{
			int op2 = getch_cont(lx);
			if (op2 < TC_IN_EOF)
				return input_failure(lx, op2);
			if (op2 == '#') {
				if (record_raw && (r = raw_add(lx, op2)) != TC_OK)
					return r;
				op = TC_PARAM_TRIM_PREFIX_L;
			} else {
				if (op2 != TC_IN_EOF)
					ungetch(lx, op2);
				op = TC_PARAM_TRIM_PREFIX_S;
			}
		}
		break;
	default:
		tc_error(lx->sh, start, "bad substitution");
		return TC_ERR_SYNTAX;
	}

	r = lex_brace_arg(lx, in_dquote, record_raw, depth, start, &arg, &arg_flags);
	if (r != TC_OK)
		return r;

emit:
	{
		struct tc_wpart *wp = tc_arena_alloc(&lx->sh->arena, sizeof(*wp));
		if (!wp) {
			tc_error(lx->sh, start, "out of memory");
			return TC_ERR_NOMEM;
		}
		memset(wp, 0, sizeof(*wp));
		wp->kind = TC_WPART_PARAM;
		wp->quoted = in_dquote;
		wp->text = name;
		wp->len = nlen;
		wp->op = op;
		wp->colon = colon;
		wp->arg = arg;
		wp->arg_flags = arg_flags;
		return wb_add_part(wb, wp);
	}
}

static int lex_dollar(struct tc_lexer *lx, struct wpart_buf *wb,
		      bool in_dquote, bool record_raw, unsigned *flags,
		      int depth, unsigned long start_line)
{
	int d = getch_cont(lx);
	if (d < TC_IN_EOF)
		return input_failure(lx, d);

	if (d == '(') {
		int r;
		if (record_raw && (r = raw_add(lx, d)) != TC_OK)
			return r;
		int e = getch_cont(lx);
		if (e < TC_IN_EOF)
			return input_failure(lx, e);
		if (e == '(') {
			if (record_raw && (r = raw_add(lx, e)) != TC_OK)
				return r;
			*flags |= TC_WORD_ARITH;
		} else {
			if (e != TC_IN_EOF)
				ungetch(lx, e);
			*flags |= TC_WORD_CMDSUB;
		}
		return wb_add_char(wb, in_dquote ? TC_WPART_QUOTED : TC_WPART_LITERAL, '$');
	}

	if (d == '\'' && !in_dquote) {
		*flags |= TC_WORD_ANSI_Q;
		ungetch(lx, d);
		return wb_add_char(wb, TC_WPART_LITERAL, '$');
	}

	if (d == '{') {
		int r;
		if (record_raw && (r = raw_add(lx, d)) != TC_OK)
			return r;
		*flags |= TC_WORD_DOLLAR;
		return lex_param_brace(lx, wb, in_dquote, record_raw, depth + 1, start_line);
	}

	if (is_special_param(d) || (d >= '0' && d <= '9')) {
		int r;
		if (record_raw && (r = raw_add(lx, d)) != TC_OK)
			return r;
		*flags |= TC_WORD_DOLLAR;
		char ch = (char)d;
		char *name = tc_arena_strndup(&lx->sh->arena, &ch, 1);
		struct tc_wpart *wp = tc_arena_alloc(&lx->sh->arena, sizeof(*wp));
		if (!name || !wp) {
			tc_error(lx->sh, start_line, "out of memory");
			return TC_ERR_NOMEM;
		}
		memset(wp, 0, sizeof(*wp));
		wp->kind = TC_WPART_PARAM;
		wp->quoted = in_dquote;
		wp->text = name;
		wp->len = 1;
		wp->op = TC_PARAM_PLAIN;
		return wb_add_part(wb, wp);
	}

	if (is_name_char(d, 1)) {
		*flags |= TC_WORD_DOLLAR;
		tam_str_t name = tam_str_new("");
		if (!name) {
			tc_error(lx->sh, start_line, "out of memory");
			return TC_ERR_NOMEM;
		}
		int r = TC_OK;
		int c = d;
		for (;;) {
			if (record_raw && (r = raw_add(lx, c)) != TC_OK)
				break;
			char ch = (char)c;
			tam_str_t next = tam_str_cat_len(name, &ch, 1);
			if (!next) {
				tc_error(lx->sh, start_line, "out of memory");
				r = TC_ERR_NOMEM;
				break;
			}
			name = next;
			c = getch_cont(lx);
			if (c < TC_IN_EOF) {
				r = input_failure(lx, c);
				break;
			}
			if (is_name_char(c, 0))
				continue;
			if (c != TC_IN_EOF)
				ungetch(lx, c);
			break;
		}
		if (r != TC_OK) {
			tam_str_free(name);
			return r;
		}
		size_t nlen = tam_str_len(name);
		char *copy = tc_arena_strndup(&lx->sh->arena, name, nlen);
		tam_str_free(name);
		struct tc_wpart *wp = tc_arena_alloc(&lx->sh->arena, sizeof(*wp));
		if (!copy || !wp) {
			tc_error(lx->sh, start_line, "out of memory");
			return TC_ERR_NOMEM;
		}
		memset(wp, 0, sizeof(*wp));
		wp->kind = TC_WPART_PARAM;
		wp->quoted = in_dquote;
		wp->text = copy;
		wp->len = nlen;
		wp->op = TC_PARAM_PLAIN;
		return wb_add_part(wb, wp);
	}

	/* Bare '$' not starting an expansion is a literal '$'. */
	if (d != TC_IN_EOF)
		ungetch(lx, d);
	return wb_add_char(wb, in_dquote ? TC_WPART_QUOTED : TC_WPART_LITERAL, '$');
}

static int lex_word(struct tc_lexer *lx, int c, struct tc_token *tok)
{
	unsigned flags = 0;
	int name_so_far = 1;
	tam_str_clear(lx->word);

	struct wpart_buf wb;
	int r = wb_init(&wb, lx);
	if (r != TC_OK)
		return r;

	for (;; c = getch_cont(lx)) {
		if (c < TC_IN_EOF) {
			r = input_failure(lx, c);
			goto out;
		}
		if (c == TC_IN_EOF || c == ' ' || c == '\t' || c == '\n' ||
		    is_operator_start(c)) {
			if (c != TC_IN_EOF)
				ungetch(lx, c);
			break;
		}
		if (c == 0) {
			r = nul_byte(lx);
			goto out;
		}
		size_t len = tam_str_len(lx->word);
		if (c == '=' && name_so_far && len > 0 && !(flags & TC_WORD_QUOTED))
			flags |= TC_WORD_ASSIGN;
		if (!is_name_char(c, len == 0))
			name_so_far = 0;

		if ((r = raw_add(lx, c)) != TC_OK)
			goto out;

		switch (c) {
		case '\'':
			flags |= TC_WORD_QUOTED;
			r = lex_single(lx, &wb, true, lx->in->line);
			break;
		case '"':
			flags |= TC_WORD_QUOTED;
			r = lex_double(lx, &wb, true, &flags, 0, lx->in->line);
			break;
		case '\\': {
			flags |= TC_WORD_QUOTED;
			int d = getch(lx);
			if (d < TC_IN_EOF) {
				r = input_failure(lx, d);
				goto out;
			}
			if (d == 0) {
				r = nul_byte(lx);
				goto out;
			}
			if (d != TC_IN_EOF) {
				if ((r = raw_add(lx, d)) != TC_OK)
					goto out;
				r = wb_add_char(&wb, TC_WPART_QUOTED, d);
			}
			break;
		}
		case '$':
			r = lex_dollar(lx, &wb, false, true, &flags, 0, lx->in->line);
			break;
		case '`':
			flags |= TC_WORD_BACKQ;
			r = wb_add_char(&wb, TC_WPART_LITERAL, c);
			break;
		case '*': case '?': case '[':
			flags |= TC_WORD_GLOB;
			r = wb_add_char(&wb, TC_WPART_LITERAL, c);
			break;
		case '~':
			if (len == 0)
				flags |= TC_WORD_TILDE;
			r = wb_add_char(&wb, TC_WPART_LITERAL, c);
			break;
		default:
			r = wb_add_char(&wb, TC_WPART_LITERAL, c);
			break;
		}
		if (r != TC_OK)
			goto out;
	}

	if ((r = wb_flush(&wb)) != TC_OK)
		goto out;

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
	tok->parts = wb.head;
	tok->text = tc_arena_strndup(&lx->sh->arena, lx->word, len);
	if (!tok->text) {
		tc_error(lx->sh, tok->line, "out of memory");
		r = TC_ERR_NOMEM;
		goto out;
	}
	r = TC_OK;

out:
	wb_finish(&wb);
	return r;
}

int tc_lex_next(struct tc_lexer *lx, struct tc_token *tok)
{
	tok->text = NULL;
	tok->len = 0;
	tok->parts = NULL;
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
			tok->line--;
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

/* Parse an unquoted here-document body into tc_wpart list. */
static int lex_heredoc_parts(struct tc_shell *sh, const char *body, size_t blen,
			     unsigned long start_line, struct tc_wpart **out)
{
	struct tc_input hin;
	tc_input_from_string(&hin, body, blen);
	hin.line = start_line;
	struct tc_lexer hlx;
	int r = tc_lexer_init(&hlx, sh, &hin);
	if (r != TC_OK)
		return r;

	struct wpart_buf wb;
	r = wb_init(&wb, &hlx);
	if (r != TC_OK) {
		tc_lexer_finish(&hlx);
		return r;
	}
	unsigned flags = 0;

	for (;;) {
		int c = getch_cont(&hlx);
		if (c == TC_IN_EOF)
			break;
		if (c < TC_IN_EOF) {
			r = input_failure(&hlx, c);
			goto out;
		}
		if (c == '\\') {
			int d = getch(&hlx);
			if (d == TC_IN_EOF) {
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, '\\')) != TC_OK)
					goto out;
				break;
			}
			if (d == '$' || d == '`' || d == '\\') {
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, d)) != TC_OK)
					goto out;
			} else {
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, '\\')) != TC_OK)
					goto out;
				if ((r = wb_add_char(&wb, TC_WPART_QUOTED, d)) != TC_OK)
					goto out;
			}
		} else if (c == '$') {
			r = lex_dollar(&hlx, &wb, true, false, &flags, 0, hin.line);
			if (r != TC_OK)
				goto out;
		} else if (c == '`') {
			flags |= TC_WORD_BACKQ;
			if ((r = wb_add_char(&wb, TC_WPART_QUOTED, c)) != TC_OK)
				goto out;
		} else {
			if ((r = wb_add_char(&wb, TC_WPART_QUOTED, c)) != TC_OK)
				goto out;
		}
	}

	if (flags & (TC_WORD_CMDSUB | TC_WORD_ARITH | TC_WORD_BACKQ)) {
		tc_error(sh, start_line,
			 "command or arithmetic substitutions inside here-documents are not supported yet; see ROADMAP.md");
		r = TC_ERR_SYNTAX;
		goto out;
	}
	if ((r = wb_flush(&wb)) == TC_OK)
		*out = wb.head;

out:
	wb_finish(&wb);
	tc_lexer_finish(&hlx);
	return r;
}

int tc_lex_read_heredoc(struct tc_lexer *lx, const char *delim, bool strip_tabs,
			bool quoted, unsigned long start_line,
			const char **out_body, size_t *out_len,
			struct tc_wpart **out_parts)
{
	*out_body = "";
	*out_len = 0;
	if (out_parts)
		*out_parts = NULL;
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
			int c = getch(lx);
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

		for (size_t i = 0; i < slen; i++) {
			if ((r = hd_append(lx, &body, s[i])) != TC_OK)
				goto out;
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
	if (!quoted && out_parts) {
		r = lex_heredoc_parts(lx->sh, body, blen, start_line, out_parts);
		if (r != TC_OK)
			goto out;
	} else {
		char *copy = tc_arena_strndup(&lx->sh->arena, body, blen);
		if (!copy) {
			tc_error(lx->sh, start_line, "out of memory");
			r = TC_ERR_NOMEM;
			goto out;
		}
		*out_body = copy;
		*out_len = blen;
	}

out:
	tam_str_free(body);
	tam_str_free(line);
	return r;
}
