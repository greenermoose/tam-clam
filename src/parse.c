/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Recursive descent over the POSIX grammar (XCU 2.10.2):
 *
 *   complete_command : list? (NEWLINE | END)
 *   list             : and_or (';' and_or)* ';'?
 *   and_or           : pipeline (('&&' | '||') linebreak pipeline)*
 *   pipeline         : '!'? command ('|' linebreak command)*
 *   command          : simple_command
 *                    | compound_command redirect_list?
 *                    | function_definition
 *   compound_command : brace_group | subshell | if | while | until | for | case
 *
 * Valid shell syntax that tam-clam does not support yet is refused with a
 * clear message, never run some other way. */
#include <limits.h>
#include <string.h>

#include "tam-clam/expand.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"

struct tc_heredoc_pending {
	struct tc_redir *redir;
	const char *delim;
	struct tc_heredoc_pending *next;
};

int tc_parser_init(struct tc_parser *p, struct tc_shell *sh, struct tc_input *in)
{
	p->sh = sh;
	p->have_tok = 0;
	p->have_peek = 0;
	p->alias_next = false;
	p->depth = 0;
	p->hd_head = NULL;
	p->hd_tail = NULL;
	return tc_lexer_init(&p->lx, sh, in);
}

void tc_parser_finish(struct tc_parser *p)
{
	tc_lexer_finish(&p->lx);
}

static int advance(struct tc_parser *p)
{
	if (p->have_peek) {
		p->tok = p->peek_tok;
		p->have_peek = 0;
		p->have_tok = 1;
		return TC_OK;
	}
	int r = tc_lex_next(&p->lx, &p->tok);
	p->have_tok = (r == TC_OK);
	return r;
}

static int peek_next(struct tc_parser *p)
{
	if (p->have_peek)
		return TC_OK;
	int r = tc_lex_next(&p->lx, &p->peek_tok);
	if (r != TC_OK)
		return r;
	p->have_peek = 1;
	return TC_OK;
}

static int flush_heredocs(struct tc_parser *p)
{
	while (p->hd_head) {
		struct tc_heredoc_pending *hd = p->hd_head;
		p->hd_head = hd->next;
		if (!p->hd_head)
			p->hd_tail = NULL;
		int r = tc_lex_read_heredoc(&p->lx, hd->delim,
					    hd->redir->strip_tabs,
					    hd->redir->heredoc_quoted,
					    hd->redir->line,
					    &hd->redir->heredoc_body,
					    &hd->redir->heredoc_len,
					    &hd->redir->heredoc_parts);
		if (r != TC_OK)
			return r;
	}
	return TC_OK;
}

static int consume_newline(struct tc_parser *p)
{
	int r = flush_heredocs(p);
	if (r != TC_OK)
		return r;
	return advance(p);
}

static int parse_linebreak(struct tc_parser *p)
{
	while (p->tok.kind == TC_TOK_NEWLINE) {
		int r = consume_newline(p);
		if (r != TC_OK)
			return r;
	}
	return TC_OK;
}

static int enter_depth(struct tc_parser *p, unsigned long line)
{
	if (p->depth >= TC_PARSE_DEPTH_MAX) {
		tc_error(p->sh, line, "nesting depth exceeds limit (%d)",
			 TC_PARSE_DEPTH_MAX);
		return TC_ERR_SYNTAX;
	}
	p->depth++;
	p->lx.parse_depth = p->depth;
	return TC_OK;
}

static void leave_depth(struct tc_parser *p)
{
	if (p->depth > 0)
		p->depth--;
	p->lx.parse_depth = p->depth;
}

static int unexpected(struct tc_parser *p)
{
	tc_error(p->sh, p->tok.line, "syntax error: unexpected %s%s%s",
		 p->tok.kind == TC_TOK_WORD ? "`" : "",
		 p->tok.kind == TC_TOK_WORD ? p->tok.text : tc_token_name(p->tok.kind),
		 p->tok.kind == TC_TOK_WORD ? "'" : "");
	return TC_ERR_SYNTAX;
}

static bool is_word_eq(const struct tc_token *t, const char *kw)
{
	return t->kind == TC_TOK_WORD && !(t->flags & TC_WORD_QUOTED) &&
	       strcmp(t->text, kw) == 0;
}

bool tc_is_reserved_word(const char *s)
{
	static const char *const words[] = {
		"!", "[[", "]]", "{", "}", "case", "do", "done", "elif", "else",
		"esac", "fi", "for", "if", "in", "then", "until", "while",
	};
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (strcmp(s, words[i]) == 0)
			return true;
	return false;
}

static int is_reserved(const struct tc_token *t)
{
	if (t->kind != TC_TOK_WORD || (t->flags & TC_WORD_QUOTED))
		return 0;
	return tc_is_reserved_word(t->text) ? 1 : 0;
}

static int expand_alias_at_tok(struct tc_parser *p)
{
	for (;;) {
		if (p->have_peek || p->tok.kind != TC_TOK_WORD ||
		    (p->tok.flags & TC_WORD_QUOTED) ||
		    (p->tok.flags & TC_WORD_ASSIGN) ||
		    is_reserved(&p->tok))
			return TC_OK;

		const char *val = tc_alias_get(&p->sh->aliases, p->tok.text);
		if (!val || tc_input_alias_active(p->lx.in, p->tok.text))
			return TC_OK;

		if (p->lx.peek >= 0) {
			if (tc_input_ungetc_byte(p->lx.in, (unsigned char)p->lx.peek) < 0) {
				tc_error(p->sh, p->tok.line, "out of memory");
				return TC_ERR_NOMEM;
			}
			tc_lexer_reset(&p->lx);
		}
		size_t vlen = strlen(val);
		int pr = tc_input_push_alias(p->lx.in, p->tok.text, val, vlen);
		if (pr == -2) {
			tc_error(p->sh, p->tok.line,
				 "alias expansion depth exceeds limit (%d)",
				 TC_ALIAS_DEPTH_MAX);
			return TC_ERR_SYNTAX;
		}
		if (pr < 0) {
			tc_error(p->sh, p->tok.line, "out of memory");
			return TC_ERR_NOMEM;
		}
		int r = advance(p);
		if (r != TC_OK)
			return r;
	}
}

static bool is_compound_end(const struct tc_token *t)
{
	if (t->kind == TC_TOK_END || t->kind == TC_TOK_RPAREN ||
	    t->kind == TC_TOK_DSEMI)
		return true;
	if (t->kind != TC_TOK_WORD || (t->flags & TC_WORD_QUOTED))
		return false;
	return strcmp(t->text, "then") == 0 || strcmp(t->text, "elif") == 0 ||
	       strcmp(t->text, "else") == 0 || strcmp(t->text, "fi") == 0 ||
	       strcmp(t->text, "do") == 0 || strcmp(t->text, "done") == 0 ||
	       strcmp(t->text, "esac") == 0 || strcmp(t->text, "}") == 0;
}

static int is_redir_op(enum tc_token_kind k)
{
	switch (k) {
	case TC_TOK_LESS:
	case TC_TOK_GREAT:
	case TC_TOK_CLOBBER:
	case TC_TOK_DGREAT:
	case TC_TOK_LESSGREAT:
	case TC_TOK_LESSAND:
	case TC_TOK_GREATAND:
	case TC_TOK_DLESS:
	case TC_TOK_DLESSDASH:
		return 1;
	default:
		return 0;
	}
}

static void tok_to_word(const struct tc_token *t, struct tc_word *w)
{
	w->text = t->text;
	w->len = t->len;
	w->parts = t->parts;
	w->flags = t->flags;
	w->decl_assign = NULL;
	w->line = t->line;
	w->col = t->col;
}

static int parse_io_number(struct tc_parser *p, const struct tc_token *t, int *out)
{
	unsigned long v = 0;
	for (size_t i = 0; i < t->len; i++) {
		unsigned d = (unsigned)(t->text[i] - '0');
		if (v > ((unsigned long)INT_MAX - d) / 10) {
			tc_error(p->sh, t->line, "file descriptor number out of range: %s",
				 t->text);
			return TC_ERR_SYNTAX;
		}
		v = v * 10 + d;
	}
	*out = (int)v;
	return TC_OK;
}

