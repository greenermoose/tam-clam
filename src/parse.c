/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Recursive descent over the POSIX grammar (XCU 2.10.2). In 0.0.1 the
 * grammar is:
 *
 *   complete_command : list? (NEWLINE | END)
 *   list             : simple_command (';' simple_command)* ';'?
 *   simple_command   : WORD+
 *
 * Valid shell syntax that tam-clam does not support yet is refused with a
 * clear message, never run some other way. */
#include <string.h>

#include "tam-clam/input.h"
#include "tam-clam/parse.h"

int tc_parser_init(struct tc_parser *p, struct tc_shell *sh, struct tc_input *in)
{
	p->sh = sh;
	p->have_tok = 0;
	return tc_lexer_init(&p->lx, sh, in);
}

void tc_parser_finish(struct tc_parser *p)
{
	tc_lexer_finish(&p->lx);
}

static int advance(struct tc_parser *p)
{
	int r = tc_lex_next(&p->lx, &p->tok);
	p->have_tok = (r == TC_OK);
	return r;
}

static int unsupported(struct tc_parser *p, unsigned long line, const char *what)
{
	tc_error(p->sh, line, "%s %s not supported yet; see ROADMAP.md", what,
		 strchr(what, ' ') ? "are" : "is");
	return TC_ERR_SYNTAX;
}

static int unexpected(struct tc_parser *p)
{
	switch (p->tok.kind) {
	case TC_TOK_PIPE:
		return unsupported(p, p->tok.line, "pipelines (`|')");
	case TC_TOK_AND_IF:
	case TC_TOK_OR_IF:
		return unsupported(p, p->tok.line, "and-or lists (`&&', `||')");
	case TC_TOK_AMP:
		return unsupported(p, p->tok.line, "background commands (`&')");
	case TC_TOK_LPAREN:
		return unsupported(p, p->tok.line, "subshells (`(')");
	case TC_TOK_LESS: case TC_TOK_GREAT: case TC_TOK_DLESS:
	case TC_TOK_DGREAT: case TC_TOK_LESSAND: case TC_TOK_GREATAND:
	case TC_TOK_LESSGREAT: case TC_TOK_DLESSDASH: case TC_TOK_CLOBBER:
		return unsupported(p, p->tok.line, "redirections");
	default:
		break;
	}
	tc_error(p->sh, p->tok.line, "syntax error: unexpected %s%s%s",
		 p->tok.kind == TC_TOK_WORD ? "`" : "",
		 p->tok.kind == TC_TOK_WORD ? p->tok.text : tc_token_name(p->tok.kind),
		 p->tok.kind == TC_TOK_WORD ? "'" : "");
	return TC_ERR_SYNTAX;
}

static int is_reserved(const struct tc_token *t)
{
	static const char *const words[] = {
		"!", "{", "}", "case", "do", "done", "elif", "else", "esac",
		"fi", "for", "if", "in", "then", "until", "while",
	};
	if (t->flags & TC_WORD_QUOTED)
		return 0;
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (strcmp(t->text, words[i]) == 0)
			return 1;
	return 0;
}

/* Refuse words that need expansions tam-clam does not do yet. */
static int check_word(struct tc_parser *p, const struct tc_token *t, int prefix)
{
	if (prefix && (t->flags & TC_WORD_ASSIGN))
		return unsupported(p, t->line, "variable assignments");
	if (t->flags & TC_WORD_DOLLAR)
		return unsupported(p, t->line, "expansions starting with `$'");
	if (t->flags & TC_WORD_BACKQ)
		return unsupported(p, t->line, "command substitutions (`...`)");
	if (t->flags & TC_WORD_GLOB)
		return unsupported(p, t->line, "patterns (`*', `?', `[')");
	if (t->flags & TC_WORD_TILDE)
		return unsupported(p, t->line, "tilde expansions (`~')");
	return TC_OK;
}

struct word_link {
	struct tc_word w;
	struct word_link *next;
};

