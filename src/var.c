/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell variable table: open-addressed hash table with lazy envp cache. */
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/var.h"

#define TC_VAR_INIT_CAP 64

struct tc_var_scope {
	struct tc_var_save *saves;
	size_t count;
	size_t cap;
};

static char *xstrdup_flags(const char *s, unsigned flags)
{
	size_t len = strlen(s);
	char *p = malloc(len + 1);
	if (!p)
		return NULL;
	memcpy(p, s, len + 1);
	if (flags & TC_VAR_LOWER) {
		for (size_t i = 0; i < len; i++)
			p[i] = (char)tolower((unsigned char)p[i]);
	} else if (flags & TC_VAR_UPPER) {
		for (size_t i = 0; i < len; i++)
			p[i] = (char)toupper((unsigned char)p[i]);
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
	v->scopes = NULL;
	v->scope_depth = 0;
	v->scope_cap = 0;
}

void tc_var_finish(struct tc_vars *v)
{
	while (v->scope_depth > 0)
		tc_var_pop_scope(v);
	free(v->scopes);
	v->scopes = NULL;
	v->scope_cap = 0;
	free_envp(v);
	if (v->slots) {
		for (size_t i = 0; i < v->cap; i++) {
			free(v->slots[i].name);
			free(v->slots[i].value);
			tc_array_free(v->slots[i].arr);
			tc_assoc_free(v->slots[i].assoc);
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

const struct tc_var_entry *tc_var_find(const struct tc_vars *v, const char *name)
{
	if (v->cap == 0)
		return NULL;
	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	return e->name ? e : NULL;
}

const char *tc_var_get(const struct tc_vars *v, const char *name)
{
	const struct tc_var_entry *e = tc_var_find(v, name);
	if (!e)
		return NULL;
	if (e->flags & TC_VAR_ARRAY) {
		const char *val = NULL;
		if (e->arr)
			(void)tc_array_get(e->arr, 0, &val);
		return val;
	}
	if (e->flags & TC_VAR_ASSOC)
		return e->assoc ? tc_assoc_get(e->assoc, "0") : NULL;
	return e->value;
}

bool tc_var_is_readonly(const struct tc_vars *v, const char *name)
{
	if (v->cap == 0)
		return false;
	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	return e->name && (e->flags & TC_VAR_READONLY) != 0;
}

static char *xstrcat(const char *a, const char *b)
{
	size_t al = a ? strlen(a) : 0;
	size_t bl = b ? strlen(b) : 0;
	char *p = malloc(al + bl + 1);
	if (!p)
		return NULL;
	if (al)
		memcpy(p, a, al);
	if (bl)
		memcpy(p + al, b, bl);
	p[al + bl] = '\0';
	return p;
}

static struct tc_var_entry *ensure_entry(struct tc_vars *v, const char *name,
					 int *err)
{
	if (!tc_var_valid_name(name, strlen(name))) {
		*err = TC_VAR_ERR_NAME;
		return NULL;
	}
	if (ensure_cap(v) != TC_VAR_OK) {
		*err = TC_VAR_ERR_NOMEM;
		return NULL;
	}
	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	if (e->name) {
		*err = TC_VAR_OK;
		return e;
	}
	char *ncopy = strdup(name);
	if (!ncopy) {
		*err = TC_VAR_ERR_NOMEM;
		return NULL;
	}
	e->name = ncopy;
	e->value = NULL;
	e->arr = NULL;
	e->assoc = NULL;
	e->flags = 0;
	v->count++;
	*err = TC_VAR_OK;
	return e;
}

static int convert_to_array(struct tc_var_entry *e)
{
	if (e->flags & TC_VAR_ASSOC)
		return TC_VAR_ERR_TYPE;
	if (e->flags & TC_VAR_ARRAY) {
		if (!e->arr) {
			e->arr = tc_array_new();
			if (!e->arr)
				return TC_VAR_ERR_NOMEM;
		}
		return TC_VAR_OK;
	}
	struct tc_array *a = tc_array_new();
	if (!a)
		return TC_VAR_ERR_NOMEM;
	if (e->value != NULL) {
		if (tc_array_set(a, 0, e->value) != TC_ARR_OK) {
			tc_array_free(a);
			return TC_VAR_ERR_NOMEM;
		}
		free(e->value);
		e->value = NULL;
	}
	e->arr = a;
	e->flags |= TC_VAR_ARRAY;
	return TC_VAR_OK;
}

static int convert_to_assoc(struct tc_var_entry *e)
{
	if (e->flags & TC_VAR_ARRAY)
		return TC_VAR_ERR_TYPE;
	if (e->flags & TC_VAR_ASSOC) {
		if (!e->assoc) {
			e->assoc = tc_assoc_new();
			if (!e->assoc)
				return TC_VAR_ERR_NOMEM;
		}
		return TC_VAR_OK;
	}
	struct tc_assoc *m = tc_assoc_new();
	if (!m)
		return TC_VAR_ERR_NOMEM;
	if (e->value != NULL) {
		if (tc_assoc_set(m, "0", e->value) != TC_ARR_OK) {
			tc_assoc_free(m);
			return TC_VAR_ERR_NOMEM;
		}
		free(e->value);
		e->value = NULL;
	}
	e->assoc = m;
	e->flags |= TC_VAR_ASSOC;
	return TC_VAR_OK;
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
		unsigned eff_flags = e->flags | add_flags;
		if (add_flags & TC_VAR_LOWER)
			eff_flags &= ~TC_VAR_UPPER;
		else if (add_flags & TC_VAR_UPPER)
			eff_flags &= ~TC_VAR_LOWER;
		if (val != NULL) {
			char *tval = xstrdup_flags(val, eff_flags);
			if (!tval)
				return TC_VAR_ERR_NOMEM;
			if (e->flags & TC_VAR_ARRAY) {
				if (!e->arr) {
					e->arr = tc_array_new();
					if (!e->arr) {
						free(tval);
						return TC_VAR_ERR_NOMEM;
					}
				}
				int ar = tc_array_set(e->arr, 0, tval);
				free(tval);
				if (ar != TC_ARR_OK)
					return TC_VAR_ERR_NOMEM;
			} else if (e->flags & TC_VAR_ASSOC) {
				if (!e->assoc) {
					e->assoc = tc_assoc_new();
					if (!e->assoc) {
						free(tval);
						return TC_VAR_ERR_NOMEM;
					}
				}
				int ar = tc_assoc_set(e->assoc, "0", tval);
				free(tval);
				if (ar != TC_ARR_OK)
					return TC_VAR_ERR_NOMEM;
			} else {
				free(e->value);
				e->value = tval;
			}
			if (strcmp(name, "PATH") == 0)
				v->path_gen++;
		}
		e->flags = eff_flags;
		if (e->flags & TC_VAR_EXPORT)
			v->env_dirty = true;
		return TC_VAR_OK;
	}

	char *ncopy = strdup(name);
	char *vcopy = val ? xstrdup_flags(val, add_flags) : NULL;
	if (!ncopy || (val && !vcopy)) {
		free(ncopy);
		free(vcopy);
		return TC_VAR_ERR_NOMEM;
	}
	e->name = ncopy;
	e->value = vcopy;
	e->arr = NULL;
	e->assoc = NULL;
	e->flags = add_flags;
	v->count++;
	if (val != NULL && strcmp(name, "PATH") == 0)
		v->path_gen++;
	if (e->flags & TC_VAR_EXPORT)
		v->env_dirty = true;
	return TC_VAR_OK;
}

int tc_var_append_scalar(struct tc_vars *v, const char *name, const char *val,
			 unsigned add_flags)
{
	const char *cur = tc_var_get(v, name);
	char *joined = xstrcat(cur, val);
	if (!joined)
		return TC_VAR_ERR_NOMEM;
	int r = tc_var_set(v, name, joined, add_flags);
	free(joined);
	return r;
}

int tc_var_declare(struct tc_vars *v, const char *name, unsigned add_flags,
		   unsigned clear_flags)
{
	if ((add_flags & TC_VAR_ARRAY) && (add_flags & TC_VAR_ASSOC))
		return TC_VAR_ERR_TYPE;
	int err = TC_VAR_OK;
	struct tc_var_entry *e = ensure_entry(v, name, &err);
	if (!e)
		return err;
	if ((clear_flags & TC_VAR_READONLY) && (e->flags & TC_VAR_READONLY))
		return TC_VAR_ERR_READONLY;
	if (add_flags & TC_VAR_ARRAY) {
		int cr = convert_to_array(e);
		if (cr != TC_VAR_OK)
			return cr;
	} else if (add_flags & TC_VAR_ASSOC) {
		int cr = convert_to_assoc(e);
		if (cr != TC_VAR_OK)
			return cr;
	}
	unsigned old_flags = e->flags;
	if (add_flags & TC_VAR_LOWER)
		clear_flags |= TC_VAR_UPPER;
	if (add_flags & TC_VAR_UPPER)
		clear_flags |= TC_VAR_LOWER;
	e->flags = (e->flags | (add_flags & ~(TC_VAR_ARRAY | TC_VAR_ASSOC))) &
		   ~(clear_flags & ~(TC_VAR_ARRAY | TC_VAR_ASSOC));
	if ((old_flags ^ e->flags) & TC_VAR_EXPORT)
		v->env_dirty = true;
	if (e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC))
		v->env_dirty = true;
	return TC_VAR_OK;
}

int tc_var_reset_array(struct tc_vars *v, const char *name, unsigned add_flags)
{
	int err = TC_VAR_OK;
	struct tc_var_entry *e = ensure_entry(v, name, &err);
	if (!e)
		return err;
	if (e->flags & TC_VAR_READONLY)
		return TC_VAR_ERR_READONLY;
	if (e->flags & TC_VAR_ASSOC)
		return TC_VAR_ERR_TYPE;
	struct tc_array *a = tc_array_new();
	if (!a)
		return TC_VAR_ERR_NOMEM;
	free(e->value);
	e->value = NULL;
	tc_array_free(e->arr);
	e->arr = a;
	e->flags |= TC_VAR_ARRAY | add_flags;
	if (e->flags & TC_VAR_EXPORT)
		v->env_dirty = true;
	if (strcmp(name, "PATH") == 0)
		v->path_gen++;
	return TC_VAR_OK;
}

int tc_var_reset_assoc(struct tc_vars *v, const char *name, unsigned add_flags)
{
	int err = TC_VAR_OK;
	struct tc_var_entry *e = ensure_entry(v, name, &err);
	if (!e)
		return err;
	if (e->flags & TC_VAR_READONLY)
		return TC_VAR_ERR_READONLY;
	if (e->flags & TC_VAR_ARRAY)
		return TC_VAR_ERR_TYPE;
	struct tc_assoc *m = tc_assoc_new();
	if (!m)
		return TC_VAR_ERR_NOMEM;
	free(e->value);
	e->value = NULL;
	tc_assoc_free(e->assoc);
	e->assoc = m;
	e->flags |= TC_VAR_ASSOC | add_flags;
	if (e->flags & TC_VAR_EXPORT)
		v->env_dirty = true;
	if (strcmp(name, "PATH") == 0)
		v->path_gen++;
	return TC_VAR_OK;
}

int tc_var_set_elem_idx(struct tc_vars *v, const char *name, int64_t idx,
			const char *val, bool append, unsigned add_flags)
{
	int err = TC_VAR_OK;
	struct tc_var_entry *e = ensure_entry(v, name, &err);
	if (!e)
		return err;
	if (e->flags & TC_VAR_READONLY)
		return TC_VAR_ERR_READONLY;
	int cr = convert_to_array(e);
	if (cr != TC_VAR_OK)
		return cr;
	int64_t ridx = 0;
	if (tc_array_resolve_idx(e->arr, idx, &ridx) != TC_ARR_OK)
		return TC_VAR_ERR_BADSUB;
	int ar;
	unsigned eff_flags = e->flags | add_flags;
	if (append) {
		const char *cur = NULL;
		(void)tc_array_get(e->arr, ridx, &cur);
		char *joined = xstrcat(cur, val);
		if (!joined)
			return TC_VAR_ERR_NOMEM;
		char *tval = xstrdup_flags(joined, eff_flags);
		free(joined);
		if (!tval)
			return TC_VAR_ERR_NOMEM;
		ar = tc_array_set(e->arr, ridx, tval);
		free(tval);
	} else {
		char *tval = xstrdup_flags(val, eff_flags);
		if (!tval)
			return TC_VAR_ERR_NOMEM;
		ar = tc_array_set(e->arr, ridx, tval);
		free(tval);
	}
	if (ar == TC_ARR_BADSUB)
		return TC_VAR_ERR_BADSUB;
	if (ar != TC_ARR_OK)
		return TC_VAR_ERR_NOMEM;
	e->flags |= add_flags;
	if (e->flags & TC_VAR_EXPORT)
		v->env_dirty = true;
	return TC_VAR_OK;
}

int tc_var_append_elem(struct tc_vars *v, const char *name, const char *val,
		       unsigned add_flags)
{
	int err = TC_VAR_OK;
	struct tc_var_entry *e = ensure_entry(v, name, &err);
	if (!e)
		return err;
	if (e->flags & TC_VAR_READONLY)
		return TC_VAR_ERR_READONLY;
	int cr = convert_to_array(e);
	if (cr != TC_VAR_OK)
		return cr;
	char *tval = xstrdup_flags(val, e->flags | add_flags);
	if (!tval)
		return TC_VAR_ERR_NOMEM;
	int ar = tc_array_append(e->arr, tval);
	free(tval);
	if (ar == TC_ARR_BADSUB)
		return TC_VAR_ERR_BADSUB;
	if (ar != TC_ARR_OK)
		return TC_VAR_ERR_NOMEM;
	e->flags |= add_flags;
	if (e->flags & TC_VAR_EXPORT)
		v->env_dirty = true;
	return TC_VAR_OK;
}

int tc_var_set_elem_key(struct tc_vars *v, const char *name, const char *key,
			const char *val, bool append, unsigned add_flags)
{
	int err = TC_VAR_OK;
	struct tc_var_entry *e = ensure_entry(v, name, &err);
	if (!e)
		return err;
	if (e->flags & TC_VAR_READONLY)
		return TC_VAR_ERR_READONLY;
	if (!(e->flags & TC_VAR_ASSOC))
		return TC_VAR_ERR_TYPE;
	if (!e->assoc) {
		e->assoc = tc_assoc_new();
		if (!e->assoc)
			return TC_VAR_ERR_NOMEM;
	}
	int ar;
	unsigned eff_flags = e->flags | add_flags;
	if (append) {
		const char *cur = tc_assoc_get(e->assoc, key);
		char *joined = xstrcat(cur, val);
		if (!joined)
			return TC_VAR_ERR_NOMEM;
		char *tval = xstrdup_flags(joined, eff_flags);
		free(joined);
		if (!tval)
			return TC_VAR_ERR_NOMEM;
		ar = tc_assoc_set(e->assoc, key, tval);
		free(tval);
	} else {
		char *tval = xstrdup_flags(val, eff_flags);
		if (!tval)
			return TC_VAR_ERR_NOMEM;
		ar = tc_assoc_set(e->assoc, key, tval);
		free(tval);
	}
	if (ar != TC_ARR_OK)
		return TC_VAR_ERR_NOMEM;
	e->flags |= add_flags;
	if (e->flags & TC_VAR_EXPORT)
		v->env_dirty = true;
	return TC_VAR_OK;
}

int tc_var_unset_elem_idx(struct tc_vars *v, const char *name, int64_t idx)
{
	if (!tc_var_valid_name(name, strlen(name)))
		return TC_VAR_ERR_NAME;
	if (v->cap == 0)
		return TC_VAR_OK;
	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	if (!e->name)
		return TC_VAR_OK;
	if (e->flags & TC_VAR_READONLY)
		return TC_VAR_ERR_READONLY;
	if (e->flags & TC_VAR_ARRAY) {
		if (!e->arr)
			return TC_VAR_OK;
		int ar = tc_array_unset(e->arr, idx);
		return ar == TC_ARR_BADSUB ? TC_VAR_ERR_BADSUB : TC_VAR_OK;
	}
	if (e->flags & TC_VAR_ASSOC)
		return TC_VAR_ERR_TYPE;
	/* Scalar variable: unsetting [0] or [-1] unsets the scalar. */
	if (idx == 0 || idx == -1)
		return tc_var_unset(v, name);
	if (idx < -1)
		return TC_VAR_ERR_BADSUB;
	return TC_VAR_OK;
}

int tc_var_unset_elem_key(struct tc_vars *v, const char *name, const char *key)
{
	if (!tc_var_valid_name(name, strlen(name)))
		return TC_VAR_ERR_NAME;
	if (v->cap == 0)
		return TC_VAR_OK;
	struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
	if (!e->name)
		return TC_VAR_OK;
	if (e->flags & TC_VAR_READONLY)
		return TC_VAR_ERR_READONLY;
	if (!(e->flags & TC_VAR_ASSOC))
		return TC_VAR_ERR_TYPE;
	if (e->assoc)
		tc_assoc_unset(e->assoc, key);
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
			tc_array_free(e->arr);
			tc_assoc_free(e->assoc);
			memset(e, 0, sizeof(*e));
			v->count--;
			/* Rehash subsequent cluster entries to preserve probing. */
			size_t next = (idx + 1) & mask;
			while (v->slots[next].name) {
				struct tc_var_entry tmp = v->slots[next];
				memset(&v->slots[next], 0, sizeof(v->slots[next]));
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
		char *name = strndup(envp[i], nlen);
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
		    !(v->slots[i].flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)) &&
		    v->slots[i].value != NULL)
			n++;
	}

	char **envp = malloc((n + 1) * sizeof(*envp));
	if (!envp)
		return NULL;
	size_t out = 0;
	for (size_t i = 0; i < v->cap; i++) {
		const struct tc_var_entry *e = &v->slots[i];
		if (!e->name || !(e->flags & TC_VAR_EXPORT) ||
		    (e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)) || !e->value)
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
		if (e->name && (e->flags & TC_VAR_EXPORT) &&
		    !(e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)) && e->value &&
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
		if (!e->name || !(e->flags & TC_VAR_EXPORT) ||
		    (e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)) || !e->value)
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
		sv->name = strdup(kvs[i].name);
		if (!sv->name) {
			tc_var_pop_overrides(v, saves, *out_saved);
			*out_saved = 0;
			return TC_VAR_ERR_NOMEM;
		}
		sv->value = NULL;
		sv->arr = NULL;
		sv->assoc = NULL;
		sv->flags = 0;
		sv->existed = false;
		if (v->cap > 0) {
			struct tc_var_entry *e = find_slot(v->slots, v->cap, kvs[i].name);
			if (e->name) {
				sv->existed = true;
				sv->flags = e->flags;
				if (e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)) {
					sv->arr = e->arr;
					sv->assoc = e->assoc;
					e->arr = NULL;
					e->assoc = NULL;
					e->flags &= ~(TC_VAR_ARRAY | TC_VAR_ASSOC);
				} else if (e->value) {
					sv->value = strdup(e->value);
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
					tc_array_free(e->arr);
					tc_assoc_free(e->assoc);
					e->value = sv->value;
					e->arr = sv->arr;
					e->assoc = sv->assoc;
					sv->value = NULL;
					sv->arr = NULL;
					sv->assoc = NULL;
					e->flags = sv->flags;
					v->env_dirty = true;
					if (strcmp(sv->name, "PATH") == 0)
						v->path_gen++;
				}
			}
		}
		free(sv->name);
		free(sv->value);
		tc_array_free(sv->arr);
		tc_assoc_free(sv->assoc);
	}
}

