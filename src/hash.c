/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Cached PATH command lookup table and the hash builtin. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/hash.h"
#include "tam-clam/shell.h"

#define TC_HASH_INIT_CAP 32

static char *xstrdup(const char *s)
{
	size_t len = strlen(s);
	char *p = malloc(len + 1);
	if (p)
		memcpy(p, s, len + 1);
	return p;
}

static uint64_t hash_str(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

void tc_hash_init(struct tc_hash_table *ht)
{
	ht->slots = NULL;
	ht->cap = 0;
	ht->count = 0;
	ht->path_gen = 0;
}

void tc_hash_clear(struct tc_hash_table *ht)
{
	if (!ht->slots)
		return;
	for (size_t i = 0; i < ht->cap; i++) {
		free(ht->slots[i].name);
		free(ht->slots[i].path);
		ht->slots[i].name = NULL;
		ht->slots[i].path = NULL;
		ht->slots[i].hits = 0;
	}
	ht->count = 0;
}

void tc_hash_finish(struct tc_hash_table *ht)
{
	tc_hash_clear(ht);
	free(ht->slots);
	ht->slots = NULL;
	ht->cap = 0;
}

void tc_hash_sync(struct tc_hash_table *ht, uint64_t path_gen)
{
	if (ht->path_gen != path_gen) {
		tc_hash_clear(ht);
		ht->path_gen = path_gen;
	}
}

static struct tc_hash_entry *find_slot(struct tc_hash_entry *slots, size_t cap,
				       const char *name)
{
	size_t mask = cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_hash_entry *e = &slots[idx];
		if (!e->name || strcmp(e->name, name) == 0)
			return e;
		idx = (idx + 1) & mask;
	}
}

static int grow_table(struct tc_hash_table *ht, size_t new_cap)
{
	struct tc_hash_entry *ns = calloc(new_cap, sizeof(*ns));
	if (!ns)
		return -1;
	for (size_t i = 0; i < ht->cap; i++) {
		if (!ht->slots[i].name)
			continue;
		struct tc_hash_entry *dst = find_slot(ns, new_cap, ht->slots[i].name);
		*dst = ht->slots[i];
	}
	free(ht->slots);
	ht->slots = ns;
	ht->cap = new_cap;
	return 0;
}

const char *tc_hash_get(struct tc_hash_table *ht, const char *name,
			bool count_hit)
{
	if (ht->cap == 0 || !name)
		return NULL;
	struct tc_hash_entry *e = find_slot(ht->slots, ht->cap, name);
	if (!e->name)
		return NULL;
	if (count_hit)
		e->hits++;
	return e->path;
}

int tc_hash_put(struct tc_hash_table *ht, const char *name, const char *path,
		bool count_hit)
{
	if (!name || !path)
		return -1;
	if (ht->cap == 0) {
		if (grow_table(ht, TC_HASH_INIT_CAP) < 0)
			return -1;
	} else if ((ht->count + 1) * 10 >= ht->cap * 7) {
		if (grow_table(ht, ht->cap * 2) < 0)
			return -1;
	}
	struct tc_hash_entry *e = find_slot(ht->slots, ht->cap, name);
	char *pcopy = xstrdup(path);
	if (!pcopy)
		return -1;
	if (e->name) {
		free(e->path);
		e->path = pcopy;
		if (count_hit)
			e->hits++;
		return 0;
	}
	char *ncopy = xstrdup(name);
	if (!ncopy) {
		free(pcopy);
		return -1;
	}
	e->name = ncopy;
	e->path = pcopy;
	e->hits = count_hit ? 1 : 0;
	ht->count++;
	return 0;
}

static int cmp_hash_entry(const void *a, const void *b)
{
	const struct tc_hash_entry *ea = *(const struct tc_hash_entry *const *)a;
	const struct tc_hash_entry *eb = *(const struct tc_hash_entry *const *)b;
	return strcmp(ea->name, eb->name);
}

int tc_hash_list(const struct tc_hash_table *ht,
		 const struct tc_hash_entry ***out, size_t *out_n)
{
	*out = NULL;
	*out_n = 0;
	if (ht->count == 0)
		return 0;
	const struct tc_hash_entry **arr = malloc(ht->count * sizeof(*arr));
	if (!arr)
		return -1;
	size_t pos = 0;
	for (size_t i = 0; i < ht->cap; i++) {
		if (ht->slots[i].name)
			arr[pos++] = &ht->slots[i];
	}
	qsort(arr, pos, sizeof(*arr), cmp_hash_entry);
	*out = arr;
	*out_n = pos;
	return 0;
}

int tc_bi_hash(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	tc_hash_sync(&sh->hash, sh->vars.path_gen);

	bool reset = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'r') {
				reset = true;
			} else {
				tc_error(sh, line, "hash: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}

	if (reset)
		tc_hash_clear(&sh->hash);

	if (i >= argc) {
		if (reset)
			return 0;
		const struct tc_hash_entry **list = NULL;
		size_t n = 0;
		if (tc_hash_list(&sh->hash, &list, &n) < 0) {
			tc_error(sh, line, "hash: out of memory");
			return 1;
		}
		int status = 0;
		for (size_t j = 0; j < n; j++) {
			if (tc_write_all(STDOUT_FILENO, list[j]->path,
					 strlen(list[j]->path)) < 0 ||
			    tc_write_all(STDOUT_FILENO, "\n", 1) < 0) {
				tc_error(sh, line, "hash: write error: %s",
					 strerror(errno));
				status = 1;
				break;
			}
		}
		free(list);
		return status;
	}

	int status = 0;
	for (; i < argc; i++) {
		const char *name = argv[i];
		if (strchr(name, '/'))
			continue;
		if (tc_builtin_find(name) || tc_func_find(&sh->funcs, name))
			continue;
		char *found = tc_exec_find_in_path(sh, name, false);
		if (!found) {
			tc_error(sh, line, "hash: %s: not found", name);
			status = 1;
			continue;
		}
		if (tc_hash_put(&sh->hash, name, found, false) < 0) {
			free(found);
			tc_error(sh, line, "hash: out of memory");
			return 1;
		}
		free(found);
	}
	return status;
}
