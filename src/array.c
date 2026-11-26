/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sparse indexed arrays (sorted dynamic array) and associative arrays
 * (open-addressed FNV-1a hash table). */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/array.h"

#define TC_ASSOC_INIT_CAP 16

static uint64_t arr_hash(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

/* ---- Indexed arrays --------------------------------------------------- */

struct tc_array *tc_array_new(void)
{
	struct tc_array *a = calloc(1, sizeof(*a));
	return a;
}

void tc_array_free(struct tc_array *a)
{
	if (!a)
		return;
	for (size_t i = 0; i < a->len; i++)
		free(a->elems[i].val);
	free(a->elems);
	free(a);
}

struct tc_array *tc_array_clone(const struct tc_array *a)
{
	if (!a)
		return NULL;
	struct tc_array *c = tc_array_new();
	if (!c)
		return NULL;
	if (a->len == 0)
		return c;
	c->elems = malloc(a->len * sizeof(*c->elems));
	if (!c->elems) {
		free(c);
		return NULL;
	}
	c->cap = a->len;
	for (size_t i = 0; i < a->len; i++) {
		c->elems[i].idx = a->elems[i].idx;
		c->elems[i].val = strdup(a->elems[i].val);
		if (!c->elems[i].val) {
			c->len = i;
			tc_array_free(c);
			return NULL;
		}
	}
	c->len = a->len;
	return c;
}

int tc_array_resolve_idx(const struct tc_array *a, int64_t idx, int64_t *out)
{
	if (idx >= 0) {
		*out = idx;
		return TC_ARR_OK;
	}
	if (!a || a->len == 0)
		return TC_ARR_BADSUB;
	int64_t max_idx = a->elems[a->len - 1].idx;
	uint64_t span = (uint64_t)max_idx + 1ULL;
	uint64_t neg = (uint64_t)(-(idx + 1)) + 1ULL;
	if (neg > span)
		return TC_ARR_BADSUB;
	*out = (int64_t)(span - neg);
	return TC_ARR_OK;
}

static size_t array_bsearch(const struct tc_array *a, int64_t idx, bool *found)
{
	size_t lo = 0, hi = a->len;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (a->elems[mid].idx < idx)
			lo = mid + 1;
		else if (a->elems[mid].idx > idx)
			hi = mid;
		else {
			*found = true;
			return mid;
		}
	}
	*found = false;
	return lo;
}

int tc_array_get(const struct tc_array *a, int64_t idx, const char **out)
{
	*out = NULL;
	int64_t ridx = 0;
	int r = tc_array_resolve_idx(a, idx, &ridx);
	if (r != TC_ARR_OK)
		return r;
	if (!a || a->len == 0)
		return TC_ARR_OK;
	bool found = false;
	size_t pos = array_bsearch(a, ridx, &found);
	if (found)
		*out = a->elems[pos].val;
	return TC_ARR_OK;
}

int tc_array_set(struct tc_array *a, int64_t idx, const char *val)
{
	int64_t ridx = 0;
	int r = tc_array_resolve_idx(a, idx, &ridx);
	if (r != TC_ARR_OK)
		return r;

	bool found = false;
	size_t pos = array_bsearch(a, ridx, &found);
	char *copy = strdup(val ? val : "");
	if (!copy)
		return TC_ARR_NOMEM;

	if (found) {
		free(a->elems[pos].val);
		a->elems[pos].val = copy;
		return TC_ARR_OK;
	}

	if (a->len >= TC_ARRAY_MAX) {
		free(copy);
		return TC_ARR_NOMEM;
	}
	if (a->len == a->cap) {
		size_t ncap = a->cap ? a->cap * 2 : 8;
		if (ncap > TC_ARRAY_MAX)
			ncap = TC_ARRAY_MAX;
		struct tc_array_elem *ne = realloc(a->elems, ncap * sizeof(*ne));
		if (!ne) {
			free(copy);
			return TC_ARR_NOMEM;
		}
		a->elems = ne;
		a->cap = ncap;
	}
	if (pos < a->len) {
		memmove(&a->elems[pos + 1], &a->elems[pos],
			(a->len - pos) * sizeof(*a->elems));
	}
	a->elems[pos].idx = ridx;
	a->elems[pos].val = copy;
	a->len++;
	return TC_ARR_OK;
}

int tc_array_append(struct tc_array *a, const char *val)
{
	if (a->len > 0 && a->elems[a->len - 1].idx == INT64_MAX)
		return TC_ARR_BADSUB;
	int64_t next_idx = a->len > 0 ? a->elems[a->len - 1].idx + 1 : 0;
	return tc_array_set(a, next_idx, val);
}

int tc_array_unset(struct tc_array *a, int64_t idx)
{
	int64_t ridx = 0;
	int r = tc_array_resolve_idx(a, idx, &ridx);
	if (r != TC_ARR_OK)
		return r;
	if (!a || a->len == 0)
		return TC_ARR_OK;
	bool found = false;
	size_t pos = array_bsearch(a, ridx, &found);
	if (!found)
		return TC_ARR_OK;
	free(a->elems[pos].val);
	if (pos + 1 < a->len) {
		memmove(&a->elems[pos], &a->elems[pos + 1],
			(a->len - pos - 1) * sizeof(*a->elems));
	}
	a->len--;
	return TC_ARR_OK;
}

size_t tc_array_len(const struct tc_array *a)
{
	return a ? a->len : 0;
}

int64_t tc_array_max_idx(const struct tc_array *a)
{
	return (a && a->len > 0) ? a->elems[a->len - 1].idx : -1;
}