/* Remove name from the live table regardless of readonly (used by scope pop/local). */
static void force_remove_slot(struct tc_vars *v, const char *name, bool free_payload)
{
	if (v->cap == 0)
		return;
	size_t mask = v->cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_var_entry *e = &v->slots[idx];
		if (!e->name)
			return;
		if (strcmp(e->name, name) == 0) {
			if (e->flags & TC_VAR_EXPORT)
				v->env_dirty = true;
			if (strcmp(name, "PATH") == 0)
				v->path_gen++;
			free(e->name);
			if (free_payload) {
				free(e->value);
				tc_array_free(e->arr);
				tc_assoc_free(e->assoc);
			}
			memset(e, 0, sizeof(*e));
			v->count--;
			size_t next = (idx + 1) & mask;
			while (v->slots[next].name) {
				struct tc_var_entry tmp = v->slots[next];
				memset(&v->slots[next], 0, sizeof(v->slots[next]));
				struct tc_var_entry *dst =
					find_slot(v->slots, v->cap, tmp.name);
				*dst = tmp;
				next = (next + 1) & mask;
			}
			return;
		}
		idx = (idx + 1) & mask;
	}
}

int tc_var_push_scope(struct tc_vars *v)
{
	if (v->scope_depth == v->scope_cap) {
		size_t ncap = v->scope_cap ? v->scope_cap * 2 : 8;
		struct tc_var_scope *ns = realloc(v->scopes, ncap * sizeof(*ns));
		if (!ns)
			return TC_VAR_ERR_NOMEM;
		v->scopes = ns;
		v->scope_cap = ncap;
	}
	v->scopes[v->scope_depth].saves = NULL;
	v->scopes[v->scope_depth].count = 0;
	v->scopes[v->scope_depth].cap = 0;
	v->scope_depth++;
	return TC_VAR_OK;
}