static int parse_simple(struct tc_parser *p, struct tc_node **out)
{
	if (p->tok.kind != TC_TOK_WORD)
		return unexpected(p);
	if (is_reserved(&p->tok)) {
		tc_error(p->sh, p->tok.line,
			 "the reserved word `%s' is not supported yet; see ROADMAP.md",
			 p->tok.text);
		return TC_ERR_SYNTAX;
	}

	struct tc_arena *a = &p->sh->arena;
	struct word_link *head = NULL, **tail = &head;
	size_t n = 0;
	unsigned long line = p->tok.line;
	int r;
	while (p->tok.kind == TC_TOK_WORD) {
		if ((r = check_word(p, &p->tok, n == 0)) != TC_OK)
			return r;
		if (n == TC_WORDS_MAX) {
			tc_error(p->sh, p->tok.line, "more than %d words in one command",
				 TC_WORDS_MAX);
			return TC_ERR_NOMEM;
		}
		struct word_link *l = tc_arena_alloc(a, sizeof(*l));
		if (!l)
			goto nomem;
		l->w.text = p->tok.text;
		l->w.len = p->tok.len;
		l->w.flags = p->tok.flags;
		l->w.line = p->tok.line;
		l->next = NULL;
		*tail = l;
		tail = &l->next;
		n++;
		if ((r = advance(p)) != TC_OK)
			return r;
	}

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_word *words = tc_arena_alloc(a, n * sizeof(*words));
	if (!node || !words)
		goto nomem;
	size_t i = 0;
	for (struct word_link *l = head; l; l = l->next)
		words[i++] = l->w;
	node->kind = TC_NODE_SIMPLE;
	node->line = line;
	node->u.simple.words = words;
	node->u.simple.nwords = n;
	*out = node;
	return TC_OK;

nomem:
	tc_error(p->sh, p->tok.line, "out of memory");
	return TC_ERR_NOMEM;
}

/* Builds a right-leaning chain of TC_NODE_SEQ without recursion, so a long
 * line of commands cannot exhaust the stack. */
static int parse_list(struct tc_parser *p, struct tc_node **out)
{
	struct tc_node *first;
	int r = parse_simple(p, &first);
	if (r != TC_OK)
		return r;
	*out = first;
	struct tc_node **slot = out;

	while (p->tok.kind == TC_TOK_SEMI) {
		if ((r = advance(p)) != TC_OK)
			return r;
		if (p->tok.kind == TC_TOK_NEWLINE || p->tok.kind == TC_TOK_END)
			break;
		struct tc_node *next;
		if ((r = parse_simple(p, &next)) != TC_OK)
			return r;
		struct tc_node *seq = tc_arena_alloc(&p->sh->arena, sizeof(*seq));
		if (!seq) {
			tc_error(p->sh, p->tok.line, "out of memory");
			return TC_ERR_NOMEM;
		}
		seq->kind = TC_NODE_SEQ;
		seq->line = (*slot)->line;
		seq->u.seq.first = *slot;
		seq->u.seq.rest = next;
		*slot = seq;
		slot = &seq->u.seq.rest;
	}
	return TC_OK;
}

int tc_parse_next(struct tc_parser *p, struct tc_node **out)
{
	*out = NULL;
	tc_input_new_command(p->lx.in);
	int r = advance(p);
	if (r != TC_OK)
		return r;
	if (p->tok.kind == TC_TOK_END)
		return TC_END;
	if (p->tok.kind == TC_TOK_NEWLINE)
		return TC_OK;
	if ((r = parse_list(p, out)) != TC_OK)
		return r;
	if (p->tok.kind != TC_TOK_NEWLINE && p->tok.kind != TC_TOK_END)
		return unexpected(p);
	return TC_OK;
}

void tc_parser_recover(struct tc_parser *p)
{
	if (p->have_tok &&
	    (p->tok.kind == TC_TOK_NEWLINE || p->tok.kind == TC_TOK_END))
		return;
	tc_lex_skip_line(&p->lx);
	p->have_tok = 0;
}

void tc_parser_reset(struct tc_parser *p)
{
	tc_lexer_reset(&p->lx);
	p->have_tok = 0;
}
