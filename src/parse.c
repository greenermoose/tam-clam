/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Recursive descent over the POSIX grammar (XCU 2.10.2):
 *
 *   complete_command : list? (NEWLINE | END)
 *   list             : and_or (';' and_or)* ';'?
 *   and_or           : pipeline (('&&' | '||') linebreak pipeline)*
 *   pipeline         : '!'? simple_command ('|' linebreak simple_command)*
 *   simple_command   : (WORD | io_redirect)+
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
	int r = tc_lex_next(&p->lx, &p->tok);
	p->have_tok = (r == TC_OK);
	return r;
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
					    &hd->redir->heredoc_len);
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

static int unsupported(struct tc_parser *p, unsigned long line, const char *what)
{
	tc_error(p->sh, line, "%s %s not supported yet; see ROADMAP.md", what,
		 strchr(what, ' ') ? "are" : "is");
	return TC_ERR_SYNTAX;
}

static int unexpected(struct tc_parser *p)
{
	switch (p->tok.kind) {
	case TC_TOK_AMP:
		return unsupported(p, p->tok.line, "background commands (`&')");
	case TC_TOK_LPAREN:
		return unsupported(p, p->tok.line, "subshells (`(')");
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

struct redir_link {
	struct tc_redir r;
	struct tc_heredoc_pending *hd;
	struct redir_link *next;
};

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
	if (kind != TC_REDIR_HEREDOC && (r = check_word(p, &p->tok, 0)) != TC_OK)
		return r;

	struct tc_arena *a = &p->sh->arena;
	struct redir_link *rl = tc_arena_alloc(a, sizeof(*rl));
	if (!rl)
		goto nomem;
	memset(rl, 0, sizeof(*rl));
	rl->r.kind = kind;
	rl->r.fd = fd;
	rl->r.target.text = p->tok.text;
	rl->r.target.len = p->tok.len;
	rl->r.target.flags = p->tok.flags;
	rl->r.target.line = p->tok.line;
	rl->r.strip_tabs = (op == TC_TOK_DLESSDASH);
	rl->r.heredoc_quoted = (p->tok.flags & TC_WORD_QUOTED) != 0;
	rl->r.line = line;

	if (kind == TC_REDIR_HEREDOC) {
		char *delim = tc_expand_word(a, &rl->r.target);
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
	struct word_link *whead = NULL, **wtail = &whead;
	struct redir_link *rhead = NULL, **rtail = &rhead;
	size_t nw = 0, nr = 0;
	unsigned long line = p->tok.line;
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
			if (nw == 0 && is_reserved(&p->tok)) {
				tc_error(p->sh, p->tok.line,
					 "the reserved word `%s' is not supported yet; see ROADMAP.md",
					 p->tok.text);
				return TC_ERR_SYNTAX;
			}
			if ((r = check_word(p, &p->tok, nw == 0)) != TC_OK)
				return r;
			if (nw == TC_WORDS_MAX) {
				tc_error(p->sh, p->tok.line,
					 "more than %d words in one command",
					 TC_WORDS_MAX);
				return TC_ERR_NOMEM;
			}
			struct word_link *wl = tc_arena_alloc(a, sizeof(*wl));
			if (!wl)
				goto nomem;
			wl->w.text = p->tok.text;
			wl->w.len = p->tok.len;
			wl->w.flags = p->tok.flags;
			wl->w.line = p->tok.line;
			wl->next = NULL;
			*wtail = wl;
			wtail = &wl->next;
			nw++;
			if ((r = advance(p)) != TC_OK)
				return r;
		} else {
			break;
		}
	}

	struct tc_node *node = tc_arena_alloc(a, sizeof(*node));
	struct tc_word *words = nw ? tc_arena_alloc(a, nw * sizeof(*words)) : NULL;
	struct tc_redir *redirs = nr ? tc_arena_alloc(a, nr * sizeof(*redirs)) : NULL;
	if (!node || (nw && !words) || (nr && !redirs))
		goto nomem;
	size_t i = 0;
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

struct node_link {
	struct tc_node *node;
	struct node_link *next;
};

static int parse_pipeline(struct tc_parser *p, struct tc_node **out)
{
	bool bang = false;
	unsigned long line = p->tok.line;
	int r;

	if (p->tok.kind == TC_TOK_WORD && !(p->tok.flags & TC_WORD_QUOTED) &&
	    strcmp(p->tok.text, "!") == 0) {
		bang = true;
		if ((r = advance(p)) != TC_OK)
			return r;
	}

	struct tc_node *first;
	if ((r = parse_simple(p, &first)) != TC_OK)
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
		if ((r = parse_simple(p, &cmd)) != TC_OK)
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

struct andor_link {
	struct tc_andor_item item;
	struct andor_link *next;
};

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

/* Builds a right-leaning chain of TC_NODE_SEQ without recursion, so a long
 * line of commands cannot exhaust the stack. */
static int parse_list(struct tc_parser *p, struct tc_node **out)
{
	struct tc_node *first;
	int r = parse_and_or(p, &first);
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

void tc_parser_recover(struct tc_parser *p)
{
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
	p->hd_head = NULL;
	p->hd_tail = NULL;
}
