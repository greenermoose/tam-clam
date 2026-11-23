/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell variables and environment table. Long-lived data uses malloc and
 * is freed explicitly; temporary command-override envp arrays use the
 * command arena. */
#ifndef TAM_CLAM_VAR_H
#define TAM_CLAM_VAR_H

#include <stdbool.h>
#include <stddef.h>

#include "tam-clam/arena.h"

enum {
	TC_VAR_EXPORT   = 1 << 0,
	TC_VAR_READONLY = 1 << 1,
};

enum tc_var_err {
	TC_VAR_OK           =  0,
	TC_VAR_ERR_NAME     = -1,
	TC_VAR_ERR_READONLY = -2,
	TC_VAR_ERR_NOMEM    = -3,
};

struct tc_var_entry {
	char *name;
	char *value;   /* NULL if declared without a value */
	unsigned flags;
};

struct tc_vars {
	struct tc_var_entry *slots;
	size_t cap;
	size_t count;
	char **envp;   /* cached NULL-terminated NAME=VALUE array */
	bool env_dirty;
};

struct tc_var_kv {
	const char *name;
	const char *value;
};

struct tc_var_save {
	char *name;
	char *value;
	unsigned flags;
	bool existed;
};

void tc_var_init(struct tc_vars *v);
void tc_var_finish(struct tc_vars *v);

/* True if name[0..len) is a valid POSIX shell variable name. */
bool tc_var_valid_name(const char *name, size_t len);

/* Import valid NAME=VALUE entries from environ as exported variables. */
int tc_var_import_environ(struct tc_vars *v, char **envp);

/* Value of name, or NULL if unset (or declared without a value). */
const char *tc_var_get(const struct tc_vars *v, const char *name);
bool tc_var_is_readonly(const struct tc_vars *v, const char *name);

/* Set name to val (if val != NULL) and OR add_flags into its flags. */
int tc_var_set(struct tc_vars *v, const char *name, const char *val,
	       unsigned add_flags);
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

/* Return a heap-allocated, name-sorted array of pointers to entries matching
 * filter_flag (0 = all entries with non-NULL value, TC_VAR_EXPORT or
 * TC_VAR_READONLY = all entries with that flag). Caller frees *out. */
int tc_var_list(const struct tc_vars *v, unsigned filter_flag,
		const struct tc_var_entry ***out, size_t *out_n);

#endif
