/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell variable table: open-addressed hash table with lazy envp cache. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/var.h"

#define TC_VAR_INIT_CAP 64

static char *xstrdup(const char *s)
{
	size_t len = strlen(s);
	char *p = malloc(len + 1);
	if (p)
		memcpy(p, s, len + 1);
	return p;
}

static char *xstrndup(const char *s, size_t len)
{
	char *p = malloc(len + 1);
	if (p) {
		memcpy(p, s, len);
		p[len] = '\0';
	}
	return p;
}

bool tc_var_valid_name(const char *name, size_t len)
{
	if (len == 0)
		return false;
	unsigned char c = (unsigned char)name[0];
	if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
		return false;
	for (size_t i = 1; i < len; i++) {
		c = (unsigned char)name[i];
		if (!(c == '_' || (c >= 'a' && c <= 'z') ||
		      (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	}
	return true;
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

static void free_envp(struct tc_vars *v)
{
	if (!v->envp)
		return;
	for (size_t i = 0; v->envp[i]; i++)
		free(v->envp[i]);
	free(v->envp);
	v->envp = NULL;
}

void tc_var_init(struct tc_vars *v)
{
	v->slots = NULL;
	v->cap = 0;
	v->count = 0;
	v->envp = NULL;
	v->env_dirty = true;
	v->path_gen = 0;
}

void tc_var_finish(struct tc_vars *v)
{
	free_envp(v);
	if (v->slots) {
		for (size_t i = 0; i < v->cap; i++) {
			free(v->slots[i].name);
			free(v->slots[i].value);
		}
		free(v->slots);
		v->slots = NULL;
	}
	v->cap = 0;
	v->count = 0;
	v->env_dirty = true;
	v->path_gen = 0;
}

static struct tc_var_entry *find_slot(struct tc_var_entry *slots, size_t cap,
				      const char *name)
{
	size_t mask = cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_var_entry *e = &slots[idx];
		if (!e->name || strcmp(e->name, name) == 0)
			return e;
		idx = (idx + 1) & mask;
	}
}

static int grow_table(struct tc_vars *v, size_t new_cap)
{
	struct tc_var_entry *ns = calloc(new_cap, sizeof(*ns));
	if (!ns)
		return TC_VAR_ERR_NOMEM;
	for (size_t i = 0; i < v->cap; i++) {
		if (!v->slots[i].name)
			continue;
		struct tc_var_entry *dst = find_slot(ns, new_cap, v->slots[i].name);
		*dst = v->slots[i];
	}
	free(v->slots);
	v->slots = ns;
	v->cap = new_cap;
	return TC_VAR_OK;
}

static int ensure_cap(struct tc_vars *v)
{
	if (v->cap == 0)
		return grow_table(v, TC_VAR_INIT_CAP);
	if ((v->count + 1) * 10 >= v->cap * 7)
		return grow_table(v, v->cap * 2);
	return TC_VAR_OK;
}

const char *tc_var_get(const struct tc_vars *v, const char *name)
{
	if (v->cap == 0)
		return NULL;
	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	return e->name ? e->value : NULL;
}

bool tc_var_is_readonly(const struct tc_vars *v, const char *name)
{
	if (v->cap == 0)
		return false;
	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	return e->name && (e->flags & TC_VAR_READONLY) != 0;
}

int tc_var_set(struct tc_vars *v, const char *name, const char *val,
	       unsigned add_flags)
{
	if (!tc_var_valid_name(name, strlen(name)))
		return TC_VAR_ERR_NAME;
	if (ensure_cap(v) != TC_VAR_OK)
		return TC_VAR_ERR_NOMEM;

	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	if (e->name) {
		if (val != NULL && (e->flags & TC_VAR_READONLY))
			return TC_VAR_ERR_READONLY;
		if (val != NULL) {
			char *copy = xstrdup(val);
			if (!copy)
				return TC_VAR_ERR_NOMEM;
			free(e->value);
			e->value = copy;
			if (strcmp(name, "PATH") == 0)
				v->path_gen++;
		}
		e->flags |= add_flags;
		if (e->flags & TC_VAR_EXPORT)
			v->env_dirty = true;
		return TC_VAR_OK;
	}

	char *ncopy = xstrdup(name);
	char *vcopy = val ? xstrdup(val) : NULL;
	if (!ncopy || (val && !vcopy)) {
		free(ncopy);
		free(vcopy);
		return TC_VAR_ERR_NOMEM;
	}
	e->name = ncopy;
	e->value = vcopy;
	e->flags = add_flags;
	v->count++;
	if (val != NULL && strcmp(name, "PATH") == 0)
		v->path_gen++;
	if (e->flags & TC_VAR_EXPORT)
		v->env_dirty = true;
	return TC_VAR_OK;
}

int tc_var_unset(struct tc_vars *v, const char *name)
{
	if (!tc_var_valid_name(name, strlen(name)))
		return TC_VAR_ERR_NAME;
	if (v->cap == 0)
		return TC_VAR_OK;

	size_t mask = v->cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_var_entry *e = &v->slots[idx];
		if (!e->name)
			return TC_VAR_OK;
		if (strcmp(e->name, name) == 0) {
			if (e->flags & TC_VAR_READONLY)
				return TC_VAR_ERR_READONLY;
			if (e->flags & TC_VAR_EXPORT)
				v->env_dirty = true;
			if (strcmp(name, "PATH") == 0)
				v->path_gen++;
			free(e->name);
			free(e->value);
			e->name = NULL;
			e->value = NULL;
			e->flags = 0;
			v->count--;
			/* Rehash subsequent cluster entries to preserve probing. */
			size_t next = (idx + 1) & mask;
			while (v->slots[next].name) {
				struct tc_var_entry tmp = v->slots[next];
				v->slots[next].name = NULL;
				v->slots[next].value = NULL;
				v->slots[next].flags = 0;
				struct tc_var_entry *dst =
					find_slot(v->slots, v->cap, tmp.name);
				*dst = tmp;
				next = (next + 1) & mask;
			}
			return TC_VAR_OK;
		}
		idx = (idx + 1) & mask;
	}
}

int tc_var_import_environ(struct tc_vars *v, char **envp)
{
	if (!envp)
		return TC_VAR_OK;
	for (size_t i = 0; envp[i]; i++) {
		const char *eq = strchr(envp[i], '=');
		if (!eq)
			continue;
		size_t nlen = (size_t)(eq - envp[i]);
		if (!tc_var_valid_name(envp[i], nlen))
			continue;
		char *name = xstrndup(envp[i], nlen);
		if (!name)
			return TC_VAR_ERR_NOMEM;
		int r = tc_var_set(v, name, eq + 1, TC_VAR_EXPORT);
		free(name);
		if (r != TC_VAR_OK)
			return r;
	}
	return TC_VAR_OK;
}

char **tc_var_envp(struct tc_vars *v)
{
	if (!v->env_dirty && v->envp)
		return v->envp;

	size_t n = 0;
	for (size_t i = 0; i < v->cap; i++) {
		if (v->slots[i].name && (v->slots[i].flags & TC_VAR_EXPORT) &&
		    v->slots[i].value != NULL)
			n++;
	}

	char **envp = malloc((n + 1) * sizeof(*envp));
	if (!envp)
		return NULL;
	size_t out = 0;
	for (size_t i = 0; i < v->cap; i++) {
		const struct tc_var_entry *e = &v->slots[i];
		if (!e->name || !(e->flags & TC_VAR_EXPORT) || !e->value)
			continue;
		size_t nlen = strlen(e->name);
		size_t vlen = strlen(e->value);
		char *s = malloc(nlen + 1 + vlen + 1);
		if (!s) {
			for (size_t j = 0; j < out; j++)
				free(envp[j]);
			free(envp);
			return NULL;
		}
		memcpy(s, e->name, nlen);
		s[nlen] = '=';
		memcpy(s + nlen + 1, e->value, vlen + 1);
		envp[out++] = s;
	}
	envp[out] = NULL;
	free_envp(v);
	v->envp = envp;
	v->env_dirty = false;
	return v->envp;
}

static bool name_in_kvs(const struct tc_var_kv *kvs, size_t n, const char *name)
{
	for (size_t i = 0; i < n; i++) {
		if (strcmp(kvs[i].name, name) == 0)
			return true;
	}
	return false;
}

char **tc_var_envp_override(const struct tc_vars *v,
			    const struct tc_var_kv *kvs, size_t n,
			    struct tc_arena *a)
{
	size_t total = 0;
	for (size_t i = 0; i < v->cap; i++) {
		const struct tc_var_entry *e = &v->slots[i];
		if (e->name && (e->flags & TC_VAR_EXPORT) && e->value &&
		    !name_in_kvs(kvs, n, e->name))
			total++;
	}
	for (size_t i = 0; i < n; i++) {
		bool dup_later = false;
		for (size_t j = i + 1; j < n; j++) {
			if (strcmp(kvs[i].name, kvs[j].name) == 0) {
				dup_later = true;
				break;
			}
		}
		if (!dup_later)
			total++;
	}

	char **envp = tc_arena_alloc(a, (total + 1) * sizeof(*envp));
	if (!envp)
		return NULL;
	size_t out = 0;
	for (size_t i = 0; i < v->cap; i++) {
		const struct tc_var_entry *e = &v->slots[i];
		if (!e->name || !(e->flags & TC_VAR_EXPORT) || !e->value)
			continue;
		if (name_in_kvs(kvs, n, e->name))
			continue;
		size_t nlen = strlen(e->name);
		size_t vlen = strlen(e->value);
		char *s = tc_arena_alloc(a, nlen + 1 + vlen + 1);
		if (!s)
			return NULL;
		memcpy(s, e->name, nlen);
		s[nlen] = '=';
		memcpy(s + nlen + 1, e->value, vlen + 1);
		envp[out++] = s;
	}
	for (size_t i = 0; i < n; i++) {
		bool dup_later = false;
		for (size_t j = i + 1; j < n; j++) {
			if (strcmp(kvs[i].name, kvs[j].name) == 0) {
				dup_later = true;
				break;
			}
		}
		if (dup_later)
			continue;
		size_t nlen = strlen(kvs[i].name);
		size_t vlen = strlen(kvs[i].value);
		char *s = tc_arena_alloc(a, nlen + 1 + vlen + 1);
		if (!s)
			return NULL;
		memcpy(s, kvs[i].name, nlen);
		s[nlen] = '=';
		memcpy(s + nlen + 1, kvs[i].value, vlen + 1);
		envp[out++] = s;
	}
	envp[out] = NULL;
	return envp;
}

const char *tc_var_lookup_override(const struct tc_vars *v,
				   const struct tc_var_kv *kvs, size_t n,
				   const char *name)
{
	for (size_t i = n; i > 0; i--) {
		if (strcmp(kvs[i - 1].name, name) == 0)
			return kvs[i - 1].value;
	}
	return tc_var_get(v, name);
}

int tc_var_push_overrides(struct tc_vars *v, const struct tc_var_kv *kvs,
			  size_t n, struct tc_var_save *saves,
			  size_t *out_saved)
{
	*out_saved = 0;
	for (size_t i = 0; i < n; i++) {
		if (tc_var_is_readonly(v, kvs[i].name)) {
			tc_var_pop_overrides(v, saves, *out_saved);
			*out_saved = 0;
			return TC_VAR_ERR_READONLY;
		}
		struct tc_var_save *sv = &saves[*out_saved];
		sv->name = xstrdup(kvs[i].name);
		if (!sv->name) {
			tc_var_pop_overrides(v, saves, *out_saved);
			*out_saved = 0;
			return TC_VAR_ERR_NOMEM;
		}
		sv->value = NULL;
		sv->flags = 0;
		sv->existed = false;
		if (v->cap > 0) {
			struct tc_var_entry *e = find_slot(v->slots, v->cap, kvs[i].name);
			if (e->name) {
				sv->existed = true;
				sv->flags = e->flags;
				if (e->value) {
					sv->value = xstrdup(e->value);
					if (!sv->value) {
						free(sv->name);
						tc_var_pop_overrides(v, saves, *out_saved);
						*out_saved = 0;
						return TC_VAR_ERR_NOMEM;
					}
				}
			}
		}
		(*out_saved)++;
		int r = tc_var_set(v, kvs[i].name, kvs[i].value, TC_VAR_EXPORT);
		if (r != TC_VAR_OK) {
			tc_var_pop_overrides(v, saves, *out_saved);
			*out_saved = 0;
			return r;
		}
	}
	return TC_VAR_OK;
}

void tc_var_pop_overrides(struct tc_vars *v, struct tc_var_save *saves,
			  size_t nsaved)
{
	while (nsaved > 0) {
		nsaved--;
		struct tc_var_save *sv = &saves[nsaved];
		if (!sv->existed) {
			(void)tc_var_unset(v, sv->name);
		} else {
			if (v->cap > 0) {
				struct tc_var_entry *e =
					find_slot(v->slots, v->cap, sv->name);
				if (e->name) {
					free(e->value);
					e->value = sv->value;
					sv->value = NULL;
					e->flags = sv->flags;
					v->env_dirty = true;
					if (strcmp(sv->name, "PATH") == 0)
						v->path_gen++;
				}
			}
		}
		free(sv->name);
		free(sv->value);
	}
}

static int cmp_var_entry(const void *a, const void *b)
{
	const struct tc_var_entry *ea = *(const struct tc_var_entry *const *)a;
	const struct tc_var_entry *eb = *(const struct tc_var_entry *const *)b;
	return strcmp(ea->name, eb->name);
}

int tc_var_list(const struct tc_vars *v, unsigned filter_flag,
		const struct tc_var_entry ***out, size_t *out_n)
{
	*out = NULL;
	*out_n = 0;
	size_t count = 0;
	for (size_t i = 0; i < v->cap; i++) {
		const struct tc_var_entry *e = &v->slots[i];
		if (!e->name)
			continue;
		if (filter_flag == 0) {
			if (e->value != NULL)
				count++;
		} else if (e->flags & filter_flag) {
			count++;
		}
	}
	if (count == 0)
		return TC_VAR_OK;

	const struct tc_var_entry **arr = malloc(count * sizeof(*arr));
	if (!arr)
		return TC_VAR_ERR_NOMEM;
	size_t pos = 0;
	for (size_t i = 0; i < v->cap; i++) {
		const struct tc_var_entry *e = &v->slots[i];
		if (!e->name)
			continue;
		if (filter_flag == 0) {
			if (e->value != NULL)
				arr[pos++] = e;
		} else if (e->flags & filter_flag) {
			arr[pos++] = e;
		}
	}
	qsort(arr, pos, sizeof(*arr), cmp_var_entry);
	*out = arr;
	*out_n = pos;
	return TC_VAR_OK;
}