void tc_var_pop_scope(struct tc_vars *v)
{
	if (v->scope_depth == 0)
		return;
	struct tc_var_scope *sc = &v->scopes[--v->scope_depth];
	while (sc->count > 0) {
		struct tc_var_save *sv = &sc->saves[--sc->count];
		if (!sv->existed) {
			force_remove_slot(v, sv->name, true);
		} else {
			if (ensure_cap(v) == TC_VAR_OK) {
				struct tc_var_entry *e =
					find_slot(v->slots, v->cap, sv->name);
				if (e->name) {
					free(e->value);
					tc_array_free(e->arr);
					tc_assoc_free(e->assoc);
				} else {
					e->name = strdup(sv->name);
					if (e->name)
						v->count++;
				}
				if (e->name) {
					e->value = sv->value;
					e->arr = sv->arr;
					e->assoc = sv->assoc;
					sv->value = NULL;
					sv->arr = NULL;
					sv->assoc = NULL;
					e->flags = sv->flags;
					v->env_dirty = true;
					if (strcmp(sv->name, "PATH") == 0)
						v->path_gen++;
				}
			}
		}
		free(sv->name);
		free(sv->value);
		tc_array_free(sv->arr);
		tc_assoc_free(sv->assoc);
	}
	free(sc->saves);
	sc->saves = NULL;
	sc->cap = 0;
}

