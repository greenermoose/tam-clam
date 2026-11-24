/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for sparse indexed arrays, associative arrays, and var integration. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "tam-clam/array.h"
#include "tam-clam/var.h"

static void test_indexed_array(void)
{
	struct tc_array *a = tc_array_new();
	CHECK(a != NULL);
	CHECK(tc_array_len(a) == 0);
	CHECK(tc_array_max_idx(a) == -1);

	/* Negative index on empty array is bad subscript. */
	const char *val = NULL;
	CHECK(tc_array_get(a, -1, &val) == TC_ARR_BADSUB);
	CHECK(tc_array_set(a, -1, "x") == TC_ARR_BADSUB);

	/* Sparse inserts out of order remain sorted. */
	CHECK(tc_array_set(a, 1000, "thousand") == TC_ARR_OK);
	CHECK(tc_array_set(a, 2, "two") == TC_ARR_OK);
	CHECK(tc_array_set(a, 50, "fifty") == TC_ARR_OK);
	CHECK(tc_array_len(a) == 3);
	CHECK(tc_array_max_idx(a) == 1000);
	CHECK(a->elems[0].idx == 2);
	CHECK(a->elems[1].idx == 50);
	CHECK(a->elems[2].idx == 1000);

	CHECK(tc_array_get(a, 50, &val) == TC_ARR_OK);
	CHECK(val != NULL && strcmp(val, "fifty") == 0);
	CHECK(tc_array_get(a, 3, &val) == TC_ARR_OK);
	CHECK(val == NULL);

	/* Negative indexing resolves against max_idx + 1. */
	CHECK(tc_array_get(a, -1, &val) == TC_ARR_OK);
	CHECK(val != NULL && strcmp(val, "thousand") == 0);
	CHECK(tc_array_get(a, -2, &val) == TC_ARR_OK);
	CHECK(val == NULL);
	CHECK(tc_array_get(a, -1002, &val) == TC_ARR_BADSUB);

	/* Append goes to max_idx + 1. */
	CHECK(tc_array_append(a, "after") == TC_ARR_OK);
	CHECK(tc_array_max_idx(a) == 1001);
	CHECK(tc_array_get(a, 1001, &val) == TC_ARR_OK);
	CHECK(val != NULL && strcmp(val, "after") == 0);

	/* Overwrite existing element. */
	CHECK(tc_array_set(a, 50, "FIFTY") == TC_ARR_OK);
	CHECK(tc_array_get(a, 50, &val) == TC_ARR_OK);
	CHECK(val != NULL && strcmp(val, "FIFTY") == 0);
	CHECK(tc_array_len(a) == 4);

	/* Unset middle and negative element. */
	CHECK(tc_array_unset(a, 50) == TC_ARR_OK);
	CHECK(tc_array_len(a) == 3);
	CHECK(tc_array_get(a, 50, &val) == TC_ARR_OK);
	CHECK(val == NULL);

	CHECK(tc_array_unset(a, -1) == TC_ARR_OK);
	CHECK(tc_array_max_idx(a) == 1000);

	/* Clone preserves all elements. */
	struct tc_array *c = tc_array_clone(a);
	CHECK(c != NULL);
	CHECK(tc_array_len(c) == tc_array_len(a));
	CHECK(tc_array_get(c, 2, &val) == TC_ARR_OK);
	CHECK(val != NULL && strcmp(val, "two") == 0);
	tc_array_free(c);
	tc_array_free(a);
}