static enum tc_redir_kind token_to_redir(enum tc_token_kind k)
{
	switch (k) {
	case TC_TOK_LESS:      return TC_REDIR_IN;
	case TC_TOK_GREAT:     return TC_REDIR_OUT;
	case TC_TOK_CLOBBER:   return TC_REDIR_CLOBBER;
	case TC_TOK_DGREAT:    return TC_REDIR_APPEND;
	case TC_TOK_LESSGREAT: return TC_REDIR_RDWR;
	case TC_TOK_LESSAND:   return TC_REDIR_DUP_IN;
	case TC_TOK_GREATAND:  return TC_REDIR_DUP_OUT;
	default:               return TC_REDIR_HEREDOC;
	}
}

struct word_link {
	struct tc_word w;
	struct word_link *next;
};

struct assign_link {
	struct tc_assign a;
	struct assign_link *next;
};

struct redir_link {
	struct tc_redir r;
	struct tc_heredoc_pending *hd;
	struct redir_link *next;
};

struct node_link {
	struct tc_node *node;
	struct node_link *next;
};

struct andor_link {
	struct tc_andor_item item;
	struct andor_link *next;
};

struct if_link {
	struct tc_if_branch b;
	struct if_link *next;
};

struct case_link {
	struct tc_case_item item;
	struct case_link *next;
};

static int parse_and_or(struct tc_parser *p, struct tc_node **out);
static int parse_compound_list(struct tc_parser *p, struct tc_node **out);
static int parse_compound_with_redirs(struct tc_parser *p, struct tc_node **out);

static int slice_wparts_range(struct tc_arena *a,
			      const struct tc_wpart *start_wp, size_t start_off,
			      const struct tc_wpart *end_wp, size_t end_off,
			      struct tc_wpart **out)
{
	*out = NULL;
	struct tc_wpart *head = NULL, **tail = &head;
	for (const struct tc_wpart *wp = start_wp; wp; wp = wp->next) {
		size_t lo = (wp == start_wp) ? start_off : 0;
		size_t hi = (wp == end_wp) ? end_off : wp->len;
		if (wp->kind == TC_WPART_LITERAL) {
			if (hi > lo) {
				struct tc_wpart *c = tc_arena_alloc(a, sizeof(*c));
				if (!c)
					return TC_ERR_NOMEM;
				*c = *wp;
				c->text = wp->text + lo;
				c->len = hi - lo;
				c->next = NULL;
				*tail = c;
				tail = &c->next;
			}
		} else {
			struct tc_wpart *c = tc_arena_alloc(a, sizeof(*c));
			if (!c)
				return TC_ERR_NOMEM;
			*c = *wp;
			c->next = NULL;
			*tail = c;
			tail = &c->next;
		}
		if (wp == end_wp)
			break;
	}
	*out = head;
	return TC_OK;
}

static int slice_wparts_after(struct tc_arena *a,
			      const struct tc_wpart *wp, size_t off,
			      struct tc_wpart **out)
{
	if (!wp) {
		*out = NULL;
		return TC_OK;
	}
	if (off < wp->len) {
		struct tc_wpart *head = tc_arena_alloc(a, sizeof(*head));
		if (!head)
			return TC_ERR_NOMEM;
		*head = *wp;
		head->text = wp->text + off;
		head->len = wp->len - off;
		*out = head;
		return TC_OK;
	}
	*out = wp->next;
	return TC_OK;
}

static int split_assign(struct tc_parser *p, const struct tc_token *t,
			struct tc_assign *out)
{
	memset(out, 0, sizeof(*out));
	struct tc_arena *a = &p->sh->arena;
	const struct tc_wpart *first = t->parts;
	if (!first || first->kind != TC_WPART_LITERAL || first->len == 0) {
		tc_error(p->sh, t->line, "internal error: malformed assignment");
		return TC_ERR_SYNTAX;
	}

	size_t nlen = 0;
	while (nlen < first->len &&
	       tc_var_valid_name(first->text, nlen + 1))
		nlen++;
	if (nlen == 0 || nlen >= first->len) {
		tc_error(p->sh, t->line, "internal error: malformed assignment");
		return TC_ERR_SYNTAX;
	}
	char *name = tc_arena_strndup(a, first->text, nlen);
	if (!name) {
		tc_error(p->sh, t->line, "out of memory");
		return TC_ERR_NOMEM;
	}

	const struct tc_wpart *eq_wp = first;
	size_t eq_off = nlen;
	bool has_sub = false;
	struct tc_wpart *sub_parts = NULL;
	bool append = false;

	if (first->text[nlen] == '[') {
		has_sub = true;
		int bdepth = 1;
		const struct tc_wpart *wp = first;
		size_t pos = nlen + 1;
		bool found = false;
		while (wp && !found) {
			if (wp->kind == TC_WPART_LITERAL) {
				while (pos < wp->len) {
					if (wp->text[pos] == '[') {
						bdepth++;
					} else if (wp->text[pos] == ']') {
						bdepth--;
						if (bdepth == 0) {
							if (slice_wparts_range(a, first, nlen + 1,
									       wp, pos, &sub_parts) != TC_OK) {
								tc_error(p->sh, t->line, "out of memory");
								return TC_ERR_NOMEM;
							}
							eq_wp = wp;
							eq_off = pos + 1;
							found = true;
							break;
						}
					}
					pos++;
				}
			}
			if (!found) {
				wp = wp->next;
				pos = 0;
			}
		}
		if (!found || eq_off >= eq_wp->len) {
			tc_error(p->sh, t->line, "internal error: malformed subscript assignment");
			return TC_ERR_SYNTAX;
		}
	}

	if (eq_wp->text[eq_off] == '+' && eq_off + 1 < eq_wp->len &&
	    eq_wp->text[eq_off + 1] == '=') {
		append = true;
		eq_off += 2;
	} else if (eq_wp->text[eq_off] == '=') {
		eq_off += 1;
	} else {
		tc_error(p->sh, t->line, "internal error: malformed assignment");
		return TC_ERR_SYNTAX;
	}

	struct tc_wpart *vparts = NULL;
	if (slice_wparts_after(a, eq_wp, eq_off, &vparts) != TC_OK) {
		tc_error(p->sh, t->line, "out of memory");
		return TC_ERR_NOMEM;
	}

	/* Compute raw RHS offset in t->text (find first '=' after prefix). */
	const char *raw_eq = memchr(t->text + nlen, '=', t->len - nlen);
	size_t raw_off = raw_eq ? (size_t)(raw_eq - t->text) + 1 : t->len;

	out->name = name;
	out->append = append;
	out->has_sub = has_sub;
	out->sub = sub_parts;
	out->is_compound = false;
	out->value.text = t->text + raw_off;
	out->value.len = t->len - raw_off;
	out->value.parts = vparts;
	out->value.flags = t->flags & ~(TC_WORD_ASSIGN | TC_WORD_ASSIGN_PLUS |
					TC_WORD_FOLLOWED_LP);
	out->value.decl_assign = NULL;
	out->value.line = t->line;
	out->value.col = t->col + raw_off;
	out->line = t->line;
	out->col = t->col;
	return TC_OK;
}

/* Split a compound array item word of the form [sub]=val or [sub]+=val. */
static int split_array_item(struct tc_parser *p, const struct tc_token *t,
			    struct tc_array_item *item)
{
	memset(item, 0, sizeof(*item));
	item->line = t->line;
	item->col = t->col;