bool tc_var_is_local(const struct tc_vars *v, const char *name)
{
	if (v->scope_depth == 0)
		return false;
	const struct tc_var_scope *sc = &v->scopes[v->scope_depth - 1];
	for (size_t i = 0; i < sc->count; i++) {
		if (strcmp(sc->saves[i].name, name) == 0)
			return true;
	}
	return false;
}

int tc_var_make_local(struct tc_vars *v, const char *name)
{
	if (!tc_var_valid_name(name, strlen(name)))
		return TC_VAR_ERR_NAME;
	if (v->scope_depth == 0)
		return TC_VAR_OK;
	struct tc_var_scope *sc = &v->scopes[v->scope_depth - 1];
	for (size_t i = 0; i < sc->count; i++) {
		if (strcmp(sc->saves[i].name, name) == 0)
			return TC_VAR_OK;
	}

	if (sc->count == sc->cap) {
		size_t ncap = sc->cap ? sc->cap * 2 : 8;
		struct tc_var_save *ns = realloc(sc->saves, ncap * sizeof(*ns));
		if (!ns)
			return TC_VAR_ERR_NOMEM;
		sc->saves = ns;
		sc->cap = ncap;
	}

	char *ncopy = strdup(name);
	if (!ncopy)
		return TC_VAR_ERR_NOMEM;

	struct tc_var_save *sv = &sc->saves[sc->count];
	memset(sv, 0, sizeof(*sv));
	sv->name = ncopy;
	if (v->cap > 0) {
		struct tc_var_entry *e = find_slot(v->slots, v->cap, name);
		if (e->name) {
			if (e->flags & TC_VAR_READONLY) {
				free(ncopy);
				return TC_VAR_ERR_READONLY;
			}
			sv->existed = true;
			sv->flags = e->flags;
			sv->value = e->value;
			sv->arr = e->arr;
			sv->assoc = e->assoc;
			e->value = NULL;
			e->arr = NULL;
			e->assoc = NULL;
			force_remove_slot(v, name, false);
		}
	}
	sc->count++;
	return TC_VAR_OK;
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
			if (e->value != NULL || (e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)))
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
			if (e->value != NULL || (e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)))
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