static void test_assoc_array(void)
{
	struct tc_assoc *m = tc_assoc_new();
	CHECK(m != NULL);
	CHECK(tc_assoc_len(m) == 0);
	CHECK(tc_assoc_get(m, "missing") == NULL);

	CHECK(tc_assoc_set(m, "beta", "2") == TC_ARR_OK);
	CHECK(tc_assoc_set(m, "alpha", "1") == TC_ARR_OK);
	CHECK(tc_assoc_set(m, "with space", "three words") == TC_ARR_OK);
	CHECK(tc_assoc_len(m) == 3);
	CHECK(strcmp(tc_assoc_get(m, "alpha"), "1") == 0);
	CHECK(strcmp(tc_assoc_get(m, "with space"), "three words") == 0);

	/* Overwrite. */
	CHECK(tc_assoc_set(m, "alpha", "ONE") == TC_ARR_OK);
	CHECK(tc_assoc_len(m) == 3);
	CHECK(strcmp(tc_assoc_get(m, "alpha"), "ONE") == 0);

	/* Sorted listing. */
	const struct tc_assoc_slot **slots = NULL;
	size_t n = 0;
	CHECK(tc_assoc_list(m, &slots, &n) == TC_ARR_OK);
	CHECK(n == 3);
	CHECK(strcmp(slots[0]->key, "alpha") == 0);
	CHECK(strcmp(slots[1]->key, "beta") == 0);
	CHECK(strcmp(slots[2]->key, "with space") == 0);
	free(slots);

	/* Delete and probe integrity across many keys (triggers resize). */
	for (int i = 0; i < 40; i++) {
		char k[16], v[16];
		snprintf(k, sizeof(k), "k%02d", i);
		snprintf(v, sizeof(v), "v%02d", i);
		CHECK(tc_assoc_set(m, k, v) == TC_ARR_OK);
	}
	CHECK(tc_assoc_len(m) == 43);
	tc_assoc_unset(m, "beta");
	CHECK(tc_assoc_get(m, "beta") == NULL);
	CHECK(strcmp(tc_assoc_get(m, "k39"), "v39") == 0);

	struct tc_assoc *c = tc_assoc_clone(m);
	CHECK(c != NULL);
	CHECK(tc_assoc_len(c) == tc_assoc_len(m));
	CHECK(strcmp(tc_assoc_get(c, "k15"), "v15") == 0);
	tc_assoc_free(c);
	tc_assoc_free(m);
}

static void test_var_array_integration(void)
{
	struct tc_vars v;
	tc_var_init(&v);

	/* Scalar converts to indexed array preserving index 0. */
	CHECK(tc_var_set(&v, "x", "first", 0) == TC_VAR_OK);
	CHECK(tc_var_append_elem(&v, "x", "second", 0) == TC_VAR_OK);
	CHECK(strcmp(tc_var_get(&v, "x"), "first") == 0);
	const struct tc_var_entry *e = tc_var_find(&v, "x");
	CHECK(e != NULL);
	CHECK((e->flags & TC_VAR_ARRAY) != 0);
	CHECK(tc_array_len(e->arr) == 2);

	/* Cannot convert indexed array to associative array. */
	CHECK(tc_var_declare(&v, "x", TC_VAR_ASSOC, 0) == TC_VAR_ERR_TYPE);

	/* Associative array declared and populated. */
	CHECK(tc_var_declare(&v, "m", TC_VAR_ASSOC, 0) == TC_VAR_OK);
	CHECK(tc_var_set_elem_key(&v, "m", "k", "v", false, 0) == TC_VAR_OK);
	CHECK(tc_var_set_elem_key(&v, "m", "k", "2", true, 0) == TC_VAR_OK);
	e = tc_var_find(&v, "m");
	CHECK(e != NULL);
	CHECK(strcmp(tc_assoc_get(e->assoc, "k"), "v2") == 0);

	/* Arrays are never exported into envp even if TC_VAR_EXPORT is set. */
	CHECK(tc_var_declare(&v, "x", TC_VAR_EXPORT, 0) == TC_VAR_OK);
	CHECK(tc_var_set(&v, "PLAIN", "yes", TC_VAR_EXPORT) == TC_VAR_OK);
	char **envp = tc_var_envp(&v);
	CHECK(envp != NULL);
	bool saw_plain = false, saw_x = false;
	for (size_t i = 0; envp[i]; i++) {
		if (strncmp(envp[i], "PLAIN=", 6) == 0)
			saw_plain = true;
		if (strncmp(envp[i], "x=", 2) == 0)
			saw_x = true;
	}
	CHECK(saw_plain);
	CHECK(!saw_x);

	/* Readonly protection. */
	CHECK(tc_var_declare(&v, "x", TC_VAR_READONLY, 0) == TC_VAR_OK);
	CHECK(tc_var_set_elem_idx(&v, "x", 0, "nope", false, 0) == TC_VAR_ERR_READONLY);
	CHECK(tc_var_unset_elem_idx(&v, "x", 0) == TC_VAR_ERR_READONLY);

	tc_var_finish(&v);
}

int main(void)
{
	test_indexed_array();
	test_assoc_array();
	test_var_array_integration();
	CHECK_DONE("test_array");
}
