/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sparse indexed arrays (int64_t -> char*) and associative arrays
 * (char* -> char*). Long-lived heap data; every allocation is checked. */
#ifndef TAM_CLAM_ARRAY_H
#define TAM_CLAM_ARRAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TC_ARRAY_MAX ((size_t)1 << 20)

enum tc_arr_err {
	TC_ARR_OK     =  0,
	TC_ARR_NOMEM  = -1,
	TC_ARR_BADSUB = -2,
};

struct tc_array_elem {
	int64_t idx;
	char *val;
};

struct tc_array {
	struct tc_array_elem *elems; /* sorted ascending by idx */
	size_t len;
	size_t cap;
};

struct tc_assoc_slot {
	char *key;
	char *val;
};

struct tc_assoc {
	struct tc_assoc_slot *slots;
	size_t cap;
	size_t count;
};

/* Indexed array operations. */
struct tc_array *tc_array_new(void);
void tc_array_free(struct tc_array *a);
struct tc_array *tc_array_clone(const struct tc_array *a);
int tc_array_resolve_idx(const struct tc_array *a, int64_t idx, int64_t *out);
int tc_array_get(const struct tc_array *a, int64_t idx, const char **out);
int tc_array_set(struct tc_array *a, int64_t idx, const char *val);
int tc_array_append(struct tc_array *a, const char *val);
int tc_array_unset(struct tc_array *a, int64_t idx);
size_t tc_array_len(const struct tc_array *a);
int64_t tc_array_max_idx(const struct tc_array *a);

/* Associative array operations. */
struct tc_assoc *tc_assoc_new(void);
void tc_assoc_free(struct tc_assoc *m);
struct tc_assoc *tc_assoc_clone(const struct tc_assoc *m);
const char *tc_assoc_get(const struct tc_assoc *m, const char *key);
int tc_assoc_set(struct tc_assoc *m, const char *key, const char *val);
void tc_assoc_unset(struct tc_assoc *m, const char *key);
size_t tc_assoc_len(const struct tc_assoc *m);
/* Return a heap-allocated, key-sorted array of slot pointers. Caller frees *out. */
int tc_assoc_list(const struct tc_assoc *m, const struct tc_assoc_slot ***out,
		  size_t *out_n);

#endif
