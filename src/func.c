/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell function table and arena-backed AST deep cloning. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/func.h"

#define TC_CLONE_DEPTH_MAX 256
#define TC_FUNCS_INIT_CAP 16

static uint64_t hash_str(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

static struct tc_node *clone_node(struct tc_arena *a,
				  const struct tc_node *src, unsigned depth);

static int clone_assign(struct tc_arena *a, const struct tc_assign *src,
			struct tc_assign *dst);

static struct tc_wpart *clone_wparts(struct tc_arena *a,
				     const struct tc_wpart *src,
				     unsigned depth)
{
	if (depth > TC_CLONE_DEPTH_MAX)
		return NULL;
	struct tc_wpart *head = NULL, **tail = &head;
	for (const struct tc_wpart *wp = src; wp; wp = wp->next) {
		struct tc_wpart *c = tc_arena_alloc(a, sizeof(*c));
		if (!c)
			return NULL;
		*c = *wp;
		c->next = NULL;
		if (wp->text) {
			c->text = tc_arena_strndup(a, wp->text, wp->len);
			if (!c->text)
				return NULL;
		}
		if (wp->sub) {
			c->sub = clone_wparts(a, wp->sub, depth + 1);
			if (!c->sub)
				return NULL;
		}
		if (wp->arg) {
			c->arg = clone_wparts(a, wp->arg, depth + 1);
			if (!c->arg)
				return NULL;
		}
		if (wp->cmd) {
			c->cmd = clone_node(a, wp->cmd, depth + 1);
			if (!c->cmd)
				return NULL;
		}
		*tail = c;
		tail = &c->next;
	}
	return head;
}

static int clone_word(struct tc_arena *a, const struct tc_word *src,
		      struct tc_word *dst)
{
	*dst = *src;
	if (src->text) {
		dst->text = tc_arena_strndup(a, src->text, src->len);
		if (!dst->text)
			return -1;
	}
	if (src->parts) {
		dst->parts = clone_wparts(a, src->parts, 0);
		if (!dst->parts)
			return -1;
	}
	if (src->decl_assign) {
		dst->decl_assign = tc_arena_alloc(a, sizeof(*dst->decl_assign));
		if (!dst->decl_assign)
			return -1;
		if (clone_assign(a, src->decl_assign, dst->decl_assign) < 0)
			return -1;
	}
	return 0;
}

static int clone_assign(struct tc_arena *a, const struct tc_assign *src,
			struct tc_assign *dst)
{
	*dst = *src;
	if (src->name) {
		dst->name = tc_arena_strndup(a, src->name, strlen(src->name));
		if (!dst->name)
			return -1;
	}
	if (src->sub) {
		dst->sub = clone_wparts(a, src->sub, 0);
		if (!dst->sub)
			return -1;
	}
	if (src->is_compound) {
		dst->items = NULL;
		if (src->nitems > 0) {
			dst->items = tc_arena_alloc(a, src->nitems * sizeof(*dst->items));
			if (!dst->items)
				return -1;
			for (size_t i = 0; i < src->nitems; i++) {
				dst->items[i] = src->items[i];
				if (src->items[i].sub) {
					dst->items[i].sub = clone_wparts(a, src->items[i].sub, 0);
					if (!dst->items[i].sub)
						return -1;
				}
				if (clone_word(a, &src->items[i].value, &dst->items[i].value) < 0)
					return -1;
			}
		}
	} else {
		if (clone_word(a, &src->value, &dst->value) < 0)
			return -1;
	}
	return 0;
}

static int clone_redirs(struct tc_arena *a, const struct tc_redir *src,
			size_t n, struct tc_redir **out)
{
	*out = NULL;
	if (n == 0)
		return 0;
	struct tc_redir *dst = tc_arena_alloc(a, n * sizeof(*dst));
	if (!dst)
		return -1;
	for (size_t i = 0; i < n; i++) {
		dst[i] = src[i];
		if (clone_word(a, &src[i].target, &dst[i].target) < 0)
			return -1;
		if (src[i].heredoc_body) {
			dst[i].heredoc_body = tc_arena_strndup(
				a, src[i].heredoc_body, src[i].heredoc_len);
			if (!dst[i].heredoc_body)
				return -1;
		}
		if (src[i].heredoc_parts) {
			dst[i].heredoc_parts = clone_wparts(
				a, src[i].heredoc_parts, 0);
			if (!dst[i].heredoc_parts)
				return -1;
		}
	}
	*out = dst;
	return 0;
}

static int clone_word(struct tc_arena *a, const struct tc_word *src,
		      struct tc_word *dst);

static struct tc_cond_expr *clone_cond_expr(struct tc_arena *a,
					    const struct tc_cond_expr *src,
					    unsigned depth)
{
	if (!src || depth > TC_CLONE_DEPTH_MAX)
		return NULL;
	struct tc_cond_expr *dst = tc_arena_alloc(a, sizeof(*dst));
	if (!dst)
		return NULL;
	*dst = *src;
	if (src->op) {
		dst->op = tc_arena_strndup(a, src->op, strlen(src->op));
		if (!dst->op)
			return NULL;
	}
	if (src->lhs) {
		dst->lhs = clone_cond_expr(a, src->lhs, depth + 1);
		if (!dst->lhs)
			return NULL;
	}
	if (src->rhs) {
		dst->rhs = clone_cond_expr(a, src->rhs, depth + 1);
		if (!dst->rhs)
			return NULL;
	}
	if (src->kind == TC_COND_UNARY || src->kind == TC_COND_BINARY ||
	    src->kind == TC_COND_WORD) {
		if (clone_word(a, &src->w1, &dst->w1) < 0)
			return NULL;
	}
	if (src->kind == TC_COND_BINARY) {
		if (clone_word(a, &src->w2, &dst->w2) < 0)
			return NULL;
	}
	return dst;
}

static struct tc_node *clone_node(struct tc_arena *a,
				  const struct tc_node *src, unsigned depth)
{
	if (!src)
		return NULL;
	if (depth > TC_CLONE_DEPTH_MAX)
		return NULL;

	/* Clone TC_NODE_SEQ chains iteratively to avoid stack recursion on lists. */
	if (src->kind == TC_NODE_SEQ) {
		struct tc_node *head = NULL, **slot = &head;
		const struct tc_node *cur = src;
		while (cur && cur->kind == TC_NODE_SEQ) {
			struct tc_node *seq = tc_arena_alloc(a, sizeof(*seq));
			if (!seq)
				return NULL;
			seq->kind = TC_NODE_SEQ;
			seq->line = cur->line;
			seq->u.seq.first = clone_node(a, cur->u.seq.first, depth + 1);
			if (cur->u.seq.first && !seq->u.seq.first)
				return NULL;
			seq->u.seq.rest = NULL;
			*slot = seq;
			slot = &seq->u.seq.rest;
			cur = cur->u.seq.rest;
		}
		if (cur) {
			*slot = clone_node(a, cur, depth + 1);
			if (!*slot)
				return NULL;
		}
		return head;
	}

	struct tc_node *dst = tc_arena_alloc(a, sizeof(*dst));
	if (!dst)
		return NULL;
	dst->kind = src->kind;
	dst->line = src->line;

	switch (src->kind) {
	case TC_NODE_SIMPLE: {
		size_t na = src->u.simple.nassigns;
		size_t nw = src->u.simple.nwords;
		size_t nr = src->u.simple.nredirs;
		dst->u.simple.nassigns = na;
		dst->u.simple.nwords = nw;
		dst->u.simple.nredirs = nr;
		dst->u.simple.assigns = NULL;
		dst->u.simple.words = NULL;
		dst->u.simple.redirs = NULL;
		if (na > 0) {
			struct tc_assign *as = tc_arena_alloc(a, na * sizeof(*as));
			if (!as)
				return NULL;
			for (size_t i = 0; i < na; i++) {
				if (clone_assign(a, &src->u.simple.assigns[i], &as[i]) < 0)
					return NULL;
			}
			dst->u.simple.assigns = as;
		}
		if (nw > 0) {
			struct tc_word *ws = tc_arena_alloc(a, nw * sizeof(*ws));
			if (!ws)
				return NULL;
			for (size_t i = 0; i < nw; i++) {
				if (clone_word(a, &src->u.simple.words[i], &ws[i]) < 0)
					return NULL;
			}
			dst->u.simple.words = ws;
		}
		if (clone_redirs(a, src->u.simple.redirs, nr,
				 &dst->u.simple.redirs) < 0)
			return NULL;
		return dst;
	}
	case TC_NODE_PIPELINE: {
		size_t n = src->u.pipeline.ncmds;
		struct tc_node **cmds = tc_arena_alloc(a, n * sizeof(*cmds));
		if (!cmds)
			return NULL;
		for (size_t i = 0; i < n; i++) {
			cmds[i] = clone_node(a, src->u.pipeline.cmds[i], depth + 1);
			if (!cmds[i])
				return NULL;
		}
		dst->u.pipeline.cmds = cmds;
		dst->u.pipeline.ncmds = n;
		dst->u.pipeline.bang = src->u.pipeline.bang;
		return dst;
	}
	case TC_NODE_AND_OR: {
		size_t n = src->u.and_or.nitems;
		struct tc_andor_item *its = tc_arena_alloc(a, n * sizeof(*its));
		if (!its)
			return NULL;
		for (size_t i = 0; i < n; i++) {
			its[i].op = src->u.and_or.items[i].op;
			its[i].cmd = clone_node(a, src->u.and_or.items[i].cmd, depth + 1);
			if (!its[i].cmd)
				return NULL;
		}
		dst->u.and_or.items = its;
		dst->u.and_or.nitems = n;
		return dst;
	}
	case TC_NODE_REDIR:
		dst->u.redir.child = clone_node(a, src->u.redir.child, depth + 1);
		if (!dst->u.redir.child)
			return NULL;
		dst->u.redir.nredirs = src->u.redir.nredirs;
		if (clone_redirs(a, src->u.redir.redirs, src->u.redir.nredirs,
				 &dst->u.redir.redirs) < 0)
			return NULL;
		return dst;
	case TC_NODE_GROUP:
	case TC_NODE_SUBSHELL:
		dst->u.group.body = clone_node(a, src->u.group.body, depth + 1);
		return dst->u.group.body ? dst : NULL;
	case TC_NODE_IF: {
		size_t nb = src->u.if_cmd.nbranches;
		struct tc_if_branch *br = tc_arena_alloc(a, nb * sizeof(*br));
		if (!br)
			return NULL;
		for (size_t i = 0; i < nb; i++) {
			br[i].cond = clone_node(a, src->u.if_cmd.branches[i].cond,
						depth + 1);
			br[i].body = clone_node(a, src->u.if_cmd.branches[i].body,
						depth + 1);
			if (!br[i].cond || !br[i].body)
				return NULL;
		}
		dst->u.if_cmd.branches = br;
		dst->u.if_cmd.nbranches = nb;
		dst->u.if_cmd.else_body = NULL;
		if (src->u.if_cmd.else_body) {
			dst->u.if_cmd.else_body = clone_node(
				a, src->u.if_cmd.else_body, depth + 1);
			if (!dst->u.if_cmd.else_body)
				return NULL;
		}
		return dst;
	}
	case TC_NODE_LOOP:
		dst->u.loop.until = src->u.loop.until;
		dst->u.loop.cond = clone_node(a, src->u.loop.cond, depth + 1);
		dst->u.loop.body = clone_node(a, src->u.loop.body, depth + 1);
		return (dst->u.loop.cond && dst->u.loop.body) ? dst : NULL;
	case TC_NODE_FOR: {
		dst->u.for_cmd.name = tc_arena_strndup(
			a, src->u.for_cmd.name, strlen(src->u.for_cmd.name));
		if (!dst->u.for_cmd.name)
			return NULL;
		dst->u.for_cmd.has_in = src->u.for_cmd.has_in;
		size_t nw = src->u.for_cmd.nwords;
		dst->u.for_cmd.nwords = nw;
		dst->u.for_cmd.words = NULL;
		if (nw > 0) {
			struct tc_word *ws = tc_arena_alloc(a, nw * sizeof(*ws));
			if (!ws)
				return NULL;
			for (size_t i = 0; i < nw; i++) {
				if (clone_word(a, &src->u.for_cmd.words[i], &ws[i]) < 0)
					return NULL;
			}
			dst->u.for_cmd.words = ws;
		}
		dst->u.for_cmd.body = clone_node(a, src->u.for_cmd.body, depth + 1);
		return dst->u.for_cmd.body ? dst : NULL;
	}
	case TC_NODE_CASE: {
		if (clone_word(a, &src->u.case_cmd.word, &dst->u.case_cmd.word) < 0)
			return NULL;
		size_t ni = src->u.case_cmd.nitems;
		dst->u.case_cmd.nitems = ni;
		dst->u.case_cmd.items = NULL;
		if (ni > 0) {
			struct tc_case_item *its = tc_arena_alloc(a, ni * sizeof(*its));
			if (!its)
				return NULL;
			for (size_t i = 0; i < ni; i++) {
				size_t np = src->u.case_cmd.items[i].npatterns;
				its[i].npatterns = np;
				its[i].patterns = tc_arena_alloc(
					a, np * sizeof(*its[i].patterns));
				if (!its[i].patterns)
					return NULL;
				for (size_t j = 0; j < np; j++) {
					if (clone_word(a,
						       &src->u.case_cmd.items[i].patterns[j],
						       &its[i].patterns[j]) < 0)
						return NULL;
				}
				its[i].body = NULL;
				if (src->u.case_cmd.items[i].body) {
					its[i].body = clone_node(
						a, src->u.case_cmd.items[i].body,
						depth + 1);
					if (!its[i].body)
						return NULL;
				}
			}
			dst->u.case_cmd.items = its;
		}
		return dst;
	}
	case TC_NODE_FUNCDEF:
		dst->u.funcdef.name = tc_arena_strndup(
			a, src->u.funcdef.name, strlen(src->u.funcdef.name));
		if (!dst->u.funcdef.name)
			return NULL;
		dst->u.funcdef.body = clone_node(a, src->u.funcdef.body, depth + 1);
		return dst->u.funcdef.body ? dst : NULL;
	case TC_NODE_BG:
		dst->u.bg.child = clone_node(a, src->u.bg.child, depth + 1);
		return dst->u.bg.child ? dst : NULL;
	case TC_NODE_COND:
		dst->u.cond.expr = clone_cond_expr(a, src->u.cond.expr, depth + 1);
		return dst->u.cond.expr ? dst : NULL;
	case TC_NODE_SEQ:
		return NULL;
	}
	return NULL;
}

void tc_funcs_init(struct tc_funcs *ft)
{
	ft->slots = NULL;
	ft->cap = 0;
	ft->count = 0;
}

void tc_func_ref(struct tc_func *fn)
{
	if (fn)
		fn->refcount++;
}

void tc_func_unref(struct tc_func *fn)
{
	if (!fn)
		return;
	if (--fn->refcount > 0)
		return;
	tc_arena_finish(&fn->arena);
	free(fn->name);
	free(fn);
}

void tc_funcs_finish(struct tc_funcs *ft)
{
	for (size_t i = 0; i < ft->cap; i++)
		tc_func_unref(ft->slots[i]);
	free(ft->slots);
	ft->slots = NULL;
	ft->cap = 0;
	ft->count = 0;
}

static int grow_table(struct tc_funcs *ft)
{
	size_t ncap = ft->cap ? ft->cap * 2 : TC_FUNCS_INIT_CAP;
	if (ncap < ft->cap)
		return -1;
	struct tc_func **nslots = calloc(ncap, sizeof(*nslots));
	if (!nslots)
		return -1;
	size_t mask = ncap - 1;
	for (size_t i = 0; i < ft->cap; i++) {
		struct tc_func *fn = ft->slots[i];
		if (!fn)
			continue;
		size_t idx = (size_t)hash_str(fn->name) & mask;
		while (nslots[idx])
			idx = (idx + 1) & mask;
		nslots[idx] = fn;
	}
	free(ft->slots);
	ft->slots = nslots;
	ft->cap = ncap;
	return 0;
}

int tc_func_define(struct tc_funcs *ft, const char *name,
		   const struct tc_node *body)
{
	struct tc_func *fn = calloc(1, sizeof(*fn));
	if (!fn)
		return -1;
	size_t nlen = strlen(name);
	fn->name = malloc(nlen + 1);
	if (!fn->name) {
		free(fn);
		return -1;
	}
	memcpy(fn->name, name, nlen + 1);
	tc_arena_init(&fn->arena, TC_ARENA_MAX);
	fn->body = clone_node(&fn->arena, body, 0);
	if (!fn->body) {
		tc_arena_finish(&fn->arena);
		free(fn->name);
		free(fn);
		return -1;
	}
	fn->refcount = 1;

	if (ft->cap == 0 || (ft->count + 1) * 10 >= ft->cap * 7) {
		if (grow_table(ft) < 0) {
			tc_func_unref(fn);
			return -1;
		}
	}

	size_t mask = ft->cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	while (ft->slots[idx]) {
		if (strcmp(ft->slots[idx]->name, name) == 0) {
			tc_func_unref(ft->slots[idx]);
			ft->slots[idx] = fn;
			return 0;
		}
		idx = (idx + 1) & mask;
	}
	ft->slots[idx] = fn;
	ft->count++;
	return 0;
}

struct tc_func *tc_func_find(const struct tc_funcs *ft, const char *name)
{
	if (ft->cap == 0)
		return NULL;
	size_t mask = ft->cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	while (ft->slots[idx]) {
		if (strcmp(ft->slots[idx]->name, name) == 0)
			return ft->slots[idx];
		idx = (idx + 1) & mask;
	}
	return NULL;
}

void tc_func_unset(struct tc_funcs *ft, const char *name)
{
	if (ft->cap == 0)
		return;
	size_t mask = ft->cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	while (ft->slots[idx]) {
		if (strcmp(ft->slots[idx]->name, name) == 0) {
			tc_func_unref(ft->slots[idx]);
			ft->slots[idx] = NULL;
			ft->count--;
			/* Back-shift subsequent entries in the probe cluster. */
			size_t cur = (idx + 1) & mask;
			while (ft->slots[cur]) {
				struct tc_func *move = ft->slots[cur];
				size_t home = (size_t)hash_str(move->name) & mask;
				/* Check if home is cyclically <= idx < cur. */
				if (((cur - home) & mask) >= ((cur - idx) & mask)) {
					ft->slots[idx] = move;
					ft->slots[cur] = NULL;
					idx = cur;
				}
				cur = (cur + 1) & mask;
			}
			return;
		}
		idx = (idx + 1) & mask;
	}
}

static int cmp_str_ptr(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int tc_funcs_list(const struct tc_funcs *ft, const char ***out, size_t *out_n)
{
	*out = NULL;
	*out_n = 0;
	if (ft->count == 0)
		return 0;
	const char **names = malloc(ft->count * sizeof(*names));
	if (!names)
		return -1;
	size_t pos = 0;
	for (size_t i = 0; i < ft->cap; i++) {
		if (ft->slots[i])
			names[pos++] = ft->slots[i]->name;
	}
	qsort(names, pos, sizeof(*names), cmp_str_ptr);
	*out = names;
	*out_n = pos;
	return 0;
}