	struct tc_arena *a = &p->sh->arena;
	const struct tc_wpart *first = t->parts;
	if (first && first->kind == TC_WPART_LITERAL && first->len > 0 &&
	    first->text[0] == '[') {
		int bdepth = 1;
		const struct tc_wpart *wp = first;
		size_t pos = 1;
		bool has_content = false;
		while (wp) {
			if (wp->kind == TC_WPART_LITERAL) {
				while (pos < wp->len) {
					if (wp->text[pos] == '[') {
						bdepth++;
						has_content = true;
					} else if (wp->text[pos] == ']') {
						bdepth--;
						if (bdepth == 0) {
							size_t after = pos + 1;
							bool is_eq = (after < wp->len && wp->text[after] == '=');
							bool is_peq = (after + 1 < wp->len &&
								       wp->text[after] == '+' &&
								       wp->text[after + 1] == '=');
							if (has_content && (is_eq || is_peq)) {
								if (slice_wparts_range(a, first, 1, wp, pos,
										       &item->sub) != TC_OK) {
									tc_error(p->sh, t->line, "out of memory");
									return TC_ERR_NOMEM;
								}
								size_t voff = after + (is_peq ? 2 : 1);
								struct tc_wpart *vparts = NULL;
								if (slice_wparts_after(a, wp, voff, &vparts) != TC_OK) {
									tc_error(p->sh, t->line, "out of memory");
									return TC_ERR_NOMEM;
								}
								const char *raw_eq = memchr(t->text, '=', t->len);
								size_t raw_off = raw_eq ? (size_t)(raw_eq - t->text) + 1 : t->len;
								item->has_sub = true;
								item->append = is_peq;
								item->value.text = t->text + raw_off;
								item->value.len = t->len - raw_off;
								item->value.parts = vparts;
								item->value.flags = t->flags & ~(TC_WORD_ASSIGN | TC_WORD_ASSIGN_PLUS |
												 TC_WORD_FOLLOWED_LP);
								item->value.decl_assign = NULL;
								item->value.line = t->line;
								item->value.col = t->col + raw_off;
								return TC_OK;
							}
							goto plain_word;
						}
						has_content = true;
					} else {
						has_content = true;
					}
					pos++;
				}
			} else {
				has_content = true;
			}
			wp = wp->next;
			pos = 0;
		}
	}

plain_word:
	tok_to_word(t, &item->value);
	return TC_OK;
}

struct item_link {
	struct tc_array_item item;
	struct item_link *next;
};

/* Parse compound array initializer '(' [NEWLINE | word]* ')' after NAME=/NAME+=.
 * Caller has already consumed the NAME= token; p->tok is TC_TOK_LPAREN. */
