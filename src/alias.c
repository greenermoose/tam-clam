/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell alias table and the alias / unalias builtins. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/alias.h"
#include "tam-clam/builtins.h"
#include "tam-clam/shell.h"

#define TC_ALIAS_INIT_CAP 32

static uint64_t hash_str(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

void tc_alias_init(struct tc_aliases *al)
{
	al->slots = NULL;
	al->cap = 0;
	al->count = 0;
}

void tc_alias_clear(struct tc_aliases *al)
{
	if (!al->slots)
		return;
	for (size_t i = 0; i < al->cap; i++) {
		free(al->slots[i].name);
		free(al->slots[i].value);
		al->slots[i].name = NULL;
		al->slots[i].value = NULL;
	}
	al->count = 0;
}

void tc_alias_finish(struct tc_aliases *al)
{
	tc_alias_clear(al);
	free(al->slots);
	al->slots = NULL;
	al->cap = 0;
}

bool tc_alias_valid_name(const char *name)
{
	if (!name || *name == '\0')
		return false;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		unsigned char c = *p;
		if (c <= 0x20 || c == 0x7f)
			return false;
		switch (c) {
		case '/':
		case '=':
		case '$':
		case '`':
		case '\'':
		case '"':
		case '\\':
		case '|':
		case '&':
		case ';':
		case '<':
		case '>':
		case '(':
		case ')':
			return false;
		default:
			break;
		}
	}
	return true;
}

static struct tc_alias_entry *find_slot(struct tc_alias_entry *slots, size_t cap,
					const char *name)
{
	size_t mask = cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_alias_entry *e = &slots[idx];
		if (!e->name || strcmp(e->name, name) == 0)
			return e;
		idx = (idx + 1) & mask;
	}
}

static int grow_table(struct tc_aliases *al, size_t new_cap)
{
	struct tc_alias_entry *ns = calloc(new_cap, sizeof(*ns));
	if (!ns)
		return -1;
	for (size_t i = 0; i < al->cap; i++) {
		if (!al->slots[i].name)
			continue;
		struct tc_alias_entry *dst = find_slot(ns, new_cap, al->slots[i].name);
		*dst = al->slots[i];
	}
	free(al->slots);
	al->slots = ns;
	al->cap = new_cap;
	return 0;
}

const char *tc_alias_get(const struct tc_aliases *al, const char *name)
{
	if (al->cap == 0 || !name)
		return NULL;
	struct tc_alias_entry *e = find_slot(al->slots, al->cap, name);
	return e->name ? e->value : NULL;
}

int tc_alias_set(struct tc_aliases *al, const char *name, const char *value)
{
	if (!tc_alias_valid_name(name))
		return -2;
	if (al->cap == 0) {
		if (grow_table(al, TC_ALIAS_INIT_CAP) < 0)
			return -1;
	} else if ((al->count + 1) * 10 >= al->cap * 7) {
		if (grow_table(al, al->cap * 2) < 0)
			return -1;
	}

	struct tc_alias_entry *e = find_slot(al->slots, al->cap, name);
	char *vcopy = strdup(value);
	if (!vcopy)
		return -1;
	if (e->name) {
		free(e->value);
		e->value = vcopy;
		return 0;
	}
	char *ncopy = strdup(name);
	if (!ncopy) {
		free(vcopy);
		return -1;
	}
	e->name = ncopy;
	e->value = vcopy;
	al->count++;
	return 0;
}

bool tc_alias_unset(struct tc_aliases *al, const char *name)
{
	if (al->cap == 0 || !name)
		return false;
	size_t mask = al->cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_alias_entry *e = &al->slots[idx];
		if (!e->name)
			return false;
		if (strcmp(e->name, name) == 0) {
			free(e->name);
			free(e->value);
			e->name = NULL;
			e->value = NULL;
			al->count--;
			size_t next = (idx + 1) & mask;
			while (al->slots[next].name) {
				struct tc_alias_entry tmp = al->slots[next];
				al->slots[next].name = NULL;
				al->slots[next].value = NULL;
				struct tc_alias_entry *dst =
					find_slot(al->slots, al->cap, tmp.name);
				*dst = tmp;
				next = (next + 1) & mask;
			}
			return true;
		}
		idx = (idx + 1) & mask;
	}
}

