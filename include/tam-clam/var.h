/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell variables and environment table. Long-lived data uses malloc and
 * is freed explicitly; temporary command-override envp arrays use the
 * command arena. */
#ifndef TAM_CLAM_VAR_H
#define TAM_CLAM_VAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tam-clam/arena.h"
#include "tam-clam/array.h"

enum {
	TC_VAR_EXPORT   = 1 << 0,
	TC_VAR_READONLY = 1 << 1,
	TC_VAR_ARRAY    = 1 << 2,
	TC_VAR_ASSOC    = 1 << 3,
	TC_VAR_INTEGER  = 1 << 4,
	TC_VAR_LOWER    = 1 << 5,
	TC_VAR_UPPER    = 1 << 6,
};

enum tc_var_err {
	TC_VAR_OK           =  0,
	TC_VAR_ERR_NAME     = -1,
	TC_VAR_ERR_READONLY = -2,
	TC_VAR_ERR_NOMEM    = -3,
	TC_VAR_ERR_TYPE     = -4,
	TC_VAR_ERR_BADSUB   = -5,
};

struct tc_var_entry {
	char *name;
	char *value;            /* scalar value, or NULL */
	struct tc_array *arr;   /* non-NULL when TC_VAR_ARRAY */
	struct tc_assoc *assoc; /* non-NULL when TC_VAR_ASSOC */
	unsigned flags;
};

struct tc_var_scope;

struct tc_vars {
	struct tc_var_entry *slots;
	size_t cap;
	size_t count;
	char **envp;       /* cached NULL-terminated NAME=VALUE array */
	bool env_dirty;
	uint64_t path_gen; /* bumped whenever PATH is set or unset */
	struct tc_var_scope *scopes; /* dynamic local scope stack */
	size_t scope_depth;
	size_t scope_cap;
};

struct tc_var_kv {
	const char *name;
	const char *value;
};

struct tc_var_save {
	char *name;
	char *value;
	struct tc_array *arr;
	struct tc_assoc *assoc;
	unsigned flags;
	bool existed;
};

void tc_var_init(struct tc_vars *v);
void tc_var_finish(struct tc_vars *v);

/* True if name[0..len) is a valid POSIX shell variable name. */
bool tc_var_valid_name(const char *name, size_t len);

/* Import valid NAME=VALUE entries from environ as exported variables. */
int tc_var_import_environ(struct tc_vars *v, char **envp);

/* Read-only lookup of the variable entry, or NULL if not present. */
const struct tc_var_entry *tc_var_find(const struct tc_vars *v, const char *name);

/* Value of name (element 0 / key "0" for arrays), or NULL if unset. */
const char *tc_var_get(const struct tc_vars *v, const char *name);
bool tc_var_is_readonly(const struct tc_vars *v, const char *name);

/* Set name to val (if val != NULL) and OR add_flags into its flags. */
int tc_var_set(struct tc_vars *v, const char *name, const char *val,
	       unsigned add_flags);
/* Append val to scalar name (or element 0 / key "0" if array/assoc). */
int tc_var_append_scalar(struct tc_vars *v, const char *name, const char *val,
			 unsigned add_flags);
/* Declare name with add_flags and clear_flags (handles array/assoc creation
 * and scalar->array conversion; rejects array<->assoc conversion). */
int tc_var_declare(struct tc_vars *v, const char *name, unsigned add_flags,
		   unsigned clear_flags);
/* Reset name to an empty indexed or associative array (respects readonly). */
int tc_var_reset_array(struct tc_vars *v, const char *name, unsigned add_flags);
int tc_var_reset_assoc(struct tc_vars *v, const char *name, unsigned add_flags);
/* Set or append an indexed array element (converts scalar to array if needed). */
int tc_var_set_elem_idx(struct tc_vars *v, const char *name, int64_t idx,
			const char *val, bool append, unsigned add_flags);
/* Append a new element at max_idx + 1 (converts scalar to array if needed). */
int tc_var_append_elem(struct tc_vars *v, const char *name, const char *val,
		       unsigned add_flags);
/* Set or append an associative array element (name must be TC_VAR_ASSOC). */
int tc_var_set_elem_key(struct tc_vars *v, const char *name, const char *key,
			const char *val, bool append, unsigned add_flags);
/* Unset a single element from an indexed or associative array. */
int tc_var_unset_elem_idx(struct tc_vars *v, const char *name, int64_t idx);
int tc_var_unset_elem_key(struct tc_vars *v, const char *name, const char *key);
int tc_var_unset(struct tc_vars *v, const char *name);

/* Return cached NULL-terminated envp for execve; NULL on OOM. */
char **tc_var_envp(struct tc_vars *v);

/* Build a one-shot envp in arena a combining exported vars with overrides. */
char **tc_var_envp_override(const struct tc_vars *v,
			    const struct tc_var_kv *kvs, size_t n,
			    struct tc_arena *a);

/* Lookup name checking overrides (last wins) before the variable table. */
const char *tc_var_lookup_override(const struct tc_vars *v,
				   const struct tc_var_kv *kvs, size_t n,
				   const char *name);

/* Temporarily apply overrides (for regular builtins), recording prior state
 * in saves[0..n). Returns TC_VAR_OK or an error code (restoring on error). */
int tc_var_push_overrides(struct tc_vars *v, const struct tc_var_kv *kvs,
			  size_t n, struct tc_var_save *saves,
			  size_t *out_saved);
void tc_var_pop_overrides(struct tc_vars *v, struct tc_var_save *saves,
			  size_t nsaved);

/* Dynamic function-local variable scopes (0.3.1). */
int tc_var_push_scope(struct tc_vars *v);
void tc_var_pop_scope(struct tc_vars *v);
/* Make name local to the top scope frame (no-op if already local in top frame). */
int tc_var_make_local(struct tc_vars *v, const char *name);
bool tc_var_is_local(const struct tc_vars *v, const char *name);

/* Return a heap-allocated, name-sorted array of pointers to entries matching
 * filter_flag (0 = all entries with non-NULL value, TC_VAR_EXPORT or
 * TC_VAR_READONLY = all entries with that flag). Caller frees *out. */
int tc_var_list(const struct tc_vars *v, unsigned filter_flag,
		const struct tc_var_entry ***out, size_t *out_n);

#endif