static int parse_compound_assign(struct tc_parser *p, struct tc_assign *out)
{
	struct tc_arena *a = &p->sh->arena;
	int r = advance(p); /* consume '(' */
	if (r != TC_OK)
		return r;

	struct item_link *ihead = NULL, **itail = &ihead;
	size_t ni = 0;
	for (;;) {
		while (p->tok.kind == TC_TOK_NEWLINE) {
			if ((r = advance(p)) != TC_OK)
				return r;
		}
		if (p->tok.kind == TC_TOK_RPAREN)
			break;
		if (p->tok.kind != TC_TOK_WORD)
			return unexpected(p);
		if (ni == TC_WORDS_MAX) {
			tc_error(p->sh, p->tok.line,
				 "more than %d elements in one array assignment",
				 TC_WORDS_MAX);
			return TC_ERR_NOMEM;
		}
		struct item_link *il = tc_arena_alloc(a, sizeof(*il));
		if (!il) {
			tc_error(p->sh, p->tok.line, "out of memory");
			return TC_ERR_NOMEM;
		}
		if ((r = split_array_item(p, &p->tok, &il->item)) != TC_OK)
			return r;
		il->next = NULL;
		*itail = il;
		itail = &il->next;
		ni++;
		if ((r = advance(p)) != TC_OK)
			return r;
	}

	if ((r = advance(p)) != TC_OK) /* consume ')' */
		return r;

	struct tc_array_item *items = ni ? tc_arena_alloc(a, ni * sizeof(*items)) : NULL;
	if (ni && !items) {
		tc_error(p->sh, out->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	size_t idx = 0;
	for (struct item_link *il = ihead; il; il = il->next)
		items[idx++] = il->item;

	out->is_compound = true;
	out->items = items;
	out->nitems = ni;
	return TC_OK;
}

static bool is_decl_builtin_word(const struct tc_word *w)
{
	if (w->flags & (TC_WORD_QUOTED | TC_WORD_DOLLAR | TC_WORD_CMDSUB |
			TC_WORD_BACKQ | TC_WORD_ARITH))
		return false;
	return strcmp(w->text, "declare") == 0 ||
	       strcmp(w->text, "typeset") == 0 ||
	       strcmp(w->text, "local") == 0 ||
	       strcmp(w->text, "readonly") == 0 ||
	       strcmp(w->text, "export") == 0;
}

static int parse_redir(struct tc_parser *p, struct redir_link **out)
{
	int fd = -1;
	int r;
	unsigned long line = p->tok.line;
	if (p->tok.kind == TC_TOK_IO_NUMBER) {
		if ((r = parse_io_number(p, &p->tok, &fd)) != TC_OK)
			return r;
		if ((r = advance(p)) != TC_OK)
			return r;
	}
	if (!is_redir_op(p->tok.kind))
		return unexpected(p);

	enum tc_token_kind op = p->tok.kind;
	if ((r = advance(p)) != TC_OK)
		return r;
	if (p->tok.kind != TC_TOK_WORD)
		return unexpected(p);

	enum tc_redir_kind kind = token_to_redir(op);

	struct tc_arena *a = &p->sh->arena;
	struct redir_link *rl = tc_arena_alloc(a, sizeof(*rl));
	if (!rl)
		goto nomem;
	memset(rl, 0, sizeof(*rl));
	rl->r.kind = kind;
	rl->r.fd = fd;
	tok_to_word(&p->tok, &rl->r.target);
	rl->r.strip_tabs = (op == TC_TOK_DLESSDASH);
	rl->r.heredoc_quoted = (p->tok.flags & TC_WORD_QUOTED) != 0;
	rl->r.line = line;

	if (kind == TC_REDIR_HEREDOC) {
		char *delim = tc_expand_delim(a, &rl->r.target);
		struct tc_heredoc_pending *hd = tc_arena_alloc(a, sizeof(*hd));
		if (!delim || !hd)
			goto nomem;
		hd->redir = &rl->r;
		hd->delim = delim;
		hd->next = NULL;
		if (p->hd_tail)
			p->hd_tail->next = hd;
		else
			p->hd_head = hd;
		p->hd_tail = hd;
		rl->hd = hd;
	}

	if ((r = advance(p)) != TC_OK)
		return r;
	*out = rl;
	return TC_OK;

nomem:
	tc_error(p->sh, line, "out of memory");
	return TC_ERR_NOMEM;
}

static int parse_simple(struct tc_parser *p, struct tc_node **out)
{
	if (p->tok.kind != TC_TOK_WORD && p->tok.kind != TC_TOK_IO_NUMBER &&
	    !is_redir_op(p->tok.kind))
		return unexpected(p);

	struct tc_arena *a = &p->sh->arena;
	struct assign_link *ahead = NULL, **atail = &ahead;
	struct word_link *whead = NULL, **wtail = &whead;
	struct redir_link *rhead = NULL, **rtail = &rhead;
	size_t na = 0, nw = 0, nr = 0;
	unsigned long line = p->tok.line;
	unsigned long col = p->tok.col;
	int r;

	for (;;) {
		if (p->tok.kind == TC_TOK_IO_NUMBER || is_redir_op(p->tok.kind)) {
			struct redir_link *rl;
			if ((r = parse_redir(p, &rl)) != TC_OK)
				return r;
			*rtail = rl;
			rtail = &rl->next;
			nr++;
		} else if (p->tok.kind == TC_TOK_WORD) {
			if (nw == 0 && (p->tok.flags & TC_WORD_ASSIGN)) {
				if (na == TC_WORDS_MAX) {
					tc_error(p->sh, p->tok.line,
						 "more than %d assignments in one command",
						 TC_WORDS_MAX);
					return TC_ERR_NOMEM;
				}
				struct assign_link *al = tc_arena_alloc(a, sizeof(*al));
				if (!al)
					goto nomem;
				bool followed_lp = (p->tok.flags & TC_WORD_FOLLOWED_LP) != 0;
				if ((r = split_assign(p, &p->tok, &al->a)) != TC_OK)
					return r;
				al->next = NULL;
				*atail = al;
				atail = &al->next;
				na++;
				if ((r = advance(p)) != TC_OK)
					return r;
				if (followed_lp && p->tok.kind == TC_TOK_LPAREN &&
				    al->a.value.len == 0 && !al->a.has_sub) {
					if ((r = parse_compound_assign(p, &al->a)) != TC_OK)
						return r;
				}
				continue;
			}
			if (nw == 0 && na == 0 && nr == 0 && is_reserved(&p->tok))
				return unexpected(p);
			if (nw == TC_WORDS_MAX) {
				tc_error(p->sh, p->tok.line,
					 "more than %d words in one command",
					 TC_WORDS_MAX);
				return TC_ERR_NOMEM;
			}
			struct word_link *wl = tc_arena_alloc(a, sizeof(*wl));
			if (!wl)
				goto nomem;
			tok_to_word(&p->tok, &wl->w);
			wl->next = NULL;
			bool decl_compound = (nw > 0 && whead &&
					      is_decl_builtin_word(&whead->w) &&
					      (p->tok.flags & TC_WORD_ASSIGN) &&
					      (p->tok.flags & TC_WORD_FOLLOWED_LP));
			struct tc_token saved_tok = p->tok;
			*wtail = wl;
			wtail = &wl->next;
			nw++;
			if ((r = advance(p)) != TC_OK)
				return r;
			if (decl_compound && p->tok.kind == TC_TOK_LPAREN) {
				struct tc_assign *da = tc_arena_alloc(a, sizeof(*da));
				if (!da)
					goto nomem;
				if ((r = split_assign(p, &saved_tok, da)) != TC_OK)
					return r;
				if (da->value.len == 0 && !da->has_sub) {
					if ((r = parse_compound_assign(p, da)) != TC_OK)
						return r;
					wl->w.decl_assign = da;
				}
			}
			if (p->lx.in->alias_ended_blank) {
				p->lx.in->alias_ended_blank = false;
				if ((r = expand_alias_at_tok(p)) != TC_OK)
					return r;
			}
		} else {
			break;
		}
	}
	p->lx.in->alias_ended_blank = false;

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_assign *assigns = na ? tc_arena_alloc(a, na * sizeof(*assigns)) : NULL;
	struct tc_word *words = nw ? tc_arena_alloc(a, nw * sizeof(*words)) : NULL;
	struct tc_redir *redirs = nr ? tc_arena_alloc(a, nr * sizeof(*redirs)) : NULL;
	if (!node || (na && !assigns) || (nw && !words) || (nr && !redirs))
		goto nomem;
	size_t i = 0;
	for (struct assign_link *al = ahead; al; al = al->next)
		assigns[i++] = al->a;
	i = 0;
	for (struct word_link *wl = whead; wl; wl = wl->next)
		words[i++] = wl->w;
	i = 0;
	for (struct redir_link *rl = rhead; rl; rl = rl->next) {
		redirs[i] = rl->r;
		if (rl->hd)
			rl->hd->redir = &redirs[i];
		i++;
	}

	node->kind = TC_NODE_SIMPLE;
	node->line = line;
	node->col = col;
	node->u.simple.assigns = assigns;
	node->u.simple.nassigns = na;
	node->u.simple.words = words;
	node->u.simple.nwords = nw;
	node->u.simple.redirs = redirs;
	node->u.simple.nredirs = nr;
	*out = node;
	return TC_OK;

nomem:
	tc_error(p->sh, p->tok.line, "out of memory");
	return TC_ERR_NOMEM;
}

static int parse_group(struct tc_parser *p, struct tc_node **out)
{
	unsigned long line = p->tok.line;
	int r;
	if ((r = enter_depth(p, line)) != TC_OK)
		return r;
	if ((r = advance(p)) != TC_OK) {
		leave_depth(p);
		return r;
	}
	struct tc_node *body = NULL;
	if ((r = parse_compound_list(p, &body)) != TC_OK) {
		leave_depth(p);
		return r;
	}
	if (!is_word_eq(&p->tok, "}")) {
		leave_depth(p);
		return unexpected(p);
	}
	leave_depth(p);
	if ((r = advance(p)) != TC_OK)
		return r;

	struct tc_node *node = tc_arena_alloc(&p->sh->arena, sizeof(*node));
	if (!node) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	node->kind = TC_NODE_GROUP;
	node->line = line;
	node->u.group.body = body;
	*out = node;
	return TC_OK;
}

static int parse_subshell(struct tc_parser *p, struct tc_node **out)
{
	unsigned long line = p->tok.line;
	int r;
	if ((r = enter_depth(p, line)) != TC_OK)
		return r;
	if ((r = advance(p)) != TC_OK) {
		leave_depth(p);
		return r;
	}
	struct tc_node *body = NULL;
	if ((r = parse_compound_list(p, &body)) != TC_OK) {
		leave_depth(p);
		return r;
	}
	if (p->tok.kind != TC_TOK_RPAREN) {
		leave_depth(p);
		return unexpected(p);
	}
	leave_depth(p);
	if ((r = advance(p)) != TC_OK)
		return r;

	struct tc_node *node = tc_arena_alloc(&p->sh->arena, sizeof(*node));
	if (!node) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	node->kind = TC_NODE_SUBSHELL;
	node->line = line;
	node->u.group.body = body;
	*out = node;
	return TC_OK;
}

static int parse_if(struct tc_parser *p, struct tc_node **out)
{
	unsigned long line = p->tok.line;
	int r;
	if ((r = enter_depth(p, line)) != TC_OK)
		return r;

	struct tc_arena *a = &p->sh->arena;
	struct if_link *head = NULL, **tail = &head;
	size_t nb = 0;
	struct tc_node *else_body = NULL;

	do {
		if ((r = advance(p)) != TC_OK)
			goto err;
		struct tc_node *cond = NULL, *body = NULL;
		if ((r = parse_compound_list(p, &cond)) != TC_OK)
			goto err;
		if (!is_word_eq(&p->tok, "then")) {
			r = unexpected(p);
			goto err;
		}
		if ((r = advance(p)) != TC_OK)
			goto err;
		if ((r = parse_compound_list(p, &body)) != TC_OK)
			goto err;
		struct if_link *il = tc_arena_alloc(a, sizeof(*il));
		if (!il) {
			tc_error(p->sh, line, "out of memory");
			r = TC_ERR_NOMEM;
			goto err;
		}
		il->b.cond = cond;
		il->b.body = body;
		il->next = NULL;
		*tail = il;
		tail = &il->next;
		nb++;
	} while (is_word_eq(&p->tok, "elif"));

	if (is_word_eq(&p->tok, "else")) {
		if ((r = advance(p)) != TC_OK)
			goto err;
		if ((r = parse_compound_list(p, &else_body)) != TC_OK)
			goto err;
	}
	if (!is_word_eq(&p->tok, "fi")) {
		r = unexpected(p);
		goto err;
	}
	leave_depth(p);
	if ((r = advance(p)) != TC_OK)
		return r;

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_if_branch *br = tc_arena_alloc(a, nb * sizeof(*br));
	if (!node || !br) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	size_t i = 0;
	for (struct if_link *il = head; il; il = il->next)
		br[i++] = il->b;
	node->kind = TC_NODE_IF;
	node->line = line;
	node->u.if_cmd.branches = br;
	node->u.if_cmd.nbranches = nb;
	node->u.if_cmd.else_body = else_body;
	*out = node;
	return TC_OK;

err:
	leave_depth(p);
	return r;
}

static int parse_loop(struct tc_parser *p, bool until, struct tc_node **out)
{
	unsigned long line = p->tok.line;
	int r;
	if ((r = enter_depth(p, line)) != TC_OK)
		return r;
	if ((r = advance(p)) != TC_OK)
		goto err;

	struct tc_node *cond = NULL, *body = NULL;
	if ((r = parse_compound_list(p, &cond)) != TC_OK)
		goto err;
	if (!is_word_eq(&p->tok, "do")) {
		r = unexpected(p);
		goto err;
	}
	if ((r = advance(p)) != TC_OK)
		goto err;
	if ((r = parse_compound_list(p, &body)) != TC_OK)
		goto err;
	if (!is_word_eq(&p->tok, "done")) {
		r = unexpected(p);
		goto err;
	}
	leave_depth(p);
	if ((r = advance(p)) != TC_OK)
		return r;

	struct tc_node *node = tc_arena_alloc(&p->sh->arena, sizeof(*node));
	if (!node) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	node->kind = TC_NODE_LOOP;
	node->line = line;
	node->u.loop.until = until;
	node->u.loop.cond = cond;
	node->u.loop.body = body;
	*out = node;
	return TC_OK;

err:
	leave_depth(p);
	return r;
}

static int parse_for(struct tc_parser *p, struct tc_node **out)
{
	unsigned long line = p->tok.line;
	unsigned long col = p->tok.col;
	int r;
	if ((r = enter_depth(p, line)) != TC_OK)
		return r;
	if ((r = advance(p)) != TC_OK)
		goto err;

	if (p->tok.kind != TC_TOK_WORD || (p->tok.flags & TC_WORD_QUOTED) ||
	    !tc_var_valid_name(p->tok.text, p->tok.len)) {
		tc_error(p->sh, p->tok.line, "syntax error: invalid for variable name");
		r = TC_ERR_SYNTAX;
		goto err;
	}
	const char *name = p->tok.text;
	if ((r = advance(p)) != TC_OK)
		goto err;
	if ((r = parse_linebreak(p)) != TC_OK)
		goto err;

	struct tc_arena *a = &p->sh->arena;
	bool has_in = false;
	struct word_link *whead = NULL, **wtail = &whead;
	size_t nw = 0;

	if (is_word_eq(&p->tok, "in")) {
		has_in = true;
		if ((r = advance(p)) != TC_OK)
			goto err;
		while (p->tok.kind == TC_TOK_WORD) {
			if (nw == TC_WORDS_MAX) {
				tc_error(p->sh, p->tok.line,
					 "more than %d words in for list",
					 TC_WORDS_MAX);
				r = TC_ERR_NOMEM;
				goto err;
			}
			struct word_link *wl = tc_arena_alloc(a, sizeof(*wl));
			if (!wl) {
				tc_error(p->sh, p->tok.line, "out of memory");
				r = TC_ERR_NOMEM;
				goto err;
			}
			tok_to_word(&p->tok, &wl->w);
			wl->next = NULL;
			*wtail = wl;
			wtail = &wl->next;
			nw++;
			if ((r = advance(p)) != TC_OK)
				goto err;
		}
		if (p->tok.kind == TC_TOK_SEMI) {
			if ((r = advance(p)) != TC_OK)
				goto err;
		} else if (p->tok.kind == TC_TOK_NEWLINE) {
			if ((r = consume_newline(p)) != TC_OK)
				goto err;
		} else {
			r = unexpected(p);
			goto err;
		}
		if ((r = parse_linebreak(p)) != TC_OK)
			goto err;
	} else if (p->tok.kind == TC_TOK_SEMI) {
		if ((r = advance(p)) != TC_OK)
			goto err;
		if ((r = parse_linebreak(p)) != TC_OK)
			goto err;
	}

	if (!is_word_eq(&p->tok, "do")) {
		r = unexpected(p);
		goto err;
	}
	if ((r = advance(p)) != TC_OK)
		goto err;
	struct tc_node *body = NULL;
	if ((r = parse_compound_list(p, &body)) != TC_OK)
		goto err;
	if (!is_word_eq(&p->tok, "done")) {
		r = unexpected(p);
		goto err;
	}
	leave_depth(p);
	if ((r = advance(p)) != TC_OK)
		return r;

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_word *words = nw ? tc_arena_alloc(a, nw * sizeof(*words)) : NULL;
	if (!node || (nw && !words)) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	size_t i = 0;
	for (struct word_link *wl = whead; wl; wl = wl->next)
		words[i++] = wl->w;
	node->kind = TC_NODE_FOR;
	node->line = line;
	node->col = col;
	node->u.for_cmd.name = name;
	node->u.for_cmd.has_in = has_in;
	node->u.for_cmd.words = words;
	node->u.for_cmd.nwords = nw;
	node->u.for_cmd.body = body;
	*out = node;
	return TC_OK;

err:
	leave_depth(p);
	return r;
}

static int parse_case(struct tc_parser *p, struct tc_node **out)
{
	unsigned long line = p->tok.line;
	unsigned long col = p->tok.col;
	int r;
	if ((r = enter_depth(p, line)) != TC_OK)
		return r;
	if ((r = advance(p)) != TC_OK)
		goto err;

	if (p->tok.kind != TC_TOK_WORD) {
		r = unexpected(p);
		goto err;
	}
	struct tc_word subject;
	tok_to_word(&p->tok, &subject);
	if ((r = advance(p)) != TC_OK)
		goto err;
	if ((r = parse_linebreak(p)) != TC_OK)
		goto err;
	if (!is_word_eq(&p->tok, "in")) {
		r = unexpected(p);
		goto err;
	}
	if ((r = advance(p)) != TC_OK)
		goto err;
	if ((r = parse_linebreak(p)) != TC_OK)
		goto err;

	struct tc_arena *a = &p->sh->arena;
	struct case_link *ihead = NULL, **itail = &ihead;
	size_t ni = 0;

	while (!is_word_eq(&p->tok, "esac")) {
		if (p->tok.kind == TC_TOK_LPAREN) {
			if ((r = advance(p)) != TC_OK)
				goto err;
		}
		struct word_link *phead = NULL, **ptail = &phead;
		size_t np = 0;
		for (;;) {
			if (p->tok.kind != TC_TOK_WORD) {
				r = unexpected(p);
				goto err;
			}
			struct word_link *wl = tc_arena_alloc(a, sizeof(*wl));
			if (!wl) {
				tc_error(p->sh, p->tok.line, "out of memory");
				r = TC_ERR_NOMEM;
				goto err;
			}
			tok_to_word(&p->tok, &wl->w);
			wl->next = NULL;
			*ptail = wl;
			ptail = &wl->next;
			np++;
			if ((r = advance(p)) != TC_OK)
				goto err;
			if (p->tok.kind == TC_TOK_PIPE) {
				if ((r = advance(p)) != TC_OK)
					goto err;
				continue;
			}
			break;
		}
		if (p->tok.kind != TC_TOK_RPAREN) {
			r = unexpected(p);
			goto err;
		}
		if ((r = advance(p)) != TC_OK)
			goto err;
		if ((r = parse_linebreak(p)) != TC_OK)
			goto err;

		struct tc_node *body = NULL;
		if (p->tok.kind != TC_TOK_DSEMI && !is_word_eq(&p->tok, "esac")) {
			if ((r = parse_compound_list(p, &body)) != TC_OK)
				goto err;
		}

		struct case_link *cl = tc_arena_alloc(a, sizeof(*cl));
		struct tc_word *pats = tc_arena_alloc(a, np * sizeof(*pats));
		if (!cl || !pats) {
			tc_error(p->sh, line, "out of memory");
			r = TC_ERR_NOMEM;
			goto err;
		}
		size_t j = 0;
		for (struct word_link *wl = phead; wl; wl = wl->next)
			pats[j++] = wl->w;
		cl->item.patterns = pats;
		cl->item.npatterns = np;
		cl->item.body = body;
		cl->next = NULL;
		*itail = cl;
		itail = &cl->next;
		ni++;

		if (p->tok.kind == TC_TOK_DSEMI) {
			if ((r = advance(p)) != TC_OK)
				goto err;
			if ((r = parse_linebreak(p)) != TC_OK)
				goto err;
		} else if (is_word_eq(&p->tok, "esac")) {
			break;
		} else {
			r = unexpected(p);
			goto err;
		}
	}

	leave_depth(p);
	if ((r = advance(p)) != TC_OK)
		return r;

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_case_item *items = ni ? tc_arena_alloc(a, ni * sizeof(*items)) : NULL;
	if (!node || (ni && !items)) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	size_t i = 0;
	for (struct case_link *cl = ihead; cl; cl = cl->next)
		items[i++] = cl->item;
	node->kind = TC_NODE_CASE;
	node->line = line;
	node->col = col;
	node->u.case_cmd.word = subject;
	node->u.case_cmd.items = items;
	node->u.case_cmd.nitems = ni;
	*out = node;
	return TC_OK;

err:
	leave_depth(p);
	return r;
}

static bool is_cond_unary_op(const struct tc_token *t)
{
	if (t->kind != TC_TOK_WORD || (t->flags & TC_WORD_QUOTED) ||
	    t->len != 2 || t->text[0] != '-')
		return false;
	switch (t->text[1]) {
	case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g':
	case 'h': case 'k': case 'L': case 'n': case 'o': case 'O': case 'G':
	case 'p': case 'r': case 'S': case 's': case 't': case 'u': case 'v':
	case 'w': case 'x': case 'z':
		return true;
	default:
		return false;
	}
}

static const char *cond_binary_op(const struct tc_token *t)
{
	if (t->kind == TC_TOK_LESS)
		return "<";
	if (t->kind == TC_TOK_GREAT)
		return ">";
	if (t->kind != TC_TOK_WORD || (t->flags & TC_WORD_QUOTED))
		return NULL;
	const char *s = t->text;
	if (strcmp(s, "==") == 0 || strcmp(s, "=") == 0 ||
	    strcmp(s, "!=") == 0 || strcmp(s, "=~") == 0 ||
	    strcmp(s, "-eq") == 0 || strcmp(s, "-ne") == 0 ||
	    strcmp(s, "-lt") == 0 || strcmp(s, "-le") == 0 ||
	    strcmp(s, "-gt") == 0 || strcmp(s, "-ge") == 0 ||
	    strcmp(s, "-ef") == 0 || strcmp(s, "-nt") == 0 ||
	    strcmp(s, "-ot") == 0)
		return s;
	return NULL;
}

static int parse_cond_or(struct tc_parser *p, struct tc_cond_expr **out);

static int parse_cond_primary(struct tc_parser *p, struct tc_cond_expr **out)
{
	int r;
	if ((r = parse_linebreak(p)) != TC_OK)
		return r;

	if (p->tok.kind == TC_TOK_LPAREN) {
		unsigned long line = p->tok.line;
		if ((r = enter_depth(p, line)) != TC_OK)
			return r;
		if ((r = advance(p)) != TC_OK) {
			leave_depth(p);
			return r;
		}
		if ((r = parse_cond_or(p, out)) != TC_OK) {
			leave_depth(p);
			return r;
		}
		if ((r = parse_linebreak(p)) != TC_OK) {
			leave_depth(p);
			return r;
		}
		if (p->tok.kind != TC_TOK_RPAREN) {
			leave_depth(p);
			return unexpected(p);
		}
		leave_depth(p);
		return advance(p);
	}

	if (p->tok.kind != TC_TOK_WORD || is_word_eq(&p->tok, "]]"))
		return unexpected(p);

	struct tc_arena *a = &p->sh->arena;
	unsigned long line = p->tok.line;
	unsigned long col = p->tok.col;

	/* Check if this is a unary operator (unless followed by a binary operator
	 * like `-f == -f`, in which case `-f` is the LHS word). */
	if (is_cond_unary_op(&p->tok)) {
		if ((r = peek_next(p)) != TC_OK)
			return r;
		if (!cond_binary_op(&p->peek_tok) &&
		    p->peek_tok.kind == TC_TOK_WORD &&
		    !is_word_eq(&p->peek_tok, "]]")) {
			const char *op = p->tok.text;
			if ((r = advance(p)) != TC_OK)
				return r;
			struct tc_cond_expr *e = tc_arena_alloc(a, sizeof(*e));
			if (!e) {
				tc_error(p->sh, line, "out of memory");
				return TC_ERR_NOMEM;
			}
			memset(e, 0, sizeof(*e));
			e->kind = TC_COND_UNARY;
			e->op = op;
			tok_to_word(&p->tok, &e->w1);
			e->line = line;
			e->col = col;
			if ((r = advance(p)) != TC_OK)
				return r;
			*out = e;
			return TC_OK;
		}
	}

	struct tc_word lhs;
	tok_to_word(&p->tok, &lhs);
	if ((r = advance(p)) != TC_OK)
		return r;

	const char *bop = cond_binary_op(&p->tok);
	if (bop) {
		bool is_regex = (strcmp(bop, "=~") == 0);
		if (is_regex) {
			p->have_peek = 0;
			r = tc_lex_next_regex_word(&p->lx, &p->tok);
			p->have_tok = (r == TC_OK);
		} else {
			r = advance(p);
		}
		if (r != TC_OK)
			return r;
		if (p->tok.kind != TC_TOK_WORD || is_word_eq(&p->tok, "]]"))
			return unexpected(p);
		struct tc_cond_expr *e = tc_arena_alloc(a, sizeof(*e));
		if (!e) {
			tc_error(p->sh, line, "out of memory");
			return TC_ERR_NOMEM;
		}
		memset(e, 0, sizeof(*e));
		e->kind = TC_COND_BINARY;
		e->op = bop;
		e->w1 = lhs;
		tok_to_word(&p->tok, &e->w2);
		e->line = line;
		e->col = col;
		if ((r = advance(p)) != TC_OK)
			return r;
		*out = e;
		return TC_OK;
	}

	struct tc_cond_expr *e = tc_arena_alloc(a, sizeof(*e));
	if (!e) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	memset(e, 0, sizeof(*e));
	e->kind = TC_COND_WORD;
	e->w1 = lhs;
	e->line = line;
	e->col = col;
	*out = e;
	return TC_OK;
}

static int parse_cond_not(struct tc_parser *p, struct tc_cond_expr **out)
{
	int r;
	if ((r = parse_linebreak(p)) != TC_OK)
		return r;
	if (is_word_eq(&p->tok, "!")) {
		/* If '!' is followed by a binary operator (e.g. ! == x), treat '!' as LHS word. */
		if ((r = peek_next(p)) != TC_OK)
			return r;
		if (!cond_binary_op(&p->peek_tok)) {
			unsigned long line = p->tok.line;
			unsigned long col = p->tok.col;
			if ((r = enter_depth(p, line)) != TC_OK)
				return r;
			if ((r = advance(p)) != TC_OK) {
				leave_depth(p);
				return r;
			}
			struct tc_cond_expr *inner = NULL;
			r = parse_cond_not(p, &inner);
			leave_depth(p);
			if (r != TC_OK)
				return r;
			struct tc_cond_expr *e = tc_arena_alloc(&p->sh->arena, sizeof(*e));
			if (!e) {
				tc_error(p->sh, line, "out of memory");
				return TC_ERR_NOMEM;
			}
			memset(e, 0, sizeof(*e));
			e->kind = TC_COND_NOT;
			e->lhs = inner;
			e->line = line;
			e->col = col;
			*out = e;
			return TC_OK;
		}
	}
	return parse_cond_primary(p, out);
}

static int parse_cond_and(struct tc_parser *p, struct tc_cond_expr **out)
{
	struct tc_cond_expr *lhs = NULL;
	int r = parse_cond_not(p, &lhs);
	if (r != TC_OK)
		return r;
	for (;;) {
		if ((r = parse_linebreak(p)) != TC_OK)
			return r;
		if (p->tok.kind != TC_TOK_AND_IF)
			break;
		unsigned long line = p->tok.line;
		unsigned long col = p->tok.col;
		if ((r = advance(p)) != TC_OK)
			return r;
		struct tc_cond_expr *rhs = NULL;
		if ((r = parse_cond_not(p, &rhs)) != TC_OK)
			return r;
		struct tc_cond_expr *e = tc_arena_alloc(&p->sh->arena, sizeof(*e));
		if (!e) {
			tc_error(p->sh, line, "out of memory");
			return TC_ERR_NOMEM;
		}
		memset(e, 0, sizeof(*e));
		e->kind = TC_COND_AND;
		e->lhs = lhs;
		e->rhs = rhs;
		e->line = line;
		e->col = col;
		lhs = e;
	}
	*out = lhs;
	return TC_OK;
}

static int parse_cond_or(struct tc_parser *p, struct tc_cond_expr **out)
{
	struct tc_cond_expr *lhs = NULL;
	int r = parse_cond_and(p, &lhs);
	if (r != TC_OK)
		return r;
	for (;;) {
		if ((r = parse_linebreak(p)) != TC_OK)
			return r;
		if (p->tok.kind != TC_TOK_OR_IF)
			break;
		unsigned long line = p->tok.line;
		unsigned long col = p->tok.col;
		if ((r = advance(p)) != TC_OK)
			return r;
		struct tc_cond_expr *rhs = NULL;
		if ((r = parse_cond_and(p, &rhs)) != TC_OK)
			return r;
		struct tc_cond_expr *e = tc_arena_alloc(&p->sh->arena, sizeof(*e));
		if (!e) {
			tc_error(p->sh, line, "out of memory");
			return TC_ERR_NOMEM;
		}
		memset(e, 0, sizeof(*e));
		e->kind = TC_COND_OR;
		e->lhs = lhs;
		e->rhs = rhs;
		e->line = line;
		e->col = col;
		lhs = e;
	}
	*out = lhs;
	return TC_OK;
}

static int parse_cond(struct tc_parser *p, struct tc_node **out)
{
	unsigned long line = p->tok.line;
	unsigned long col = p->tok.col;
	int r;
	if ((r = enter_depth(p, line)) != TC_OK)
		return r;
	if ((r = advance(p)) != TC_OK) { /* consume '[[' */
		leave_depth(p);
		return r;
	}
	struct tc_cond_expr *expr = NULL;
	if ((r = parse_cond_or(p, &expr)) != TC_OK) {
		leave_depth(p);
		return r;
	}
	if ((r = parse_linebreak(p)) != TC_OK) {
		leave_depth(p);
		return r;
	}
	if (!is_word_eq(&p->tok, "]]")) {
		leave_depth(p);
		if (p->tok.kind == TC_TOK_END) {
			tc_error(p->sh, line, "syntax error: missing `]]'");
			return TC_ERR_SYNTAX;
		}
		return unexpected(p);
	}
	if ((r = advance(p)) != TC_OK) { /* consume ']]' */
		leave_depth(p);
		return r;
	}
	leave_depth(p);

	struct tc_node *node = tc_arena_alloc(&p->sh->arena, sizeof(*node));
	if (!node) {
		tc_error(p->sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	node->kind = TC_NODE_COND;
	node->line = line;
	node->col = col;
	node->u.cond.expr = expr;
	*out = node;
	return TC_OK;
}

static bool starts_compound(const struct tc_token *t)
{
	if (t->kind == TC_TOK_LPAREN)
		return true;
	return is_word_eq(t, "{") || is_word_eq(t, "[[") ||
	       is_word_eq(t, "if") || is_word_eq(t, "while") ||
	       is_word_eq(t, "until") || is_word_eq(t, "for") ||
	       is_word_eq(t, "case");
}

static int parse_compound_with_redirs(struct tc_parser *p, struct tc_node **out)
{
	struct tc_node *cmd = NULL;
	int r;
	if (p->tok.kind == TC_TOK_LPAREN)
		r = parse_subshell(p, &cmd);
	else if (is_word_eq(&p->tok, "{"))
		r = parse_group(p, &cmd);
	else if (is_word_eq(&p->tok, "[["))
		r = parse_cond(p, &cmd);
	else if (is_word_eq(&p->tok, "if"))
		r = parse_if(p, &cmd);
	else if (is_word_eq(&p->tok, "while"))
		r = parse_loop(p, false, &cmd);
	else if (is_word_eq(&p->tok, "until"))
		r = parse_loop(p, true, &cmd);
	else if (is_word_eq(&p->tok, "for"))
		r = parse_for(p, &cmd);
	else if (is_word_eq(&p->tok, "case"))
		r = parse_case(p, &cmd);
	else
		return unexpected(p);

	if (r != TC_OK)
		return r;

	if (p->tok.kind != TC_TOK_IO_NUMBER && !is_redir_op(p->tok.kind)) {
		*out = cmd;
		return TC_OK;
	}

	struct tc_arena *a = &p->sh->arena;
	struct redir_link *rhead = NULL, **rtail = &rhead;
	size_t nr = 0;
	while (p->tok.kind == TC_TOK_IO_NUMBER || is_redir_op(p->tok.kind)) {
		struct redir_link *rl;
		if ((r = parse_redir(p, &rl)) != TC_OK)
			return r;
		*rtail = rl;
		rtail = &rl->next;
		nr++;
	}

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_redir *redirs = tc_arena_alloc(a, nr * sizeof(*redirs));
	if (!node || !redirs) {
		tc_error(p->sh, cmd->line, "out of memory");
		return TC_ERR_NOMEM;
	}
	size_t i = 0;
	for (struct redir_link *rl = rhead; rl; rl = rl->next) {
		redirs[i] = rl->r;
		if (rl->hd)
			rl->hd->redir = &redirs[i];
		i++;
	}
	node->kind = TC_NODE_REDIR;
	node->line = cmd->line;
	node->u.redir.child = cmd;
	node->u.redir.redirs = redirs;
	node->u.redir.nredirs = nr;
	*out = node;
	return TC_OK;
}

static int parse_command(struct tc_parser *p, struct tc_node **out)
{
	int er = expand_alias_at_tok(p);
	if (er != TC_OK)
		return er;

	if (starts_compound(&p->tok)) {
		p->alias_next = false;
		return parse_compound_with_redirs(p, out);
	}

	/* Function definition: fname '(' ')' linebreak compound_command [redirs] */
	if (p->tok.kind == TC_TOK_WORD && !(p->tok.flags & TC_WORD_QUOTED) &&
	    !(p->tok.flags & TC_WORD_ASSIGN) && !is_reserved(&p->tok)) {
		int r = peek_next(p);
		if (r != TC_OK)
			return r;
		if (p->peek_tok.kind == TC_TOK_LPAREN) {
			unsigned long line = p->tok.line;
			const char *name = p->tok.text;
			size_t nlen = p->tok.len;
			if (!tc_var_valid_name(name, nlen)) {
				tc_error(p->sh, line,
					 "syntax error: invalid function name `%s'", name);
				return TC_ERR_SYNTAX;
			}
			if ((r = enter_depth(p, line)) != TC_OK)
				return r;
			if ((r = advance(p)) != TC_OK) /* now at '(' */
				goto func_err;
			if ((r = advance(p)) != TC_OK) /* now at ')' */
				goto func_err;
			if (p->tok.kind != TC_TOK_RPAREN) {
				r = unexpected(p);
				goto func_err;
			}
			if ((r = advance(p)) != TC_OK)
				goto func_err;
			if ((r = parse_linebreak(p)) != TC_OK)
				goto func_err;
			if (!starts_compound(&p->tok)) {
				r = unexpected(p);
				goto func_err;
			}
			struct tc_node *body = NULL;
			if ((r = parse_compound_with_redirs(p, &body)) != TC_OK)
				goto func_err;
			leave_depth(p);

			struct tc_node *node = tc_arena_alloc(&p->sh->arena, sizeof(*node));
			if (!node) {
				tc_error(p->sh, line, "out of memory");
				return TC_ERR_NOMEM;
			}
			node->kind = TC_NODE_FUNCDEF;
			node->line = line;
			node->u.funcdef.name = name;
			node->u.funcdef.body = body;
			*out = node;
			return TC_OK;
func_err:
			leave_depth(p);
			return r;
		}
	}

	return parse_simple(p, out);
}

static int parse_pipeline(struct tc_parser *p, struct tc_node **out)
{
	bool bang = false;
	unsigned long line = p->tok.line;
	int r;

	if ((r = expand_alias_at_tok(p)) != TC_OK)
		return r;

	if (is_word_eq(&p->tok, "!")) {
		bang = true;
		p->alias_next = false;
		if ((r = advance(p)) != TC_OK)
			return r;
	}

	struct tc_node *first;
	if ((r = parse_command(p, &first)) != TC_OK)
		return r;

	if (!bang && p->tok.kind != TC_TOK_PIPE) {
		*out = first;
		return TC_OK;
	}

	struct tc_arena *a = &p->sh->arena;
	struct node_link *head = tc_arena_alloc(a, sizeof(*head));
	if (!head)
		goto nomem;
	head->node = first;
	head->next = NULL;
	struct node_link **tail = &head->next;
	size_t n = 1;

	while (p->tok.kind == TC_TOK_PIPE) {
		if ((r = advance(p)) != TC_OK)
			return r;
		if ((r = parse_linebreak(p)) != TC_OK)
			return r;
		struct tc_node *cmd;
		if ((r = parse_command(p, &cmd)) != TC_OK)
			return r;
		struct node_link *nl = tc_arena_alloc(a, sizeof(*nl));
		if (!nl)
			goto nomem;
		nl->node = cmd;
		nl->next = NULL;
		*tail = nl;
		tail = &nl->next;
		n++;
	}

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_node **cmds = tc_arena_alloc(a, n * sizeof(*cmds));
	if (!node || !cmds)
		goto nomem;
	size_t i = 0;
	for (struct node_link *nl = head; nl; nl = nl->next)
		cmds[i++] = nl->node;
	node->kind = TC_NODE_PIPELINE;
	node->line = line;
	node->u.pipeline.cmds = cmds;
	node->u.pipeline.ncmds = n;
	node->u.pipeline.bang = bang;
	*out = node;
	return TC_OK;

nomem:
	tc_error(p->sh, line, "out of memory");
	return TC_ERR_NOMEM;
}

static int parse_and_or(struct tc_parser *p, struct tc_node **out)
{
	struct tc_node *first;
	int r = parse_pipeline(p, &first);
	if (r != TC_OK)
		return r;
	if (p->tok.kind != TC_TOK_AND_IF && p->tok.kind != TC_TOK_OR_IF) {
		*out = first;
		return TC_OK;
	}

	struct tc_arena *a = &p->sh->arena;
	struct andor_link *head = tc_arena_alloc(a, sizeof(*head));
	if (!head)
		goto nomem;
	head->item.op = TC_ANDOR_FIRST;
	head->item.cmd = first;
	head->next = NULL;
	struct andor_link **tail = &head->next;
	size_t n = 1;

	while (p->tok.kind == TC_TOK_AND_IF || p->tok.kind == TC_TOK_OR_IF) {
		enum tc_andor_op op = (p->tok.kind == TC_TOK_AND_IF)
					  ? TC_ANDOR_AND
					  : TC_ANDOR_OR;
		if ((r = advance(p)) != TC_OK)
			return r;
		if ((r = parse_linebreak(p)) != TC_OK)
			return r;
		struct tc_node *next_cmd;
		if ((r = parse_pipeline(p, &next_cmd)) != TC_OK)
			return r;
		struct andor_link *al = tc_arena_alloc(a, sizeof(*al));
		if (!al)
			goto nomem;
		al->item.op = op;
		al->item.cmd = next_cmd;
		al->next = NULL;
		*tail = al;
		tail = &al->next;
		n++;
	}

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_andor_item *items = tc_arena_alloc(a, n * sizeof(*items));
	if (!node || !items)
		goto nomem;
	size_t i = 0;
	for (struct andor_link *al = head; al; al = al->next)
		items[i++] = al->item;
	node->kind = TC_NODE_AND_OR;
	node->line = first->line;
	node->u.and_or.items = items;
	node->u.and_or.nitems = n;
	*out = node;
	return TC_OK;

nomem:
	tc_error(p->sh, first->line, "out of memory");
	return TC_ERR_NOMEM;
}

static int parse_compound_list(struct tc_parser *p, struct tc_node **out)
{
	int r = parse_linebreak(p);
	if (r != TC_OK)
		return r;
	if (is_compound_end(&p->tok))
		return unexpected(p);

	struct tc_node *first;
	if ((r = parse_and_or(p, &first)) != TC_OK)
		return r;
	*out = first;
	struct tc_node **slot = out;

	for (;;) {
		if (p->tok.kind == TC_TOK_SEMI) {
			if ((r = advance(p)) != TC_OK)
				return r;
			if ((r = parse_linebreak(p)) != TC_OK)
				return r;
		} else if (p->tok.kind == TC_TOK_NEWLINE) {
			if ((r = parse_linebreak(p)) != TC_OK)
				return r;
		} else if (p->tok.kind == TC_TOK_AMP) {
			struct tc_node *bg = tc_arena_alloc(&p->sh->arena, sizeof(*bg));
			if (!bg) {
				tc_error(p->sh, p->tok.line, "out of memory");
				return TC_ERR_NOMEM;
			}
			bg->kind = TC_NODE_BG;
			bg->line = (*slot)->line;
			bg->u.bg.child = *slot;
			*slot = bg;
			if ((r = advance(p)) != TC_OK)
				return r;
			if ((r = parse_linebreak(p)) != TC_OK)
				return r;
		} else {
			break;
		}
		if (is_compound_end(&p->tok))
			break;
		struct tc_node *next;
		if ((r = parse_and_or(p, &next)) != TC_OK)
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

static int parse_list(struct tc_parser *p, struct tc_node **out)
{
	struct tc_node *first;
	int r = parse_and_or(p, &first);
	if (r != TC_OK)
		return r;
	*out = first;
	struct tc_node **slot = out;

	while (p->tok.kind == TC_TOK_SEMI || p->tok.kind == TC_TOK_AMP) {
		if (p->tok.kind == TC_TOK_AMP) {
			struct tc_node *bg = tc_arena_alloc(&p->sh->arena, sizeof(*bg));
			if (!bg) {
				tc_error(p->sh, p->tok.line, "out of memory");
				return TC_ERR_NOMEM;
			}
			bg->kind = TC_NODE_BG;
			bg->line = (*slot)->line;
			bg->u.bg.child = *slot;
			*slot = bg;
		}
		if ((r = advance(p)) != TC_OK)
			return r;
		if (p->tok.kind == TC_TOK_NEWLINE || p->tok.kind == TC_TOK_END)
			break;
		struct tc_node *next;
		if ((r = parse_and_or(p, &next)) != TC_OK)
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
	p->have_peek = 0;
	p->alias_next = false;
	p->depth = 0;
	p->lx.parse_depth = 0;
	p->hd_head = NULL;
	p->hd_tail = NULL;
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
	return flush_heredocs(p);
}

int tc_parse_cmdsub(struct tc_lexer *lx, unsigned long start_line,
		    struct tc_node **out)
{
	*out = NULL;
	if (lx->parse_depth >= TC_PARSE_DEPTH_MAX) {
		tc_error(lx->sh, start_line, "nesting depth exceeds limit (%d)",
			 TC_PARSE_DEPTH_MAX);
		return TC_ERR_SYNTAX;
	}
	struct tc_parser sub;
	int r = tc_parser_init(&sub, lx->sh, lx->in);
	if (r != TC_OK) {
		tc_error(lx->sh, start_line, "out of memory");
		return r;
	}
	sub.lx.peek = lx->peek;
	sub.lx.peek2 = lx->peek2;
	lx->peek = -10;
	lx->peek2 = -10;
	sub.depth = lx->parse_depth + 1;
	sub.lx.parse_depth = sub.depth;

	r = advance(&sub);
	if (r == TC_OK)
		r = parse_linebreak(&sub);
	if (r == TC_OK) {
		if (sub.tok.kind == TC_TOK_RPAREN) {
			*out = NULL;
		} else {
			r = parse_compound_list(&sub, out);
			if (r == TC_OK) {
				if (sub.tok.kind == TC_TOK_END) {
					tc_error(lx->sh, start_line,
						 "unterminated `$(' at end of input");
					r = TC_ERR_SYNTAX;
				} else if (sub.tok.kind != TC_TOK_RPAREN) {
					r = unexpected(&sub);
				} else {
					r = flush_heredocs(&sub);
				}
			}
		}
	}

	lx->peek = sub.lx.peek;
	lx->peek2 = sub.lx.peek2;
	tc_parser_finish(&sub);
	return r;
}

int tc_parse_cmdsub_string(struct tc_shell *sh, const char *src, size_t len,
			   unsigned long start_line, unsigned depth,
			   struct tc_node **out)
{
	*out = NULL;
	if (depth >= TC_PARSE_DEPTH_MAX) {
		tc_error(sh, start_line, "nesting depth exceeds limit (%d)",
			 TC_PARSE_DEPTH_MAX);
		return TC_ERR_SYNTAX;
	}
	struct tc_input in;
	tc_input_from_string(&in, src, len);
	in.line = start_line;
	in.sh = sh;

	struct tc_parser sub;
	int r = tc_parser_init(&sub, sh, &in);
	if (r != TC_OK) {
		tc_error(sh, start_line, "out of memory");
		tc_input_finish(&in);
		return r;
	}
	sub.depth = depth + 1;
	sub.lx.parse_depth = sub.depth;

	r = advance(&sub);
	if (r == TC_OK)
		r = parse_linebreak(&sub);
	if (r == TC_OK) {
		if (sub.tok.kind == TC_TOK_END) {
			*out = NULL;
		} else {
			r = parse_compound_list(&sub, out);
			if (r == TC_OK) {
				if (sub.tok.kind != TC_TOK_END)
					r = unexpected(&sub);
				else
					r = flush_heredocs(&sub);
			}
		}
	}

	tc_parser_finish(&sub);
	tc_input_finish(&in);
	return r;
}

void tc_parser_recover(struct tc_parser *p)
{
	p->have_peek = 0;
	p->alias_next = false;
	p->depth = 0;
	p->lx.parse_depth = 0;
	p->hd_head = NULL;
	p->hd_tail = NULL;
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
	p->have_peek = 0;
	p->alias_next = false;
	p->depth = 0;
	p->lx.parse_depth = 0;
	p->hd_head = NULL;
	p->hd_tail = NULL;
}