static int cmp_alias_entry(const void *a, const void *b)
{
	const struct tc_alias_entry *ea = *(const struct tc_alias_entry *const *)a;
	const struct tc_alias_entry *eb = *(const struct tc_alias_entry *const *)b;
	return strcmp(ea->name, eb->name);
}

int tc_alias_list(const struct tc_aliases *al,
		  const struct tc_alias_entry ***out, size_t *out_n)
{
	*out = NULL;
	*out_n = 0;
	if (al->count == 0)
		return 0;
	const struct tc_alias_entry **arr = malloc(al->count * sizeof(*arr));
	if (!arr)
		return -1;
	size_t pos = 0;
	for (size_t i = 0; i < al->cap; i++) {
		if (al->slots[i].name)
			arr[pos++] = &al->slots[i];
	}
	qsort(arr, pos, sizeof(*arr), cmp_alias_entry);
	*out = arr;
	*out_n = pos;
	return 0;
}

static int print_one_alias(const char *name, const char *value)
{
	if (tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
	    tc_write_all(STDOUT_FILENO, "=", 1) < 0 ||
	    tc_write_shell_quoted(STDOUT_FILENO, value) < 0 ||
	    tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
		return -1;
	return 0;
}

int tc_bi_alias(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		tc_error(sh, line, "alias: %s: invalid option", argv[i]);
		return TC_STATUS_USAGE;
	}

	if (i >= argc) {
		const struct tc_alias_entry **list = NULL;
		size_t n = 0;
		if (tc_alias_list(&sh->aliases, &list, &n) < 0) {
			tc_error(sh, line, "alias: out of memory");
			return 1;
		}
		int status = 0;
		for (size_t j = 0; j < n; j++) {
			if (print_one_alias(list[j]->name, list[j]->value) < 0) {
				tc_error(sh, line, "alias: write error: %s",
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
		const char *arg = argv[i];
		const char *eq = strchr(arg, '=');
		if (eq && eq > arg) {
			size_t nlen = (size_t)(eq - arg);
			char *name = malloc(nlen + 1);
			if (!name) {
				tc_error(sh, line, "alias: out of memory");
				return 1;
			}
			memcpy(name, arg, nlen);
			name[nlen] = '\0';
			int r = tc_alias_set(&sh->aliases, name, eq + 1);
			if (r == -2) {
				tc_error(sh, line, "alias: `%s': invalid alias name",
					 name);
				status = 1;
			} else if (r < 0) {
				tc_error(sh, line, "alias: out of memory");
				free(name);
				return 1;
			}
			free(name);
		} else {
			const char *val = tc_alias_get(&sh->aliases, arg);
			if (!val) {
				tc_error(sh, line, "alias: %s: not found", arg);
				status = 1;
			} else if (print_one_alias(arg, val) < 0) {
				tc_error(sh, line, "alias: write error: %s",
					 strerror(errno));
				return 1;
			}
		}
	}
	return status;
}

int tc_bi_unalias(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	bool all = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'a') {
				all = true;
			} else {
				tc_error(sh, line, "unalias: -%c: invalid option", *p);
				return TC_STATUS_USAGE;
			}
		}
	}

	if (all) {
		tc_alias_clear(&sh->aliases);
		return 0;
	}
	if (i >= argc) {
		tc_error(sh, line, "unalias: usage: unalias [-a] name...");
		return TC_STATUS_USAGE;
	}

	int status = 0;
	for (; i < argc; i++) {
		if (!tc_alias_unset(&sh->aliases, argv[i])) {
			tc_error(sh, line, "unalias: %s: not found", argv[i]);
			status = 1;
		}
	}
	return status;
}