/* ---- Associative arrays ----------------------------------------------- */

struct tc_assoc *tc_assoc_new(void)
{
	struct tc_assoc *m = calloc(1, sizeof(*m));
	return m;
}

void tc_assoc_free(struct tc_assoc *m)
{
	if (!m)
		return;
	if (m->slots) {
		for (size_t i = 0; i < m->cap; i++) {
			free(m->slots[i].key);
			free(m->slots[i].val);
		}
		free(m->slots);
	}
	free(m);
}

static struct tc_assoc_slot *assoc_find(struct tc_assoc_slot *slots, size_t cap,
					const char *key)
{
	size_t mask = cap - 1;
	size_t idx = (size_t)arr_hash(key) & mask;
	for (;;) {
		struct tc_assoc_slot *s = &slots[idx];
		if (!s->key || strcmp(s->key, key) == 0)
			return s;
		idx = (idx + 1) & mask;
	}
}

static int assoc_grow(struct tc_assoc *m, size_t ncap)
{
	struct tc_assoc_slot *ns = calloc(ncap, sizeof(*ns));
	if (!ns)
		return TC_ARR_NOMEM;
	for (size_t i = 0; i < m->cap; i++) {
		if (!m->slots[i].key)
			continue;
		struct tc_assoc_slot *dst = assoc_find(ns, ncap, m->slots[i].key);
		*dst = m->slots[i];
	}
	free(m->slots);
	m->slots = ns;
	m->cap = ncap;
	return TC_ARR_OK;
}

static int assoc_ensure(struct tc_assoc *m)
{
	if (m->cap == 0)
		return assoc_grow(m, TC_ASSOC_INIT_CAP);
	if ((m->count + 1) * 10 >= m->cap * 7)
		return assoc_grow(m, m->cap * 2);
	return TC_ARR_OK;
}

struct tc_assoc *tc_assoc_clone(const struct tc_assoc *m)
{
	if (!m)
		return NULL;
	struct tc_assoc *c = tc_assoc_new();
	if (!c)
		return NULL;
	for (size_t i = 0; i < m->cap; i++) {
		if (!m->slots[i].key)
			continue;
		if (tc_assoc_set(c, m->slots[i].key, m->slots[i].val) != TC_ARR_OK) {
			tc_assoc_free(c);
			return NULL;
		}
	}
	return c;
}

const char *tc_assoc_get(const struct tc_assoc *m, const char *key)
{
	if (!m || m->cap == 0)
		return NULL;
	struct tc_assoc_slot *s = assoc_find(m->slots, m->cap, key);
	return s->key ? s->val : NULL;
}

int tc_assoc_set(struct tc_assoc *m, const char *key, const char *val)
{
	if (m->count >= TC_ARRAY_MAX && tc_assoc_get(m, key) == NULL)
		return TC_ARR_NOMEM;
	if (assoc_ensure(m) != TC_ARR_OK)
		return TC_ARR_NOMEM;

	struct tc_assoc_slot *s = assoc_find(m->slots, m->cap, key);
	char *vcopy = strdup(val ? val : "");
	if (!vcopy)
		return TC_ARR_NOMEM;
	if (s->key) {
		free(s->val);
		s->val = vcopy;
		return TC_ARR_OK;
	}
	char *kcopy = strdup(key);
	if (!kcopy) {
		free(vcopy);
		return TC_ARR_NOMEM;
	}
	s->key = kcopy;
	s->val = vcopy;
	m->count++;
	return TC_ARR_OK;
}

void tc_assoc_unset(struct tc_assoc *m, const char *key)
{
	if (!m || m->cap == 0)
		return;
	size_t mask = m->cap - 1;
	size_t idx = (size_t)arr_hash(key) & mask;
	for (;;) {
		struct tc_assoc_slot *s = &m->slots[idx];
		if (!s->key)
			return;
		if (strcmp(s->key, key) == 0) {
			free(s->key);
			free(s->val);
			s->key = NULL;
			s->val = NULL;
			m->count--;
			size_t next = (idx + 1) & mask;
			while (m->slots[next].key) {
				struct tc_assoc_slot tmp = m->slots[next];
				m->slots[next].key = NULL;
				m->slots[next].val = NULL;
				struct tc_assoc_slot *dst =
					assoc_find(m->slots, m->cap, tmp.key);
				*dst = tmp;
				next = (next + 1) & mask;
			}
			return;
		}
		idx = (idx + 1) & mask;
	}
}

size_t tc_assoc_len(const struct tc_assoc *m)
{
	return m ? m->count : 0;
}

static int cmp_assoc_slot(const void *a, const void *b)
{
	const struct tc_assoc_slot *sa = *(const struct tc_assoc_slot *const *)a;
	const struct tc_assoc_slot *sb = *(const struct tc_assoc_slot *const *)b;
	return strcmp(sa->key, sb->key);
}

int tc_assoc_list(const struct tc_assoc *m, const struct tc_assoc_slot ***out,
		  size_t *out_n)
{
	*out = NULL;
	*out_n = 0;
	if (!m || m->count == 0)
		return TC_ARR_OK;
	const struct tc_assoc_slot **arr = malloc(m->count * sizeof(*arr));
	if (!arr)
		return TC_ARR_NOMEM;
	size_t pos = 0;
	for (size_t i = 0; i < m->cap; i++) {
		if (m->slots[i].key)
			arr[pos++] = &m->slots[i];
	}
	qsort(arr, pos, sizeof(*arr), cmp_assoc_slot);
	*out = arr;
	*out_n = pos;
	return TC_ARR_OK;
}
