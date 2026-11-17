/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/arena.h"

#define TC_ARENA_BLOCK ((size_t)16 << 10)

struct tc_arena_block {
	struct tc_arena_block *next;
	size_t used, cap;
	max_align_t data[];
};

void tc_arena_init(struct tc_arena *a, size_t max)
{
	a->head = NULL;
	a->total = 0;
	a->max = max;
}

void *tc_arena_alloc(struct tc_arena *a, size_t n)
{
	const size_t align = alignof(max_align_t);
	if (n == 0)
		n = 1;
	if (n > a->max)
		return NULL;
	n = (n + align - 1) & ~(align - 1);

	struct tc_arena_block *b = a->head;
	if (!b || b->cap - b->used < n) {
		size_t cap = n > TC_ARENA_BLOCK ? n : TC_ARENA_BLOCK;
		if (a->total > a->max || cap > a->max - a->total)
			return NULL;
		b = malloc(sizeof(*b) + cap);
		if (!b)
			return NULL;
		b->next = a->head;
		b->used = 0;
		b->cap = cap;
		a->head = b;
		a->total += cap;
	}
	void *p = (char *)b->data + b->used;
	b->used += n;
	return p;
}

char *tc_arena_strndup(struct tc_arena *a, const char *s, size_t len)
{
	if (len == (size_t)-1)
		return NULL;
	char *p = tc_arena_alloc(a, len + 1);
	if (!p)
		return NULL;
	memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

void tc_arena_reset(struct tc_arena *a)
{
	struct tc_arena_block *b = a->head;
	if (!b)
		return;
	/* Keep the oldest block: it is the standard size. */
	while (b->next) {
		struct tc_arena_block *next = b->next;
		a->total -= b->cap;
		free(b);
		b = next;
	}
	b->used = 0;
	a->head = b;
}

void tc_arena_finish(struct tc_arena *a)
{
	struct tc_arena_block *b = a->head;
	while (b) {
		struct tc_arena_block *next = b->next;
		free(b);
		b = next;
	}
	a->head = NULL;
	a->total = 0;
}
